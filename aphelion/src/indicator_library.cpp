// ============================================================
// Aphelion Research — Indicator Library Implementation
//
// 100 indicator primitives. Each computes a float tape from bars.
// Organized: helpers → compute groups → main dispatch → metadata.
// ============================================================

#include "aphelion/indicator_library.h"
#include "aphelion/intelligence.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace aphelion {

// ════════════════════════════════════════════════════════════
//  HELPER FUNCTIONS
// ════════════════════════════════════════════════════════════

namespace {

inline float clamp01(float v) { return std::max(0.0f, std::min(1.0f, v)); }
inline float clamp11(float v) { return std::max(-1.0f, std::min(1.0f, v)); }

// ── True range ──────────────────────────────────────────────
inline double true_range_at(const Bar* bars, size_t i) {
    double hl = bars[i].high - bars[i].low;
    if (i == 0) return hl;
    double hc = std::fabs(bars[i].high - bars[i - 1].close);
    double lc = std::fabs(bars[i].low - bars[i - 1].close);
    return std::max({hl, hc, lc});
}

// ── SMA into output buffer ──────────────────────────────────
void fill_sma(const Bar* bars, size_t n, int period, float* out) {
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sum += bars[i].close;
        if (i >= static_cast<size_t>(period)) sum -= bars[i - period].close;
        size_t cnt = std::min(i + 1, static_cast<size_t>(period));
        out[i] = static_cast<float>(sum / cnt);
    }
}

// ── EMA into output buffer ──────────────────────────────────
void fill_ema(const Bar* bars, size_t n, int period, float* out) {
    if (n == 0) return;
    float alpha = 2.0f / (period + 1.0f);
    out[0] = static_cast<float>(bars[0].close);
    for (size_t i = 1; i < n; ++i) {
        out[i] = alpha * static_cast<float>(bars[i].close) + (1.0f - alpha) * out[i - 1];
    }
}

// ── WMA ─────────────────────────────────────────────────────
void fill_wma(const Bar* bars, size_t n, int period, float* out) {
    for (size_t i = 0; i < n; ++i) {
        size_t start = (i >= static_cast<size_t>(period - 1)) ? (i - period + 1) : 0;
        double wsum = 0.0, wdiv = 0.0;
        int w = 1;
        for (size_t j = start; j <= i; ++j, ++w) {
            wsum += bars[j].close * w;
            wdiv += w;
        }
        out[i] = static_cast<float>(wsum / std::max(wdiv, 1.0));
    }
}

// ── ATR ─────────────────────────────────────────────────────
void fill_atr(const Bar* bars, size_t n, int period, float* out) {
    if (n == 0) return;
    out[0] = static_cast<float>(bars[0].high - bars[0].low);
    float alpha = 1.0f / std::max(period, 1);
    for (size_t i = 1; i < n; ++i) {
        float tr = static_cast<float>(true_range_at(bars, i));
        if (i < static_cast<size_t>(period)) {
            // Simple average during warmup
            float sum = 0;
            for (size_t j = 0; j <= i; ++j) sum += static_cast<float>(true_range_at(bars, j));
            out[i] = sum / (i + 1);
        } else {
            out[i] = out[i - 1] + alpha * (tr - out[i - 1]);
        }
    }
}

// ── Rolling standard deviation of log returns ───────────────
void fill_stddev(const Bar* bars, size_t n, int period, float* out) {
    for (size_t i = 0; i < n; ++i) {
        if (i < 1 || period < 2) { out[i] = 0.0f; continue; }
        size_t start = (i >= static_cast<size_t>(period)) ? (i - period + 1) : 1;
        double sum = 0, sq = 0;
        int cnt = 0;
        for (size_t j = start; j <= i; ++j) {
            if (bars[j - 1].close <= 0) continue;
            double r = std::log(bars[j].close / bars[j - 1].close);
            sum += r; sq += r * r; cnt++;
        }
        if (cnt < 2) { out[i] = 0.0f; continue; }
        double var = (sq - sum * sum / cnt) / (cnt - 1);
        out[i] = static_cast<float>(std::sqrt(std::max(0.0, var)));
    }
}

// ── Rolling percentile rank (value among recent N values) ───
float percentile_rank(const float* vals, size_t idx, int period) {
    if (period < 1) return 0.5f;
    size_t start = (idx >= static_cast<size_t>(period - 1)) ? (idx - period + 1) : 0;
    float cur = vals[idx];
    int below = 0, total = 0;
    for (size_t j = start; j <= idx; ++j) { if (vals[j] < cur) below++; total++; }
    return (total > 0) ? static_cast<float>(below) / total : 0.5f;
}

// ── Rolling max/min ─────────────────────────────────────────
float rolling_high(const Bar* bars, size_t idx, int period) {
    size_t start = (idx >= static_cast<size_t>(period - 1)) ? (idx - period + 1) : 0;
    double h = bars[start].high;
    for (size_t j = start + 1; j <= idx; ++j) h = std::max(h, bars[j].high);
    return static_cast<float>(h);
}

float rolling_low(const Bar* bars, size_t idx, int period) {
    size_t start = (idx >= static_cast<size_t>(period - 1)) ? (idx - period + 1) : 0;
    double l = bars[start].low;
    for (size_t j = start + 1; j <= idx; ++j) l = std::min(l, bars[j].low);
    return static_cast<float>(l);
}

// ── Normalize by ATR ────────────────────────────────────────
void normalize_deviation(const Bar* bars, size_t n, const float* raw, const float* atr, float* out) {
    for (size_t i = 0; i < n; ++i) {
        float a = (atr[i] > 1e-8f) ? atr[i] : 1.0f;
        out[i] = (static_cast<float>(bars[i].close) - raw[i]) / a;
    }
}

} // anonymous namespace


// ════════════════════════════════════════════════════════════
//  METADATA TABLE
// ════════════════════════════════════════════════════════════

static const IndicatorMeta META_TABLE[] = {
    // A. TREND / MOMENTUM
    {IndicatorId::SMA,                     "SMA",                   "trend",     20,  2, 500, false, false, -5.0f, 5.0f},
    {IndicatorId::EMA,                     "EMA",                   "trend",     20,  2, 500, false, false, -5.0f, 5.0f},
    {IndicatorId::WMA,                     "WMA",                   "trend",     20,  2, 500, false, false, -5.0f, 5.0f},
    {IndicatorId::DEMA,                    "DEMA",                  "trend",     20,  2, 500, false, false, -5.0f, 5.0f},
    {IndicatorId::TEMA,                    "TEMA",                  "trend",     20,  2, 500, false, false, -5.0f, 5.0f},
    {IndicatorId::KAMA,                    "KAMA",                  "trend",     20,  2, 500, false, false, -5.0f, 5.0f},
    {IndicatorId::LINEAR_SLOPE,            "Slope",                 "trend",     20,  3, 500, false, false, -1.0f, 1.0f},
    {IndicatorId::MOMENTUM,                "Momentum",              "trend",     10,  1, 200, false, false, -0.5f, 0.5f},
    {IndicatorId::ROC,                     "ROC",                   "trend",     10,  1, 200, false, false, -0.5f, 0.5f},
    {IndicatorId::ACCELERATION,            "Acceleration",          "trend",      5,  2, 100, false, false, -0.2f, 0.2f},
    {IndicatorId::DIRECTIONAL_PERSISTENCE, "DirPersistence",        "trend",     10,  3, 100, false, false, -1.0f, 1.0f},
    {IndicatorId::TREND_STRENGTH,          "TrendStrength",         "trend",     20,  5, 200, false, false, -1.0f, 1.0f},
    {IndicatorId::MACD_LINE,               "MACD",                  "trend",     12,  5, 100, true,  false, -5.0f, 5.0f},
    {IndicatorId::MACD_SIGNAL,             "MACD_Signal",           "trend",     12,  5, 100, true,  false, -5.0f, 5.0f},
    {IndicatorId::MACD_HISTOGRAM,          "MACD_Hist",             "trend",     12,  5, 100, true,  false, -5.0f, 5.0f},
    {IndicatorId::RSI,                     "RSI",                   "trend",     14,  2, 100, false, false,  0.0f, 1.0f},
    {IndicatorId::STOCHASTIC_K,            "Stoch_K",               "trend",     14,  5,  50, false, false,  0.0f, 1.0f},
    {IndicatorId::STOCHASTIC_D,            "Stoch_D",               "trend",     14,  5,  50, true,  false,  0.0f, 1.0f},
    {IndicatorId::CCI,                     "CCI",                   "trend",     20,  5, 100, false, false, -3.0f, 3.0f},
    {IndicatorId::WILLIAMS_R,              "Williams_R",            "trend",     14,  5,  50, false, false, -1.0f, 0.0f},

    // B. VOLATILITY / RANGE
    {IndicatorId::ATR,                     "ATR",                   "volatility", 14, 2, 200, false, false,  0.0f, 1.0f},
    {IndicatorId::ROLLING_STDDEV,          "StdDev",                "volatility", 20, 2, 200, false, false,  0.0f, 0.5f},
    {IndicatorId::RANGE_PERCENTILE,        "RangePct",              "volatility", 50, 5, 500, false, false,  0.0f, 1.0f},
    {IndicatorId::COMPRESSION_RATIO,       "CompRatio",             "volatility", 20, 5, 200, false, false,  0.0f, 1.0f},
    {IndicatorId::EXPANSION_RATIO,         "ExpRatio",              "volatility", 20, 5, 200, false, false,  0.0f, 5.0f},
    {IndicatorId::REALIZED_VOL,            "RealVol",               "volatility", 20, 5, 200, false, false,  0.0f, 1.0f},
    {IndicatorId::VOL_CHANGE,              "VolChange",             "volatility", 20, 5, 200, false, false, -2.0f, 2.0f},
    {IndicatorId::BOLLINGER_WIDTH,         "BollingerW",            "volatility", 20, 5, 200, false, false,  0.0f, 10.0f},
    {IndicatorId::KELTNER_WIDTH,           "KeltnerW",              "volatility", 20, 5, 200, false, false,  0.0f, 10.0f},
    {IndicatorId::TRUE_RANGE_NORM,         "TrueRange",             "volatility", 14, 2, 100, false, false,  0.0f, 5.0f},
    {IndicatorId::RANGE_BODY_RATIO,        "RangeBody",             "volatility",  1, 1,   1, false, false,  1.0f, 20.0f},
    {IndicatorId::HIGH_LOW_RANGE,          "HLRange",               "volatility", 14, 2, 200, false, false,  0.0f, 5.0f},
    {IndicatorId::GARMAN_KLASS_VOL,        "GarmanKlass",           "volatility", 20, 5, 200, false, false,  0.0f, 0.5f},
    {IndicatorId::PARKINSON_VOL,           "Parkinson",             "volatility", 20, 5, 200, false, false,  0.0f, 0.5f},
    {IndicatorId::VOL_OF_VOL,              "VolOfVol",              "volatility", 20, 5, 200, false, false,  0.0f, 5.0f},

    // C. STRUCTURE / LOCATION
    {IndicatorId::DIST_ROLLING_HIGH,       "DistHigh",              "structure",  20,  5, 200, false, false, -2.0f, 0.0f},
    {IndicatorId::DIST_ROLLING_LOW,        "DistLow",               "structure",  20,  5, 200, false, false,  0.0f, 2.0f},
    {IndicatorId::BREAKOUT_PROXIMITY,      "BreakoutProx",          "structure",  20,  5, 200, false, false,  0.0f, 1.0f},
    {IndicatorId::MEAN_DISTANCE,           "MeanDist",              "structure",  20,  5, 200, false, false, -5.0f, 5.0f},
    {IndicatorId::OVERLAP_RATIO,           "Overlap",               "structure",   1,  1,   1, false, false,  0.0f, 1.0f},
    {IndicatorId::LOCAL_EXTREMA_COUNT,     "ExtremaCnt",            "structure",  20,  5, 100, false, false,  0.0f, 20.0f},
    {IndicatorId::HIGHER_HIGH_COUNT,       "HigherHighs",           "structure",  10,  3,  50, false, false,  0.0f, 10.0f},
    {IndicatorId::LOWER_LOW_COUNT,         "LowerLows",             "structure",  10,  3,  50, false, false,  0.0f, 10.0f},
    {IndicatorId::PIVOT_DISTANCE,          "PivotDist",             "structure",  20,  5, 100, false, false, -3.0f, 3.0f},
    {IndicatorId::BAR_POSITION,            "BarPos",                "structure",   1,  1,   1, false, false,  0.0f, 1.0f},
    {IndicatorId::RANGE_POSITION,          "RangePos",              "structure",  20,  5, 200, false, false,  0.0f, 1.0f},
    {IndicatorId::CHANNEL_POSITION,        "ChannelPos",            "structure",  50, 10, 300, false, false, -1.0f, 1.0f},
    {IndicatorId::MIDLINE_DISTANCE,        "MidlineDist",           "structure",  20,  5, 200, false, false, -3.0f, 3.0f},
    {IndicatorId::SUPPORT_STRENGTH,        "SupportStr",            "structure",  50, 10, 300, false, false,  0.0f, 1.0f},
    {IndicatorId::RESISTANCE_STRENGTH,     "ResistStr",             "structure",  50, 10, 300, false, false,  0.0f, 1.0f},

    // D. REGIME / STATE
    {IndicatorId::REGIME_IS_TREND,         "RegTrend",              "regime",      1, 1, 1, false, true, 0.0f, 1.0f},
    {IndicatorId::REGIME_IS_RANGE,         "RegRange",              "regime",      1, 1, 1, false, true, 0.0f, 1.0f},
    {IndicatorId::REGIME_IS_VOLATILE,      "RegVolatile",           "regime",      1, 1, 1, false, true, 0.0f, 1.0f},
    {IndicatorId::REGIME_IS_COMPRESSED,    "RegCompressed",         "regime",      1, 1, 1, false, true, 0.0f, 1.0f},
    {IndicatorId::REGIME_IS_TRANSITION,    "RegTransition",         "regime",      1, 1, 1, false, true, 0.0f, 1.0f},
    {IndicatorId::STABILITY_SCORE,         "Stability",             "regime",      1, 1, 1, false, true, 0.0f, 1.0f},
    {IndicatorId::ENTROPY_SCORE,           "Entropy",               "regime",      1, 1, 1, false, true, 0.0f, 1.0f},
    {IndicatorId::REGIME_AGE,              "RegimeAge",             "regime",      1, 1, 1, false, true, 0.0f, 255.0f},
    {IndicatorId::DIRECTIONAL_ENERGY_IND,  "DirEnergy",             "regime",      1, 1, 1, false, true, -1.0f, 1.0f},
    {IndicatorId::REGIME_STRENGTH_IND,     "RegStrength",           "regime",      1, 1, 1, false, true, 0.0f, 1.0f},

    // E. FRICTION / EXECUTION
    {IndicatorId::SPREAD_RANK,             "SpreadRank",            "friction",   50, 5, 200, false, false, 0.0f, 1.0f},
    {IndicatorId::GAP_PRESENT,             "GapFlag",               "friction",    1, 1,   1, false, false, 0.0f, 1.0f},
    {IndicatorId::SESSION_BOUNDARY,        "SessionBound",          "friction",    1, 1,   1, false, false, 0.0f, 1.0f},
    {IndicatorId::ABNORMAL_RANGE_FLAG,     "AbnormalRange",         "friction",   20, 5, 100, false, false, 0.0f, 1.0f},
    {IndicatorId::EXECUTION_QUALITY,       "ExecQuality",           "friction",   20, 5, 100, false, false, 0.0f, 1.0f},

    // F. CROSS-TIMEFRAME
    {IndicatorId::HTF_TREND_AGREEMENT,     "HTF_TrendAgree",        "htf", 1, 1, 1, false, true, -1.0f, 1.0f},
    {IndicatorId::HTF_MOMENTUM_AGREEMENT,  "HTF_MomAgree",          "htf", 1, 1, 1, false, true, -1.0f, 1.0f},
    {IndicatorId::HTF_REGIME_GATE,         "HTF_RegGate",           "htf", 1, 1, 1, false, true,  0.0f, 1.0f},
    {IndicatorId::HTF_STRUCTURE_DIVERGE,   "HTF_StructDiv",         "htf", 1, 1, 1, false, true, -1.0f, 1.0f},
    {IndicatorId::HTF_VOL_RATIO_IND,       "HTF_VolRatio",          "htf", 1, 1, 1, false, true,  0.0f, 5.0f},
    {IndicatorId::HTF_BIAS_SCORE,          "HTF_Bias",              "htf", 1, 1, 1, false, true, -1.0f, 1.0f},
    {IndicatorId::TF_TREND_RATIO,          "TF_TrendRatio",         "htf", 1, 1, 1, false, true, -3.0f, 3.0f},
    {IndicatorId::TF_MOMENTUM_RATIO,       "TF_MomRatio",           "htf", 1, 1, 1, false, true, -3.0f, 3.0f},
    {IndicatorId::TF_RANGE_RATIO,          "TF_RangeRatio",         "htf", 1, 1, 1, false, true,  0.0f, 5.0f},
    {IndicatorId::TF_COHERENCE,            "TF_Coherence",          "htf", 1, 1, 1, false, true,  0.0f, 1.0f},

    // G. PATTERN PRIMITIVES
    {IndicatorId::BREAKOUT_ATTEMPT,        "BreakoutAttempt",       "pattern", 20, 5, 100, false, false, 0.0f, 1.0f},
    {IndicatorId::BREAKOUT_FAILURE,        "BreakoutFail",          "pattern", 20, 5, 100, false, false, 0.0f, 1.0f},
    {IndicatorId::PULLBACK_CONTINUATION,   "PullbackCont",          "pattern", 20, 5, 100, false, false, 0.0f, 1.0f},
    {IndicatorId::EXHAUSTION_SIGNAL,       "Exhaustion",            "pattern", 10, 3,  50, false, false, 0.0f, 1.0f},
    {IndicatorId::COMPRESSION_TO_EXPANSION,"CompToExp",             "pattern", 20, 5, 100, false, false, 0.0f, 1.0f},
    {IndicatorId::REVERSAL_IMPULSE,        "ReversalImpulse",       "pattern",  5, 2,  30, false, false,-1.0f, 1.0f},
    {IndicatorId::INSIDE_BAR,              "InsideBar",             "pattern",  1, 1,   1, false, false, 0.0f, 1.0f},
    {IndicatorId::OUTSIDE_BAR,             "OutsideBar",            "pattern",  1, 1,   1, false, false, 0.0f, 1.0f},
    {IndicatorId::ENGULFING_BULLISH,       "EngulfBull",            "pattern",  1, 1,   1, false, false, 0.0f, 1.0f},
    {IndicatorId::ENGULFING_BEARISH,       "EngulfBear",            "pattern",  1, 1,   1, false, false, 0.0f, 1.0f},
    {IndicatorId::PIN_BAR_BULLISH,         "PinBull",               "pattern",  1, 1,   1, false, false, 0.0f, 1.0f},
    {IndicatorId::PIN_BAR_BEARISH,         "PinBear",               "pattern",  1, 1,   1, false, false, 0.0f, 1.0f},
    {IndicatorId::DOJI_PATTERN,            "Doji",                  "pattern",  1, 1,   1, false, false, 0.0f, 1.0f},
    {IndicatorId::THREE_BAR_REVERSAL,      "ThreeBarRev",           "pattern",  1, 1,   1, false, false,-1.0f, 1.0f},
    {IndicatorId::MOMENTUM_DIVERGENCE,     "MomDivergence",         "pattern", 20, 5, 100, false, false,-1.0f, 1.0f},
    {IndicatorId::VOLUME_CLIMAX,           "VolClimax",             "pattern", 20, 5, 100, false, false, 0.0f, 1.0f},
    {IndicatorId::RANGE_CONTRACTION_PATTERN,"RangeContract",        "pattern", 10, 3,  50, false, false, 0.0f, 1.0f},
    {IndicatorId::RANGE_EXPANSION_PATTERN, "RangeExpand",           "pattern", 10, 3,  50, false, false, 0.0f, 1.0f},
    {IndicatorId::MEAN_REVERSION_SETUP,    "MeanRevSetup",          "pattern", 20, 5, 100, false, false, 0.0f, 1.0f},
    {IndicatorId::TREND_CONTINUATION_SETUP,"TrendContSetup",        "pattern", 20, 5, 100, false, false, 0.0f, 1.0f},
    {IndicatorId::FAILED_BREAKOUT_REVERSAL,"FailBreakRev",          "pattern", 20, 5, 100, false, false, 0.0f, 1.0f},
    {IndicatorId::SQUEEZE_FIRE,            "SqueezeFire",           "pattern", 20, 5, 100, false, false, 0.0f, 1.0f},
    {IndicatorId::HIGHER_LOW_SETUP,        "HigherLow",             "pattern", 10, 3,  50, false, false, 0.0f, 1.0f},
    {IndicatorId::LOWER_HIGH_SETUP,        "LowerHigh",             "pattern", 10, 3,  50, false, false, 0.0f, 1.0f},
    {IndicatorId::PERSISTENCE_CHAIN,       "PersistChain",          "pattern", 10, 3,  50, false, false,-1.0f, 1.0f},
};

static_assert(sizeof(META_TABLE) / sizeof(META_TABLE[0]) == NUM_INDICATORS,
              "META_TABLE must have exactly NUM_INDICATORS entries");

const IndicatorMeta& get_indicator_meta(IndicatorId id) {
    size_t idx = static_cast<size_t>(id);
    if (idx >= NUM_INDICATORS) idx = 0;
    return META_TABLE[idx];
}

const char* indicator_name(IndicatorId id) { return get_indicator_meta(id).name; }
const char* indicator_category(IndicatorId id) { return get_indicator_meta(id).category; }


// ════════════════════════════════════════════════════════════
//  INDICATOR COMPUTATION
// ════════════════════════════════════════════════════════════

IndicatorTape compute_indicator(
    const Bar* bars,
    size_t n,
    const IndicatorParam& param,
    const IntelligenceState* intel
) {
    IndicatorTape tape;
    tape.param = param;
    tape.values.resize(n, 0.0f);
    if (n == 0) return tape;

    float* out = tape.values.data();
    const int P = std::max(static_cast<int>(param.period), 1);
    const int P2 = std::max(static_cast<int>(param.period2), 3);

    // Temporary buffers allocated on demand
    std::vector<float> tmp, tmp2, atr_buf;

    switch (param.id) {

    // ════════════════════════════════════════════════════════
    //  A. TREND / MOMENTUM
    // ════════════════════════════════════════════════════════

    case IndicatorId::SMA: {
        tmp.resize(n);
        atr_buf.resize(n);
        fill_sma(bars, n, P, tmp.data());
        fill_atr(bars, n, std::max(P, 14), atr_buf.data());
        normalize_deviation(bars, n, tmp.data(), atr_buf.data(), out);
        break;
    }
    case IndicatorId::EMA: {
        tmp.resize(n);
        atr_buf.resize(n);
        fill_ema(bars, n, P, tmp.data());
        fill_atr(bars, n, std::max(P, 14), atr_buf.data());
        normalize_deviation(bars, n, tmp.data(), atr_buf.data(), out);
        break;
    }
    case IndicatorId::WMA: {
        tmp.resize(n);
        atr_buf.resize(n);
        fill_wma(bars, n, P, tmp.data());
        fill_atr(bars, n, std::max(P, 14), atr_buf.data());
        normalize_deviation(bars, n, tmp.data(), atr_buf.data(), out);
        break;
    }
    case IndicatorId::DEMA: {
        tmp.resize(n); tmp2.resize(n); atr_buf.resize(n);
        fill_ema(bars, n, P, tmp.data());
        // EMA of EMA
        float alpha = 2.0f / (P + 1.0f);
        tmp2[0] = tmp[0];
        for (size_t i = 1; i < n; ++i)
            tmp2[i] = alpha * tmp[i] + (1 - alpha) * tmp2[i - 1];
        // DEMA = 2*EMA - EMA(EMA)
        fill_atr(bars, n, 14, atr_buf.data());
        for (size_t i = 0; i < n; ++i) {
            float dema = 2 * tmp[i] - tmp2[i];
            float a = (atr_buf[i] > 1e-8f) ? atr_buf[i] : 1.0f;
            out[i] = (static_cast<float>(bars[i].close) - dema) / a;
        }
        break;
    }
    case IndicatorId::TEMA: {
        tmp.resize(n); tmp2.resize(n); atr_buf.resize(n);
        std::vector<float> tmp3(n);
        fill_ema(bars, n, P, tmp.data());
        float alpha = 2.0f / (P + 1.0f);
        tmp2[0] = tmp[0]; tmp3[0] = tmp2[0];
        for (size_t i = 1; i < n; ++i) {
            tmp2[i] = alpha * tmp[i] + (1 - alpha) * tmp2[i - 1];
            tmp3[i] = alpha * tmp2[i] + (1 - alpha) * tmp3[i - 1];
        }
        fill_atr(bars, n, 14, atr_buf.data());
        for (size_t i = 0; i < n; ++i) {
            float tema = 3 * tmp[i] - 3 * tmp2[i] + tmp3[i];
            float a = (atr_buf[i] > 1e-8f) ? atr_buf[i] : 1.0f;
            out[i] = (static_cast<float>(bars[i].close) - tema) / a;
        }
        break;
    }
    case IndicatorId::KAMA: {
        atr_buf.resize(n);
        fill_atr(bars, n, 14, atr_buf.data());
        float fast_sc = 2.0f / 3.0f, slow_sc = 2.0f / 31.0f;
        float kama = static_cast<float>(bars[0].close);
        for (size_t i = 0; i < n; ++i) {
            if (i < static_cast<size_t>(P)) {
                kama = static_cast<float>(bars[i].close);
                float a = (atr_buf[i] > 1e-8f) ? atr_buf[i] : 1.0f;
                out[i] = (static_cast<float>(bars[i].close) - kama) / a;
                continue;
            }
            float direction = std::fabs(static_cast<float>(bars[i].close - bars[i - P].close));
            float volatility = 0;
            for (int j = 1; j <= P; ++j)
                volatility += std::fabs(static_cast<float>(bars[i - P + j].close - bars[i - P + j - 1].close));
            float er = (volatility > 1e-8f) ? direction / volatility : 0.0f;
            float sc = er * (fast_sc - slow_sc) + slow_sc;
            sc = sc * sc;
            kama = kama + sc * (static_cast<float>(bars[i].close) - kama);
            float a = (atr_buf[i] > 1e-8f) ? atr_buf[i] : 1.0f;
            out[i] = (static_cast<float>(bars[i].close) - kama) / a;
        }
        break;
    }
    case IndicatorId::LINEAR_SLOPE: {
        for (size_t i = 0; i < n; ++i) {
            if (i < static_cast<size_t>(P - 1)) { out[i] = 0; continue; }
            size_t start = i - P + 1;
            double sx = 0, sy = 0, sxy = 0, sxx = 0;
            for (int k = 0; k < P; ++k) {
                double x = k, y = bars[start + k].close;
                sx += x; sy += y; sxy += x * y; sxx += x * x;
            }
            double dn = P * sxx - sx * sx;
            double slope = (std::fabs(dn) > 1e-15) ? (P * sxy - sx * sy) / dn : 0;
            double avg = sy / P;
            out[i] = (avg > 0) ? static_cast<float>(slope / avg) : 0.0f;
        }
        break;
    }
    case IndicatorId::MOMENTUM: {
        for (size_t i = 0; i < n; ++i) {
            if (i < static_cast<size_t>(P)) { out[i] = 0; continue; }
            double prev = bars[i - P].close;
            out[i] = (prev > 0) ? static_cast<float>((bars[i].close - prev) / prev) : 0;
        }
        break;
    }
    case IndicatorId::ROC: {
        for (size_t i = 0; i < n; ++i) {
            if (i < static_cast<size_t>(P)) { out[i] = 0; continue; }
            double prev = bars[i - P].close;
            out[i] = (prev > 0) ? static_cast<float>((bars[i].close - prev) / prev) : 0;
        }
        break;
    }
    case IndicatorId::ACCELERATION: {
        // Momentum acceleration (2nd derivative)
        tmp.resize(n, 0.0f);
        for (size_t i = static_cast<size_t>(P); i < n; ++i) {
            double prev = bars[i - P].close;
            tmp[i] = (prev > 0) ? static_cast<float>((bars[i].close - prev) / prev) : 0;
        }
        for (size_t i = 1; i < n; ++i) out[i] = tmp[i] - tmp[i - 1];
        break;
    }
    case IndicatorId::DIRECTIONAL_PERSISTENCE: {
        int up_count = 0, down_count = 0;
        for (size_t i = 0; i < n; ++i) {
            if (i > 0) {
                if (bars[i].close > bars[i - 1].close) { up_count++; down_count = 0; }
                else if (bars[i].close < bars[i - 1].close) { down_count++; up_count = 0; }
                else { up_count = 0; down_count = 0; }
            }
            float raw = static_cast<float>(up_count - down_count);
            out[i] = clamp11(raw / std::max(P * 0.5f, 1.0f));
        }
        break;
    }
    case IndicatorId::TREND_STRENGTH: {
        // Agreement of short/medium/long slopes
        for (size_t i = 0; i < n; ++i) {
            auto slope_fn = [&](int p) -> float {
                if (i < static_cast<size_t>(p - 1) || p < 2) return 0;
                size_t s = i - p + 1;
                double sx = 0, sy = 0, sxy = 0, sxx = 0;
                for (int k = 0; k < p; ++k) {
                    double x = k, y = bars[s + k].close;
                    sx += x; sy += y; sxy += x * y; sxx += x * x;
                }
                double dn = p * sxx - sx * sx;
                return (std::fabs(dn) > 1e-15) ? static_cast<float>((p * sxy - sx * sy) / dn / (sy / p)) : 0;
            };
            float s1 = (slope_fn(P) > 0) ? 1.0f : (slope_fn(P) < 0 ? -1.0f : 0);
            float s2 = (slope_fn(P * 2) > 0) ? 1.0f : (slope_fn(P * 2) < 0 ? -1.0f : 0);
            float s3 = (slope_fn(P * 4) > 0) ? 1.0f : (slope_fn(P * 4) < 0 ? -1.0f : 0);
            out[i] = (s1 + s2 + s3) / 3.0f;
        }
        break;
    }
    case IndicatorId::MACD_LINE: {
        tmp.resize(n); tmp2.resize(n); atr_buf.resize(n);
        fill_ema(bars, n, P, tmp.data());        // fast
        fill_ema(bars, n, P2, tmp2.data());      // slow
        fill_atr(bars, n, 14, atr_buf.data());
        for (size_t i = 0; i < n; ++i) {
            float a = (atr_buf[i] > 1e-8f) ? atr_buf[i] : 1.0f;
            out[i] = (tmp[i] - tmp2[i]) / a;
        }
        break;
    }
    case IndicatorId::MACD_SIGNAL: {
        tmp.resize(n); tmp2.resize(n); atr_buf.resize(n);
        fill_ema(bars, n, P, tmp.data());
        fill_ema(bars, n, P2, tmp2.data());
        fill_atr(bars, n, 14, atr_buf.data());
        std::vector<float> macd_raw(n);
        for (size_t i = 0; i < n; ++i) macd_raw[i] = tmp[i] - tmp2[i];
        // Signal line = EMA of MACD (9-period)
        float sig_alpha = 2.0f / 10.0f;
        out[0] = macd_raw[0];
        for (size_t i = 1; i < n; ++i) out[i] = sig_alpha * macd_raw[i] + (1 - sig_alpha) * out[i - 1];
        for (size_t i = 0; i < n; ++i) {
            float a = (atr_buf[i] > 1e-8f) ? atr_buf[i] : 1.0f;
            out[i] /= a;
        }
        break;
    }
    case IndicatorId::MACD_HISTOGRAM: {
        tmp.resize(n); tmp2.resize(n); atr_buf.resize(n);
        fill_ema(bars, n, P, tmp.data());
        fill_ema(bars, n, P2, tmp2.data());
        fill_atr(bars, n, 14, atr_buf.data());
        std::vector<float> macd_raw(n), sig(n);
        for (size_t i = 0; i < n; ++i) macd_raw[i] = tmp[i] - tmp2[i];
        float sa = 2.0f / 10.0f;
        sig[0] = macd_raw[0];
        for (size_t i = 1; i < n; ++i) sig[i] = sa * macd_raw[i] + (1 - sa) * sig[i - 1];
        for (size_t i = 0; i < n; ++i) {
            float a = (atr_buf[i] > 1e-8f) ? atr_buf[i] : 1.0f;
            out[i] = (macd_raw[i] - sig[i]) / a;
        }
        break;
    }
    case IndicatorId::RSI: {
        float avg_gain = 0, avg_loss = 0;
        for (size_t i = 0; i < n; ++i) {
            if (i == 0) { out[i] = 0.5f; continue; }
            float change = static_cast<float>(bars[i].close - bars[i - 1].close);
            float gain = std::max(0.0f, change);
            float loss = std::max(0.0f, -change);
            if (i <= static_cast<size_t>(P)) {
                avg_gain += gain; avg_loss += loss;
                if (i == static_cast<size_t>(P)) { avg_gain /= P; avg_loss /= P; }
                out[i] = 0.5f;
            } else {
                avg_gain = (avg_gain * (P - 1) + gain) / P;
                avg_loss = (avg_loss * (P - 1) + loss) / P;
                float rs = (avg_loss > 1e-8f) ? avg_gain / avg_loss : 100.0f;
                out[i] = 1.0f - 1.0f / (1.0f + rs);  // 0-1 range
            }
        }
        break;
    }
    case IndicatorId::STOCHASTIC_K: {
        for (size_t i = 0; i < n; ++i) {
            float hi = rolling_high(bars, i, P);
            float lo = rolling_low(bars, i, P);
            float range = hi - lo;
            out[i] = (range > 1e-8f) ? clamp01((static_cast<float>(bars[i].close) - lo) / range) : 0.5f;
        }
        break;
    }
    case IndicatorId::STOCHASTIC_D: {
        // %K then smooth with SMA(period2)
        tmp.resize(n);
        for (size_t i = 0; i < n; ++i) {
            float hi = rolling_high(bars, i, P);
            float lo = rolling_low(bars, i, P);
            float range = hi - lo;
            tmp[i] = (range > 1e-8f) ? clamp01((static_cast<float>(bars[i].close) - lo) / range) : 0.5f;
        }
        // Smooth
        double sum = 0;
        for (size_t i = 0; i < n; ++i) {
            sum += tmp[i];
            if (i >= static_cast<size_t>(P2)) sum -= tmp[i - P2];
            size_t cnt = std::min(i + 1, static_cast<size_t>(P2));
            out[i] = static_cast<float>(sum / cnt);
        }
        break;
    }
    case IndicatorId::CCI: {
        for (size_t i = 0; i < n; ++i) {
            double tp = (bars[i].high + bars[i].low + bars[i].close) / 3.0;
            size_t start = (i >= static_cast<size_t>(P - 1)) ? (i - P + 1) : 0;
            double sum_tp = 0;
            int cnt = 0;
            for (size_t j = start; j <= i; ++j) {
                sum_tp += (bars[j].high + bars[j].low + bars[j].close) / 3.0;
                cnt++;
            }
            double mean_tp = sum_tp / cnt;
            double mean_dev = 0;
            for (size_t j = start; j <= i; ++j) {
                mean_dev += std::fabs((bars[j].high + bars[j].low + bars[j].close) / 3.0 - mean_tp);
            }
            mean_dev /= cnt;
            out[i] = (mean_dev > 1e-8) ? clamp11(static_cast<float>((tp - mean_tp) / (0.015 * mean_dev) / 100.0)) : 0;
        }
        break;
    }
    case IndicatorId::WILLIAMS_R: {
        for (size_t i = 0; i < n; ++i) {
            float hi = rolling_high(bars, i, P);
            float lo = rolling_low(bars, i, P);
            float range = hi - lo;
            out[i] = (range > 1e-8f) ? (static_cast<float>(bars[i].close) - hi) / range : -0.5f;
        }
        break;
    }

    // ════════════════════════════════════════════════════════
    //  B. VOLATILITY / RANGE
    // ════════════════════════════════════════════════════════

    case IndicatorId::ATR: {
        fill_atr(bars, n, P, out);
        // Percentile normalize
        for (size_t i = 0; i < n; ++i) out[i] = percentile_rank(out, i, std::max(P * 5, 50));
        break;
    }
    case IndicatorId::ROLLING_STDDEV: {
        fill_stddev(bars, n, P, out);
        break;
    }
    case IndicatorId::RANGE_PERCENTILE: {
        tmp.resize(n);
        for (size_t i = 0; i < n; ++i) tmp[i] = static_cast<float>(bars[i].high - bars[i].low);
        for (size_t i = 0; i < n; ++i) out[i] = percentile_rank(tmp.data(), i, P);
        break;
    }
    case IndicatorId::COMPRESSION_RATIO: {
        for (size_t i = 0; i < n; ++i) {
            int short_p = std::max(P / 2, 2);
            size_t ss = (i >= static_cast<size_t>(short_p - 1)) ? (i - short_p + 1) : 0;
            size_t ls = (i >= static_cast<size_t>(P - 1)) ? (i - P + 1) : 0;
            double sh = bars[ss].high, sl = bars[ss].low;
            for (size_t j = ss + 1; j <= i; ++j) { sh = std::max(sh, bars[j].high); sl = std::min(sl, bars[j].low); }
            double lh = bars[ls].high, ll = bars[ls].low;
            for (size_t j = ls + 1; j <= i; ++j) { lh = std::max(lh, bars[j].high); ll = std::min(ll, bars[j].low); }
            double lr = lh - ll;
            out[i] = (lr > 1e-10) ? clamp01(1.0f - static_cast<float>((sh - sl) / lr)) : 0.0f;
        }
        break;
    }
    case IndicatorId::EXPANSION_RATIO: {
        atr_buf.resize(n);
        fill_atr(bars, n, P, atr_buf.data());
        for (size_t i = 0; i < n; ++i) {
            float tr = static_cast<float>(true_range_at(bars, i));
            out[i] = (atr_buf[i] > 1e-8f) ? tr / atr_buf[i] : 1.0f;
        }
        break;
    }
    case IndicatorId::REALIZED_VOL: {
        fill_stddev(bars, n, P, out);
        // Annualize (approximate)
        float ann_factor = std::sqrt(252.0f * 24.0f); // hourly data approximation
        for (size_t i = 0; i < n; ++i) out[i] *= ann_factor;
        break;
    }
    case IndicatorId::VOL_CHANGE: {
        tmp.resize(n);
        fill_stddev(bars, n, P, tmp.data());
        for (size_t i = 1; i < n; ++i)
            out[i] = (tmp[i - 1] > 1e-8f) ? (tmp[i] - tmp[i - 1]) / tmp[i - 1] : 0;
        break;
    }
    case IndicatorId::BOLLINGER_WIDTH: {
        tmp.resize(n); tmp2.resize(n);
        fill_sma(bars, n, P, tmp.data());
        fill_stddev(bars, n, P, tmp2.data());
        for (size_t i = 0; i < n; ++i) {
            out[i] = (tmp[i] > 1e-8f) ? (4.0f * tmp2[i] * static_cast<float>(bars[i].close)) / tmp[i] : 0;
        }
        break;
    }
    case IndicatorId::KELTNER_WIDTH: {
        tmp.resize(n); atr_buf.resize(n);
        fill_ema(bars, n, P, tmp.data());
        fill_atr(bars, n, P, atr_buf.data());
        for (size_t i = 0; i < n; ++i) {
            out[i] = (tmp[i] > 1e-8f) ? (4.0f * atr_buf[i]) / tmp[i] : 0;
        }
        break;
    }
    case IndicatorId::TRUE_RANGE_NORM: {
        atr_buf.resize(n);
        fill_atr(bars, n, P, atr_buf.data());
        for (size_t i = 0; i < n; ++i) {
            float tr = static_cast<float>(true_range_at(bars, i));
            out[i] = (atr_buf[i] > 1e-8f) ? tr / atr_buf[i] : 1.0f;
        }
        break;
    }
    case IndicatorId::RANGE_BODY_RATIO: {
        for (size_t i = 0; i < n; ++i) {
            double range = bars[i].high - bars[i].low;
            double body = std::fabs(bars[i].close - bars[i].open);
            out[i] = (body > 1e-10) ? static_cast<float>(range / body) : 10.0f;
        }
        break;
    }
    case IndicatorId::HIGH_LOW_RANGE: {
        atr_buf.resize(n);
        fill_atr(bars, n, P, atr_buf.data());
        for (size_t i = 0; i < n; ++i) {
            float range = static_cast<float>(bars[i].high - bars[i].low);
            out[i] = (atr_buf[i] > 1e-8f) ? range / atr_buf[i] : 1.0f;
        }
        break;
    }
    case IndicatorId::GARMAN_KLASS_VOL: {
        for (size_t i = 0; i < n; ++i) {
            size_t start = (i >= static_cast<size_t>(P - 1)) ? (i - P + 1) : 0;
            double sum_gk = 0;
            int cnt = 0;
            for (size_t j = start; j <= i; ++j) {
                double h = bars[j].high, l = bars[j].low, o = bars[j].open, c = bars[j].close;
                if (h > l && o > 0) {
                    double lnhl = std::log(h / l);
                    double lnco = std::log(c / o);
                    sum_gk += 0.5 * lnhl * lnhl - (2 * std::log(2.0) - 1) * lnco * lnco;
                }
                cnt++;
            }
            out[i] = static_cast<float>(std::sqrt(std::max(0.0, sum_gk / std::max(cnt, 1))));
        }
        break;
    }
    case IndicatorId::PARKINSON_VOL: {
        for (size_t i = 0; i < n; ++i) {
            size_t start = (i >= static_cast<size_t>(P - 1)) ? (i - P + 1) : 0;
            double sum_pk = 0;
            int cnt = 0;
            for (size_t j = start; j <= i; ++j) {
                if (bars[j].low > 0 && bars[j].high > bars[j].low) {
                    double lnhl = std::log(bars[j].high / bars[j].low);
                    sum_pk += lnhl * lnhl;
                }
                cnt++;
            }
            out[i] = static_cast<float>(std::sqrt(sum_pk / (4.0 * std::log(2.0) * std::max(cnt, 1))));
        }
        break;
    }
    case IndicatorId::VOL_OF_VOL: {
        tmp.resize(n);
        fill_stddev(bars, n, P, tmp.data());
        // Std dev of volatility
        for (size_t i = 0; i < n; ++i) {
            size_t start = (i >= static_cast<size_t>(P - 1)) ? (i - P + 1) : 0;
            double sum = 0, sq = 0;
            int cnt = 0;
            for (size_t j = start; j <= i; ++j) { sum += tmp[j]; sq += tmp[j] * tmp[j]; cnt++; }
            if (cnt < 2) { out[i] = 0; continue; }
            double var = (sq - sum * sum / cnt) / (cnt - 1);
            double mean = sum / cnt;
            out[i] = (mean > 1e-8) ? static_cast<float>(std::sqrt(std::max(0.0, var)) / mean) : 0;
        }
        break;
    }

    // ════════════════════════════════════════════════════════
    //  C. STRUCTURE / LOCATION
    // ════════════════════════════════════════════════════════

    case IndicatorId::DIST_ROLLING_HIGH: {
        for (size_t i = 0; i < n; ++i) {
            float hi = rolling_high(bars, i, P);
            float lo = rolling_low(bars, i, P);
            float range = hi - lo;
            out[i] = (range > 1e-8f) ? (static_cast<float>(bars[i].close) - hi) / range : 0;
        }
        break;
    }
    case IndicatorId::DIST_ROLLING_LOW: {
        for (size_t i = 0; i < n; ++i) {
            float hi = rolling_high(bars, i, P);
            float lo = rolling_low(bars, i, P);
            float range = hi - lo;
            out[i] = (range > 1e-8f) ? (static_cast<float>(bars[i].close) - lo) / range : 0;
        }
        break;
    }
    case IndicatorId::BREAKOUT_PROXIMITY: {
        for (size_t i = 0; i < n; ++i) {
            float hi = rolling_high(bars, i, P);
            float lo = rolling_low(bars, i, P);
            float range = hi - lo;
            if (range < 1e-8f) { out[i] = 0.5f; continue; }
            float dh = (hi - static_cast<float>(bars[i].close)) / range;
            float dl = (static_cast<float>(bars[i].close) - lo) / range;
            out[i] = clamp01(1.0f - std::min(dh, dl));
        }
        break;
    }
    case IndicatorId::MEAN_DISTANCE: {
        tmp.resize(n); atr_buf.resize(n);
        fill_sma(bars, n, P, tmp.data());
        fill_atr(bars, n, 14, atr_buf.data());
        normalize_deviation(bars, n, tmp.data(), atr_buf.data(), out);
        break;
    }
    case IndicatorId::OVERLAP_RATIO: {
        for (size_t i = 0; i < n; ++i) {
            if (i == 0) { out[i] = 0.5f; continue; }
            double ol = std::max(bars[i].low, bars[i - 1].low);
            double oh = std::min(bars[i].high, bars[i - 1].high);
            double ul = std::min(bars[i].low, bars[i - 1].low);
            double uh = std::max(bars[i].high, bars[i - 1].high);
            double overlap = std::max(0.0, oh - ol);
            double uni = std::max(1e-9, uh - ul);
            out[i] = static_cast<float>(overlap / uni);
        }
        break;
    }
    case IndicatorId::LOCAL_EXTREMA_COUNT: {
        for (size_t i = 0; i < n; ++i) {
            size_t start = (i >= static_cast<size_t>(P - 1)) ? (i - P + 1) : 0;
            int count = 0;
            for (size_t j = start + 1; j < i; ++j) {
                if (bars[j].high > bars[j - 1].high && bars[j].high > bars[j + 1].high) count++;
                if (bars[j].low < bars[j - 1].low && bars[j].low < bars[j + 1].low) count++;
            }
            out[i] = static_cast<float>(count);
        }
        break;
    }
    case IndicatorId::HIGHER_HIGH_COUNT: {
        int count = 0;
        for (size_t i = 0; i < n; ++i) {
            if (i > 0 && bars[i].high > bars[i - 1].high) count++;
            else count = 0;
            out[i] = static_cast<float>(count);
        }
        break;
    }
    case IndicatorId::LOWER_LOW_COUNT: {
        int count = 0;
        for (size_t i = 0; i < n; ++i) {
            if (i > 0 && bars[i].low < bars[i - 1].low) count++;
            else count = 0;
            out[i] = static_cast<float>(count);
        }
        break;
    }
    case IndicatorId::PIVOT_DISTANCE: {
        atr_buf.resize(n);
        fill_atr(bars, n, 14, atr_buf.data());
        for (size_t i = 0; i < n; ++i) {
            double pivot = (bars[i].high + bars[i].low + bars[i].close) / 3.0;
            float a = (atr_buf[i] > 1e-8f) ? atr_buf[i] : 1.0f;
            out[i] = static_cast<float>((bars[i].close - pivot) / a);
        }
        break;
    }
    case IndicatorId::BAR_POSITION: {
        for (size_t i = 0; i < n; ++i) {
            double range = bars[i].high - bars[i].low;
            out[i] = (range > 1e-10) ? clamp01(static_cast<float>((bars[i].close - bars[i].low) / range)) : 0.5f;
        }
        break;
    }
    case IndicatorId::RANGE_POSITION: {
        for (size_t i = 0; i < n; ++i) {
            float hi = rolling_high(bars, i, P);
            float lo = rolling_low(bars, i, P);
            float range = hi - lo;
            out[i] = (range > 1e-8f) ? clamp01((static_cast<float>(bars[i].close) - lo) / range) : 0.5f;
        }
        break;
    }
    case IndicatorId::CHANNEL_POSITION: {
        for (size_t i = 0; i < n; ++i) {
            if (i < static_cast<size_t>(P - 1)) { out[i] = 0; continue; }
            size_t start = i - P + 1;
            double sx = 0, sy = 0, sxy = 0, sxx = 0;
            for (int k = 0; k < P; ++k) {
                double x = k, y = bars[start + k].close;
                sx += x; sy += y; sxy += x * y; sxx += x * x;
            }
            double dn = P * sxx - sx * sx;
            double slope = (std::fabs(dn) > 1e-15) ? (P * sxy - sx * sy) / dn : 0;
            double intercept = (sy - slope * sx) / P;
            double expected = intercept + slope * (P - 1);
            // Compute channel std dev
            double sum_sq = 0;
            for (int k = 0; k < P; ++k) {
                double exp_k = intercept + slope * k;
                double diff = bars[start + k].close - exp_k;
                sum_sq += diff * diff;
            }
            double ch_std = std::sqrt(sum_sq / P);
            out[i] = (ch_std > 1e-8) ? clamp11(static_cast<float>((bars[i].close - expected) / ch_std)) : 0;
        }
        break;
    }
    case IndicatorId::MIDLINE_DISTANCE: {
        atr_buf.resize(n);
        fill_atr(bars, n, 14, atr_buf.data());
        for (size_t i = 0; i < n; ++i) {
            float hi = rolling_high(bars, i, P);
            float lo = rolling_low(bars, i, P);
            float mid = (hi + lo) * 0.5f;
            float a = (atr_buf[i] > 1e-8f) ? atr_buf[i] : 1.0f;
            out[i] = (static_cast<float>(bars[i].close) - mid) / a;
        }
        break;
    }
    case IndicatorId::SUPPORT_STRENGTH:
    case IndicatorId::RESISTANCE_STRENGTH: {
        bool is_support = (param.id == IndicatorId::SUPPORT_STRENGTH);
        atr_buf.resize(n);
        fill_atr(bars, n, 14, atr_buf.data());
        for (size_t i = 0; i < n; ++i) {
            size_t start = (i >= static_cast<size_t>(P - 1)) ? (i - P + 1) : 0;
            float level = is_support ? static_cast<float>(bars[i].low) : static_cast<float>(bars[i].high);
            float tolerance = atr_buf[i] * 0.5f;
            int touches = 0;
            for (size_t j = start; j < i; ++j) {
                float test = is_support ? static_cast<float>(bars[j].low) : static_cast<float>(bars[j].high);
                if (std::fabs(test - level) < tolerance) touches++;
            }
            out[i] = clamp01(static_cast<float>(touches) / std::max(P * 0.3f, 1.0f));
        }
        break;
    }

    // ════════════════════════════════════════════════════════
    //  D. REGIME / STATE (read from intelligence tape)
    // ════════════════════════════════════════════════════════

    case IndicatorId::REGIME_IS_TREND: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i)
            out[i] = (intel[i].regime == Regime::TRENDING_UP || intel[i].regime == Regime::TRENDING_DOWN) ? 1.0f : 0.0f;
        break;
    }
    case IndicatorId::REGIME_IS_RANGE: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i)
            out[i] = (intel[i].regime == Regime::RANGE_BOUND) ? 1.0f : 0.0f;
        break;
    }
    case IndicatorId::REGIME_IS_VOLATILE: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i)
            out[i] = (intel[i].regime == Regime::VOLATILE_EXPANSION) ? 1.0f : 0.0f;
        break;
    }
    case IndicatorId::REGIME_IS_COMPRESSED: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i)
            out[i] = (intel[i].regime == Regime::COMPRESSION) ? 1.0f : 0.0f;
        break;
    }
    case IndicatorId::REGIME_IS_TRANSITION: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i)
            out[i] = (intel[i].regime == Regime::TRANSITION) ? 1.0f : 0.0f;
        break;
    }
    case IndicatorId::STABILITY_SCORE: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i) out[i] = intel[i].stability_score;
        break;
    }
    case IndicatorId::ENTROPY_SCORE: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i) out[i] = intel[i].total_entropy;
        break;
    }
    case IndicatorId::REGIME_AGE: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i) out[i] = static_cast<float>(intel[i].features.regime_persistence);
        break;
    }
    case IndicatorId::DIRECTIONAL_ENERGY_IND: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i) out[i] = intel[i].directional_energy;
        break;
    }
    case IndicatorId::REGIME_STRENGTH_IND: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i)
            out[i] = clamp01(intel[i].stability_score * 0.5f + intel[i].bias_strength * 0.5f);
        break;
    }

    // ════════════════════════════════════════════════════════
    //  E. FRICTION / EXECUTION
    // ════════════════════════════════════════════════════════

    case IndicatorId::SPREAD_RANK: {
        tmp.resize(n);
        for (size_t i = 0; i < n; ++i) tmp[i] = static_cast<float>(bars[i].spread);
        for (size_t i = 0; i < n; ++i) out[i] = percentile_rank(tmp.data(), i, P);
        break;
    }
    case IndicatorId::GAP_PRESENT: {
        for (size_t i = 0; i < n; ++i) out[i] = (bars[i].flags & Bar::FLAG_GAP) ? 1.0f : 0.0f;
        break;
    }
    case IndicatorId::SESSION_BOUNDARY: {
        for (size_t i = 0; i < n; ++i)
            out[i] = (bars[i].flags & (Bar::FLAG_SESSION_GAP | Bar::FLAG_WEEKEND_GAP)) ? 1.0f : 0.0f;
        break;
    }
    case IndicatorId::ABNORMAL_RANGE_FLAG: {
        atr_buf.resize(n);
        fill_atr(bars, n, P, atr_buf.data());
        for (size_t i = 0; i < n; ++i) {
            float tr = static_cast<float>(true_range_at(bars, i));
            out[i] = (atr_buf[i] > 1e-8f && tr > 3.0f * atr_buf[i]) ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::EXECUTION_QUALITY: {
        atr_buf.resize(n);
        fill_atr(bars, n, 14, atr_buf.data());
        for (size_t i = 0; i < n; ++i) {
            float spread_factor = clamp01(1.0f - static_cast<float>(bars[i].spread) * 0.01f / std::max(atr_buf[i], 1e-6f));
            float gap_factor = (bars[i].flags & Bar::FLAG_GAP) ? 0.5f : 1.0f;
            float range_factor = clamp01(atr_buf[i] > 1e-8f ?
                static_cast<float>(true_range_at(bars, i)) / (3.0f * atr_buf[i]) : 0.5f);
            range_factor = 1.0f - clamp01(range_factor);
            out[i] = clamp01(spread_factor * 0.4f + gap_factor * 0.3f + range_factor * 0.3f);
        }
        break;
    }

    // ════════════════════════════════════════════════════════
    //  F. CROSS-TIMEFRAME (read from intelligence tape)
    // ════════════════════════════════════════════════════════

    case IndicatorId::HTF_TREND_AGREEMENT: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i) out[i] = intel[i].features.htf_trend_alignment;
        break;
    }
    case IndicatorId::HTF_MOMENTUM_AGREEMENT: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i) out[i] = clamp11(intel[i].features.htf_bias * intel[i].directional_bias);
        break;
    }
    case IndicatorId::HTF_REGIME_GATE: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i) {
            bool trending = (intel[i].regime == Regime::TRENDING_UP || intel[i].regime == Regime::TRENDING_DOWN);
            bool htf_supports = std::fabs(intel[i].features.htf_bias) > 0.3f;
            out[i] = (trending && htf_supports) ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::HTF_STRUCTURE_DIVERGE: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i) {
            float primary_dir = (intel[i].directional_bias > 0) ? 1.0f : -1.0f;
            float htf_dir = (intel[i].features.htf_bias > 0) ? 1.0f : -1.0f;
            out[i] = primary_dir * htf_dir * -1.0f; // positive = diverging
        }
        break;
    }
    case IndicatorId::HTF_VOL_RATIO_IND: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i) out[i] = intel[i].features.htf_volatility_ratio;
        break;
    }
    case IndicatorId::HTF_BIAS_SCORE: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i) out[i] = intel[i].features.htf_bias;
        break;
    }
    case IndicatorId::TF_TREND_RATIO: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i) {
            float primary = intel[i].features.trend_alignment;
            float htf = intel[i].features.htf_trend_alignment;
            out[i] = (std::fabs(htf) > 0.01f) ? clamp11(primary / std::max(std::fabs(htf), 0.1f) * (htf > 0 ? 1.0f : -1.0f)) : 0;
        }
        break;
    }
    case IndicatorId::TF_MOMENTUM_RATIO: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i) {
            float primary_mom = intel[i].features.momentum_short;
            float htf_mom = intel[i].features.htf_bias * 0.01f;
            out[i] = (std::fabs(htf_mom) > 1e-6f) ? clamp11(primary_mom / std::max(std::fabs(htf_mom), 1e-4f)) : 0;
        }
        break;
    }
    case IndicatorId::TF_RANGE_RATIO: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i) out[i] = std::max(0.0f, intel[i].features.htf_volatility_ratio);
        break;
    }
    case IndicatorId::TF_COHERENCE: {
        if (!intel) break;
        for (size_t i = 0; i < n; ++i) out[i] = intel[i].cross_timeframe_coherence;
        break;
    }

    // ════════════════════════════════════════════════════════
    //  G. PATTERN PRIMITIVES
    // ════════════════════════════════════════════════════════

    case IndicatorId::BREAKOUT_ATTEMPT: {
        for (size_t i = 1; i < n; ++i) {
            float hi = rolling_high(bars, i - 1, P);
            float lo = rolling_low(bars, i - 1, P);
            float range = hi - lo;
            if (range < 1e-8f) continue;
            float close = static_cast<float>(bars[i].close);
            float dh = (hi - close) / range;
            float dl = (close - lo) / range;
            out[i] = clamp01(1.0f - std::min(dh, dl) * 5.0f);
        }
        break;
    }
    case IndicatorId::BREAKOUT_FAILURE: {
        for (size_t i = 2; i < n; ++i) {
            float hi = rolling_high(bars, i - 2, P);
            float lo = rolling_low(bars, i - 2, P);
            bool broke_hi = bars[i - 1].high > hi;
            bool broke_lo = bars[i - 1].low < lo;
            bool closed_inside = bars[i].close <= hi && bars[i].close >= lo;
            out[i] = (broke_hi || broke_lo) && closed_inside ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::PULLBACK_CONTINUATION: {
        if (!intel) break;
        for (size_t i = 2; i < n; ++i) {
            bool trending = (intel[i].regime == Regime::TRENDING_UP || intel[i].regime == Regime::TRENDING_DOWN);
            if (!trending) continue;
            bool uptrend = intel[i].regime == Regime::TRENDING_UP;
            bool pullback = uptrend ? (bars[i - 1].close < bars[i - 2].close) : (bars[i - 1].close > bars[i - 2].close);
            bool resume = uptrend ? (bars[i].close > bars[i - 1].close) : (bars[i].close < bars[i - 1].close);
            out[i] = (pullback && resume) ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::EXHAUSTION_SIGNAL: {
        atr_buf.resize(n);
        fill_atr(bars, n, P, atr_buf.data());
        for (size_t i = 1; i < n; ++i) {
            float body = static_cast<float>(std::fabs(bars[i].close - bars[i].open));
            float range = static_cast<float>(bars[i].high - bars[i].low);
            bool large_range = range > 2.5f * atr_buf[i];
            bool small_body = body < range * 0.3f;
            out[i] = (large_range && small_body) ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::COMPRESSION_TO_EXPANSION: {
        atr_buf.resize(n);
        fill_atr(bars, n, P, atr_buf.data());
        for (size_t i = 1; i < n; ++i) {
            float prev_range = static_cast<float>(bars[i - 1].high - bars[i - 1].low);
            float curr_range = static_cast<float>(bars[i].high - bars[i].low);
            bool was_compressed = (atr_buf[i - 1] > 1e-8f) && (prev_range < 0.6f * atr_buf[i - 1]);
            bool now_expanded = (atr_buf[i] > 1e-8f) && (curr_range > 1.5f * atr_buf[i]);
            out[i] = (was_compressed && now_expanded) ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::REVERSAL_IMPULSE: {
        atr_buf.resize(n);
        fill_atr(bars, n, 14, atr_buf.data());
        for (size_t i = 1; i < n; ++i) {
            float change = static_cast<float>(bars[i].close - bars[i - 1].close);
            float a = (atr_buf[i] > 1e-8f) ? atr_buf[i] : 1.0f;
            float norm_change = change / a;
            // Strong reversal = large move opposite to prior direction
            float prev_dir = (i > 1) ? static_cast<float>(bars[i - 1].close - bars[i - 2].close) : 0;
            bool reversal = (prev_dir > 0 && change < 0) || (prev_dir < 0 && change > 0);
            out[i] = reversal ? clamp11(norm_change) : 0.0f;
        }
        break;
    }
    case IndicatorId::INSIDE_BAR: {
        for (size_t i = 1; i < n; ++i)
            out[i] = (bars[i].high <= bars[i - 1].high && bars[i].low >= bars[i - 1].low) ? 1.0f : 0.0f;
        break;
    }
    case IndicatorId::OUTSIDE_BAR: {
        for (size_t i = 1; i < n; ++i)
            out[i] = (bars[i].high > bars[i - 1].high && bars[i].low < bars[i - 1].low) ? 1.0f : 0.0f;
        break;
    }
    case IndicatorId::ENGULFING_BULLISH: {
        for (size_t i = 1; i < n; ++i) {
            bool prev_bear = bars[i - 1].close < bars[i - 1].open;
            bool curr_bull = bars[i].close > bars[i].open;
            bool engulfs = bars[i].close > bars[i - 1].open && bars[i].open < bars[i - 1].close;
            out[i] = (prev_bear && curr_bull && engulfs) ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::ENGULFING_BEARISH: {
        for (size_t i = 1; i < n; ++i) {
            bool prev_bull = bars[i - 1].close > bars[i - 1].open;
            bool curr_bear = bars[i].close < bars[i].open;
            bool engulfs = bars[i].close < bars[i - 1].open && bars[i].open > bars[i - 1].close;
            out[i] = (prev_bull && curr_bear && engulfs) ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::PIN_BAR_BULLISH: {
        for (size_t i = 0; i < n; ++i) {
            double range = bars[i].high - bars[i].low;
            if (range < 1e-10) continue;
            double body = std::fabs(bars[i].close - bars[i].open);
            double lower_wick = std::min(bars[i].open, bars[i].close) - bars[i].low;
            out[i] = (lower_wick > 2.0 * body && lower_wick > 0.6 * range) ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::PIN_BAR_BEARISH: {
        for (size_t i = 0; i < n; ++i) {
            double range = bars[i].high - bars[i].low;
            if (range < 1e-10) continue;
            double body = std::fabs(bars[i].close - bars[i].open);
            double upper_wick = bars[i].high - std::max(bars[i].open, bars[i].close);
            out[i] = (upper_wick > 2.0 * body && upper_wick > 0.6 * range) ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::DOJI_PATTERN: {
        for (size_t i = 0; i < n; ++i) {
            double range = bars[i].high - bars[i].low;
            double body = std::fabs(bars[i].close - bars[i].open);
            out[i] = (range > 1e-10 && body < 0.1 * range) ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::THREE_BAR_REVERSAL: {
        for (size_t i = 2; i < n; ++i) {
            bool down_down_up = bars[i - 2].close < bars[i - 2].open &&
                                bars[i - 1].close < bars[i - 1].open &&
                                bars[i].close > bars[i].open &&
                                bars[i].close > bars[i - 1].high;
            bool up_up_down = bars[i - 2].close > bars[i - 2].open &&
                              bars[i - 1].close > bars[i - 1].open &&
                              bars[i].close < bars[i].open &&
                              bars[i].close < bars[i - 1].low;
            if (down_down_up) out[i] = 1.0f;
            else if (up_up_down) out[i] = -1.0f;
        }
        break;
    }
    case IndicatorId::MOMENTUM_DIVERGENCE: {
        // Price makes new high but momentum declining (bearish divergence → negative)
        // Price makes new low but momentum rising (bullish divergence → positive)
        for (size_t i = static_cast<size_t>(P); i < n; ++i) {
            float hi = rolling_high(bars, i - 1, P);
            float lo = rolling_low(bars, i - 1, P);
            float prev_mom = (i >= 2 * static_cast<size_t>(P)) ?
                static_cast<float>((bars[i - P].close - bars[i - 2 * P].close) / std::max(bars[i - 2 * P].close, 1.0)) : 0;
            float curr_mom = static_cast<float>((bars[i].close - bars[i - P].close) / std::max(bars[i - P].close, 1.0));
            if (bars[i].high > hi && curr_mom < prev_mom) out[i] = -1.0f; // bearish divergence
            else if (bars[i].low < lo && curr_mom > prev_mom) out[i] = 1.0f; // bullish divergence
        }
        break;
    }
    case IndicatorId::VOLUME_CLIMAX: {
        tmp.resize(n);
        for (size_t i = 0; i < n; ++i) tmp[i] = static_cast<float>(bars[i].tick_volume);
        for (size_t i = 0; i < n; ++i) {
            float pct = percentile_rank(tmp.data(), i, P);
            out[i] = (pct > 0.95f) ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::RANGE_CONTRACTION_PATTERN: {
        for (size_t i = static_cast<size_t>(P); i < n; ++i) {
            int contracting = 0;
            for (size_t j = i - P + 1; j <= i; ++j) {
                if (j > 0 && (bars[j].high - bars[j].low) < (bars[j - 1].high - bars[j - 1].low))
                    contracting++;
            }
            out[i] = clamp01(static_cast<float>(contracting) / std::max(P - 1, 1));
        }
        break;
    }
    case IndicatorId::RANGE_EXPANSION_PATTERN: {
        for (size_t i = static_cast<size_t>(P); i < n; ++i) {
            int expanding = 0;
            for (size_t j = i - P + 1; j <= i; ++j) {
                if (j > 0 && (bars[j].high - bars[j].low) > (bars[j - 1].high - bars[j - 1].low))
                    expanding++;
            }
            out[i] = clamp01(static_cast<float>(expanding) / std::max(P - 1, 1));
        }
        break;
    }
    case IndicatorId::MEAN_REVERSION_SETUP: {
        tmp.resize(n); atr_buf.resize(n);
        fill_sma(bars, n, P, tmp.data());
        fill_atr(bars, n, 14, atr_buf.data());
        for (size_t i = 1; i < n; ++i) {
            float dev = (static_cast<float>(bars[i].close) - tmp[i]);
            float a = (atr_buf[i] > 1e-8f) ? atr_buf[i] : 1.0f;
            float norm_dev = dev / a;
            // Overextended (>2 ATR from mean) and starting to revert
            bool overextended = std::fabs(norm_dev) > 2.0f;
            bool reverting = (norm_dev > 0 && bars[i].close < bars[i - 1].close) ||
                             (norm_dev < 0 && bars[i].close > bars[i - 1].close);
            out[i] = (overextended && reverting) ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::TREND_CONTINUATION_SETUP: {
        if (!intel) break;
        atr_buf.resize(n);
        fill_atr(bars, n, 14, atr_buf.data());
        for (size_t i = 2; i < n; ++i) {
            bool trending = (intel[i].regime == Regime::TRENDING_UP || intel[i].regime == Regime::TRENDING_DOWN);
            if (!trending) continue;
            bool up = intel[i].regime == Regime::TRENDING_UP;
            // Pullback (1-3 bars against trend) then resume
            bool pullback = up ? (bars[i - 1].close < bars[i - 2].close) : (bars[i - 1].close > bars[i - 2].close);
            bool resume = up ? (bars[i].close > bars[i - 1].high) : (bars[i].close < bars[i - 1].low);
            bool shallow = std::fabs(static_cast<float>(bars[i - 1].close - bars[i - 2].close)) < atr_buf[i];
            out[i] = (pullback && resume && shallow) ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::FAILED_BREAKOUT_REVERSAL: {
        for (size_t i = 3; i < n; ++i) {
            float hi = rolling_high(bars, i - 3, P);
            float lo = rolling_low(bars, i - 3, P);
            // Failed upside breakout → reversal down
            bool broke_up = bars[i - 2].high > hi || bars[i - 1].high > hi;
            bool reversed_down = bars[i].close < lo + (hi - lo) * 0.5f;
            // Failed downside breakout → reversal up
            bool broke_down = bars[i - 2].low < lo || bars[i - 1].low < lo;
            bool reversed_up = bars[i].close > lo + (hi - lo) * 0.5f;
            if (broke_up && reversed_down) out[i] = 1.0f;
            else if (broke_down && reversed_up) out[i] = 1.0f;
        }
        break;
    }
    case IndicatorId::SQUEEZE_FIRE: {
        atr_buf.resize(n);
        fill_atr(bars, n, P, atr_buf.data());
        tmp.resize(n);
        fill_stddev(bars, n, P, tmp.data());
        for (size_t i = 1; i < n; ++i) {
            float bb_width = tmp[i] * 2.0f;
            float kc_width = atr_buf[i] * 1.5f;
            bool was_squeezed = (i > 0) && (tmp[i - 1] * 2.0f < atr_buf[i - 1] * 1.5f);
            bool now_expanded = bb_width > kc_width;
            out[i] = (was_squeezed && now_expanded) ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::HIGHER_LOW_SETUP: {
        for (size_t i = 3; i < n; ++i) {
            // Find two recent swing lows
            float low1 = static_cast<float>(bars[i - 2].low);
            float low2 = static_cast<float>(bars[i].low);
            bool higher_low = low2 > low1;
            bool price_rising = bars[i].close > bars[i - 1].close;
            out[i] = (higher_low && price_rising) ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::LOWER_HIGH_SETUP: {
        for (size_t i = 3; i < n; ++i) {
            float hi1 = static_cast<float>(bars[i - 2].high);
            float hi2 = static_cast<float>(bars[i].high);
            bool lower_high = hi2 < hi1;
            bool price_falling = bars[i].close < bars[i - 1].close;
            out[i] = (lower_high && price_falling) ? 1.0f : 0.0f;
        }
        break;
    }
    case IndicatorId::PERSISTENCE_CHAIN: {
        int streak = 0;
        for (size_t i = 0; i < n; ++i) {
            if (i == 0) { out[i] = 0; continue; }
            if (bars[i].close > bars[i - 1].close) {
                streak = (streak > 0) ? streak + 1 : 1;
            } else if (bars[i].close < bars[i - 1].close) {
                streak = (streak < 0) ? streak - 1 : -1;
            } else {
                streak = 0;
            }
            out[i] = clamp11(static_cast<float>(streak) / std::max(static_cast<float>(P), 1.0f));
        }
        break;
    }

    default:
        break;
    }

    return tape;
}


// ── Batch compute ───────────────────────────────────────────
std::vector<IndicatorTape> compute_indicators_batch(
    const Bar* bars,
    size_t num_bars,
    const std::vector<IndicatorParam>& params,
    const IntelligenceState* intelligence
) {
    std::vector<IndicatorTape> result;
    result.reserve(params.size());
    for (const auto& p : params) {
        result.push_back(compute_indicator(bars, num_bars, p, intelligence));
    }
    return result;
}

} // namespace aphelion
