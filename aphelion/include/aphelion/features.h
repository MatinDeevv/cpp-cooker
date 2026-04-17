#pragma once
// ============================================================
// Aphelion Research — Feature / State Fabric (Layer B)
//
// Precomputes a dense feature tape from raw bars BEFORE replay.
// One BarFeatures per bar. Strategies read from this tape at
// zero cost during the hot loop — it's just a pointer read.
//
// Design:
//   - All computations are O(n) single-pass or O(n*k) windowed
//   - No heap allocations during computation (pre-sized vectors)
//   - Features are normalized / percentiled for strategy consumption
//   - Optional higher-timeframe features via MultiTimeframeContext
//
// This is NOT indicator spam. Each feature encodes a distinct
// dimension of market state that strategies can reason about.
// ============================================================

#include "aphelion/types.h"
#include "aphelion/data_ingest.h"
#include "aphelion/multi_timeframe.h"
#include <vector>

namespace aphelion {

// ── Feature computation parameters ──────────────────────────
struct FeatureConfig {
    int trend_short_period    = 10;    // bars for short trend slope
    int trend_medium_period   = 50;    // bars for medium trend slope
    int trend_long_period     = 200;   // bars for long trend slope
    int momentum_short_period = 5;     // bars for short momentum
    int momentum_medium_period= 20;    // bars for medium momentum
    int volatility_period     = 20;    // bars for realized volatility
    int volatility_long_period= 100;   // bars for long-term vol (percentile baseline)
    int structure_lookback    = 20;    // bars for high/low structure
    int compression_period    = 10;    // bars for compression detection
    int spread_lookback       = 50;    // bars for spread percentile
};

// ── Feature tape ────────────────────────────────────────────
struct FeatureTape {
    std::vector<BarFeatures> features;

    const BarFeatures& at(size_t idx) const { return features[idx]; }
    const BarFeatures* data() const { return features.data(); }
    size_t size() const { return features.size(); }
    bool empty() const { return features.empty(); }
};

// ── Compute feature tape from bar tape ──────────────────────
// Called once before replay starts. O(n * max_period).
// This is cold-path — correctness > speed, but still efficient.
FeatureTape compute_features(
    const BarTape& tape,
    const FeatureConfig& config = FeatureConfig{}
);

// ── Overlay higher-timeframe features ───────────────────────
// Fills htf_trend_alignment, htf_bias, htf_volatility_ratio
// using aligned secondary tape data. No lookahead.
void overlay_htf_features(
    FeatureTape& features,
    const BarTape& primary,
    const BarTape& secondary,
    const TimeframeAlignment& alignment,
    const FeatureConfig& config = FeatureConfig{}
);

} // namespace aphelion
