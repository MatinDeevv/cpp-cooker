#pragma once
// ============================================================
// Aphelion Research — Account / Risk Core (Layer D)
// Balance, equity, margin, liquidation, trade lifecycle
// ============================================================

#include "aphelion/types.h"
#include "aphelion/market_state.h"
#include <vector>
#include <cstddef>

namespace aphelion {

// Maximum concurrent positions per account (compile-time bound)
constexpr int MAX_POSITIONS_PER_ACCOUNT = 8;

struct Account {
    AccountState state;
    Position     positions[MAX_POSITIONS_PER_ACCOUNT];
    std::vector<TradeRecord>  trade_log;    // cold path, appended on close
    std::vector<float>        equity_curve; // one entry per bar

    void init(uint32_t id, const SimulationParams& params);

    // Mark-to-market all open positions, update equity/margin/drawdown.
    // This is the HOT path — called once per bar per account.
    void mark_to_market(const MarketState& market);

    // Check and execute stop-loss / take-profit exits.
    // Returns number of positions closed.
    int check_sl_tp(const MarketState& market);

    // Enforce margin stop-out.  Returns true if liquidation occurred.
    bool enforce_stop_out(const MarketState& market, double stop_out_level);

    // Record equity snapshot for equity curve.
    void snapshot_equity();
};

// Compute maximum position size (lots) given risk parameters.
double compute_position_size(
    const AccountState& acct,
    double stop_distance,      // price units
    double entry_price,
    double risk_fraction
);

} // namespace aphelion
