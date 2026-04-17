#pragma once
// ============================================================
// Aphelion Research — Indicator Library
//
// ~100 composable primitives across 7 categories:
//   A. Trend / Momentum     (20)
//   B. Volatility / Range   (15)
//   C. Structure / Location  (15)
//   D. Regime / State        (10)
//   E. Friction / Execution  (5)
//   F. Cross-Timeframe       (10)
//   G. Pattern Primitives    (25)
//
// Each indicator:
//   - Takes bars + params → float tape (one value per bar)
//   - Has metadata (name, category, bounds, defaults)
//   - Can be composed into strategy conditions
//   - Is precomputed once before replay (cold path)
// ============================================================

#include "aphelion/types.h"
#include <vector>
#include <cstdint>

namespace aphelion {

struct IntelligenceState;  // forward declaration

// ── 100 indicator primitives ────────────────────────────────
enum class IndicatorId : uint16_t {
    // ── A. TREND / MOMENTUM (20) ────────────────────────────
    SMA = 0,
    EMA,
    WMA,
    DEMA,
    TEMA,
    KAMA,
    LINEAR_SLOPE,
    MOMENTUM,
    ROC,
    ACCELERATION,
    DIRECTIONAL_PERSISTENCE,
    TREND_STRENGTH,
    MACD_LINE,
    MACD_SIGNAL,
    MACD_HISTOGRAM,
    RSI,
    STOCHASTIC_K,
    STOCHASTIC_D,
    CCI,
    WILLIAMS_R,

    // ── B. VOLATILITY / RANGE (15) ──────────────────────────
    ATR,
    ROLLING_STDDEV,
    RANGE_PERCENTILE,
    COMPRESSION_RATIO,
    EXPANSION_RATIO,
    REALIZED_VOL,
    VOL_CHANGE,
    BOLLINGER_WIDTH,
    KELTNER_WIDTH,
    TRUE_RANGE_NORM,
    RANGE_BODY_RATIO,
    HIGH_LOW_RANGE,
    GARMAN_KLASS_VOL,
    PARKINSON_VOL,
    VOL_OF_VOL,

    // ── C. STRUCTURE / LOCATION (15) ────────────────────────
    DIST_ROLLING_HIGH,
    DIST_ROLLING_LOW,
    BREAKOUT_PROXIMITY,
    MEAN_DISTANCE,
    OVERLAP_RATIO,
    LOCAL_EXTREMA_COUNT,
    HIGHER_HIGH_COUNT,
    LOWER_LOW_COUNT,
    PIVOT_DISTANCE,
    BAR_POSITION,
    RANGE_POSITION,
    CHANNEL_POSITION,
    MIDLINE_DISTANCE,
    SUPPORT_STRENGTH,
    RESISTANCE_STRENGTH,

    // ── D. REGIME / STATE (10) ──────────────────────────────
    REGIME_IS_TREND,
    REGIME_IS_RANGE,
    REGIME_IS_VOLATILE,
    REGIME_IS_COMPRESSED,
    REGIME_IS_TRANSITION,
    STABILITY_SCORE,
    ENTROPY_SCORE,
    REGIME_AGE,
    DIRECTIONAL_ENERGY_IND,
    REGIME_STRENGTH_IND,

    // ── E. FRICTION / EXECUTION (5) ─────────────────────────
    SPREAD_RANK,
    GAP_PRESENT,
    SESSION_BOUNDARY,
    ABNORMAL_RANGE_FLAG,
    EXECUTION_QUALITY,

    // ── F. CROSS-TIMEFRAME (10) ─────────────────────────────
    HTF_TREND_AGREEMENT,
    HTF_MOMENTUM_AGREEMENT,
    HTF_REGIME_GATE,
    HTF_STRUCTURE_DIVERGE,
    HTF_VOL_RATIO_IND,
    HTF_BIAS_SCORE,
    TF_TREND_RATIO,
    TF_MOMENTUM_RATIO,
    TF_RANGE_RATIO,
    TF_COHERENCE,

    // ── G. PATTERN PRIMITIVES (25) ──────────────────────────
    BREAKOUT_ATTEMPT,
    BREAKOUT_FAILURE,
    PULLBACK_CONTINUATION,
    EXHAUSTION_SIGNAL,
    COMPRESSION_TO_EXPANSION,
    REVERSAL_IMPULSE,
    INSIDE_BAR,
    OUTSIDE_BAR,
    ENGULFING_BULLISH,
    ENGULFING_BEARISH,
    PIN_BAR_BULLISH,
    PIN_BAR_BEARISH,
    DOJI_PATTERN,
    THREE_BAR_REVERSAL,
    MOMENTUM_DIVERGENCE,
    VOLUME_CLIMAX,
    RANGE_CONTRACTION_PATTERN,
    RANGE_EXPANSION_PATTERN,
    MEAN_REVERSION_SETUP,
    TREND_CONTINUATION_SETUP,
    FAILED_BREAKOUT_REVERSAL,
    SQUEEZE_FIRE,
    HIGHER_LOW_SETUP,
    LOWER_HIGH_SETUP,
    PERSISTENCE_CHAIN,

    INDICATOR_COUNT
};

constexpr size_t NUM_INDICATORS = static_cast<size_t>(IndicatorId::INDICATOR_COUNT);

// ── Indicator parameter ─────────────────────────────────────
struct IndicatorParam {
    IndicatorId id     = IndicatorId::SMA;
    int16_t     period = 20;      // primary lookback
    int16_t     period2 = 0;      // secondary (signal line, slow EMA, etc.)
    float       param1 = 0.0f;    // generic float (smoothing, multiplier)
    float       param2 = 0.0f;    // generic float

    bool operator==(const IndicatorParam& o) const {
        return id == o.id && period == o.period && period2 == o.period2;
    }
};

// ── Indicator metadata ──────────────────────────────────────
struct IndicatorMeta {
    IndicatorId id;
    const char* name;
    const char* category;
    int16_t     default_period;
    int16_t     min_period;
    int16_t     max_period;
    bool        needs_period2;
    bool        needs_intelligence;  // needs IntelligenceState*
    float       output_min;
    float       output_max;
};

const IndicatorMeta& get_indicator_meta(IndicatorId id);

// ── Indicator tape ──────────────────────────────────────────
struct IndicatorTape {
    std::vector<float> values;
    IndicatorParam     param;

    float at(size_t idx) const {
        return (idx < values.size()) ? values[idx] : 0.0f;
    }
    size_t size() const { return values.size(); }
    bool   empty() const { return values.empty(); }
};

// ── Compute a single indicator ──────────────────────────────
IndicatorTape compute_indicator(
    const Bar* bars,
    size_t num_bars,
    const IndicatorParam& param,
    const IntelligenceState* intelligence = nullptr
);

// ── Batch compute ───────────────────────────────────────────
std::vector<IndicatorTape> compute_indicators_batch(
    const Bar* bars,
    size_t num_bars,
    const std::vector<IndicatorParam>& params,
    const IntelligenceState* intelligence = nullptr
);

// ── Name accessors ──────────────────────────────────────────
const char* indicator_name(IndicatorId id);
const char* indicator_category(IndicatorId id);

} // namespace aphelion
