// ============================================================
// Aphelion Research — Strategy (Layer E)
// SMA Crossover baseline + factory
// ============================================================

#include "aphelion/strategy.h"
#include <cmath>
#include <algorithm>

namespace aphelion {

// ── SMA Crossover ───────────────────────────────────────────

SmaCrossoverStrategy::SmaCrossoverStrategy(int fast_period, int slow_period)
    : fast_period_(fast_period), slow_period_(slow_period) {}

void SmaCrossoverStrategy::prepare(const Bar* tape, size_t tape_size) {
    // Pre-compute SMAs over the entire tape.
    // This runs ONCE before replay — not in the hot loop.
    fast_sma_.resize(tape_size, 0.0);
    slow_sma_.resize(tape_size, 0.0);

    // Running sum approach for O(n) SMA computation.
    double fast_sum = 0.0;
    double slow_sum = 0.0;

    for (size_t i = 0; i < tape_size; ++i) {
        fast_sum += tape[i].close;
        slow_sum += tape[i].close;

        if (i >= static_cast<size_t>(fast_period_)) {
            fast_sum -= tape[i - fast_period_].close;
        }
        if (i >= static_cast<size_t>(slow_period_)) {
            slow_sum -= tape[i - slow_period_].close;
        }

        size_t fast_count = std::min(i + 1, static_cast<size_t>(fast_period_));
        size_t slow_count = std::min(i + 1, static_cast<size_t>(slow_period_));

        fast_sma_[i] = fast_sum / static_cast<double>(fast_count);
        slow_sma_[i] = slow_sum / static_cast<double>(slow_count);
    }

    // ── V2: Precompute signal tape ──────────────────────────
    // One byte per bar. Eliminates SMA double-lookups from hot path.
    // ~90-99% of bars produce NONE, allowing the replay engine to
    // skip virtual dispatch entirely on those bars.
    signal_tape_.resize(tape_size, static_cast<uint8_t>(Signal::NONE));

    for (size_t i = static_cast<size_t>(slow_period_); i < tape_size; ++i) {
        double fast_now  = fast_sma_[i];
        double slow_now  = slow_sma_[i];
        double fast_prev = fast_sma_[i - 1];
        double slow_prev = slow_sma_[i - 1];

        if (fast_prev <= slow_prev && fast_now > slow_now) {
            signal_tape_[i] = static_cast<uint8_t>(Signal::BULLISH_CROSS);
        } else if (fast_prev >= slow_prev && fast_now < slow_now) {
            signal_tape_[i] = static_cast<uint8_t>(Signal::BEARISH_CROSS);
        }
    }

    // Free SMA vectors — signals are precomputed, no longer needed
    // (keeps memory footprint tight for large tournaments)
    fast_sma_.clear();
    fast_sma_.shrink_to_fit();
    slow_sma_.clear();
    slow_sma_.shrink_to_fit();
}

StrategyDecision SmaCrossoverStrategy::decide(
    const MarketState& market,
    const AccountState& account,
    size_t bar_index
) {
    // V2 fast path: use precomputed signal tape if available
    if (!signal_tape_.empty() && bar_index < signal_tape_.size()) {
        Signal sig = static_cast<Signal>(signal_tape_[bar_index]);
        if (sig == Signal::NONE) {
            StrategyDecision d;
            d.action = ActionType::HOLD;
            d.risk_fraction = account.risk_per_trade;
            return d;
        }
        return build_decision_from_signal(
            sig,
            market.current_bar->close,
            market.current_bar->high,
            market.current_bar->low,
            account.open_position_count,
            account.risk_per_trade
        );
    }

    // V1 fallback: direct SMA comparison (only if signal tape wasn't built)
    StrategyDecision d;
    d.action = ActionType::HOLD;
    d.risk_fraction = account.risk_per_trade;

    if (bar_index < static_cast<size_t>(slow_period_)) return d;
    if (fast_sma_.empty() || slow_sma_.empty()) return d;

    double fast_now  = fast_sma_[bar_index];
    double slow_now  = slow_sma_[bar_index];
    double fast_prev = fast_sma_[bar_index - 1];
    double slow_prev = slow_sma_[bar_index - 1];

    double price = market.current_bar->close;
    double atr_proxy = market.current_bar->high - market.current_bar->low;
    if (atr_proxy <= 0.0) atr_proxy = price * 0.001;

    bool has_position = (account.open_position_count > 0);

    if (fast_prev <= slow_prev && fast_now > slow_now) {
        if (!has_position) {
            d.action      = ActionType::OPEN_LONG;
            d.stop_loss   = price - atr_proxy * 2.0;
            d.take_profit = price + atr_proxy * 3.0;
        } else {
            d.action = ActionType::CLOSE;
        }
        return d;
    }

    if (fast_prev >= slow_prev && fast_now < slow_now) {
        if (!has_position) {
            d.action      = ActionType::OPEN_SHORT;
            d.stop_loss   = price + atr_proxy * 2.0;
            d.take_profit = price - atr_proxy * 3.0;
        } else {
            d.action = ActionType::CLOSE;
        }
        return d;
    }

    return d;
}

// ── Factory ─────────────────────────────────────────────────

std::unique_ptr<IStrategy> create_strategy(const SimulationParams& params) {
    switch (params.strategy_id) {
        case 1:
            return std::make_unique<ContextAwareSmaStrategy>(
                params.fast_period, params.slow_period
            );
        case 0:
        default:
            return std::make_unique<SmaCrossoverStrategy>(
                params.fast_period, params.slow_period
            );
    }
}


// ============================================================
// Context-Aware SMA Crossover (V3)
// ============================================================

ContextAwareSmaStrategy::ContextAwareSmaStrategy(int fast_period, int slow_period)
    : fast_period_(fast_period), slow_period_(slow_period) {}

void ContextAwareSmaStrategy::prepare(const Bar* tape, size_t tape_size) {
    // Compute SMAs and signal tape (same as SmaCrossoverStrategy)
    std::vector<double> fast_sma(tape_size, 0.0);
    std::vector<double> slow_sma(tape_size, 0.0);

    double fast_sum = 0.0, slow_sum = 0.0;
    for (size_t i = 0; i < tape_size; ++i) {
        fast_sum += tape[i].close;
        slow_sum += tape[i].close;
        if (i >= static_cast<size_t>(fast_period_))
            fast_sum -= tape[i - fast_period_].close;
        if (i >= static_cast<size_t>(slow_period_))
            slow_sum -= tape[i - slow_period_].close;
        size_t fc = std::min(i + 1, static_cast<size_t>(fast_period_));
        size_t sc = std::min(i + 1, static_cast<size_t>(slow_period_));
        fast_sma[i] = fast_sum / static_cast<double>(fc);
        slow_sma[i] = slow_sum / static_cast<double>(sc);
    }

    signal_tape_.resize(tape_size, static_cast<uint8_t>(Signal::NONE));
    for (size_t i = static_cast<size_t>(slow_period_); i < tape_size; ++i) {
        if (fast_sma[i - 1] <= slow_sma[i - 1] && fast_sma[i] > slow_sma[i])
            signal_tape_[i] = static_cast<uint8_t>(Signal::BULLISH_CROSS);
        else if (fast_sma[i - 1] >= slow_sma[i - 1] && fast_sma[i] < slow_sma[i])
            signal_tape_[i] = static_cast<uint8_t>(Signal::BEARISH_CROSS);
    }
}

void ContextAwareSmaStrategy::prepare_with_features(
    const Bar* tape, size_t tape_size,
    const BarFeatures* features
) {
    prepare(tape, tape_size);
    feature_tape_ = features;
    feature_tape_size_ = tape_size;
}

StrategyDecision ContextAwareSmaStrategy::decide(
    const MarketState& market,
    const AccountState& account,
    size_t bar_index
) {
    // Fallback without features: same as basic SMA
    Signal sig = signal_at(bar_index);
    if (sig == Signal::NONE) {
        StrategyDecision d;
        d.action = ActionType::HOLD;
        return d;
    }
    return build_decision_from_signal(
        sig, market.current_bar->close, market.current_bar->high,
        market.current_bar->low, account.open_position_count,
        account.risk_per_trade
    );
}

StrategyDecision ContextAwareSmaStrategy::decide_with_context(
    const MarketState& market,
    const AccountState& account,
    size_t bar_index,
    const BarFeatures& feat
) {
    Signal sig = signal_at(bar_index);
    if (sig == Signal::NONE) {
        StrategyDecision d;
        d.action = ActionType::HOLD;
        return d;
    }

    StrategyDecision d;
    d.action = ActionType::HOLD;
    d.risk_fraction = account.risk_per_trade;
    d.confidence = Confidence::MEDIUM;

    double close = market.current_bar->close;
    double high  = market.current_bar->high;
    double low   = market.current_bar->low;
    double atr_proxy = high - low;
    if (atr_proxy <= 0.0) atr_proxy = close * 0.001;

    bool has_position = (account.open_position_count > 0);
    Regime regime = static_cast<Regime>(feat.regime);

    // ── Regime gating ───────────────────────────────────────
    // Skip new entries during transitions (unstable)
    if (!has_position && regime == Regime::TRANSITION) {
        return d; // HOLD — wait for regime to settle
    }

    // ── Direction agreement check ───────────────────────────
    bool is_bullish_signal = (sig == Signal::BULLISH_CROSS);
    bool trend_agrees = is_bullish_signal
        ? (feat.trend_alignment > 0.0f)
        : (feat.trend_alignment < 0.0f);

    bool htf_agrees = is_bullish_signal
        ? (feat.htf_bias > 0.0f || feat.htf_bias == 0.0f) // neutral = don't block
        : (feat.htf_bias < 0.0f || feat.htf_bias == 0.0f);

    // ── Confidence computation ──────────────────────────────
    int agreement_count = 0;
    if (trend_agrees) agreement_count++;
    if (htf_agrees) agreement_count++;

    // Momentum agreement
    bool momentum_agrees = is_bullish_signal
        ? (feat.momentum_short > 0.0f)
        : (feat.momentum_short < 0.0f);
    if (momentum_agrees) agreement_count++;

    // Regime suitability
    bool regime_favorable = (regime == Regime::TRENDING_UP && is_bullish_signal) ||
                            (regime == Regime::TRENDING_DOWN && !is_bullish_signal) ||
                            (regime == Regime::VOLATILE_EXPANSION);
    if (regime_favorable) agreement_count++;

    // Map agreement to confidence
    if (agreement_count >= 4) d.confidence = Confidence::HIGH;
    else if (agreement_count >= 2) d.confidence = Confidence::MEDIUM;
    else d.confidence = Confidence::LOW;

    // ── Preferred regime ────────────────────────────────────
    d.preferred_regime = is_bullish_signal ? Regime::TRENDING_UP : Regime::TRENDING_DOWN;

    // ── Dynamic stop/TP based on volatility ─────────────────
    // High volatility → wider stops. Low volatility → tighter stops.
    float vol_mult = 1.0f;
    if (feat.volatility_percentile > 0.7f) vol_mult = 1.5f;
    else if (feat.volatility_percentile < 0.3f) vol_mult = 0.75f;

    double sl_distance = atr_proxy * 2.0 * vol_mult;
    double tp_distance = atr_proxy * 3.0 * vol_mult;

    // ── Build decision ──────────────────────────────────────
    if (is_bullish_signal) {
        if (!has_position) {
            d.action      = ActionType::OPEN_LONG;
            d.stop_loss   = close - sl_distance;
            d.take_profit = close + tp_distance;
        } else {
            d.action = ActionType::CLOSE;
        }
    } else {
        if (!has_position) {
            d.action      = ActionType::OPEN_SHORT;
            d.stop_loss   = close + sl_distance;
            d.take_profit = close - tp_distance;
        } else {
            d.action = ActionType::CLOSE;
        }
    }

    return d;
}

} // namespace aphelion
