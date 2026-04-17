#pragma once
// ============================================================
// Aphelion Research — Unified Intelligence Tape
// One precomputed brain shared by all accounts.
//
// Pipeline:
//   Market -> Features -> Regime -> Intelligence -> Strategy/Risk
//
// Features remain deterministic transforms.
// Regime remains global context.
// Intelligence collapses them into one reusable per-bar state.
// ============================================================

#include "aphelion/data_ingest.h"
#include "aphelion/features.h"
#include "aphelion/multi_timeframe.h"
#include "aphelion/regime.h"

#include <vector>

namespace aphelion {

struct MultiTimeframeInput {
    const BarTape* secondary = nullptr;
    const TimeframeAlignment* alignment = nullptr;
    float weight = 1.0f;
};

struct EchConfig {
    bool  enabled = true;
    int   entropy_window = 24;
    int   structural_window = 12;
    int   derivative_window = 3;
    float directional_weight = 0.42f;
    float volatility_weight = 0.33f;
    float structural_weight = 0.25f;
    float smoothing = 0.65f;
    float collapse_scale = 6.0f;
    float energy_decay = 0.88f;
    float failure_decay = 0.90f;
    float reversal_trigger = 1.10f;
};

struct IntelligenceState {
    BarFeatures features{};
    Regime regime = Regime::UNKNOWN;
    float directional_bias = 0.0f;
    float bias_strength = 0.0f;
    float stability_score = 0.0f;
    float volatility_state = 0.0f;
    float context_validity = 0.0f;
    float total_entropy = 0.0f;
    float entropy_collapse = 0.0f;
    float temporal_stability = 0.0f;
    float directional_energy = 0.0f;
    float failure_memory = 0.0f;
    float cross_timeframe_coherence = 0.5f;
    float path_coherence = 0.5f;
    float long_pressure = 0.5f;
    float short_pressure = 0.5f;
    float entry_aggression = 0.5f;
    float hold_time_multiplier = 1.0f;
    float exit_urgency = 0.0f;
};

struct IntelligenceTape {
    std::vector<IntelligenceState> states;

    const IntelligenceState& at(size_t idx) const { return states[idx]; }
    const IntelligenceState* data() const { return states.data(); }
    size_t size() const { return states.size(); }
    bool empty() const { return states.empty(); }
};

IntelligenceTape build_intelligence_tape(
    const BarTape& primary,
    const MultiTimeframeInput* contexts,
    size_t num_contexts,
    const FeatureConfig& feature_config = FeatureConfig{},
    const RegimeConfig& regime_config = RegimeConfig{},
    const EchConfig& ech_config = EchConfig{}
);

} // namespace aphelion
