#pragma once
// ============================================================
// Aphelion Research — Execution / Fill Engine (Layer F)
// Translates strategy decisions into fills or rejections
// Applies spread, slippage, commission
// Mutates account state — this is the authority boundary
// ============================================================

#include "aphelion/types.h"
#include "aphelion/account.h"
#include "aphelion/market_state.h"

namespace aphelion {

enum class FillResult : uint8_t {
    FILLED          = 0,
    REJECTED_MARGIN = 1,
    REJECTED_SIZE   = 2,
    REJECTED_LIQUIDATED = 3,
    NO_ACTION       = 4
};

struct FillReport {
    FillResult result;
    double     fill_price    = 0.0;
    double     fill_quantity = 0.0;
    double     margin_used   = 0.0;
};

// Execute a strategy decision against an account.
// This is the single authority point for position entry/exit.
FillReport execute_decision(
    Account& account,
    const StrategyDecision& decision,
    const MarketState& market,
    const SimulationParams& params
);

// Close a specific position by index.
void close_position(
    Account& account,
    int pos_idx,
    double exit_price,
    ExitReason reason,
    const MarketState& market,
    double commission
);

} // namespace aphelion
