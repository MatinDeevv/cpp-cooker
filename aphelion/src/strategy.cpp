// ============================================================
// Aphelion Research - Strategy (Layer E)
// SMA crossover baseline + contextual deformation
// ============================================================

#include "aphelion/strategy.h"

#include <algorithm>
#include <cmath>

namespace aphelion {

namespace {

float clamp01(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

float clamp11(float value) {
    return std::max(-1.0f, std::min(1.0f, value));
}

Confidence confidence_from_score(float score) {
    if (score >= 0.88f) return Confidence::EXTREME;
    if (score >= 0.68f) return Confidence::HIGH;
    if (score >= 0.42f) return Confidence::MEDIUM;
    return Confidence::LOW;
}

uint16_t base_hold_bars_for_timeframe(const Bar* bar) {
    if (bar == nullptr || bar->timeframe_sec <= 0) return 8;
    const int bars = std::clamp(3600 / bar->timeframe_sec, 2, 18);
    return static_cast<uint16_t>(bars);
}

} // namespace

SmaCrossoverStrategy::SmaCrossoverStrategy(int fast_period, int slow_period)
    : fast_period_(fast_period), slow_period_(slow_period) {}

void SmaCrossoverStrategy::prepare(const Bar* tape, size_t tape_size) {
    fast_sma_.resize(tape_size, 0.0);
    slow_sma_.resize(tape_size, 0.0);

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

        const size_t fast_count = std::min(i + 1, static_cast<size_t>(fast_period_));
        const size_t slow_count = std::min(i + 1, static_cast<size_t>(slow_period_));

        fast_sma_[i] = fast_sum / static_cast<double>(fast_count);
        slow_sma_[i] = slow_sum / static_cast<double>(slow_count);
    }

    signal_tape_.resize(tape_size, static_cast<uint8_t>(Signal::NONE));
    for (size_t i = static_cast<size_t>(slow_period_); i < tape_size; ++i) {
        const double fast_now  = fast_sma_[i];
        const double slow_now  = slow_sma_[i];
        const double fast_prev = fast_sma_[i - 1];
        const double slow_prev = slow_sma_[i - 1];

        if (fast_prev <= slow_prev && fast_now > slow_now) {
            signal_tape_[i] = static_cast<uint8_t>(Signal::BULLISH_CROSS);
        } else if (fast_prev >= slow_prev && fast_now < slow_now) {
            signal_tape_[i] = static_cast<uint8_t>(Signal::BEARISH_CROSS);
        }
    }

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
    if (!signal_tape_.empty() && bar_index < signal_tape_.size()) {
        const Signal sig = static_cast<Signal>(signal_tape_[bar_index]);
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

    StrategyDecision d;
    d.action = ActionType::HOLD;
    d.risk_fraction = account.risk_per_trade;

    if (bar_index < static_cast<size_t>(slow_period_)) return d;
    if (fast_sma_.empty() || slow_sma_.empty()) return d;

    const double fast_now  = fast_sma_[bar_index];
    const double slow_now  = slow_sma_[bar_index];
    const double fast_prev = fast_sma_[bar_index - 1];
    const double slow_prev = slow_sma_[bar_index - 1];

    double atr_proxy = market.current_bar->high - market.current_bar->low;
    if (atr_proxy <= 0.0) atr_proxy = market.current_bar->close * 0.001;

    const bool has_position = (account.open_position_count > 0);

    if (fast_prev <= slow_prev && fast_now > slow_now) {
        if (!has_position) {
            d.action      = ActionType::OPEN_LONG;
            d.stop_loss   = market.current_bar->close - atr_proxy * 2.0;
            d.take_profit = market.current_bar->close + atr_proxy * 3.0;
        } else {
            d.action = ActionType::CLOSE;
        }
        return d;
    }

    if (fast_prev >= slow_prev && fast_now < slow_now) {
        if (!has_position) {
            d.action      = ActionType::OPEN_SHORT;
            d.stop_loss   = market.current_bar->close + atr_proxy * 2.0;
            d.take_profit = market.current_bar->close - atr_proxy * 3.0;
        } else {
            d.action = ActionType::CLOSE;
        }
        return d;
    }

    return d;
}

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

ContextAwareSmaStrategy::ContextAwareSmaStrategy(int fast_period, int slow_period)
    : fast_period_(fast_period), slow_period_(slow_period) {}

void ContextAwareSmaStrategy::prepare(const Bar* tape, size_t tape_size) {
    std::vector<double> fast_sma(tape_size, 0.0);
    std::vector<double> slow_sma(tape_size, 0.0);

    double fast_sum = 0.0;
    double slow_sum = 0.0;
    for (size_t i = 0; i < tape_size; ++i) {
        fast_sum += tape[i].close;
        slow_sum += tape[i].close;
        if (i >= static_cast<size_t>(fast_period_))
            fast_sum -= tape[i - fast_period_].close;
        if (i >= static_cast<size_t>(slow_period_))
            slow_sum -= tape[i - slow_period_].close;
        const size_t fc = std::min(i + 1, static_cast<size_t>(fast_period_));
        const size_t sc = std::min(i + 1, static_cast<size_t>(slow_period_));
        fast_sma[i] = fast_sum / static_cast<double>(fc);
        slow_sma[i] = slow_sum / static_cast<double>(sc);
    }

    signal_tape_.resize(tape_size, static_cast<uint8_t>(Signal::NONE));
    for (size_t i = static_cast<size_t>(slow_period_); i < tape_size; ++i) {
        if (fast_sma[i - 1] <= slow_sma[i - 1] && fast_sma[i] > slow_sma[i]) {
            signal_tape_[i] = static_cast<uint8_t>(Signal::BULLISH_CROSS);
        } else if (fast_sma[i - 1] >= slow_sma[i - 1] && fast_sma[i] < slow_sma[i]) {
            signal_tape_[i] = static_cast<uint8_t>(Signal::BEARISH_CROSS);
        }
    }
}

void ContextAwareSmaStrategy::prepare_with_intelligence(
    const Bar* tape, size_t tape_size,
    const IntelligenceState* intelligence
) {
    prepare(tape, tape_size);
    intelligence_tape_ = intelligence;
    intelligence_tape_size_ = tape_size;
}

StrategyDecision ContextAwareSmaStrategy::decide(
    const MarketState& market,
    const AccountState& account,
    size_t bar_index
) {
    const Signal sig = signal_at(bar_index);
    if (sig == Signal::NONE) {
        StrategyDecision d;
        d.action = ActionType::HOLD;
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

StrategyDecision ContextAwareSmaStrategy::decide_with_intelligence(
    const MarketState& market,
    const AccountState& account,
    size_t bar_index,
    const IntelligenceState& intelligence
) {
    const Signal sig = signal_at(bar_index);
    if (sig == Signal::NONE) {
        StrategyDecision d;
        d.action = ActionType::HOLD;
        return d;
    }

    StrategyDecision d;
    d.action = ActionType::HOLD;
    d.risk_fraction = account.risk_per_trade;

    const bool has_position = (account.open_position_count > 0);
    if (has_position) {
        d.action = ActionType::CLOSE;
        d.close_reason = ExitReason::STRATEGY;
        return d;
    }

    const bool is_bullish_signal = (sig == Signal::BULLISH_CROSS);
    const float direction_sign = is_bullish_signal ? 1.0f : -1.0f;
    const float aligned_pressure = is_bullish_signal
        ? intelligence.long_pressure
        : intelligence.short_pressure;
    const float opposing_pressure = is_bullish_signal
        ? intelligence.short_pressure
        : intelligence.long_pressure;
    const float ech_aligned = intelligence.entropy_collapse * direction_sign;
    const float energy_aligned = intelligence.directional_energy * direction_sign;
    const float cross_tf = intelligence.cross_timeframe_coherence;
    const float confidence_score = clamp01(
        0.22f +
        aligned_pressure * 0.28f +
        intelligence.context_validity * 0.18f +
        intelligence.temporal_stability * 0.12f +
        std::max(0.0f, ech_aligned) * 0.10f +
        std::max(0.0f, energy_aligned) * 0.08f +
        cross_tf * 0.08f -
        intelligence.failure_memory * 0.12f -
        std::max(0.0f, -ech_aligned) * 0.08f
    );

    if (intelligence.context_validity < 0.03f ||
        (intelligence.failure_memory > 0.95f && intelligence.exit_urgency > 0.90f)) {
        return d;
    }

    d.confidence = confidence_from_score(confidence_score);
    d.size_multiplier = clamp01(
        0.55f +
        aligned_pressure * 0.35f +
        std::max(0.0f, ech_aligned) * 0.18f +
        std::max(0.0f, energy_aligned) * 0.14f +
        cross_tf * 0.08f -
        intelligence.failure_memory * 0.20f
    ) + 0.15f;
    d.stop_width_multiplier = std::clamp(
        0.90f +
        std::max(0.0f, ech_aligned) * 0.18f +
        std::max(0.0f, intelligence.volatility_state) * 0.10f -
        intelligence.failure_memory * 0.12f,
        0.70f,
        1.35f
    );
    d.take_profit_multiplier = std::clamp(
        1.00f +
        std::max(0.0f, ech_aligned) * 0.28f +
        std::max(0.0f, energy_aligned) * 0.18f +
        aligned_pressure * 0.16f -
        intelligence.exit_urgency * 0.20f,
        0.75f,
        1.90f
    );
    d.entry_aggression = clamp01(
        0.30f +
        intelligence.entry_aggression * 0.45f +
        std::max(0.0f, ech_aligned) * 0.12f -
        intelligence.failure_memory * 0.12f
    );
    d.exit_urgency = intelligence.exit_urgency;
    d.context_pressure = clamp01(aligned_pressure);

    const uint16_t base_hold = base_hold_bars_for_timeframe(market.current_bar);
    const float hold_scale = std::clamp(
        intelligence.hold_time_multiplier *
        (0.80f + 0.40f * d.context_pressure) *
        (1.0f + 0.18f * std::max(0.0f, ech_aligned)),
        0.50f,
        2.20f
    );
    d.expected_hold_bars = static_cast<uint16_t>(std::clamp(
        static_cast<int>(std::lround(static_cast<double>(base_hold) * hold_scale)),
        2,
        64
    ));
    d.minimum_hold_bars = static_cast<uint16_t>(std::max(1, static_cast<int>(d.expected_hold_bars / 3)));

    d.preferred_regime = is_bullish_signal ? Regime::TRENDING_UP : Regime::TRENDING_DOWN;
    if (intelligence.regime == Regime::COMPRESSION && ech_aligned > 0.15f) {
        d.preferred_regime = Regime::VOLATILE_EXPANSION;
    }

    double atr_proxy = market.current_bar->high - market.current_bar->low;
    if (atr_proxy <= 0.0) atr_proxy = market.current_bar->close * 0.001;

    const double sl_distance = atr_proxy *
        (1.55 + 0.35 * (1.0 - d.entry_aggression)) *
        d.stop_width_multiplier;
    const double tp_distance = atr_proxy *
        (2.10 + 0.65 * d.context_pressure) *
        d.take_profit_multiplier;

    if (is_bullish_signal) {
        d.action      = ActionType::OPEN_LONG;
        d.stop_loss   = market.current_bar->close - sl_distance;
        d.take_profit = market.current_bar->close + tp_distance;
    } else {
        d.action      = ActionType::OPEN_SHORT;
        d.stop_loss   = market.current_bar->close + sl_distance;
        d.take_profit = market.current_bar->close - tp_distance;
    }

    return d;
}

} // namespace aphelion
