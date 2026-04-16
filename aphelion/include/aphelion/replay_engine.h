#pragma once
// ============================================================
// Aphelion Research — Replay Engine (Layer core)
// The sacred inner loop
// ============================================================

#include "aphelion/types.h"
#include "aphelion/market_state.h"
#include "aphelion/account.h"
#include "aphelion/strategy.h"
#include "aphelion/execution.h"
#include <vector>

namespace aphelion {

struct ReplayStats {
    size_t   bars_processed  = 0;
    size_t   total_fills     = 0;
    size_t   total_rejects   = 0;
    size_t   total_stopouts  = 0;
    size_t   total_sl_tp     = 0;
    double   elapsed_seconds = 0.0;
};

// Run the replay loop over a bar tape for a set of entries.
// This is the performance-critical path.
// entries: vector of (Account*, IStrategy*, SimulationParams*)
// Returns stats.
struct ReplayEntry {
    Account*          account;
    IStrategy*        strategy;
    SimulationParams* params;
};

ReplayStats run_replay(
    const Bar* tape,
    size_t tape_size,
    std::vector<ReplayEntry>& entries
);

} // namespace aphelion
