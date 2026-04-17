#pragma once
// ============================================================
// Aphelion Research — Replay Engine (Layer core)
// The sacred inner loop — V3
//
// V2 optimizations (preserved):
//   - Fused per-bar update (single position pass)
//   - Signal-driven strategy dispatch (skip virtual on NONE)
//   - Benchmark mode (skip equity snapshots)
//   - Precomputed bid/ask per bar (hoisted out of account loop)
//
// V3 intelligence additions:
//   - Feature tape access (zero-cost: pointer read per bar)
//   - Context-aware strategy dispatch
//   - Dynamic risk modulation per signal
//   - Regime-aware trade gating
// ============================================================

#include "aphelion/types.h"
#include "aphelion/market_state.h"
#include "aphelion/account.h"
#include "aphelion/strategy.h"
#include "aphelion/execution.h"
#include "aphelion/features.h"
#include "aphelion/risk_manager.h"
#include <vector>

namespace aphelion {

struct ReplayStats {
    size_t   bars_processed      = 0;
    size_t   total_fills         = 0;
    size_t   total_rejects       = 0;
    size_t   total_stopouts      = 0;
    size_t   total_sl_tp         = 0;
    size_t   total_signals       = 0;
    size_t   total_skipped_liq   = 0;
    size_t   total_risk_vetoes   = 0;  // V3: trades vetoed by risk manager
    size_t   total_regime_skips  = 0;  // V3: signals skipped due to regime
    double   elapsed_seconds     = 0.0;
    double   acct_bars_per_sec   = 0.0;
};

struct ReplayEntry {
    Account*          account;
    IStrategy*        strategy;
    SimulationParams* params;
};

// V1 API preserved: full replay with all outputs
ReplayStats run_replay(
    const Bar* tape,
    size_t tape_size,
    std::vector<ReplayEntry>& entries
);

// V2 API: replay with explicit run mode control
ReplayStats run_replay_v2(
    const Bar* tape,
    size_t tape_size,
    std::vector<ReplayEntry>& entries,
    RunMode mode
);

// V3 API: replay with features, regime, and risk modulation
ReplayStats run_replay_v3(
    const Bar* tape,
    size_t tape_size,
    std::vector<ReplayEntry>& entries,
    RunMode mode,
    const BarFeatures* features,     // precomputed, may be nullptr for V2 fallback
    const RiskConfig& risk_config = RiskConfig{}
);

} // namespace aphelion
