// ============================================================
// Aphelion Research - Execution / Fill Engine (Layer F)
// The authority boundary: validates, fills, or rejects
// ============================================================

#include "aphelion/execution.h"

#include <algorithm>
#include <cmath>

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

    const double price_delta = (p.direction == Direction::LONG)
        ? (exit_price - p.entry_price)
        : (p.entry_price - exit_price);
    const double gross = price_delta * p.quantity * CONTRACT_SIZE;
    const double net   = gross - commission * p.quantity;

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
    account.register_closed_trade(rec, p.used_margin);
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

    if (decision.action == ActionType::CLOSE) {
        for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
            if (!account.positions[i].active) continue;

            double exit_price = (account.positions[i].direction == Direction::LONG)
                ? market.bid()
                : market.ask();
            if (account.positions[i].direction == Direction::LONG)
                exit_price -= params.slippage_points * POINT_VALUE;
            else
                exit_price += params.slippage_points * POINT_VALUE;

            close_position(
                account,
                i,
                exit_price,
                decision.close_reason,
                market,
                params.commission_per_lot
            );
        }
        report.result = FillResult::FILLED;
        return report;
    }

    if (decision.action == ActionType::OPEN_LONG || decision.action == ActionType::OPEN_SHORT) {
        if (account.session.trading_disabled || account.session.flatten_requested) {
            report.result = FillResult::REJECTED_MARGIN;
            return report;
        }

        if (account.state.open_position_count >= params.max_positions) {
            report.result = FillResult::REJECTED_MARGIN;
            return report;
        }

        int slot = -1;
        for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
            if (!account.positions[i].active) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            report.result = FillResult::REJECTED_MARGIN;
            return report;
        }

        const Direction dir = (decision.action == ActionType::OPEN_LONG)
            ? Direction::LONG
            : Direction::SHORT;

        const double entry_price = (dir == Direction::LONG)
            ? market.ask() + params.slippage_points * POINT_VALUE
            : market.bid() - params.slippage_points * POINT_VALUE;

        double stop_distance = 0.0;
        if (decision.stop_loss > 0.0) {
            stop_distance = std::fabs(entry_price - decision.stop_loss);
        }

        if (stop_distance <= 0.0) {
            report.result = FillResult::REJECTED_SIZE;
            return report;
        }

        double lots = compute_position_size(
            account.state, stop_distance, entry_price, decision.risk_fraction
        );
        if (lots <= 0.0) {
            report.result = FillResult::REJECTED_SIZE;
            return report;
        }

        double notional = lots * entry_price * CONTRACT_SIZE;
        if (params.live_safe_mode != 0 && params.max_position_notional > 0.0) {
            const double capped_lots = std::floor(
                (params.max_position_notional / (entry_price * CONTRACT_SIZE)) / LOT_STEP
            ) * LOT_STEP;
            if (capped_lots < MIN_LOT) {
                report.result = FillResult::REJECTED_MARGIN;
                return report;
            }
            lots = std::min(lots, capped_lots);
            notional = lots * entry_price * CONTRACT_SIZE;
        }

        if (params.live_safe_mode != 0 && params.max_total_notional > 0.0) {
            const double available_notional = params.max_total_notional - account.active_notional();
            if (available_notional <= 0.0) {
                report.result = FillResult::REJECTED_MARGIN;
                return report;
            }
            const double capped_lots = std::floor(
                (available_notional / (entry_price * CONTRACT_SIZE)) / LOT_STEP
            ) * LOT_STEP;
            if (capped_lots < MIN_LOT) {
                report.result = FillResult::REJECTED_MARGIN;
                return report;
            }
            lots = std::min(lots, capped_lots);
            notional = lots * entry_price * CONTRACT_SIZE;
        }

        if (lots < MIN_LOT) {
            report.result = FillResult::REJECTED_SIZE;
            return report;
        }

        double required_margin = notional / account.state.max_leverage;
        if (required_margin > account.state.free_margin) {
            const double max_affordable_notional = account.state.free_margin * account.state.max_leverage;
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

        if (notional > account.state.equity * account.state.max_leverage) {
            report.result = FillResult::REJECTED_MARGIN;
            return report;
        }

        Position& pos = account.positions[slot];
        pos.entry_price = entry_price;
        pos.stop_loss = decision.stop_loss;
        pos.take_profit = decision.take_profit;
        pos.quantity = lots;
        pos.used_margin = required_margin;
        pos.unrealized_pnl = 0.0;
        pos.entry_time_ms = market.current_bar->time_ms;
        pos.entry_bar_idx = static_cast<uint32_t>(market.bar_index);
        pos.direction = dir;
        pos.active = 1;
        pos.planned_hold_bars = std::max<uint16_t>(decision.expected_hold_bars, 1u);
        pos.minimum_hold_bars = std::min(pos.planned_hold_bars, std::max<uint16_t>(decision.minimum_hold_bars, 1u));
        pos.entry_context_pressure = std::clamp(decision.context_pressure, 0.0f, 1.0f);
        pos.initial_risk_cash = static_cast<float>(stop_distance * lots * CONTRACT_SIZE);
        pos.exit_urgency_bias = std::clamp(decision.exit_urgency, 0.0f, 1.0f);

        const double commission = params.commission_per_lot * lots;
        account.state.balance -= commission;
        account.state.used_margin += required_margin;
        account.state.free_margin -= required_margin;
        account.state.open_position_count++;
        account.session.entry_count++;

        report.result        = FillResult::FILLED;
        report.fill_price    = entry_price;
        report.fill_quantity = lots;
        report.margin_used   = required_margin;
        return report;
    }

    return report;
}

} // namespace aphelion
