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

struct IntelligenceState;

// Maximum concurrent positions per account (compile-time bound)
constexpr int MAX_POSITIONS_PER_ACCOUNT = 8;

struct SessionRiskState {
    int64_t session_key = -1;
    double  session_start_balance = 0.0;
    double  session_peak_equity = 0.0;
    int     entry_count = 0;
    uint8_t trading_disabled = 0;
    uint8_t flatten_requested = 0;
    uint8_t _pad[2] = {};
};

struct Account {
    AccountState state;
    SessionRiskState session;
    Position     positions[MAX_POSITIONS_PER_ACCOUNT];
    std::vector<TradeRecord>  trade_log;    // cold path, appended on close
    std::vector<float>        equity_curve; // one entry per bar

    void init(uint32_t id, const SimulationParams& params);
    void reset_session(int64_t session_key);
    void register_closed_trade(const TradeRecord& trade, double margin_released);
    double active_notional() const;

    // ── FUSED HOT PATH ──────────────────────────────────────
    // Single pass over positions: mark-to-market + SL/TP + stop-out.
    // Replaces the old mark_to_market() + check_sl_tp() + enforce_stop_out()
    // sequence which required 2-3 separate position loops.
    // This is THE performance-critical function.
    PerBarResult update_per_bar(
        double bid, double ask,
        const Bar* bar, size_t bar_index,
        double stop_out_level,
        const IntelligenceState* intelligence = nullptr
    );

    // Mark-to-market all open positions, update equity/margin/drawdown.
    // Kept for backward compat and cold-path use (e.g., final MTM).
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
