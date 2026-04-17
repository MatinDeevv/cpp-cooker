// ============================================================
// Aphelion Research — Feature / State Fabric
//
// Precomputes all market features in a single forward pass.
// Each feature is normalized or percentiled for easy consumption.
//
// Computational structure:
//   Pass 1: Rolling windows for trend, momentum, volatility
//   Pass 2: Structure detection (local extrema, compression)
//   Pass 3: Normalization and percentile ranking
//   Pass 4: (Optional) Higher-timeframe overlay
// ============================================================

#include "aphelion/features.h"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <iostream>
#include <cstring>

namespace aphelion {

// ── Helper: linear regression slope (normalized by price) ───
static float compute_slope(const Bar* tape, size_t end_idx, int period) {
    if (period < 2 || end_idx < static_cast<size_t>(period - 1)) return 0.0f;

    size_t start = end_idx - period + 1;
    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_xx = 0.0;
    double n = static_cast<double>(period);

    for (int i = 0; i < period; ++i) {
        double x = static_cast<double>(i);
        double y = tape[start + i].close;
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_xx += x * x;
    }

    double denom = n * sum_xx - sum_x * sum_x;
    if (std::fabs(denom) < 1e-15) return 0.0f;

    double slope = (n * sum_xy - sum_x * sum_y) / denom;

    // Normalize: slope per bar / average price
    double avg_price = sum_y / n;
    if (avg_price > 0.0)
        slope = slope / avg_price;

    return static_cast<float>(slope);
}

// ── Helper: rolling realized volatility (log returns stdev) ─
static float compute_volatility(const Bar* tape, size_t end_idx, int period) {
    if (period < 2 || end_idx < static_cast<size_t>(period)) return 0.0f;

    double sum = 0.0, sum_sq = 0.0;
    int count = 0;

    for (int i = 1; i <= period; ++i) {
        size_t idx = end_idx - period + i;
        double prev_close = tape[idx - 1].close;
        if (prev_close <= 0.0) continue;
        double log_ret = std::log(tape[idx].close / prev_close);
        sum += log_ret;
        sum_sq += log_ret * log_ret;
        count++;
    }

    if (count < 2) return 0.0f;
    double n = static_cast<double>(count);
    double var = (sum_sq - (sum * sum) / n) / (n - 1.0);
    return static_cast<float>(std::sqrt(std::max(0.0, var)));
}

// ── Helper: percentile rank within a rolling window ─────────
// Returns 0.0-1.0 where 1.0 means current value is at the max.
static float rolling_percentile(const float* values, size_t end_idx, int period) {
    if (period < 1 || end_idx < static_cast<size_t>(period - 1)) return 0.5f;

    float current = values[end_idx];
    int below = 0, total = 0;
    size_t start = end_idx - std::min(end_idx, static_cast<size_t>(period - 1));

    for (size_t i = start; i <= end_idx; ++i) {
        if (values[i] < current) below++;
        total++;
    }

    return (total > 0) ? static_cast<float>(below) / static_cast<float>(total) : 0.5f;
}


FeatureTape compute_features(
    const BarTape& tape,
    const FeatureConfig& config
) {
    FeatureTape result;
    const size_t n = tape.bars.size();
    if (n == 0) return result;

    result.features.resize(n);
    std::memset(result.features.data(), 0, n * sizeof(BarFeatures));

    const Bar* bars = tape.bars.data();

    // ── Temporary buffers for rolling computations ──────────
    std::vector<float> raw_volatility(n, 0.0f);
    std::vector<float> raw_spreads(n, 0.0f);

    // ── Pass 1: Per-bar feature computation ─────────────────
    for (size_t i = 0; i < n; ++i) {
        BarFeatures& f = result.features[i];

        // Trend slopes (normalized by price level)
        f.trend_slope_short  = compute_slope(bars, i, config.trend_short_period);
        f.trend_slope_medium = compute_slope(bars, i, config.trend_medium_period);
        f.trend_slope_long   = compute_slope(bars, i, config.trend_long_period);

        // Trend alignment: do all three slopes agree in sign?
        // +1.0 = all bullish, -1.0 = all bearish, near 0 = mixed
        {
            float s1 = (f.trend_slope_short > 0) ? 1.0f : (f.trend_slope_short < 0 ? -1.0f : 0.0f);
            float s2 = (f.trend_slope_medium > 0) ? 1.0f : (f.trend_slope_medium < 0 ? -1.0f : 0.0f);
            float s3 = (f.trend_slope_long > 0) ? 1.0f : (f.trend_slope_long < 0 ? -1.0f : 0.0f);
            f.trend_alignment = (s1 + s2 + s3) / 3.0f;
        }

        // Momentum: simple returns over window
        if (i >= static_cast<size_t>(config.momentum_short_period)) {
            double prev = bars[i - config.momentum_short_period].close;
            f.momentum_short = (prev > 0) ? static_cast<float>((bars[i].close - prev) / prev) : 0.0f;
        }
        if (i >= static_cast<size_t>(config.momentum_medium_period)) {
            double prev = bars[i - config.momentum_medium_period].close;
            f.momentum_medium = (prev > 0) ? static_cast<float>((bars[i].close - prev) / prev) : 0.0f;
        }

        // Momentum acceleration (change in short momentum)
        if (i >= 1) {
            f.momentum_acceleration = f.momentum_short - result.features[i - 1].momentum_short;
        }

        // Volatility (log-return stdev)
        f.volatility_raw = compute_volatility(bars, i, config.volatility_period);
        raw_volatility[i] = f.volatility_raw;

        // Volatility ratio: short / long (expansion vs contraction)
        {
            float long_vol = compute_volatility(bars, i, config.volatility_long_period);
            f.volatility_ratio = (long_vol > 1e-10f) ? f.volatility_raw / long_vol : 1.0f;
        }

        // Structure: distance to recent high/low
        {
            int lookback = config.structure_lookback;
            size_t start = (i >= static_cast<size_t>(lookback)) ? (i - lookback) : 0;
            double recent_high = bars[start].high;
            double recent_low  = bars[start].low;
            for (size_t j = start + 1; j <= i; ++j) {
                if (bars[j].high > recent_high) recent_high = bars[j].high;
                if (bars[j].low < recent_low) recent_low = bars[j].low;
            }
            double range = recent_high - recent_low;
            if (range > 1e-10) {
                f.distance_to_high = static_cast<float>((bars[i].close - recent_high) / range);
                f.distance_to_low  = static_cast<float>((bars[i].close - recent_low) / range);
            } else {
                f.distance_to_high = 0.0f;
                f.distance_to_low  = 0.0f;
            }
        }

        // Bar range z-score
        {
            double current_range = bars[i].high - bars[i].low;
            int lookback = config.structure_lookback;
            size_t start = (i >= static_cast<size_t>(lookback)) ? (i - lookback) : 0;
            double sum = 0.0, sum_sq = 0.0;
            int count = 0;
            for (size_t j = start; j <= i; ++j) {
                double r = bars[j].high - bars[j].low;
                sum += r;
                sum_sq += r * r;
                count++;
            }
            double mean = sum / count;
            double var = (count > 1) ? (sum_sq - sum * sum / count) / (count - 1) : 0.0;
            double std = std::sqrt(std::max(0.0, var));
            f.bar_range_zscore = (std > 1e-10) ? static_cast<float>((current_range - mean) / std) : 0.0f;
        }

        // Compression score: ratio of recent range to lookback range
        {
            int short_lb = config.compression_period;
            int long_lb  = config.structure_lookback;
            size_t short_start = (i >= static_cast<size_t>(short_lb)) ? (i - short_lb) : 0;
            size_t long_start  = (i >= static_cast<size_t>(long_lb)) ? (i - long_lb) : 0;

            double short_high = bars[short_start].high, short_low = bars[short_start].low;
            for (size_t j = short_start + 1; j <= i; ++j) {
                if (bars[j].high > short_high) short_high = bars[j].high;
                if (bars[j].low < short_low)   short_low = bars[j].low;
            }
            double long_high = bars[long_start].high, long_low = bars[long_start].low;
            for (size_t j = long_start + 1; j <= i; ++j) {
                if (bars[j].high > long_high) long_high = bars[j].high;
                if (bars[j].low < long_low)   long_low = bars[j].low;
            }
            double short_range = short_high - short_low;
            double long_range  = long_high - long_low;
            // Compression = 1.0 when short range is tiny vs long range
            f.compression_score = (long_range > 1e-10)
                ? 1.0f - static_cast<float>(std::min(short_range / long_range, 1.0))
                : 0.0f;
        }

        // Spread percentile (friction)
        raw_spreads[i] = static_cast<float>(bars[i].spread);

        // Copy gap flag
        f.gap_flag = bars[i].flags;

        // HTF fields default to zero (filled by overlay_htf_features)
        f.htf_trend_alignment = 0.0f;
        f.htf_bias = 0.0f;
        f.htf_volatility_ratio = 0.0f;
    }

    // ── Pass 2: Percentile rankings (requires full arrays) ──
    for (size_t i = 0; i < n; ++i) {
        BarFeatures& f = result.features[i];
        f.volatility_percentile = rolling_percentile(
            raw_volatility.data(), i, config.volatility_long_period
        );
        f.spread_percentile = rolling_percentile(
            raw_spreads.data(), i, config.spread_lookback
        );
    }

    std::cout << "[features] Computed " << n << " bar features ("
              << (n * sizeof(BarFeatures)) / 1024 << " KB feature tape)"
              << std::endl;

    return result;
}


void overlay_htf_features(
    FeatureTape& features,
    const BarTape& primary,
    const BarTape& secondary,
    const TimeframeAlignment& alignment,
    const FeatureConfig& config
) {
    const size_t n = features.size();
    if (n == 0 || secondary.bars.empty()) return;

    // Precompute HTF slopes for the secondary tape
    const Bar* htf_bars = secondary.bars.data();
    const size_t htf_n  = secondary.bars.size();

    std::vector<float> htf_slopes(htf_n, 0.0f);
    std::vector<float> htf_vol(htf_n, 0.0f);

    for (size_t i = 0; i < htf_n; ++i) {
        htf_slopes[i] = compute_slope(htf_bars, i, std::min(config.trend_medium_period, static_cast<int>(i + 1)));
        htf_vol[i] = compute_volatility(htf_bars, i, std::min(config.volatility_period, static_cast<int>(i + 1)));
    }

    // Overlay onto primary feature tape
    for (size_t i = 0; i < n; ++i) {
        uint32_t htf_idx = alignment.index[i];
        if (htf_idx == UINT32_MAX) continue;

        BarFeatures& f = features.features[i];

        // HTF bias: sign and magnitude of HTF trend slope
        float htf_slope = htf_slopes[htf_idx];
        f.htf_bias = std::max(-1.0f, std::min(1.0f, htf_slope * 1000.0f));

        // HTF trend alignment: do primary and HTF agree?
        float primary_sign = (f.trend_slope_medium > 0) ? 1.0f : (f.trend_slope_medium < 0 ? -1.0f : 0.0f);
        float htf_sign = (htf_slope > 0) ? 1.0f : (htf_slope < 0 ? -1.0f : 0.0f);
        f.htf_trend_alignment = primary_sign * htf_sign; // +1 = agree, -1 = disagree

        // HTF volatility ratio: primary vol / HTF vol
        float hv = htf_vol[htf_idx];
        f.htf_volatility_ratio = (hv > 1e-10f) ? f.volatility_raw / hv : 1.0f;
    }

    std::cout << "[features] HTF overlay applied from " << secondary.timeframe
              << " (" << htf_n << " bars)" << std::endl;
}

} // namespace aphelion
