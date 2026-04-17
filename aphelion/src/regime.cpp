// ============================================================
// Aphelion Research — Regime Classification Engine
//
// Single-pass regime classification over precomputed features.
// Each bar is assigned a Regime enum value and a persistence
// counter (how many consecutive bars in the same regime).
//
// Classification logic (priority order):
//   1. VOLATILE_EXPANSION: vol ratio high + bar range extreme
//   2. COMPRESSION: compression score high + vol ratio low
//   3. TRENDING_UP: strong trend alignment + positive slopes
//   4. TRENDING_DOWN: strong trend alignment + negative slopes
//   5. TRANSITION: recent regime change (persistence < threshold)
//   6. RANGE_BOUND: default when no strong signal
// ============================================================

#include "aphelion/regime.h"
#include <iostream>
#include <cmath>

namespace aphelion {

void classify_regimes(
    FeatureTape& features,
    const RegimeConfig& config
) {
    const size_t n = features.size();
    if (n == 0) return;

    Regime prev_regime = Regime::UNKNOWN;
    int persistence = 0;

    // Regime counters for diagnostics
    size_t regime_counts[7] = {};

    for (size_t i = 0; i < n; ++i) {
        BarFeatures& f = features.features[i];
        Regime candidate = Regime::RANGE_BOUND; // default

        // Priority 1: Volatile expansion
        // High vol ratio AND extreme bar range → breakout / high vol environment
        if (f.volatility_ratio > config.vol_expansion_threshold &&
            f.bar_range_zscore > 1.5f) {
            candidate = Regime::VOLATILE_EXPANSION;
        }
        // Priority 2: Compression / squeeze
        else if (f.compression_score > config.compression_threshold &&
                 f.volatility_ratio < config.vol_compression_threshold) {
            candidate = Regime::COMPRESSION;
        }
        // Priority 3-4: Trending
        else if (std::fabs(f.trend_alignment) > config.trend_threshold) {
            float dominant_slope = f.trend_slope_medium;
            if (std::fabs(dominant_slope) > config.slope_threshold) {
                candidate = (dominant_slope > 0) ? Regime::TRENDING_UP : Regime::TRENDING_DOWN;
            }
        }

        // Track persistence
        if (candidate == prev_regime) {
            persistence++;
        } else {
            persistence = 1;
            prev_regime = candidate;
        }

        // If persistence is below threshold, mark as transitional
        Regime final_regime = candidate;
        if (persistence < config.min_persistence && i > 0) {
            final_regime = Regime::TRANSITION;
        }

        f.regime = static_cast<uint8_t>(final_regime);
        f.regime_persistence = static_cast<uint8_t>(std::min(persistence, 255));

        regime_counts[static_cast<int>(final_regime)]++;
    }

    // Diagnostics
    std::cout << "[regime] Classification complete over " << n << " bars:" << std::endl;
    const char* names[] = {
        "UNKNOWN", "TRENDING_UP", "TRENDING_DOWN", "RANGE_BOUND",
        "VOLATILE_EXPANSION", "COMPRESSION", "TRANSITION"
    };
    for (int r = 0; r < 7; ++r) {
        if (regime_counts[r] > 0) {
            double pct = 100.0 * regime_counts[r] / n;
            std::cout << "  " << names[r] << ": " << regime_counts[r]
                      << " (" << std::fixed << std::setprecision(1) << pct << "%)" << std::endl;
        }
    }
}

} // namespace aphelion
