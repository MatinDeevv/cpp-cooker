#pragma once
// ============================================================
// Aphelion Research — Strategy Interface (Layer E)
// Decision generation only — no authority over risk/execution
// ============================================================

#include "aphelion/types.h"
#include "aphelion/market_state.h"
#include "aphelion/account.h"
#include <memory>
#include <vector>

namespace aphelion {

// ── Abstract strategy base ──────────────────────────────────
// Strategies are cold-path objects. They propose; engine decides.
// Virtual dispatch cost is acceptable: one call per bar per account,
// dominated by the mark-to-market / margin arithmetic.
class IStrategy {
public:
    virtual ~IStrategy() = default;

    virtual StrategyDecision decide(
        const MarketState& market,
        const AccountState& account,
        size_t bar_index
    ) = 0;

    virtual const char* name() const = 0;

    // Called once before replay starts. Tape provided for indicator precompute.
    virtual void prepare(const Bar* tape, size_t tape_size) {}
};

// ── SMA Crossover (baseline V1 strategy) ────────────────────
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

private:
    int fast_period_;
    int slow_period_;
    std::vector<double> fast_sma_;
    std::vector<double> slow_sma_;
};

// ── Strategy Factory ────────────────────────────────────────
std::unique_ptr<IStrategy> create_strategy(const SimulationParams& params);

} // namespace aphelion
