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

struct IntelligenceState {
    BarFeatures features{};
    Regime regime = Regime::UNKNOWN;
    float directional_bias = 0.0f;
    float bias_strength = 0.0f;
    float stability_score = 0.0f;
    float volatility_state = 0.0f;
    float context_validity = 0.0f;
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
    const RegimeConfig& regime_config = RegimeConfig{}
);

} // namespace aphelion