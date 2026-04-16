#pragma once
// ============================================================
// Aphelion Research — Core Types
// Flat, cache-friendly, hot-path-aware data structures
// ============================================================

#include <cstdint>
#include <cstddef>
#include <limits>
#include <cmath>

namespace aphelion {

// ── Timestamp: milliseconds since Unix epoch (UTC) ──────────
using Timestamp = int64_t;

// ── Bar: 80 bytes, cache-aligned ────────────────────────────
// Laid out for sequential streaming.  Two bars per cache line
// on 64-byte lines; three per 128-byte prefetch window.
// No pointers, no padding surprises on any LP64 / LLP64 ABI.
struct alignas(16) Bar {
    Timestamp time_ms;          // 8
    double    open;             // 8
    double    high;             // 8
    double    low;              // 8
    double    close;            // 8
    uint64_t  tick_volume;      // 8
    int32_t   spread;           // 4
    uint32_t  real_volume_lo;   // 4  (low 32 bits of real_volume)
    int32_t   timeframe_sec;    // 4  (computed at load)
    int32_t   delta_sec;        // 4  (seconds since previous bar)
    uint8_t   flags;            // 1  (bit field below)
    uint8_t   _pad[7];         // 7  → total = 80 bytes

    // Flag bits
    static constexpr uint8_t FLAG_GAP            = 0x01;
    static constexpr uint8_t FLAG_WEEKEND_GAP    = 0x02;
    static constexpr uint8_t FLAG_SESSION_GAP    = 0x04;
    static constexpr uint8_t FLAG_UNEXPECTED_GAP = 0x08;
    static constexpr uint8_t FLAG_MISSING_BARS   = 0x10;

    bool is_gap()            const { return flags & FLAG_GAP; }
    bool is_weekend_gap()    const { return flags & FLAG_WEEKEND_GAP; }
    bool is_session_gap()    const { return flags & FLAG_SESSION_GAP; }
    bool is_unexpected_gap() const { return flags & FLAG_UNEXPECTED_GAP; }
    bool has_missing_bars()  const { return flags & FLAG_MISSING_BARS; }

    double spread_price() const {
        // Spread is in points; for XAUUSD 1 point = 0.01
        return static_cast<double>(spread) * 0.01;
    }
};

static_assert(sizeof(Bar) == 80, "Bar must be exactly 80 bytes");

// ── Trade Direction ─────────────────────────────────────────
enum class Direction : uint8_t {
    LONG  = 0,
    SHORT = 1
};

// ── Exit Reason ─────────────────────────────────────────────
enum class ExitReason : uint8_t {
    NONE        = 0,
    STOP_LOSS   = 1,
    TAKE_PROFIT = 2,
    STRATEGY    = 3,
    LIQUIDATION = 4,
    END_OF_DATA = 5
};

// ── Open Position: 96 bytes ─────────────────────────────────
// Stored in a flat pool per account.  Max positions bounded.
struct alignas(16) Position {
    double    entry_price;      // 8
    double    stop_loss;        // 8
    double    take_profit;      // 8
    double    quantity;          // 8  (lots or units)
    double    used_margin;      // 8
    double    unrealized_pnl;   // 8
    Timestamp entry_time_ms;    // 8
    uint32_t  entry_bar_idx;    // 4
    Direction direction;        // 1
    uint8_t   active;           // 1  (0=closed, 1=open)
    uint8_t   _pad[18];        // 18 → total = 80 bytes
};

static_assert(sizeof(Position) == 80, "Position must be exactly 80 bytes");

// ── Closed Trade Record (for trade log) ─────────────────────
struct TradeRecord {
    Direction direction;
    uint32_t  entry_bar_idx;
    uint32_t  exit_bar_idx;
    Timestamp entry_time_ms;
    Timestamp exit_time_ms;
    double    entry_price;
    double    exit_price;
    double    quantity;
    double    gross_pnl;
    double    net_pnl;
    ExitReason exit_reason;
};

// ── Strategy Decision ───────────────────────────────────────
enum class ActionType : uint8_t {
    HOLD      = 0,
    OPEN_LONG = 1,
    OPEN_SHORT= 2,
    CLOSE     = 3
};

struct StrategyDecision {
    ActionType action       = ActionType::HOLD;
    double     stop_loss    = 0.0;   // price
    double     take_profit  = 0.0;   // price
    double     risk_fraction= 0.01;  // fraction of equity to risk
};

// ── Account Hot State: 128 bytes (2 cache lines) ────────────
// This is the ONLY state touched per bar per account in the
// inner replay loop.  Everything else is cold-path.
struct alignas(64) AccountState {
    double   balance;           // 8
    double   equity;            // 8
    double   used_margin;       // 8
    double   free_margin;       // 8
    double   margin_level;      // 8  (percent, 0 if no margin used)
    double   peak_equity;       // 8  (for drawdown tracking)
    double   max_drawdown;      // 8  (running max drawdown fraction)
    double   max_leverage;      // 8  → 64 bytes (one cache line)

    double   initial_balance;   // 8
    double   risk_per_trade;    // 8
    double   realized_pnl;      // 8
    int32_t  open_position_count; // 4
    int32_t  total_trades;      // 4
    int32_t  winning_trades;    // 4
    uint8_t  liquidated;        // 1
    uint8_t  _pad[3];          // 3
    double   gross_profit;      // 8
    double   gross_loss;        // 8
    uint32_t account_id;        // 4 (+4 implicit pad → 128 bytes)

    void reset(uint32_t id, double init_bal, double max_lev, double risk_pct) {
        balance          = init_bal;
        equity           = init_bal;
        used_margin      = 0.0;
        free_margin      = init_bal;
        margin_level     = 0.0;
        peak_equity      = init_bal;
        max_drawdown     = 0.0;
        max_leverage     = max_lev;
        initial_balance  = init_bal;
        risk_per_trade   = risk_pct;
        realized_pnl     = 0.0;
        open_position_count = 0;
        total_trades     = 0;
        winning_trades   = 0;
        liquidated       = 0;
        gross_profit     = 0.0;
        gross_loss       = 0.0;
        account_id       = id;
    }
};
static_assert(sizeof(AccountState) == 128, "AccountState must be exactly 128 bytes (2 cache lines)");

// ── Simulation Parameters ───────────────────────────────────
struct SimulationParams {
    double initial_balance    = 10000.0;
    double max_leverage       = 500.0;
    double risk_per_trade     = 0.01;     // 1% of equity
    double stop_out_level     = 50.0;     // margin level % for stop-out
    double commission_per_lot = 0.0;      // round-trip commission
    double slippage_points    = 0.0;      // additional slippage in points
    int    max_positions      = 1;        // max concurrent positions per account
    int    strategy_id        = 0;        // which strategy to use
    // Strategy-specific params
    int    fast_period        = 10;
    int    slow_period        = 50;
};

// ── Constants ───────────────────────────────────────────────
constexpr double POINT_VALUE = 0.01;   // XAUUSD: 1 point = $0.01
constexpr double CONTRACT_SIZE = 100.0; // 1 lot = 100 oz for XAUUSD
constexpr double MIN_LOT = 0.01;
constexpr double MAX_LOT = 100.0;
constexpr double LOT_STEP = 0.01;

} // namespace aphelion
