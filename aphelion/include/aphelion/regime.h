#pragma once
// ============================================================
// Aphelion Research — Regime Classification Engine (Layer C)
//
// Classifies each bar into a market regime based on
// precomputed features. Runs as a single pass AFTER feature
// computation. Strategies use regime for signal gating,
// risk layer uses it for leverage/size modulation.
//
// The regime engine does NOT allocate during replay.
// It writes uint8_t regime + regime_persistence into BarFeatures.
// ============================================================

#include "aphelion/types.h"
#include "aphelion/features.h"

namespace aphelion {

// ── Regime classification thresholds ────────────────────────
struct RegimeConfig {
    // Trend thresholds (on trend_alignment)
    float trend_threshold     = 0.5f;   // alignment > this = trending
    float slope_threshold     = 0.0001f; // minimum slope magnitude for trend

    // Volatility thresholds (on volatility_ratio)
    float vol_expansion_threshold  = 1.5f;  // short vol / long vol > this = expansion
    float vol_compression_threshold = 0.6f; // short vol / long vol < this = compression

    // Compression score threshold
    float compression_threshold = 0.6f; // compression_score > this = compressed

    // Minimum persistence before regime is confirmed
    int min_persistence = 3;  // bars
};

// ── Classify regimes across the entire feature tape ─────────
// Modifies features in-place: sets regime and regime_persistence.
// Must be called AFTER compute_features().
void classify_regimes(
    FeatureTape& features,
    const RegimeConfig& config = RegimeConfig{}
);

} // namespace aphelion
