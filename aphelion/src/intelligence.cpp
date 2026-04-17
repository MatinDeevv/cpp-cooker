// ============================================================
// Aphelion Research - Unified Intelligence Tape
// ============================================================

#include "aphelion/intelligence.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace aphelion {

namespace {

constexpr float kLog2 = 0.6931471805599453f;

float clamp01(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

float clamp11(float value) {
    return std::max(-1.0f, std::min(1.0f, value));
}

float trend_sign(float value) {
    return (value > 0.0f) ? 1.0f : (value < 0.0f ? -1.0f : 0.0f);
}

float binary_entropy(float p_up) {
    const float p = clamp01(p_up);
    if (p <= 1e-6f || p >= 1.0f - 1e-6f) return 0.0f;
    const float q = 1.0f - p;
    return -((p * std::log(p)) + (q * std::log(q))) / kLog2;
}

float bucket_entropy(const std::array<int, 4>& counts, int total) {
    if (total <= 0) return 0.0f;

    float entropy = 0.0f;
    for (int count : counts) {
        if (count <= 0) continue;
        const float p = static_cast<float>(count) / static_cast<float>(total);
        entropy -= p * std::log(p);
    }
    return entropy / std::log(4.0f);
}

int volatility_bucket(float normalized_range) {
    if (normalized_range < 0.65f) return 0;
    if (normalized_range < 1.00f) return 1;
    if (normalized_range < 1.35f) return 2;
    return 3;
}

void apply_multi_timeframe_context(
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
            const uint32_t secondary_idx = context.alignment->index[bar_idx];
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

float compute_stability_score(const BarFeatures& feature, Regime regime) {
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
    const RegimeConfig& regime_config,
    const EchConfig& ech_config
) {
    IntelligenceTape intelligence;
    if (primary.bars.empty()) {
        return intelligence;
    }

    FeatureTape feature_tape = compute_features(primary, feature_config);
    apply_multi_timeframe_context(feature_tape, contexts, num_contexts, feature_config);
    classify_regimes(feature_tape, regime_config);

    const size_t n = feature_tape.size();
    const Bar* bars = primary.bars.data();

    std::vector<float> returns(n, 0.0f);
    std::vector<float> impulse_seed(n, 0.0f);
    std::vector<int> up_prefix(n + 1, 0);
    std::vector<int> flip_prefix(n + 1, 0);
    std::vector<float> overlap_prefix(n + 1, 0.0f);
    std::vector<float> range_prefix(n + 1, 0.0f);
    std::array<std::vector<int>, 4> bucket_prefix;
    for (auto& prefix : bucket_prefix) {
        prefix.assign(n + 1, 0);
    }

    for (size_t i = 0; i < n; ++i) {
        const Bar& bar = bars[i];
        const BarFeatures& feature = feature_tape.at(i);
        const double range = std::max(0.0, bar.high - bar.low);

        range_prefix[i + 1] = range_prefix[i] + static_cast<float>(range);

        const size_t range_window_start = (i + 1 > static_cast<size_t>(ech_config.structural_window))
            ? (i + 1 - static_cast<size_t>(ech_config.structural_window))
            : 0;
        const float range_window_sum = range_prefix[i + 1] - range_prefix[range_window_start];
        const float range_window_count = static_cast<float>(i - range_window_start + 1);
        const float mean_range = (range_window_count > 0.0f)
            ? range_window_sum / range_window_count
            : static_cast<float>(range);
        const float normalized_range = (mean_range > 1e-6f)
            ? static_cast<float>(range) / mean_range
            : 1.0f;
        const int bucket = volatility_bucket(normalized_range);

        for (size_t b = 0; b < bucket_prefix.size(); ++b) {
            bucket_prefix[b][i + 1] = bucket_prefix[b][i] + ((static_cast<int>(b) == bucket) ? 1 : 0);
        }

        const float momentum_component = clamp11(
            feature.momentum_short * 80.0f + feature.momentum_medium * 35.0f
        );
        const float structure_component = clamp11(feature.distance_to_low + feature.distance_to_high);
        impulse_seed[i] = clamp11(
            feature.trend_alignment * 0.45f +
            momentum_component * 0.30f +
            feature.htf_bias * 0.15f +
            structure_component * 0.10f
        );

        if (i == 0) {
            up_prefix[i + 1] = up_prefix[i];
            flip_prefix[i + 1] = flip_prefix[i];
            overlap_prefix[i + 1] = overlap_prefix[i];
            continue;
        }

        const double prev_close = bars[i - 1].close;
        const float ret = (prev_close > 0.0)
            ? static_cast<float>((bar.close - prev_close) / prev_close)
            : 0.0f;
        returns[i] = ret;
        up_prefix[i + 1] = up_prefix[i] + ((ret > 0.0f) ? 1 : 0);

        const float prev_ret = returns[i - 1];
        const bool has_flip = (i > 1) &&
            (trend_sign(ret) != 0.0f) &&
            (trend_sign(prev_ret) != 0.0f) &&
            (trend_sign(ret) != trend_sign(prev_ret));
        flip_prefix[i + 1] = flip_prefix[i] + (has_flip ? 1 : 0);

        const double overlap_low = std::max(bar.low, bars[i - 1].low);
        const double overlap_high = std::min(bar.high, bars[i - 1].high);
        const double union_low = std::min(bar.low, bars[i - 1].low);
        const double union_high = std::max(bar.high, bars[i - 1].high);
        const double overlap = std::max(0.0, overlap_high - overlap_low);
        const double union_range = std::max(1e-9, union_high - union_low);
        overlap_prefix[i + 1] = overlap_prefix[i] + static_cast<float>(overlap / union_range);
    }

    intelligence.states.resize(n);
    std::vector<float> smoothed_entropy(n, 0.0f);

    for (size_t bar_idx = 0; bar_idx < n; ++bar_idx) {
        const BarFeatures& feature = feature_tape.at(bar_idx);
        IntelligenceState& state = intelligence.states[bar_idx];

        state.features = feature;
        state.regime = static_cast<Regime>(feature.regime);

        const float momentum_component = clamp11(
            feature.momentum_short * 80.0f + feature.momentum_medium * 35.0f
        );
        const float structure_component = clamp11(feature.distance_to_low + feature.distance_to_high);

        if (!ech_config.enabled) {
            state.directional_bias = clamp11(
                feature.trend_alignment * 0.45f +
                feature.htf_bias * 0.25f +
                momentum_component * 0.20f +
                structure_component * 0.10f
            );

            state.stability_score = compute_stability_score(feature, state.regime);
            state.temporal_stability = state.stability_score;
            state.cross_timeframe_coherence = clamp01(
                0.5f + 0.5f * clamp11(feature.htf_trend_alignment * 0.70f + feature.htf_bias * 0.30f)
            );
            state.bias_strength = clamp01(
                std::fabs(state.directional_bias) * (0.55f + 0.45f * state.stability_score)
            );
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
            state.total_entropy = 0.0f;
            state.entropy_collapse = 0.0f;
            state.directional_energy = 0.0f;
            state.failure_memory = 0.0f;
            state.path_coherence = state.stability_score;
            state.long_pressure = clamp01(0.5f + state.directional_bias * 0.30f);
            state.short_pressure = clamp01(0.5f - state.directional_bias * 0.30f);
            state.entry_aggression = clamp01(0.35f + state.stability_score * 0.25f);
            state.hold_time_multiplier = 1.0f;
            state.exit_urgency = clamp01((1.0f - state.stability_score) * 0.45f);
            continue;
        }

        const size_t entropy_window_start = (bar_idx + 1 > static_cast<size_t>(ech_config.entropy_window))
            ? (bar_idx + 1 - static_cast<size_t>(ech_config.entropy_window))
            : 0;
        const int sample_count = static_cast<int>(bar_idx - entropy_window_start + 1);
        const int up_count = up_prefix[bar_idx + 1] - up_prefix[entropy_window_start];
        const float directional_entropy = (sample_count > 0)
            ? binary_entropy(static_cast<float>(up_count) / static_cast<float>(sample_count))
            : 0.0f;

        std::array<int, 4> bucket_counts{};
        for (size_t b = 0; b < bucket_prefix.size(); ++b) {
            bucket_counts[b] = bucket_prefix[b][bar_idx + 1] - bucket_prefix[b][entropy_window_start];
        }
        const float volatility_entropy = bucket_entropy(bucket_counts, sample_count);

        const size_t overlap_start = std::max<size_t>(1, entropy_window_start + 1);
        const int overlap_count = static_cast<int>((bar_idx + 1 > overlap_start) ? (bar_idx - overlap_start + 1) : 0);
        const int flip_count = flip_prefix[bar_idx + 1] - flip_prefix[overlap_start];
        const float overlap_sum = overlap_prefix[bar_idx + 1] - overlap_prefix[overlap_start];
        const float flip_rate = (overlap_count > 0)
            ? static_cast<float>(flip_count) / static_cast<float>(overlap_count)
            : 0.5f;
        const float overlap_mean = (overlap_count > 0)
            ? overlap_sum / static_cast<float>(overlap_count)
            : 0.5f;
        const float structural_entropy = clamp01(flip_rate * 0.65f + (1.0f - overlap_mean) * 0.35f);

        state.total_entropy =
            directional_entropy * ech_config.directional_weight +
            volatility_entropy * ech_config.volatility_weight +
            structural_entropy * ech_config.structural_weight;

        smoothed_entropy[bar_idx] = (bar_idx == 0)
            ? state.total_entropy
            : (smoothed_entropy[bar_idx - 1] * ech_config.smoothing +
               state.total_entropy * (1.0f - ech_config.smoothing));

        const size_t derivative_idx = (bar_idx >= static_cast<size_t>(ech_config.derivative_window))
            ? (bar_idx - static_cast<size_t>(ech_config.derivative_window))
            : 0;
        const float derivative_ref = smoothed_entropy[derivative_idx];
        const float ech_raw = derivative_ref - smoothed_entropy[bar_idx];
        state.entropy_collapse = clamp11(std::tanh(ech_raw * ech_config.collapse_scale));

        float failure_event = 0.0f;
        if (bar_idx > 0) {
            const float prev_impulse = impulse_seed[bar_idx - 1];
            const float prev_ret = returns[bar_idx - 1];
            const float curr_ret = returns[bar_idx];
            const bool strong_prev_impulse = std::fabs(prev_impulse) > 0.40f;
            const bool reversed = trend_sign(curr_ret) != 0.0f &&
                trend_sign(prev_impulse) != 0.0f &&
                trend_sign(curr_ret) == -trend_sign(prev_impulse);
            const float reversal_ratio = std::fabs(curr_ret) / std::max(std::fabs(prev_ret), 1e-6f);
            if (strong_prev_impulse && reversed && reversal_ratio > ech_config.reversal_trigger) {
                failure_event += 0.70f;
            }
            if (feature_tape.at(bar_idx - 1).compression_score > 0.55f &&
                feature_tape.at(bar_idx - 1).bar_range_zscore > 0.80f &&
                reversed) {
                failure_event += 0.30f;
            }
        }

        const float prev_failure_memory = (bar_idx > 0) ? intelligence.states[bar_idx - 1].failure_memory : 0.0f;
        state.failure_memory = clamp01(
            prev_failure_memory * ech_config.failure_decay +
            failure_event * (1.0f - ech_config.failure_decay + 0.20f)
        );

        state.cross_timeframe_coherence = clamp01(
            0.5f + 0.5f * clamp11(feature.htf_trend_alignment * 0.65f + feature.htf_bias * 0.35f)
        );

        const float base_stability = compute_stability_score(feature, state.regime);
        state.path_coherence = clamp01(
            (1.0f - structural_entropy) * 0.45f +
            base_stability * 0.30f +
            state.cross_timeframe_coherence * 0.15f +
            (1.0f - state.failure_memory) * 0.10f
        );
        state.temporal_stability = clamp01(
            base_stability * 0.40f +
            state.path_coherence * 0.25f +
            (1.0f - state.total_entropy) * 0.20f +
            state.cross_timeframe_coherence * 0.15f
        );
        state.stability_score = state.temporal_stability;

        const float energy_seed = impulse_seed[bar_idx] * (
            0.55f + 0.25f * std::max(0.0f, state.entropy_collapse) + 0.20f * state.path_coherence
        );
        const float prev_energy = (bar_idx > 0) ? intelligence.states[bar_idx - 1].directional_energy : 0.0f;
        state.directional_energy = clamp11(
            prev_energy * ech_config.energy_decay +
            energy_seed * (1.0f - ech_config.energy_decay + 0.18f)
        );

        state.directional_bias = clamp11(
            feature.trend_alignment * 0.35f +
            feature.htf_bias * 0.20f +
            momentum_component * 0.18f +
            structure_component * 0.10f +
            state.directional_energy * 0.17f +
            state.entropy_collapse * 0.08f * trend_sign(impulse_seed[bar_idx]) -
            state.failure_memory * 0.08f * trend_sign(impulse_seed[bar_idx])
        );

        state.bias_strength = clamp01(
            std::fabs(state.directional_bias) *
            (0.45f + 0.25f * state.temporal_stability +
             0.15f * std::max(0.0f, state.entropy_collapse) +
             0.15f * state.cross_timeframe_coherence)
        );
        state.volatility_state = clamp11(
            (feature.volatility_percentile - 0.5f) * 1.6f +
            (feature.volatility_ratio - 1.0f) * 0.6f
        );

        state.long_pressure = clamp01(
            0.50f +
            state.directional_bias * 0.28f +
            state.directional_energy * 0.18f +
            std::max(0.0f, state.entropy_collapse) * 0.14f +
            (state.cross_timeframe_coherence - 0.5f) * 0.16f -
            state.failure_memory * 0.14f -
            std::max(0.0f, -state.entropy_collapse) * 0.08f
        );
        state.short_pressure = clamp01(
            0.50f -
            state.directional_bias * 0.28f -
            state.directional_energy * 0.18f +
            std::max(0.0f, -state.entropy_collapse) * 0.14f +
            ((1.0f - state.cross_timeframe_coherence) - 0.5f) * 0.16f -
            state.failure_memory * 0.14f -
            std::max(0.0f, state.entropy_collapse) * 0.08f
        );

        state.entry_aggression = clamp01(
            0.30f +
            std::max(state.long_pressure, state.short_pressure) * 0.25f +
            std::max(0.0f, state.entropy_collapse) * 0.20f +
            state.temporal_stability * 0.15f +
            state.cross_timeframe_coherence * 0.10f -
            state.failure_memory * 0.20f
        );
        state.hold_time_multiplier = std::clamp(
            0.65f +
            state.temporal_stability * 0.35f +
            std::max(0.0f, state.entropy_collapse) * 0.20f +
            std::fabs(state.directional_energy) * 0.15f -
            state.failure_memory * 0.25f -
            std::max(0.0f, -state.entropy_collapse) * 0.15f,
            0.45f,
            1.60f
        );
        state.exit_urgency = clamp01(
            (1.0f - state.temporal_stability) * 0.45f +
            state.failure_memory * 0.25f +
            std::max(0.0f, -state.entropy_collapse) * 0.20f +
            clamp01(std::fabs(state.volatility_state)) * 0.10f +
            (1.0f - state.path_coherence) * 0.10f
        );

        float validity =
            state.temporal_stability * 0.45f +
            state.cross_timeframe_coherence * 0.20f +
            (1.0f - state.failure_memory) * 0.20f +
            (1.0f - structural_entropy) * 0.15f;
        if (state.regime == Regime::TRANSITION) {
            validity *= 0.65f;
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
              << " bars using " << num_contexts << " context timeframe(s)"
              << " | ECH=" << (ech_config.enabled ? "on" : "off")
              << std::endl;

    return intelligence;
}

} // namespace aphelion
