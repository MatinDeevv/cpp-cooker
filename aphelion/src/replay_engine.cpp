// ============================================================
// Aphelion Research — Replay Engine V3
// THE SACRED INNER LOOP — INTELLIGENCE EVOLVED
//
// V2 optimizations preserved:
//   1. Fused update_per_bar(): single pass over positions
//   2. Signal-driven strategy dispatch: 1-byte read, skip NONE
//   3. Benchmark mode: skip equity_curve push_back
//   4. Hoisted bid/ask: once per bar
//   5. Early liquidation skip
//
// V3 intelligence additions:
//   6. Feature tape: precomputed BarFeatures[bar_idx] — one
//      pointer read per signal bar. Zero cost on NONE bars.
//   7. Context-aware strategy dispatch: strategies that implement
//      decide_with_context() receive features for richer decisions.
//   8. Dynamic risk modulation: confidence × drawdown × regime ×
//      volatility → size_scale applied to risk_fraction before
//      execution. Pure arithmetic, no allocations.
//   9. Risk veto: the risk manager can veto trades in bad
//      conditions (regime mismatch, extreme drawdown, etc.)
//
// The hot path cost of V3 over V2:
//   - Per bar (NONE signal): ZERO additional cost
//   - Per signal bar: ~20 float multiplies for risk modulation
//     + one pointer read for feature access
//   This keeps throughput well above 100M acct×bars/sec.
// ============================================================

#include "aphelion/replay_engine.h"
#include <chrono>
#include <iostream>

namespace aphelion {

// V1 backward compat
ReplayStats run_replay(
    const Bar* tape,
    size_t tape_size,
    std::vector<ReplayEntry>& entries
) {
    return run_replay_v3(tape, tape_size, entries, RunMode::FULL, nullptr);
}

// V2 backward compat
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
    const BarFeatures* features,
    const RiskConfig& risk_config
) {
    ReplayStats stats;

    if (tape_size == 0 || entries.empty()) return stats;

    const size_t num_entries = entries.size();
    const bool collect_equity = (mode != RunMode::BENCHMARK);
    const bool has_features = (features != nullptr);

    // Pre-reserve equity curves to avoid reallocation during replay
    if (collect_equity) {
        for (auto& e : entries) {
            e.account->equity_curve.reserve(tape_size);
        }
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    // ── THE LOOP (V3) ──────────────────────────────────────
    //
    // For each bar:
    //   1. Hoist bid/ask
    //   2. Build market state
    //   3. For each account:
    //      a. Skip if liquidated
    //      b. Fused update: MTM + SL/TP + stop-out
    //      c. Read precomputed signal (1 byte)
    //      d. If signal != NONE:
    //         V3: context-aware decision + risk modulation
    //      e. Optional equity snapshot

    MarketState market;
    market.tape_begin = tape;
    market.total_bars = tape_size;

    for (size_t bar_idx = 0; bar_idx < tape_size; ++bar_idx) {
        const Bar& bar = tape[bar_idx];

        const double bid = bar.close;
        const double ask = bar.close + static_cast<double>(bar.spread) * 0.01;

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

            PerBarResult pbr = acct.update_per_bar(
                bid, ask, &bar, bar_idx, e.params->stop_out_level
            );
            stats.total_sl_tp += pbr.positions_closed;

            if (pbr.stopped_out) {
                stats.total_stopouts++;
                if (collect_equity) acct.snapshot_equity();
                continue;
            }

            // Signal-driven dispatch
            Signal sig = e.strategy->signal_at(bar_idx);

            if (sig != Signal::NONE) {
                stats.total_signals++;

                // ── V3: Context-aware decision path ─────────
                StrategyDecision decision;

                if (has_features && e.strategy->is_context_aware()) {
                    // Rich path: features + context
                    decision = e.strategy->decide_with_context(
                        market, acct.state, bar_idx, features[bar_idx]
                    );
                } else {
                    // V2 path: basic signal-to-decision
                    decision = build_decision_from_signal(
                        sig, bar.close, bar.high, bar.low,
                        acct.state.open_position_count,
                        acct.state.risk_per_trade
                    );
                }

                // ── V3: Risk modulation ─────────────────────
                if (decision.action != ActionType::HOLD &&
                    decision.action != ActionType::CLOSE &&
                    has_features) {
                    // Build lightweight account risk context
                    AccountRiskContext arc;
                    arc.current_drawdown = static_cast<float>(acct.state.max_drawdown);
                    // Approximate loss streak from recent state
                    // (full trade log scan is cold-path only)
                    arc.recent_loss_streak = 0;
                    arc.recent_trades = acct.state.total_trades;
                    arc.recent_win_rate = (acct.state.total_trades > 0)
                        ? static_cast<float>(acct.state.winning_trades) / acct.state.total_trades
                        : 0.5f;

                    // Check recent trades for loss streak (last few only)
                    if (!acct.trade_log.empty()) {
                        int streak = 0;
                        for (int t = static_cast<int>(acct.trade_log.size()) - 1;
                             t >= 0 && t >= static_cast<int>(acct.trade_log.size()) - 5; --t) {
                            if (acct.trade_log[t].net_pnl <= 0) streak++;
                            else break;
                        }
                        arc.recent_loss_streak = streak;
                    }

                    RiskModulation mod = compute_risk_modulation(
                        decision, features[bar_idx], arc, risk_config
                    );

                    if (mod.veto) {
                        stats.total_risk_vetoes++;
                        if (collect_equity) acct.snapshot_equity();
                        continue;
                    }

                    // Apply size modulation
                    decision.risk_fraction *= static_cast<double>(mod.size_scale);
                }

                // Execute
                if (decision.action != ActionType::HOLD) {
                    FillReport fill = execute_decision(acct, decision, market, *e.params);
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
