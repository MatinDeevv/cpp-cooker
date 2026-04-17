#pragma once
// ============================================================
// Aphelion Research — Strategy Interface (Layer E)
// Decision generation only — no authority over risk/execution
//
// V2: Signal precomputation for hot-path devirtualization.
// V3: Context-aware strategies with features, regime, confidence.
//     Strategies receive the full precomputed feature context
//     and can produce richer decisions with confidence and
//     regime compatibility metadata.
// ============================================================

#include "aphelion/types.h"
#include "aphelion/market_state.h"
#include "aphelion/account.h"
#include "aphelion/intelligence.h"
#include <memory>
#include <vector>

namespace aphelion {

// ── Abstract strategy base ──────────────────────────────────
class IStrategy {
public:
    virtual ~IStrategy() = default;

    // Full decision with account context (virtual, used on signal bars only)
    virtual StrategyDecision decide(
        const MarketState& market,
        const AccountState& account,
        size_t bar_index
    ) = 0;

    // V3: Context-aware decision with feature access.
    // Default: delegates to basic decide(). Override for intelligence.
    virtual StrategyDecision decide_with_intelligence(
        const MarketState& market,
        const AccountState& account,
        size_t bar_index,
        const IntelligenceState& intelligence
    ) {
        return decide(market, account, bar_index);
    }

    virtual const char* name() const = 0;

    // Called once before replay starts. Tape provided for indicator precompute.
    virtual void prepare(const Bar* tape, size_t tape_size) {}

    // V3: Prepare with feature tape access.
    // Default: delegates to prepare(tape, size). Override for feature-aware prep.
    virtual void prepare_with_intelligence(
        const Bar* tape, size_t tape_size,
        const IntelligenceState* intelligence
    ) {
        prepare(tape, tape_size);
    }

    // ── Signal tape interface (V2) ──────────────────────────
    virtual Signal signal_at(size_t bar_index) const { return Signal::NONE; }
    virtual bool has_signal_tape() const { return false; }
    virtual size_t min_signal_bar() const { return 0; }

    // V3: Does this strategy produce confidence-aware decisions?
    virtual bool is_intelligence_aware() const { return false; }
};

// ── SMA Crossover (baseline V1 strategy, V2 signal-enabled) ─
class SmaCrossoverStrategy : public IStrategy {
public:
    SmaCrossoverStrategy(int fast_period, int slow_period);

    StrategyDecision decide(
        const MarketState& market,
        const AccountState& account,
        size_t bar_index
    ) override;

    const char* name() const override { return "SMA_Crossover"; }

    void prepare(const Bar* tape, size_t tape_size) override;

    // V2: precomputed signal tape
    Signal signal_at(size_t bar_index) const override {
        return (bar_index < signal_tape_.size())
            ? static_cast<Signal>(signal_tape_[bar_index])
            : Signal::NONE;
    }
    bool has_signal_tape() const override { return !signal_tape_.empty(); }
    size_t min_signal_bar() const override { return static_cast<size_t>(slow_period_); }

    int fast_period() const { return fast_period_; }
    int slow_period() const { return slow_period_; }

private:
    int fast_period_;
    int slow_period_;
    std::vector<double> fast_sma_;
    std::vector<double> slow_sma_;
    std::vector<uint8_t> signal_tape_;
};

// ── Context-Aware SMA Crossover (V3) ────────────────────────
// Same SMA signal logic but enriched with:
//   - Regime-gated entries (skip signals in bad regimes)
//   - Confidence from multi-factor agreement
//   - Dynamic stop/TP from volatility state
//   - HTF alignment filtering
class ContextAwareSmaStrategy : public IStrategy {
public:
    ContextAwareSmaStrategy(int fast_period, int slow_period);

    StrategyDecision decide(
        const MarketState& market,
        const AccountState& account,
        size_t bar_index
    ) override;

    StrategyDecision decide_with_intelligence(
        const MarketState& market,
        const AccountState& account,
        size_t bar_index,
        const IntelligenceState& intelligence
    ) override;

    const char* name() const override { return "ContextSMA"; }

    void prepare(const Bar* tape, size_t tape_size) override;
    void prepare_with_intelligence(
        const Bar* tape, size_t tape_size,
        const IntelligenceState* intelligence
    ) override;

    Signal signal_at(size_t bar_index) const override {
        return (bar_index < signal_tape_.size())
            ? static_cast<Signal>(signal_tape_[bar_index])
            : Signal::NONE;
    }
    bool has_signal_tape() const override { return !signal_tape_.empty(); }
    size_t min_signal_bar() const override { return static_cast<size_t>(slow_period_); }
    bool is_intelligence_aware() const override { return true; }

    int fast_period() const { return fast_period_; }
    int slow_period() const { return slow_period_; }

private:
    int fast_period_;
    int slow_period_;
    std::vector<uint8_t> signal_tape_;
    const IntelligenceState* intelligence_tape_ = nullptr;
    size_t intelligence_tape_size_ = 0;
};

// ── Inline signal-to-decision builder (no virtual dispatch) ─
// Used by replay engine when signal != NONE.
inline StrategyDecision build_decision_from_signal(
    Signal sig,
    double close, double high, double low,
    int open_position_count,
    double risk_per_trade
) {
    StrategyDecision d;
    d.action = ActionType::HOLD;
    d.risk_fraction = risk_per_trade;
    d.confidence = Confidence::MEDIUM;

    double atr_proxy = high - low;
    if (atr_proxy <= 0.0) atr_proxy = close * 0.001;

    bool has_position = (open_position_count > 0);

    if (sig == Signal::BULLISH_CROSS) {
        if (!has_position) {
            d.action      = ActionType::OPEN_LONG;
            d.stop_loss   = close - atr_proxy * 2.0;
            d.take_profit = close + atr_proxy * 3.0;
        } else {
            d.action = ActionType::CLOSE;
        }
    } else if (sig == Signal::BEARISH_CROSS) {
        if (!has_position) {
            d.action      = ActionType::OPEN_SHORT;
            d.stop_loss   = close + atr_proxy * 2.0;
            d.take_profit = close - atr_proxy * 3.0;
        } else {
            d.action = ActionType::CLOSE;
        }
    }

    return d;
}

// ── Strategy Factory ────────────────────────────────────────
// strategy_id: 0 = SmaCrossover (V2), 1 = ContextAwareSma (V3)
std::unique_ptr<IStrategy> create_strategy(const SimulationParams& params);

} // namespace aphelion
