// ============================================================
// Aphelion Research - Replay Engine V3
// ============================================================

#include "aphelion/replay_engine.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace aphelion {

namespace {

constexpr int64_t kSessionMs = 24LL * 60LL * 60LL * 1000LL;

bool update_session_kill_state(Account& account, const SimulationParams& params) {
    if (params.emergency_flatten != 0) {
        account.session.trading_disabled = 1;
        account.session.flatten_requested = 1;
        return true;
    }

    if (params.live_safe_mode == 0) {
        return false;
    }

    const bool hit_trade_limit =
        params.session_trade_limit > 0 &&
        account.session.entry_count >= params.session_trade_limit;
    const bool hit_session_drawdown =
        params.session_drawdown_kill > 0.0 &&
        account.session.session_peak_equity > 0.0 &&
        ((account.session.session_peak_equity - account.state.equity) / account.session.session_peak_equity) >=
            params.session_drawdown_kill;
    const bool hit_session_loss =
        params.session_loss_kill > 0.0 &&
        account.session.session_start_balance > 0.0 &&
        ((account.session.session_start_balance - account.state.balance) / account.session.session_start_balance) >=
            params.session_loss_kill;

    if (hit_trade_limit || hit_session_drawdown || hit_session_loss) {
        account.session.trading_disabled = 1;
        account.session.flatten_requested = 1;
        return true;
    }

    return false;
}

RiskConfig effective_risk_config(const ReplayEntry& entry, const RiskConfig& base) {
    RiskConfig config = base;
    config.enable_ech = base.enable_ech && (entry.params->enable_ech != 0);
    config.live_safe_mode = base.live_safe_mode && (entry.params->live_safe_mode != 0);
    return config;
}

} // namespace

ReplayStats run_replay(
    const Bar* tape,
    size_t tape_size,
    std::vector<ReplayEntry>& entries
) {
    return run_replay_v3(tape, tape_size, entries, RunMode::FULL, nullptr);
}

ReplayStats run_replay_v2(
    const Bar* tape,
    size_t tape_size,
    std::vector<ReplayEntry>& entries,
    RunMode mode
) {
    return run_replay_v3(tape, tape_size, entries, mode, nullptr);
}

ReplayStats run_replay_v3(
    const Bar* tape,
    size_t tape_size,
    std::vector<ReplayEntry>& entries,
    RunMode mode,
    const IntelligenceState* intelligence,
    const RiskConfig& risk_config
) {
    ReplayStats stats;

    if (tape_size == 0 || entries.empty()) return stats;

    const size_t num_entries = entries.size();
    const bool collect_equity = (mode != RunMode::BENCHMARK);
    const bool has_intelligence = (intelligence != nullptr);

    if (collect_equity) {
        for (auto& e : entries) {
            e.account->equity_curve.reserve(tape_size);
        }
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    MarketState market;
    market.tape_begin = tape;
    market.total_bars = tape_size;

    int64_t active_session_key = std::numeric_limits<int64_t>::min();

    for (size_t bar_idx = 0; bar_idx < tape_size; ++bar_idx) {
        const Bar& bar = tape[bar_idx];
        const double bid = bar.close;
        const double ask = bar.close + static_cast<double>(bar.spread) * 0.01;
        const int64_t session_key = bar.time_ms / kSessionMs;

        if (session_key != active_session_key) {
            active_session_key = session_key;
            for (auto& e : entries) {
                e.account->reset_session(session_key);
            }
        }

        market.bar_index   = bar_idx;
        market.current_bar = &bar;
        market.prev_bar    = (bar_idx > 0) ? &tape[bar_idx - 1] : nullptr;

        for (size_t eidx = 0; eidx < num_entries; ++eidx) {
            ReplayEntry& e = entries[eidx];
            Account& acct  = *e.account;

            if (acct.state.liquidated) {
                if (collect_equity) acct.snapshot_equity();
                stats.total_skipped_liq++;
                continue;
            }

            const IntelligenceState* bar_intelligence =
                has_intelligence ? &intelligence[bar_idx] : nullptr;

            PerBarResult pbr = acct.update_per_bar(
                bid,
                ask,
                &bar,
                bar_idx,
                e.params->stop_out_level,
                bar_intelligence
            );
            stats.total_sl_tp += pbr.positions_closed;

            if (pbr.stopped_out) {
                stats.total_stopouts++;
                if (collect_equity) acct.snapshot_equity();
                continue;
            }

            const bool was_session_disabled = acct.session.trading_disabled != 0;
            const bool session_killed = update_session_kill_state(acct, *e.params);
            if (session_killed && !was_session_disabled) {
                stats.total_session_kills++;
            }

            if (acct.session.flatten_requested && acct.state.open_position_count > 0) {
                StrategyDecision flat;
                flat.action = ActionType::CLOSE;
                flat.close_reason = ExitReason::KILL_SWITCH;
                FillReport flat_fill = execute_decision(acct, flat, market, *e.params);
                if (flat_fill.result == FillResult::FILLED) {
                    stats.total_emergency_flats++;
                }
                acct.session.flatten_requested = 0;
                if (collect_equity) acct.snapshot_equity();
                continue;
            }

            if (acct.session.trading_disabled) {
                acct.session.flatten_requested = 0;
                if (collect_equity) acct.snapshot_equity();
                continue;
            }

            const Signal sig = e.strategy->signal_at(bar_idx);
            if (sig != Signal::NONE) {
                stats.total_signals++;

                StrategyDecision decision;
                if (has_intelligence && e.strategy->is_intelligence_aware()) {
                    decision = e.strategy->decide_with_intelligence(
                        market, acct.state, bar_idx, intelligence[bar_idx]
                    );
                } else {
                    decision = build_decision_from_signal(
                        sig,
                        bar.close,
                        bar.high,
                        bar.low,
                        acct.state.open_position_count,
                        acct.state.risk_per_trade
                    );
                }

                if (decision.action != ActionType::HOLD &&
                    decision.action != ActionType::CLOSE &&
                    has_intelligence) {
                    const int recent_trade_count = static_cast<int>(acct.trade_log.size());
                    const TradeRecord* recent_trade_ptr =
                        recent_trade_count > 0 ? acct.trade_log.data() : nullptr;
                    const AccountRiskContext account_ctx = AccountRiskContext::from_account(
                        acct.state,
                        recent_trade_ptr,
                        recent_trade_count
                    );

                    const RiskModulation mod = compute_risk_modulation(
                        decision,
                        intelligence[bar_idx],
                        account_ctx,
                        effective_risk_config(e, risk_config)
                    );

                    if (mod.veto) {
                        stats.total_risk_vetoes++;
                        if (collect_equity) acct.snapshot_equity();
                        continue;
                    }

                    decision.risk_fraction *= static_cast<double>(mod.size_scale);
                }

                if (decision.action == ActionType::HOLD &&
                    acct.state.open_position_count == 0) {
                    stats.total_regime_skips++;
                }

                if (decision.action != ActionType::HOLD) {
                    const FillReport fill = execute_decision(acct, decision, market, *e.params);
                    if (fill.result == FillResult::FILLED) {
                        stats.total_fills++;
                    } else if (fill.result != FillResult::NO_ACTION) {
                        stats.total_rejects++;
                    }
                }
            }

            if (collect_equity) acct.snapshot_equity();
        }

        stats.bars_processed++;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    stats.elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();
    stats.acct_bars_per_sec = (stats.bars_processed * num_entries)
        / std::max(stats.elapsed_seconds, 1e-9);

    return stats;
}

} // namespace aphelion
