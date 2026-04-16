// ============================================================
// Aphelion Research — Execution / Fill Engine (Layer F)
// The authority boundary: validates, fills, or rejects
// ============================================================

#include "aphelion/execution.h"
#include <cmath>
#include <cstring>

namespace aphelion {

void close_position(
    Account& account,
    int pos_idx,
    double exit_price,
    ExitReason reason,
    const MarketState& market,
    double commission
) {
    Position& p = account.positions[pos_idx];
    if (!p.active) return;

    double price_delta = (p.direction == Direction::LONG)
                         ? (exit_price - p.entry_price)
                         : (p.entry_price - exit_price);
    double gross = price_delta * p.quantity * CONTRACT_SIZE;
    double net   = gross - commission * p.quantity;

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
    rec.net_pnl       = net;
    rec.exit_reason   = reason;
    account.trade_log.push_back(rec);

    account.state.balance += net;
    account.state.realized_pnl += net;
    account.state.total_trades++;
    if (net > 0) { account.state.winning_trades++; account.state.gross_profit += net; }
    else { account.state.gross_loss += std::fabs(net); }

    account.state.used_margin -= p.used_margin;
    account.state.open_position_count--;

    p.active = 0;
}

FillReport execute_decision(
    Account& account,
    const StrategyDecision& decision,
    const MarketState& market,
    const SimulationParams& params
) {
    FillReport report;
    report.result = FillResult::NO_ACTION;

    if (account.state.liquidated) {
        report.result = FillResult::REJECTED_LIQUIDATED;
        return report;
    }

    // ── CLOSE ───────────────────────────────────────────────
    if (decision.action == ActionType::CLOSE) {
        for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
            if (!account.positions[i].active) continue;

            double exit_price = (account.positions[i].direction == Direction::LONG)
                                ? market.bid()
                                : market.ask();
            // Apply slippage (adverse)
            if (account.positions[i].direction == Direction::LONG)
                exit_price -= params.slippage_points * POINT_VALUE;
            else
                exit_price += params.slippage_points * POINT_VALUE;

            close_position(account, i, exit_price, ExitReason::STRATEGY, market, params.commission_per_lot);
        }
        report.result = FillResult::FILLED;
        return report;
    }

    // ── OPEN LONG / OPEN SHORT ──────────────────────────────
    if (decision.action == ActionType::OPEN_LONG || decision.action == ActionType::OPEN_SHORT) {
        // Check position limit
        if (account.state.open_position_count >= params.max_positions) {
            report.result = FillResult::REJECTED_MARGIN;
            return report;
        }

        // Find a free position slot
        int slot = -1;
        for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
            if (!account.positions[i].active) { slot = i; break; }
        }
        if (slot < 0) {
            report.result = FillResult::REJECTED_MARGIN;
            return report;
        }

        Direction dir = (decision.action == ActionType::OPEN_LONG)
                        ? Direction::LONG : Direction::SHORT;

        // Determine entry price with spread and slippage
        double entry_price;
        if (dir == Direction::LONG) {
            entry_price = market.ask() + params.slippage_points * POINT_VALUE;
        } else {
            entry_price = market.bid() - params.slippage_points * POINT_VALUE;
        }

        // Compute stop distance
        double stop_distance = 0.0;
        if (decision.stop_loss > 0.0) {
            stop_distance = std::fabs(entry_price - decision.stop_loss);
        }

        if (stop_distance <= 0.0) {
            report.result = FillResult::REJECTED_SIZE;
            return report;
        }

        // Position sizing
        double lots = compute_position_size(
            account.state, stop_distance, entry_price, decision.risk_fraction
        );

        if (lots <= 0.0) {
            report.result = FillResult::REJECTED_SIZE;
            return report;
        }

        // Compute required margin
        double notional = lots * entry_price * CONTRACT_SIZE;
        double required_margin = notional / account.state.max_leverage;

        // Final margin check
        if (required_margin > account.state.free_margin) {
            // Try to size down to fit
            double max_affordable_notional = account.state.free_margin * account.state.max_leverage;
            double affordable_lots = max_affordable_notional / (entry_price * CONTRACT_SIZE);
            affordable_lots = std::floor(affordable_lots / LOT_STEP) * LOT_STEP;

            if (affordable_lots < MIN_LOT) {
                report.result = FillResult::REJECTED_MARGIN;
                return report;
            }

            lots = affordable_lots;
            notional = lots * entry_price * CONTRACT_SIZE;
            required_margin = notional / account.state.max_leverage;
        }

        // Validate notional <= equity * max_leverage
        if (notional > account.state.equity * account.state.max_leverage) {
            report.result = FillResult::REJECTED_MARGIN;
            return report;
        }

        // ── FILL ────────────────────────────────────────────
        Position& pos = account.positions[slot];
        pos.entry_price   = entry_price;
        pos.stop_loss     = decision.stop_loss;
        pos.take_profit   = decision.take_profit;
        pos.quantity       = lots;
        pos.used_margin    = required_margin;
        pos.unrealized_pnl = 0.0;
        pos.entry_time_ms  = market.current_bar->time_ms;
        pos.entry_bar_idx  = static_cast<uint32_t>(market.bar_index);
        pos.direction      = dir;
        pos.active         = 1;
        std::memset(pos._pad, 0, sizeof(pos._pad));

        // Apply commission on entry
        double commission = params.commission_per_lot * lots;
        account.state.balance -= commission;

        account.state.used_margin += required_margin;
        account.state.free_margin -= required_margin;
        account.state.open_position_count++;

        report.result        = FillResult::FILLED;
        report.fill_price    = entry_price;
        report.fill_quantity = lots;
        report.margin_used   = required_margin;
        return report;
    }

    return report; // HOLD
}

} // namespace aphelion
