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
}

StrategyDecision SmaCrossoverStrategy::decide(
    const MarketState& market,
    const AccountState& account,
    size_t bar_index
) {
    StrategyDecision d;
    d.action = ActionType::HOLD;
    d.risk_fraction = account.risk_per_trade;

    // Need at least slow_period bars before acting
    if (bar_index < static_cast<size_t>(slow_period_)) return d;

    double fast_now  = fast_sma_[bar_index];
    double slow_now  = slow_sma_[bar_index];
    double fast_prev = fast_sma_[bar_index - 1];
    double slow_prev = slow_sma_[bar_index - 1];

    double price = market.current_bar->close;
    double atr_proxy = market.current_bar->high - market.current_bar->low;
    if (atr_proxy <= 0.0) atr_proxy = price * 0.001; // fallback 0.1%

    bool has_position = (account.open_position_count > 0);

    // Bullish crossover: fast crosses above slow
    if (fast_prev <= slow_prev && fast_now > slow_now && !has_position) {
        d.action      = ActionType::OPEN_LONG;
        d.stop_loss   = price - atr_proxy * 2.0;
        d.take_profit = price + atr_proxy * 3.0;
        return d;
    }

    // Bearish crossover: fast crosses below slow
    if (fast_prev >= slow_prev && fast_now < slow_now && !has_position) {
        d.action      = ActionType::OPEN_SHORT;
        d.stop_loss   = price + atr_proxy * 2.0;
        d.take_profit = price - atr_proxy * 3.0;
        return d;
    }

    // Close on reverse signal when holding
    if (has_position) {
        if (fast_prev <= slow_prev && fast_now > slow_now) {
            d.action = ActionType::CLOSE;
            return d;
        }
        if (fast_prev >= slow_prev && fast_now < slow_now) {
            d.action = ActionType::CLOSE;
            return d;
        }
    }

    return d;
}

// ── Factory ─────────────────────────────────────────────────

std::unique_ptr<IStrategy> create_strategy(const SimulationParams& params) {
    switch (params.strategy_id) {
        case 0:
        default:
            return std::make_unique<SmaCrossoverStrategy>(
                params.fast_period, params.slow_period
            );
    }
}

} // namespace aphelion
