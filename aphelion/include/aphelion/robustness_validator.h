#pragma once
// ============================================================
// Aphelion Research — Robustness Validator
//
// Anti-overfit defense layer. Tests candidate strategies against:
//   - Walk-forward windows (train/validate/test splits)
//   - Parameter sensitivity (small perturbations)
//   - Cross-regime consistency
//   - Complexity penalties
//   - Minimum sample requirements
// ============================================================

#include "aphelion/strategy_genome.h"
#include "aphelion/data_ingest.h"
#include "aphelion/intelligence.h"
#include <vector>

namespace aphelion {

struct RobustnessResult {
    double  oos_return          = 0.0;
    double  oos_drawdown        = 0.0;
    double  oos_profit_factor   = 0.0;
    double  oos_consistency     = 0.0;
    double  degradation_ratio   = 0.0;   // OOS / IS performance
    int     oos_trade_count     = 0;
    double  window_variance     = 0.0;   // variance across windows
    double  regime_stability    = 0.0;   // cross-regime consistency
    double  param_sensitivity   = 0.0;   // sensitivity to perturbation
    double  composite_robustness = 0.0;
    bool    passes              = false;
};

struct RobustnessConfig {
    int    num_windows              = 5;
    float  train_pct                = 0.6f;
    float  val_pct                  = 0.2f;
    float  test_pct                 = 0.2f;
    double min_degradation_ratio    = 0.35;
    double max_window_variance      = 0.60;
    double min_regime_stability     = 0.25;
    int    param_sensitivity_samples = 8;
    float  param_perturbation       = 0.10f;
    int    min_oos_trades           = 10;
};

RobustnessResult validate_robustness(
    const StrategyGenome& genome,
    const BarTape& tape,
    const IntelligenceState* intelligence,
    size_t num_bars,
    const RobustnessConfig& config = RobustnessConfig{}
);

} // namespace aphelion
