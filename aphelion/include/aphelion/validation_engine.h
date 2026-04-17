#pragma once
// ============================================================
// Aphelion Research - Robustness / Validation Engine
// Sequential holdout, walk-forward, stress, and regime validation
// ============================================================

#include "aphelion/account.h"
#include "aphelion/data_ingest.h"
#include "aphelion/intelligence.h"
#include "aphelion/risk_manager.h"
#include "aphelion/types.h"

#include <string>
#include <vector>

namespace aphelion {

struct ValidationConfig {
    bool   enabled                     = true;
    int    quick_min_trades            = 24;
    double quick_min_return_pct        = 0.0;
    double quick_max_drawdown_pct      = 35.0;
    int    heavy_validation_limit      = 24;
    int    holdout_train_percent       = 60;
    int    holdout_validation_percent  = 20;
    int    walkforward_segments        = 6;
    int    walkforward_train_segments  = 2;
    int    min_positive_test_segments  = 2;
    int    min_regime_trades           = 8;
    int    monte_carlo_iterations      = 128;
    int    bootstrap_iterations        = 128;
    double stress_price_noise_sigma_bps = 2.5;
    double stress_slippage_multiplier  = 1.75;
    double stress_slippage_floor_points = 2.0;
    int    parameter_jitter_fast_step  = 2;
    int    parameter_jitter_slow_step  = 10;
    double robust_pass_threshold       = 0.58;
    double acceptable_test_return_pct  = 0.0;
    double acceptable_timeframe_return_pct = -2.0;
    double max_stress_drawdown_pct     = 40.0;
    double rare_regime_bar_share       = 0.15;
    double rare_regime_profit_share    = 0.70;
};

struct ValidationCandidate {
    uint32_t      account_id = 0;
    std::string   strategy_name;
    SimulationParams params;
    const Account* base_account = nullptr;
};

struct ValidationExecutionConfig {
    FeatureConfig feature_config;
    RegimeConfig  regime_config;
    EchConfig     ech_config;
    RiskConfig    risk_config;
};

struct CandidateMetrics {
    double final_equity      = 0.0;
    double total_return_pct  = 0.0;
    double max_drawdown_pct  = 0.0;
    double profit_factor     = 0.0;
    double expectancy        = 0.0;
    double win_rate_pct      = 0.0;
    double avg_win           = 0.0;
    double avg_loss          = 0.0;
    double pnl_stddev        = 0.0;
    double return_volatility = 0.0;
    double tail_loss_pct     = 0.0;
    int    trade_count       = 0;
    bool   liquidated        = false;
};

struct HoldoutValidation {
    CandidateMetrics train;
    CandidateMetrics validation;
    CandidateMetrics test;
};

struct WalkForwardWindow {
    int    index = 0;
    size_t train_begin = 0;
    size_t train_end = 0;
    size_t test_begin = 0;
    size_t test_end = 0;
    CandidateMetrics train;
    CandidateMetrics test;
    double generalization_ratio = 0.0;
};

struct StressScenarioResult {
    std::string     name;
    CandidateMetrics metrics;
    bool            passed = false;
};

struct RegimeBreakdown {
    Regime regime = Regime::UNKNOWN;
    int    trades = 0;
    double bar_share = 0.0;
    double profit_share = 0.0;
    double net_pnl = 0.0;
    double expectancy = 0.0;
    double win_rate_pct = 0.0;
    double profit_factor = 0.0;
    bool   failing = false;
};

struct ConditionBreakdown {
    std::string name;
    int         trades = 0;
    double      net_pnl = 0.0;
    double      expectancy = 0.0;
    double      win_rate_pct = 0.0;
};

struct ValidationReport {
    uint32_t account_id = 0;
    std::string strategy_name;
    int fast_period = 0;
    int slow_period = 0;

    CandidateMetrics base_metrics;
    HoldoutValidation holdout;
    std::vector<WalkForwardWindow> walkforward;
    std::vector<StressScenarioResult> stress_tests;
    std::vector<StressScenarioResult> timeframe_tests;
    std::vector<RegimeBreakdown> regime_breakdown;
    std::vector<ConditionBreakdown> condition_breakdown;

    double robust_score = 0.0;
    double return_quality = 0.0;
    double drawdown_score = 0.0;
    double stability_score = 0.0;
    double robustness_component = 0.0;
    double regime_consistency = 0.0;
    double monte_carlo_resilience = 0.0;
    double timeframe_consistency = 0.0;
    double overfit_penalty = 0.0;
    double degradation_slope = 0.0;
    double parameter_sensitivity = 0.0;

    bool quick_filter_passed = false;
    bool heavy_validation_run = false;
    bool passed = false;

    std::vector<std::string> rejection_reasons;
    std::vector<std::string> failure_modes;
};

struct ValidationSummary {
    std::vector<ValidationReport> reports;
    std::vector<uint32_t> passing_account_ids;
    size_t heavy_validated = 0;
};

ValidationSummary run_validation_suite(
    const BarTape& primary,
    const std::vector<const BarTape*>& validation_tapes,
    const IntelligenceTape& intelligence_tape,
    const std::vector<ValidationCandidate>& candidates,
    const ValidationExecutionConfig& execution_config,
    const ValidationConfig& validation_config = ValidationConfig{}
);

} // namespace aphelion
