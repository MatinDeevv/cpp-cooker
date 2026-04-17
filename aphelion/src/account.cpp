// ============================================================
// Aphelion Research — Account / Risk Core (Layer D)
// Hot-path mark-to-market, SL/TP, stop-out, position sizing
// ============================================================

#include "aphelion/account.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace aphelion {

void Account::init(uint32_t id, const SimulationParams& params) {
    state.reset(id, params.initial_balance, params.max_leverage, params.risk_per_trade);
    std::memset(positions, 0, sizeof(positions));
    trade_log.clear();
    equity_curve.clear();
}

// ============================================================
// FUSED HOT PATH: Single-pass MTM + SL/TP + Stop-out
// ============================================================
// This replaces the old 3-function sequence (mark_to_market +
// check_sl_tp + re-MTM + enforce_stop_out) with a SINGLE pass
// over the position array.
//
// Performance gains:
//   - 1 position loop instead of 2-3
//   - Early exit when all open positions have been visited
//   - No redundant equity recomputation after SL/TP closes
//   - Inline close accounting avoids function call overhead

PerBarResult Account::update_per_bar(
    double bid, double ask,
    const Bar* bar, size_t bar_index,
    double stop_out_level
) {
    PerBarResult result;

    if (state.liquidated) return result;

    double total_unrealized = 0.0;
    double total_margin     = 0.0;
    int    remaining        = state.open_position_count;

    for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
        if (remaining <= 0) break;  // Early exit: no more open positions

        Position& p = positions[i];
        if (!p.active) continue;
        --remaining;

        // ── SL/TP check using bar extremes ──────────────────
        bool hit_sl = false;
        bool hit_tp = false;
        double exit_price;

        if (p.direction == Direction::LONG) {
            if (p.stop_loss > 0.0 && bar->low <= p.stop_loss) {
                hit_sl = true;
                exit_price = p.stop_loss;
            } else if (p.take_profit > 0.0 && bar->high >= p.take_profit) {
                hit_tp = true;
                exit_price = p.take_profit;
            }
        } else {
            if (p.stop_loss > 0.0 && bar->high >= p.stop_loss) {
                hit_sl = true;
                exit_price = p.stop_loss;
            } else if (p.take_profit > 0.0 && bar->low <= p.take_profit) {
                hit_tp = true;
                exit_price = p.take_profit;
            }
        }

        if (hit_sl || hit_tp) {
            // ── Close inline ────────────────────────────────
            double price_delta = (p.direction == Direction::LONG)
                ? (exit_price - p.entry_price)
                : (p.entry_price - exit_price);
            double gross = price_delta * p.quantity * CONTRACT_SIZE;

            // Cold path: record trade
            TradeRecord rec;
            rec.direction     = p.direction;
            rec.entry_bar_idx = p.entry_bar_idx;
            rec.exit_bar_idx  = static_cast<uint32_t>(bar_index);
            rec.entry_time_ms = p.entry_time_ms;
            rec.exit_time_ms  = bar->time_ms;
            rec.entry_price   = p.entry_price;
            rec.exit_price    = exit_price;
            rec.quantity      = p.quantity;
            rec.gross_pnl     = gross;
            rec.net_pnl       = gross;
            rec.exit_reason   = hit_sl ? ExitReason::STOP_LOSS : ExitReason::TAKE_PROFIT;
            trade_log.push_back(rec);

            state.balance += gross;
            state.realized_pnl += gross;
            state.total_trades++;
            if (gross > 0.0) { state.winning_trades++; state.gross_profit += gross; }
            else { state.gross_loss += std::fabs(gross); }

            state.open_position_count--;
            p.active = 0;
            result.positions_closed++;
            continue;  // Don't accumulate closed position
        }

        // ── Still open: accumulate MTM ──────────────────────
        double current_price = (p.direction == Direction::LONG) ? bid : ask;
        double price_delta   = (p.direction == Direction::LONG)
            ? (current_price - p.entry_price)
            : (p.entry_price - current_price);
        p.unrealized_pnl = price_delta * p.quantity * CONTRACT_SIZE;
        total_unrealized += p.unrealized_pnl;
        total_margin     += p.used_margin;
    }

    // ── Update account state ────────────────────────────────
    state.equity      = state.balance + total_unrealized;
    state.used_margin = total_margin;
    state.free_margin = state.equity - total_margin;
    state.margin_level = (total_margin > 0.0)
        ? (state.equity / total_margin) * 100.0
        : 0.0;

    // Drawdown tracking
    if (state.equity > state.peak_equity)
        state.peak_equity = state.equity;
    double dd = (state.peak_equity > 0.0)
        ? (state.peak_equity - state.equity) / state.peak_equity
        : 0.0;
    if (dd > state.max_drawdown)
        state.max_drawdown = dd;

    // ── Stop-out check ──────────────────────────────────────
    if (state.open_position_count > 0 && total_margin > 0.0 &&
        state.margin_level > 0.0 && state.margin_level < stop_out_level) {
        // Liquidate all remaining
        for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
            Position& p = positions[i];
            if (!p.active) continue;

            double ep = (p.direction == Direction::LONG) ? bid : ask;
            double pd = (p.direction == Direction::LONG)
                ? (ep - p.entry_price) : (p.entry_price - ep);
            double gross = pd * p.quantity * CONTRACT_SIZE;

            TradeRecord rec;
            rec.direction     = p.direction;
            rec.entry_bar_idx = p.entry_bar_idx;
            rec.exit_bar_idx  = static_cast<uint32_t>(bar_index);
            rec.entry_time_ms = p.entry_time_ms;
            rec.exit_time_ms  = bar->time_ms;
            rec.entry_price   = p.entry_price;
            rec.exit_price    = ep;
            rec.quantity      = p.quantity;
            rec.gross_pnl     = gross;
            rec.net_pnl       = gross;
            rec.exit_reason   = ExitReason::LIQUIDATION;
            trade_log.push_back(rec);

            state.balance += gross;
            state.realized_pnl += gross;
            state.total_trades++;
            if (gross > 0.0) { state.winning_trades++; state.gross_profit += gross; }
            else { state.gross_loss += std::fabs(gross); }

            p.active = 0;
        }

        state.open_position_count = 0;
        state.used_margin = 0.0;
        state.liquidated = 1;
        result.stopped_out = true;
    }

    return result;
}

// ── Mark-to-market: legacy path (kept for cold-path use) ────
// Called once per bar per account.  Must be branch-minimal.
// No allocations.  No virtual calls.  Pure arithmetic.

void Account::mark_to_market(const MarketState& market) {
    if (state.liquidated) return;

    double bid = market.bid();
    double ask = market.ask();
    double total_unrealized = 0.0;
    double total_margin     = 0.0;

    for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
        Position& p = positions[i];
        if (!p.active) continue;

        double current_price = (p.direction == Direction::LONG) ? bid : ask;
        double price_delta   = (p.direction == Direction::LONG)
                                ? (current_price - p.entry_price)
                                : (p.entry_price - current_price);

        p.unrealized_pnl = price_delta * p.quantity * CONTRACT_SIZE;
        total_unrealized += p.unrealized_pnl;
        total_margin     += p.used_margin;
    }

    state.equity      = state.balance + total_unrealized;
    state.used_margin  = total_margin;
    state.free_margin  = state.equity - total_margin;

    // Margin level: equity / used_margin * 100 (0 if no positions)
    state.margin_level = (total_margin > 0.0)
                         ? (state.equity / total_margin) * 100.0
                         : 0.0;

    // Drawdown tracking
    if (state.equity > state.peak_equity)
        state.peak_equity = state.equity;

    double dd = (state.peak_equity > 0.0)
                ? (state.peak_equity - state.equity) / state.peak_equity
                : 0.0;
    if (dd > state.max_drawdown)
        state.max_drawdown = dd;
}

// ── SL/TP check ─────────────────────────────────────────────

int Account::check_sl_tp(const MarketState& market) {
    if (state.liquidated) return 0;

    int closed = 0;
    double bid = market.bid();
    double ask = market.ask();

    for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
        Position& p = positions[i];
        if (!p.active) continue;

        double check_price = (p.direction == Direction::LONG) ? bid : ask;
        bool hit_sl = false;
        bool hit_tp = false;

        if (p.direction == Direction::LONG) {
            // Use bar low for SL check (conservative: could have been hit intra-bar)
            if (p.stop_loss > 0.0 && market.current_bar->low <= p.stop_loss) {
                hit_sl = true;
                check_price = p.stop_loss;
            }
            if (p.take_profit > 0.0 && market.current_bar->high >= p.take_profit) {
                hit_tp = true;
                check_price = p.take_profit;
            }
        } else {
            // SHORT
            if (p.stop_loss > 0.0 && market.current_bar->high >= p.stop_loss) {
                hit_sl = true;
                check_price = p.stop_loss;
            }
            if (p.take_profit > 0.0 && market.current_bar->low <= p.take_profit) {
                hit_tp = true;
                check_price = p.take_profit;
            }
        }

        // If both hit on same bar, SL takes priority (conservative)
        if (hit_sl) {
            ExitReason reason = ExitReason::STOP_LOSS;
            double price_delta = (p.direction == Direction::LONG)
                                 ? (check_price - p.entry_price)
                                 : (p.entry_price - check_price);
            double gross = price_delta * p.quantity * CONTRACT_SIZE;

            TradeRecord rec;
            rec.direction     = p.direction;
            rec.entry_bar_idx = p.entry_bar_idx;
            rec.exit_bar_idx  = static_cast<uint32_t>(market.bar_index);
            rec.entry_time_ms = p.entry_time_ms;
            rec.exit_time_ms  = market.current_bar->time_ms;
            rec.entry_price   = p.entry_price;
            rec.exit_price    = check_price;
            rec.quantity      = p.quantity;
            rec.gross_pnl     = gross;
            rec.net_pnl       = gross; // commission applied at open
            rec.exit_reason   = reason;
            trade_log.push_back(rec);

            state.balance += gross;
            state.realized_pnl += gross;
            state.total_trades++;
            if (gross > 0) { state.winning_trades++; state.gross_profit += gross; }
            else { state.gross_loss += std::fabs(gross); }

            state.used_margin -= p.used_margin;
            state.open_position_count--;

            p.active = 0;
            ++closed;
        } else if (hit_tp) {
            ExitReason reason = ExitReason::TAKE_PROFIT;
            double price_delta = (p.direction == Direction::LONG)
                                 ? (check_price - p.entry_price)
                                 : (p.entry_price - check_price);
            double gross = price_delta * p.quantity * CONTRACT_SIZE;

            TradeRecord rec;
            rec.direction     = p.direction;
            rec.entry_bar_idx = p.entry_bar_idx;
            rec.exit_bar_idx  = static_cast<uint32_t>(market.bar_index);
            rec.entry_time_ms = p.entry_time_ms;
            rec.exit_time_ms  = market.current_bar->time_ms;
            rec.entry_price   = p.entry_price;
            rec.exit_price    = check_price;
            rec.quantity      = p.quantity;
            rec.gross_pnl     = gross;
            rec.net_pnl       = gross;
            rec.exit_reason   = reason;
            trade_log.push_back(rec);

            state.balance += gross;
            state.realized_pnl += gross;
            state.total_trades++;
            if (gross > 0) { state.winning_trades++; state.gross_profit += gross; }
            else { state.gross_loss += std::fabs(gross); }

            state.used_margin -= p.used_margin;
            state.open_position_count--;

            p.active = 0;
            ++closed;
        }
    }

    return closed;
}

// ── Stop-out enforcement ────────────────────────────────────

bool Account::enforce_stop_out(const MarketState& market, double stop_out_level) {
    if (state.liquidated) return false;
    if (state.open_position_count == 0) return false;
    if (state.used_margin <= 0.0) return false;

    if (state.margin_level > 0.0 && state.margin_level < stop_out_level) {
        // Liquidate all positions
        double bid = market.bid();
        double ask = market.ask();

        for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
            Position& p = positions[i];
            if (!p.active) continue;

            double exit_price = (p.direction == Direction::LONG) ? bid : ask;
            double price_delta = (p.direction == Direction::LONG)
                                 ? (exit_price - p.entry_price)
                                 : (p.entry_price - exit_price);
            double gross = price_delta * p.quantity * CONTRACT_SIZE;

            TradeRecord rec;
            rec.direction     = p.direction;
            rec.entry_bar_idx = p.entry_bar_idx;
            rec.exit_bar_idx  = static_cast<uint32_t>(market.bar_index);
            rec.entry_time_ms = p.entry_time_ms;
            rec.exit_time_ms  = market.current_bar->time_ms;
            rec.entry_price   = p.entry_price;
            rec.exit_price    = exit_price;
            rec.quantity      = p.quantity;
            rec.gross_pnl     = gross;
            rec.net_pnl       = gross;
            rec.exit_reason   = ExitReason::LIQUIDATION;
            trade_log.push_back(rec);

            state.balance += gross;
            state.realized_pnl += gross;
            state.total_trades++;
            if (gross > 0) { state.winning_trades++; state.gross_profit += gross; }
            else { state.gross_loss += std::fabs(gross); }

            p.active = 0;
        }

        state.open_position_count = 0;
        state.used_margin = 0.0;
        state.liquidated = 1;

        return true;
    }

    return false;
}

// ── Equity snapshot ─────────────────────────────────────────

void Account::snapshot_equity() {
    equity_curve.push_back(static_cast<float>(state.equity));
}

// ── Position sizing ─────────────────────────────────────────
// Risk-based sizing with leverage and margin constraints.

double compute_position_size(
    const AccountState& acct,
    double stop_distance,
    double entry_price,
    double risk_fraction
) {
    if (stop_distance <= 0.0 || entry_price <= 0.0) return 0.0;
    if (acct.free_margin <= 0.0) return 0.0;
    if (acct.liquidated) return 0.0;

    // Risk amount = equity * risk_fraction
    double risk_amount = acct.equity * risk_fraction;

    // Lots from risk: risk_amount / (stop_distance * contract_size)
    double lots_from_risk = risk_amount / (stop_distance * CONTRACT_SIZE);

    // Lots from leverage: free_margin * max_leverage / (entry_price * contract_size)
    double max_notional = acct.free_margin * acct.max_leverage;
    double lots_from_leverage = max_notional / (entry_price * CONTRACT_SIZE);

    // Take the more conservative
    double lots = std::min(lots_from_risk, lots_from_leverage);

    // Round down to lot step
    lots = std::floor(lots / LOT_STEP) * LOT_STEP;

    // Clamp
    lots = std::max(lots, 0.0);
    lots = std::min(lots, MAX_LOT);

    if (lots < MIN_LOT) return 0.0;

    return lots;
}

} // namespace aphelion
