#pragma once
// ============================================================
// Aphelion Research — Multi-Timeframe Infrastructure
//
// Deterministic no-lookahead alignment of multiple bar tapes.
// A secondary timeframe bar is visible to the primary replay
// only when it has FULLY COMPLETED (its open time + duration
// is <= the primary bar's time). This guarantees zero lookahead.
//
// Usage:
//   1. Load primary tape (e.g., M5)
//   2. Load secondary tapes (e.g., H1, H4)
//   3. Build alignment indices
//   4. Pass TimeframeView array to strategy context
//   5. Strategy reads secondary bars via alignment index
// ============================================================

#include "aphelion/types.h"
#include "aphelion/data_ingest.h"
#include <vector>
#include <string>
#include <cstdint>

namespace aphelion {

// ── Alignment index ─────────────────────────────────────────
// For each primary bar index, stores the index of the latest
// COMPLETED bar in the secondary timeframe.
// UINT32_MAX means no secondary bar is available yet.
struct TimeframeAlignment {
    std::vector<uint32_t> index;   // primary_bar_idx → secondary_bar_idx
    std::string           timeframe;
    int32_t               timeframe_seconds = 0;
};

// ── Timeframe view (non-owning, hot-path-safe) ──────────────
struct TimeframeView {
    const Bar*     tape           = nullptr;
    size_t         tape_size      = 0;
    const uint32_t* alignment    = nullptr;  // indexed by primary bar
    int32_t        timeframe_seconds = 0;
    const char*    timeframe_name = nullptr;
};

// ── Multi-timeframe context for strategy access ─────────────
struct MultiTimeframeContext {
    const TimeframeView* views    = nullptr;
    size_t               num_views = 0;

    // Get the latest completed bar for a secondary timeframe
    // at a given primary bar index.  Returns nullptr if no bar
    // is available yet (before first completed secondary bar).
    const Bar* latest_bar(size_t view_idx, size_t primary_bar_idx) const {
        if (view_idx >= num_views) return nullptr;
        const auto& v = views[view_idx];
        uint32_t aligned_idx = v.alignment[primary_bar_idx];
        if (aligned_idx == UINT32_MAX) return nullptr;
        return &v.tape[aligned_idx];
    }

    // Number of secondary bars available (for lookback) at a
    // given primary bar index.  0 means none available yet.
    size_t available_bars(size_t view_idx, size_t primary_bar_idx) const {
        if (view_idx >= num_views) return 0;
        uint32_t aligned_idx = views[view_idx].alignment[primary_bar_idx];
        if (aligned_idx == UINT32_MAX) return 0;
        return static_cast<size_t>(aligned_idx) + 1;
    }

    // Find a view by timeframe name.  Returns index or SIZE_MAX if not found.
    size_t find_view(const char* tf_name) const;
};

// ── Build alignment index ───────────────────────────────────
// Precomputes the mapping from primary tape → secondary tape.
// Guarantees no-lookahead: secondary bar at index j is visible
// only when secondary.bars[j].time_ms + secondary.timeframe_seconds*1000
// <= primary.bars[i].time_ms.
TimeframeAlignment build_alignment(
    const BarTape& primary,
    const BarTape& secondary
);

} // namespace aphelion
