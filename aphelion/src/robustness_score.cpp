// ============================================================
// Aphelion Research - Robustness Scoring / Rejection Rules
// ============================================================

#include "aphelion/robustness_score.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace aphelion {

namespace {

double clamp01(double value) {
    return std::max(0.0, std::min(1.0, value));
}

const char* regime_name(Regime regime) {
    switch (regime) {
        case Regime::TRENDING_UP: return "TRENDING_UP";
        case Regime::TRENDING_DOWN: return "TRENDING_DOWN";
        case Regime::RANGE_BOUND: return "RANGE_BOUND";
        case Regime::VOLATILE_EXPANSION: return "VOLATILE_EXPANSION";
        case Regime::COMPRESSION: return "COMPRESSION";
        case Regime::TRANSITION: return "TRANSITION";
        case Regime::UNKNOWN:
        default: return "UNKNOWN";
    }
}

double walkforward_positive_ratio(const ValidationReport& report) {
    if (report.walkforward.empty()) return 0.0;
    int positive = 0;
    for (const auto& window : report.walkforward) {
        if (window.test.total_return_pct > 0.0) positive++;
    }
    return static_cast<double>(positive) / static_cast<double>(report.walkforward.size());
}

double walkforward_return_stability(const ValidationReport& report) {
    if (report.walkforward.size() < 2) return 0.5;

    double mean = 0.0;
    for (const auto& window : report.walkforward) {
        mean += window.test.total_return_pct;
    }
    mean /= static_cast<double>(report.walkforward.size());

    double var = 0.0;
    for (const auto& window : report.walkforward) {
        const double diff = window.test.total_return_pct - mean;
        var += diff * diff;
    }
    var /= static_cast<double>(report.walkforward.size() - 1);
    const double stdev = std::sqrt(std::max(0.0, var));
    return clamp01(1.0 - stdev / std::max(8.0, std::fabs(mean) + 4.0));
}

double rare_regime_dependency_penalty(
    const ValidationReport& report,
    const ValidationConfig& config
) {
    for (const auto& regime : report.regime_breakdown) {
        if (regime.bar_share < config.rare_regime_bar_share &&
            regime.profit_share > config.rare_regime_profit_share) {
            return 0.35;
        }
    }
    return 0.0;
}

double worst_stress_return(const ValidationReport& report) {
    double worst = report.base_metrics.total_return_pct;
    for (const auto& stress : report.stress_tests) {
        worst = std::min(worst, stress.metrics.total_return_pct);
    }
    for (const auto& stress : report.timeframe_tests) {
        worst = std::min(worst, stress.metrics.total_return_pct);
    }
    return worst;
}

double worst_stress_drawdown(const ValidationReport& report) {
    double worst = report.base_metrics.max_drawdown_pct;
    for (const auto& stress : report.stress_tests) {
        worst = std::max(worst, stress.metrics.max_drawdown_pct);
    }
    for (const auto& stress : report.timeframe_tests) {
        worst = std::max(worst, stress.metrics.max_drawdown_pct);
    }
    return worst;
}

} // namespace

RobustnessScoreBreakdown score_validation_report(
    const ValidationReport& report,
    const ValidationConfig& config
) {
    RobustnessScoreBreakdown breakdown{};

    const double sample_score = clamp01(
        static_cast<double>(report.base_metrics.trade_count - config.quick_min_trades) / 80.0
    );
    const double base_return_score = clamp01((report.base_metrics.total_return_pct + 5.0) / 25.0);
    const double holdout_test_score = clamp01((report.holdout.test.total_return_pct + 3.0) / 15.0);
    const double pf_score = clamp01((report.base_metrics.profit_factor - 0.9) / 1.2);
    const double expectancy_score = clamp01(
        report.base_metrics.expectancy / std::max(25.0, std::fabs(report.base_metrics.avg_loss) + 5.0)
    );
    breakdown.return_quality = clamp01(
        base_return_score * 0.28 +
        holdout_test_score * 0.28 +
        pf_score * 0.18 +
        expectancy_score * 0.12 +
        sample_score * 0.14
    );

    const double worst_dd = worst_stress_drawdown(report);
    breakdown.drawdown_score = clamp01(1.0 - worst_dd / config.max_stress_drawdown_pct);

    const double wf_positive_ratio = walkforward_positive_ratio(report);
    const double wf_stability = walkforward_return_stability(report);
    const double generalization_score = report.walkforward.empty()
        ? 0.4
        : clamp01(std::accumulate(
            report.walkforward.begin(),
            report.walkforward.end(),
            0.0,
            [](double acc, const WalkForwardWindow& window) {
                return acc + clamp01((window.generalization_ratio + 0.25) / 1.25);
            }
        ) / static_cast<double>(report.walkforward.size()));
    const double degradation_score = clamp01(1.0 - std::max(0.0, -report.degradation_slope) / 8.0);
    breakdown.stability_score = clamp01(
        wf_positive_ratio * 0.35 +
        wf_stability * 0.25 +
        generalization_score * 0.20 +
        degradation_score * 0.20
    );

    breakdown.robustness_component = clamp01(
        clamp01((report.holdout.test.total_return_pct + 2.0) / 12.0) * 0.35 +
        clamp01((report.holdout.validation.total_return_pct + 2.0) / 12.0) * 0.20 +
        report.parameter_sensitivity * 0.45
    );

    int failing_regimes = 0;
    for (const auto& regime : report.regime_breakdown) {
        if (regime.failing) failing_regimes++;
    }
    const double failing_regime_penalty = report.regime_breakdown.empty()
        ? 0.25
        : static_cast<double>(failing_regimes) / static_cast<double>(report.regime_breakdown.size());
    breakdown.regime_consistency = clamp01(
        0.70 - failing_regime_penalty * 0.35 - rare_regime_dependency_penalty(report, config) +
        clamp01((report.base_metrics.trade_count - 20) / 60.0) * 0.15
    );

    if (report.stress_tests.empty()) {
        breakdown.monte_carlo_resilience = 0.45;
    } else {
        int passed = 0;
        for (const auto& stress : report.stress_tests) {
            if (stress.passed) passed++;
        }
        const double pass_ratio = static_cast<double>(passed) / static_cast<double>(report.stress_tests.size());
        const double worst_return_score = clamp01((worst_stress_return(report) + 6.0) / 16.0);
        const double drawdown_resilience = clamp01(1.0 - worst_stress_drawdown(report) / config.max_stress_drawdown_pct);
        breakdown.monte_carlo_resilience = clamp01(
            pass_ratio * 0.40 +
            worst_return_score * 0.30 +
            drawdown_resilience * 0.30
        );
    }

    if (report.timeframe_tests.empty()) {
        breakdown.timeframe_consistency = 0.50;
    } else {
        int passed = 0;
        for (const auto& test : report.timeframe_tests) {
            if (test.metrics.total_return_pct >= config.acceptable_timeframe_return_pct &&
                test.metrics.max_drawdown_pct <= config.max_stress_drawdown_pct) {
                passed++;
            }
        }
        const double pass_ratio = static_cast<double>(passed) / static_cast<double>(report.timeframe_tests.size());
        double mean_return = 0.0;
        for (const auto& test : report.timeframe_tests) {
            mean_return += test.metrics.total_return_pct;
        }
        mean_return /= static_cast<double>(report.timeframe_tests.size());
        breakdown.timeframe_consistency = clamp01(
            pass_ratio * 0.65 +
            clamp01((mean_return + 4.0) / 14.0) * 0.35
        );
    }

    breakdown.overfit_penalty = 0.0;
    if (report.base_metrics.total_return_pct > 10.0 && report.base_metrics.trade_count < config.quick_min_trades + 6) {
        breakdown.overfit_penalty += 0.18;
    }
    if (report.base_metrics.profit_factor > 3.25 && report.base_metrics.trade_count < 40) {
        breakdown.overfit_penalty += 0.18;
    }
    if (report.parameter_sensitivity < 0.45) {
        breakdown.overfit_penalty += 0.18;
    }
    if (wf_positive_ratio < 0.50) {
        breakdown.overfit_penalty += 0.14;
    }
    if (breakdown.timeframe_consistency < 0.40) {
        breakdown.overfit_penalty += 0.08;
    }
    breakdown.overfit_penalty += rare_regime_dependency_penalty(report, config);
    breakdown.overfit_penalty = clamp01(breakdown.overfit_penalty);

    breakdown.final_score = clamp01(
        breakdown.return_quality * 0.18 +
        breakdown.drawdown_score * 0.17 +
        breakdown.stability_score * 0.20 +
        breakdown.robustness_component * 0.16 +
        breakdown.regime_consistency * 0.11 +
        breakdown.monte_carlo_resilience * 0.11 +
        breakdown.timeframe_consistency * 0.07 -
        breakdown.overfit_penalty * 0.20
    );

    return breakdown;
}

std::vector<std::string> evaluate_rejection_rules(
    const ValidationReport& report,
    const ValidationConfig& config,
    const RobustnessScoreBreakdown& breakdown
) {
    std::vector<std::string> reasons;

    if (!report.quick_filter_passed) {
        reasons.push_back("failed cheap filter");
    }
    if (report.base_metrics.liquidated) {
        reasons.push_back("liquidated in base run");
    }
    if (report.base_metrics.trade_count < config.quick_min_trades) {
        reasons.push_back("insufficient trade count");
    }
    if (!report.heavy_validation_run) {
        if (report.base_metrics.total_return_pct < config.quick_min_return_pct) {
            reasons.push_back("base return below quick threshold");
        }
        if (report.base_metrics.max_drawdown_pct > config.quick_max_drawdown_pct) {
            reasons.push_back("base drawdown above quick threshold");
        }
        return reasons;
    }
    if (report.holdout.test.trade_count > 0 &&
        report.holdout.test.total_return_pct < config.acceptable_test_return_pct) {
        reasons.push_back("negative out-of-sample holdout return");
    }

    int positive_test_segments = 0;
    for (const auto& window : report.walkforward) {
        if (window.test.total_return_pct > 0.0) positive_test_segments++;
    }
    if (!report.walkforward.empty() &&
        positive_test_segments < config.min_positive_test_segments) {
        reasons.push_back("walk-forward consistency too weak");
    }
    if (report.parameter_sensitivity < 0.35) {
        reasons.push_back("parameter sensitivity too high");
    }
    if (breakdown.monte_carlo_resilience < 0.40) {
        reasons.push_back("stress resilience too weak");
    }
    if (!report.timeframe_tests.empty() &&
        breakdown.timeframe_consistency < 0.35) {
        reasons.push_back("fails adjacent timeframe transfer");
    }
    if (breakdown.overfit_penalty > 0.65) {
        reasons.push_back("explicit overfitting flags triggered");
    }
    for (const auto& regime : report.regime_breakdown) {
        if (regime.failing &&
            regime.trades >= config.min_regime_trades &&
            regime.net_pnl < 0.0) {
            reasons.push_back(std::string("uncontrolled failure regime: ") + regime_name(regime.regime));
            break;
        }
    }
    if (breakdown.final_score < config.robust_pass_threshold) {
        reasons.push_back("robust score below pass threshold");
    }

    return reasons;
}

std::vector<std::string> derive_failure_modes(
    const ValidationReport& report
) {
    std::vector<std::string> failure_modes;

    if (report.degradation_slope < -1e-6) {
        failure_modes.push_back("degrades across walk-forward windows");
    }

    const RegimeBreakdown* worst_regime = nullptr;
    for (const auto& regime : report.regime_breakdown) {
        if (regime.trades <= 0) continue;
        if (worst_regime == nullptr || regime.net_pnl < worst_regime->net_pnl) {
            worst_regime = &regime;
        }
    }
    if (worst_regime != nullptr && worst_regime->net_pnl < 0.0) {
        failure_modes.push_back(std::string("fails in ") + regime_name(worst_regime->regime));
    }

    const StressScenarioResult* worst_stress = nullptr;
    for (const auto& stress : report.stress_tests) {
        if (worst_stress == nullptr ||
            stress.metrics.total_return_pct < worst_stress->metrics.total_return_pct) {
            worst_stress = &stress;
        }
    }
    if (worst_stress != nullptr && !worst_stress->passed) {
        failure_modes.push_back(std::string("fragile under ") + worst_stress->name);
    }

    return failure_modes;
}

} // namespace aphelion
