#pragma once
// ============================================================
// Aphelion Research - Robustness Scoring / Rejection Rules
// ============================================================

#include "aphelion/validation_engine.h"

#include <vector>
#include <string>

namespace aphelion {

struct RobustnessScoreBreakdown {
    double return_quality = 0.0;
    double drawdown_score = 0.0;
    double stability_score = 0.0;
    double robustness_component = 0.0;
    double regime_consistency = 0.0;
    double monte_carlo_resilience = 0.0;
    double timeframe_consistency = 0.0;
    double overfit_penalty = 0.0;
    double final_score = 0.0;
};

RobustnessScoreBreakdown score_validation_report(
    const ValidationReport& report,
    const ValidationConfig& config
);

std::vector<std::string> evaluate_rejection_rules(
    const ValidationReport& report,
    const ValidationConfig& config,
    const RobustnessScoreBreakdown& breakdown
);

std::vector<std::string> derive_failure_modes(
    const ValidationReport& report
);

} // namespace aphelion
