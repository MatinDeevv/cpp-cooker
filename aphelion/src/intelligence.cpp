// ============================================================
// Aphelion Research — Unified Intelligence Tape
// ============================================================

#include "aphelion/intelligence.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace aphelion {

namespace {

static float clamp01(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

static float clamp11(float value) {
    return std::max(-1.0f, std::min(1.0f, value));
}

static float trend_sign(float value) {
    return (value > 0.0f) ? 1.0f : (value < 0.0f ? -1.0f : 0.0f);
}

static void apply_multi_timeframe_context(
    FeatureTape& feature_tape,
    const MultiTimeframeInput* contexts,
    size_t num_contexts,
    const FeatureConfig& feature_config
) {
    if (feature_tape.empty() || contexts == nullptr || num_contexts == 0) {
        return;
    }

    const size_t primary_size = feature_tape.size();
    std::vector<float> bias_sum(primary_size, 0.0f);
    std::vector<float> alignment_sum(primary_size, 0.0f);
    std::vector<float> vol_ratio_sum(primary_size, 0.0f);
    std::vector<float> weight_sum(primary_size, 0.0f);

    for (size_t context_idx = 0; context_idx < num_contexts; ++context_idx) {
        const MultiTimeframeInput& context = contexts[context_idx];
        if (context.secondary == nullptr || context.alignment == nullptr) {
            continue;
        }
        if (context.secondary->bars.empty() || context.alignment->index.empty()) {
            continue;
        }

        FeatureTape secondary_features = compute_features(*context.secondary, feature_config);
        const float weight = std::max(0.1f, context.weight);

        for (size_t bar_idx = 0; bar_idx < primary_size; ++bar_idx) {
            uint32_t secondary_idx = context.alignment->index[bar_idx];
            if (secondary_idx == UINT32_MAX || secondary_idx >= secondary_features.size()) {
                continue;
            }

            const BarFeatures& secondary_feature = secondary_features.at(secondary_idx);
            const float secondary_bias = clamp11(
                secondary_feature.trend_alignment * 0.65f +
                clamp11(secondary_feature.momentum_medium * 40.0f) * 0.35f
            );
            const float primary_sign = trend_sign(feature_tape.features[bar_idx].trend_slope_medium);
            const float secondary_sign = trend_sign(secondary_feature.trend_slope_medium);
            const float agreement = primary_sign * secondary_sign;
            const float vol_ratio = (secondary_feature.volatility_raw > 1e-8f)
                ? feature_tape.features[bar_idx].volatility_raw / secondary_feature.volatility_raw
                : 1.0f;

            bias_sum[bar_idx] += secondary_bias * weight;
            alignment_sum[bar_idx] += agreement * weight;
            vol_ratio_sum[bar_idx] += vol_ratio * weight;
            weight_sum[bar_idx] += weight;
        }
    }

    for (size_t bar_idx = 0; bar_idx < primary_size; ++bar_idx) {
        BarFeatures& feature = feature_tape.features[bar_idx];
        if (weight_sum[bar_idx] <= 0.0f) {
            feature.htf_trend_alignment = 0.0f;
            feature.htf_bias = 0.0f;
            feature.htf_volatility_ratio = 1.0f;
            continue;
        }

        const float inv_weight = 1.0f / weight_sum[bar_idx];
        feature.htf_bias = clamp11(bias_sum[bar_idx] * inv_weight);
        feature.htf_trend_alignment = clamp11(alignment_sum[bar_idx] * inv_weight);
        feature.htf_volatility_ratio = std::max(0.0f, vol_ratio_sum[bar_idx] * inv_weight);
    }
}

static float compute_stability_score(const BarFeatures& feature, Regime regime) {
    const float persistence = clamp01(static_cast<float>(feature.regime_persistence) / 8.0f);
    const float trend_quality = clamp01(std::fabs(feature.trend_alignment));
    const float acceleration_penalty = clamp01(std::fabs(feature.momentum_acceleration) * 150.0f);
    const float friction_penalty = clamp01(std::max(0.0f, feature.spread_percentile - 0.75f) / 0.25f);

    float stability = 0.20f + trend_quality * 0.35f + persistence * 0.35f;
    stability += (1.0f - acceleration_penalty) * 0.10f;

    if (regime == Regime::TRANSITION) {
        stability *= 0.35f;
    } else if (regime == Regime::VOLATILE_EXPANSION) {
        stability *= 0.80f;
    } else if (regime == Regime::COMPRESSION) {
        stability *= 0.85f;
    }

    if (feature.gap_flag != 0) {
        stability *= 0.80f;
    }

    stability *= (1.0f - friction_penalty * 0.25f);
    return clamp01(stability);
}

} // namespace

IntelligenceTape build_intelligence_tape(
    const BarTape& primary,
    const MultiTimeframeInput* contexts,
    size_t num_contexts,
    const FeatureConfig& feature_config,
    const RegimeConfig& regime_config
) {
    IntelligenceTape intelligence;
    if (primary.bars.empty()) {
        return intelligence;
    }

    FeatureTape feature_tape = compute_features(primary, feature_config);
    apply_multi_timeframe_context(feature_tape, contexts, num_contexts, feature_config);
    classify_regimes(feature_tape, regime_config);

    intelligence.states.resize(feature_tape.size());
    for (size_t bar_idx = 0; bar_idx < feature_tape.size(); ++bar_idx) {
        const BarFeatures& feature = feature_tape.at(bar_idx);
        IntelligenceState& state = intelligence.states[bar_idx];

        state.features = feature;
        state.regime = static_cast<Regime>(feature.regime);

        const float momentum_component = clamp11(
            feature.momentum_short * 80.0f + feature.momentum_medium * 35.0f
        );
        const float structure_component = clamp11(feature.distance_to_low + feature.distance_to_high);
        state.directional_bias = clamp11(
            feature.trend_alignment * 0.45f +
            feature.htf_bias * 0.25f +
            momentum_component * 0.20f +
            structure_component * 0.10f
        );

        state.stability_score = compute_stability_score(feature, state.regime);
        state.bias_strength = clamp01(std::fabs(state.directional_bias) * (0.55f + 0.45f * state.stability_score));
        state.volatility_state = clamp11(
            (feature.volatility_percentile - 0.5f) * 1.6f +
            (feature.volatility_ratio - 1.0f) * 0.6f
        );

        float validity = state.stability_score;
        if (state.regime == Regime::TRANSITION) {
            validity *= 0.50f;
        }
        if (feature.spread_percentile > 0.85f) {
            validity *= 0.85f;
        }
        if (feature.gap_flag != 0) {
            validity *= 0.85f;
        }
        state.context_validity = clamp01(validity);
    }

    std::cout << "[intelligence] Unified tape built over " << intelligence.size()
              << " bars using " << num_contexts << " context timeframe(s)" << std::endl;

    return intelligence;
}

} // namespace aphelion