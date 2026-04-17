// ============================================================
// Aphelion Research - Account / Risk Core (Layer D)
// Hot-path mark-to-market, SL/TP, stop-out, position sizing
// ============================================================

#include "aphelion/account.h"
#include "aphelion/intelligence.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aphelion {

void Account::init(uint32_t id, const SimulationParams& params) {
    const double effective_max_leverage = (params.live_safe_mode != 0)
        ? std::min(params.max_leverage, params.live_max_leverage_cap)
        : params.max_leverage;
    const double effective_risk = (params.live_safe_mode != 0)
        ? params.risk_per_trade * params.live_reduced_risk_scale
        : params.risk_per_trade;

    state.reset(id, params.initial_balance, effective_max_leverage, effective_risk);
    std::memset(positions, 0, sizeof(positions));
    trade_log.clear();
    equity_curve.clear();
    reset_session(-1);
}

void Account::reset_session(int64_t session_key) {
    session.session_key = session_key;
    session.session_start_balance = state.balance;
    session.session_peak_equity = state.equity;
    session.entry_count = 0;
    session.trading_disabled = 0;
    session.flatten_requested = 0;
}

void Account::register_closed_trade(const TradeRecord& trade, double margin_released) {
    trade_log.push_back(trade);

    state.balance += trade.net_pnl;
    state.realized_pnl += trade.net_pnl;
    state.total_trades++;
    if (trade.net_pnl > 0.0) {
        state.winning_trades++;
        state.gross_profit += trade.net_pnl;
    } else {
        state.gross_loss += std::fabs(trade.net_pnl);
    }

    state.used_margin = std::max(0.0, state.used_margin - margin_released);
    state.open_position_count = std::max(0, state.open_position_count - 1);
}

double Account::active_notional() const {
    double total = 0.0;
    for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
        if (!positions[i].active) continue;
        total += positions[i].entry_price * positions[i].quantity * CONTRACT_SIZE;
    }
    return total;
}

PerBarResult Account::update_per_bar(
    double bid, double ask,
    const Bar* bar, size_t bar_index,
    double stop_out_level,
    const IntelligenceState* intelligence
) {
    PerBarResult result;

    if (state.liquidated) return result;

    double total_unrealized = 0.0;
    double total_margin     = 0.0;
    int    remaining        = state.open_position_count;

    for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
        if (remaining <= 0) break;

        Position& p = positions[i];
        if (!p.active) continue;
        --remaining;

        bool hit_sl = false;
        bool hit_tp = false;
        double exit_price = 0.0;
        ExitReason exit_reason = ExitReason::NONE;

        if (p.direction == Direction::LONG) {
            if (p.stop_loss > 0.0 && bar->low <= p.stop_loss) {
                hit_sl = true;
                exit_price = p.stop_loss;
                exit_reason = ExitReason::STOP_LOSS;
            } else if (p.take_profit > 0.0 && bar->high >= p.take_profit) {
                hit_tp = true;
                exit_price = p.take_profit;
                exit_reason = ExitReason::TAKE_PROFIT;
            }
        } else {
            if (p.stop_loss > 0.0 && bar->high >= p.stop_loss) {
                hit_sl = true;
                exit_price = p.stop_loss;
                exit_reason = ExitReason::STOP_LOSS;
            } else if (p.take_profit > 0.0 && bar->low <= p.take_profit) {
                hit_tp = true;
                exit_price = p.take_profit;
                exit_reason = ExitReason::TAKE_PROFIT;
            }
        }

        const double current_price = (p.direction == Direction::LONG) ? bid : ask;
        const double price_delta = (p.direction == Direction::LONG)
            ? (current_price - p.entry_price)
            : (p.entry_price - current_price);
        p.unrealized_pnl = price_delta * p.quantity * CONTRACT_SIZE;

        if (!hit_sl && !hit_tp && intelligence != nullptr && p.planned_hold_bars > 0) {
            const int held_bars = static_cast<int>(bar_index - p.entry_bar_idx);
            const float aligned_pressure = (p.direction == Direction::LONG)
                ? intelligence->long_pressure
                : intelligence->short_pressure;
            const float opposing_pressure = (p.direction == Direction::LONG)
                ? intelligence->short_pressure
                : intelligence->long_pressure;
            const float hold_multiplier = std::clamp(
                intelligence->hold_time_multiplier * (0.85f + 0.25f * p.entry_context_pressure),
                0.35f,
                1.60f
            );
            const int dynamic_hold_limit = std::max(
                static_cast<int>(p.minimum_hold_bars),
                static_cast<int>(std::lround(static_cast<double>(p.planned_hold_bars) * hold_multiplier))
            );
            const double risk_soft_buffer = std::max(1.0, static_cast<double>(p.initial_risk_cash) * 0.15);
            const float adaptive_urgency = std::max(
                intelligence->exit_urgency,
                std::min(1.0f, p.exit_urgency_bias + intelligence->failure_memory * 0.30f)
            );

            const bool timed_out = held_bars >= dynamic_hold_limit &&
                (p.unrealized_pnl > -risk_soft_buffer || adaptive_urgency > 0.82f);
            const bool pressure_flip = held_bars >= static_cast<int>(p.minimum_hold_bars) &&
                (aligned_pressure + 0.18f) < opposing_pressure &&
                adaptive_urgency > 0.70f &&
                intelligence->failure_memory > 0.45f;

            if (timed_out || pressure_flip) {
                hit_tp = true;
                exit_price = current_price;
                exit_reason = ExitReason::ADAPTIVE_EXIT;
            }
        }

        if (hit_sl || hit_tp) {
            const double closed_price_delta = (p.direction == Direction::LONG)
                ? (exit_price - p.entry_price)
                : (p.entry_price - exit_price);
            const double gross = closed_price_delta * p.quantity * CONTRACT_SIZE;

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
            rec.exit_reason   = exit_reason;
            register_closed_trade(rec, p.used_margin);
            p.active = 0;
            result.positions_closed++;
            continue;
        }

        total_unrealized += p.unrealized_pnl;
        total_margin     += p.used_margin;
    }

    state.equity      = state.balance + total_unrealized;
    state.used_margin = total_margin;
    state.free_margin = state.equity - total_margin;
    state.margin_level = (total_margin > 0.0)
        ? (state.equity / total_margin) * 100.0
        : 0.0;

    if (state.equity > state.peak_equity)
        state.peak_equity = state.equity;
    const double dd = (state.peak_equity > 0.0)
        ? (state.peak_equity - state.equity) / state.peak_equity
        : 0.0;
    if (dd > state.max_drawdown)
        state.max_drawdown = dd;
    if (state.equity > session.session_peak_equity)
        session.session_peak_equity = state.equity;

    if (state.open_position_count > 0 && total_margin > 0.0 &&
        state.margin_level > 0.0 && state.margin_level < stop_out_level) {
        for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
            Position& p = positions[i];
            if (!p.active) continue;

            const double ep = (p.direction == Direction::LONG) ? bid : ask;
            const double pd = (p.direction == Direction::LONG)
                ? (ep - p.entry_price)
                : (p.entry_price - ep);
            const double gross = pd * p.quantity * CONTRACT_SIZE;

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
            register_closed_trade(rec, p.used_margin);
            p.active = 0;
        }

        state.open_position_count = 0;
        state.used_margin = 0.0;
        state.liquidated = 1;
        session.trading_disabled = 1;
        result.stopped_out = true;
    }

    return result;
}

void Account::mark_to_market(const MarketState& market) {
    if (state.liquidated) return;

    const double bid = market.bid();
    const double ask = market.ask();
    double total_unrealized = 0.0;
    double total_margin     = 0.0;

    for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
        Position& p = positions[i];
        if (!p.active) continue;

        const double current_price = (p.direction == Direction::LONG) ? bid : ask;
        const double price_delta   = (p.direction == Direction::LONG)
            ? (current_price - p.entry_price)
            : (p.entry_price - current_price);

        p.unrealized_pnl = price_delta * p.quantity * CONTRACT_SIZE;
        total_unrealized += p.unrealized_pnl;
        total_margin     += p.used_margin;
    }

    state.equity      = state.balance + total_unrealized;
    state.used_margin = total_margin;
    state.free_margin = state.equity - total_margin;
    state.margin_level = (total_margin > 0.0)
        ? (state.equity / total_margin) * 100.0
        : 0.0;

    if (state.equity > state.peak_equity)
        state.peak_equity = state.equity;

    const double dd = (state.peak_equity > 0.0)
        ? (state.peak_equity - state.equity) / state.peak_equity
        : 0.0;
    if (dd > state.max_drawdown)
        state.max_drawdown = dd;
}

int Account::check_sl_tp(const MarketState& market) {
    if (state.liquidated) return 0;

    int closed = 0;
    const double bid = market.bid();
    const double ask = market.ask();

    for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
        Position& p = positions[i];
        if (!p.active) continue;

        double check_price = (p.direction == Direction::LONG) ? bid : ask;
        bool hit_sl = false;
        bool hit_tp = false;

        if (p.direction == Direction::LONG) {
            if (p.stop_loss > 0.0 && market.current_bar->low <= p.stop_loss) {
                hit_sl = true;
                check_price = p.stop_loss;
            }
            if (!hit_sl && p.take_profit > 0.0 && market.current_bar->high >= p.take_profit) {
                hit_tp = true;
                check_price = p.take_profit;
            }
        } else {
            if (p.stop_loss > 0.0 && market.current_bar->high >= p.stop_loss) {
                hit_sl = true;
                check_price = p.stop_loss;
            }
            if (!hit_sl && p.take_profit > 0.0 && market.current_bar->low <= p.take_profit) {
                hit_tp = true;
                check_price = p.take_profit;
            }
        }

        if (hit_sl || hit_tp) {
            const double price_delta = (p.direction == Direction::LONG)
                ? (check_price - p.entry_price)
                : (p.entry_price - check_price);
            const double gross = price_delta * p.quantity * CONTRACT_SIZE;

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
            rec.exit_reason   = hit_sl ? ExitReason::STOP_LOSS : ExitReason::TAKE_PROFIT;
            register_closed_trade(rec, p.used_margin);
            p.active = 0;
            ++closed;
        }
    }

    return closed;
}

bool Account::enforce_stop_out(const MarketState& market, double stop_out_level) {
    if (state.liquidated) return false;
    if (state.open_position_count == 0) return false;
    if (state.used_margin <= 0.0) return false;

    if (state.margin_level > 0.0 && state.margin_level < stop_out_level) {
        const double bid = market.bid();
        const double ask = market.ask();

        for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
            Position& p = positions[i];
            if (!p.active) continue;

            const double exit_price = (p.direction == Direction::LONG) ? bid : ask;
            const double price_delta = (p.direction == Direction::LONG)
                ? (exit_price - p.entry_price)
                : (p.entry_price - exit_price);
            const double gross = price_delta * p.quantity * CONTRACT_SIZE;

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
            register_closed_trade(rec, p.used_margin);
            p.active = 0;
        }

        state.open_position_count = 0;
        state.used_margin = 0.0;
        state.liquidated = 1;
        session.trading_disabled = 1;
        return true;
    }

    return false;
}

void Account::snapshot_equity() {
    equity_curve.push_back(static_cast<float>(state.equity));
}

double compute_position_size(
    const AccountState& acct,
    double stop_distance,
    double entry_price,
    double risk_fraction
) {
    if (stop_distance <= 0.0 || entry_price <= 0.0) return 0.0;
    if (acct.free_margin <= 0.0) return 0.0;
    if (acct.liquidated) return 0.0;

    const double risk_amount = acct.equity * risk_fraction;
    const double lots_from_risk = risk_amount / (stop_distance * CONTRACT_SIZE);
    const double max_notional = acct.free_margin * acct.max_leverage;
    const double lots_from_leverage = max_notional / (entry_price * CONTRACT_SIZE);

    double lots = std::min(lots_from_risk, lots_from_leverage);
    lots = std::floor(lots / LOT_STEP) * LOT_STEP;
    lots = std::max(lots, 0.0);
    lots = std::min(lots, MAX_LOT);

    if (lots < MIN_LOT) return 0.0;
    return lots;
}

} // namespace aphelion
