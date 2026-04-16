// ============================================================
// Aphelion Research — Replay Engine
// THE SACRED INNER LOOP
//
// Design notes:
//   - Single pass over bar tape, sequential access
//   - No heap allocations inside the loop
//   - No virtual dispatch except strategy->decide() which is
//     dominated by arithmetic cost
//   - Market state is stack-local and advanced by pointer bump
//   - Account mark-to-market is pure FP arithmetic on
//     cache-hot state (128 bytes per account)
//   - Equity snapshots are append-only to pre-reserved vectors
// ============================================================

#include "aphelion/replay_engine.h"
#include <chrono>
#include <iostream>

namespace aphelion {

ReplayStats run_replay(
    const Bar* tape,
    size_t tape_size,
    std::vector<ReplayEntry>& entries
) {
    ReplayStats stats;

    if (tape_size == 0 || entries.empty()) return stats;

    const size_t num_entries = entries.size();

    // Pre-reserve equity curves to avoid reallocation during replay
    for (auto& e : entries) {
        e.account->equity_curve.reserve(tape_size);
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    // ── THE LOOP ────────────────────────────────────────────
    // For each bar:
    //   1. Update market state (pointer bump)
    //   2. For each account:
    //      a. Mark to market
    //      b. Check SL/TP exits
    //      c. Enforce stop-out
    //      d. Get strategy decision
    //      e. Execute or reject
    //      f. Snapshot equity

    MarketState market;
    market.tape_begin = tape;
    market.total_bars = tape_size;

    for (size_t bar_idx = 0; bar_idx < tape_size; ++bar_idx) {
        // Step 1: Advance market
        market.bar_index   = bar_idx;
        market.current_bar = &tape[bar_idx];
        market.prev_bar    = (bar_idx > 0) ? &tape[bar_idx - 1] : nullptr;

        // Step 2: Process all accounts
        for (size_t eidx = 0; eidx < num_entries; ++eidx) {
            ReplayEntry& e = entries[eidx];
            Account& acct  = *e.account;

            if (acct.state.liquidated) {
                acct.snapshot_equity();
                continue;
            }

            // 2a. Mark to market
            acct.mark_to_market(market);

            // 2b. SL/TP exits
            int sl_tp_closed = acct.check_sl_tp(market);
            stats.total_sl_tp += sl_tp_closed;

            // Re-mark after SL/TP closes
            if (sl_tp_closed > 0) {
                acct.mark_to_market(market);
            }

            // 2c. Stop-out check
            bool stopped_out = acct.enforce_stop_out(market, e.params->stop_out_level);
            if (stopped_out) {
                stats.total_stopouts++;
                acct.snapshot_equity();
                continue;
            }

            // 2d. Strategy decision
            StrategyDecision decision = e.strategy->decide(market, acct.state, bar_idx);

            // 2e. Execute
            if (decision.action != ActionType::HOLD) {
                FillReport fill = execute_decision(acct, decision, market, *e.params);
                if (fill.result == FillResult::FILLED) {
                    stats.total_fills++;
                } else if (fill.result != FillResult::NO_ACTION) {
                    stats.total_rejects++;
                }
            }

            // 2f. Snapshot equity
            acct.snapshot_equity();
        }

        stats.bars_processed++;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    stats.elapsed_seconds = std::chrono::duration<double>(t1 - t0).count();

    return stats;
}

} // namespace aphelion
