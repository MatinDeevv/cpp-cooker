#pragma once
// ============================================================
// Aphelion Research — Tournament Orchestration (Layer G)
// Runs N simulations over one replay stream, ranks, outputs
//
// V3: Feature/regime computation, composite scoring,
//     context-aware strategy support, dynamic risk
// ============================================================

#include "aphelion/types.h"
#include "aphelion/account.h"
#include "aphelion/strategy.h"
#include "aphelion/data_ingest.h"
#include "aphelion/replay_engine.h"
#include "aphelion/intelligence.h"
#include "aphelion/risk_manager.h"
#include "aphelion/validation_engine.h"
#include <vector>
#include <memory>
#include <string>

namespace aphelion {

struct TournamentEntry {
    Account                    account;
    std::unique_ptr<IStrategy> strategy;
    SimulationParams           params;
};

struct LeaderboardRow {
    uint32_t account_id;
    double   final_balance;
    double   final_equity;
    double   total_return;
    double   max_drawdown;
    double   win_rate;
    double   profit_factor;
    double   expectancy;
    int      trade_count;
    bool     liquidated;
    const char* strategy_name;
    int      fast_period;
    int      slow_period;
    // V3: Composite quality metrics
    double   composite_score;     // weighted quality metric
    double   risk_adjusted_return; // return / max_drawdown
    double   consistency_score;    // 0-1: how stable the equity curve
    double   robust_score = 0.0;
    double   stability_score = 0.0;
    double   monte_carlo_resilience = 0.0;
    double   regime_consistency = 0.0;
    double   overfit_penalty = 0.0;
    bool     validation_passed = false;
    bool     heavy_validation_run = false;
    std::string rejection_reason;
};

struct TournamentConfig {
    int          num_accounts     = 100;
    double       initial_balance  = 10000.0;
    double       max_leverage     = 500.0;
    double       stop_out_level   = 50.0;
    double       risk_per_trade   = 0.01;
    double       commission       = 0.0;
    double       slippage         = 0.0;
    int          max_positions    = 1;
    RunMode      mode             = RunMode::FULL;
    int          strategy_id      = 0;  // V3: 0=SMA, 1=ContextSMA
    bool         live_safe_mode   = true;
    bool         emergency_flatten = false;
    double       live_reduced_risk_scale = 0.50;
    double       live_max_leverage_cap   = 25.0;
    double       max_position_notional   = 5000.0;
    double       max_total_notional      = 10000.0;
    int          session_trade_limit     = 48;
    double       session_drawdown_kill   = 0.03;
    double       session_loss_kill       = 0.02;
    // Strategy parameter ranges
    int          fast_period_min  = 5;
    int          fast_period_max  = 50;
    int          slow_period_min  = 20;
    int          slow_period_max  = 200;
    // V3: Feature/regime/risk configs
    FeatureConfig feature_config;
    RegimeConfig  regime_config;
    EchConfig     ech_config;
    RiskConfig    risk_config;
    ValidationConfig validation_config;
    std::vector<MultiTimeframeInput> context_inputs;
    std::vector<const BarTape*> validation_tapes;
};

class Tournament {
public:
    Tournament(const TournamentConfig& config, const BarTape& tape);

    void initialize();
    void run();
    std::vector<LeaderboardRow> leaderboard() const;

    const std::vector<TournamentEntry>& entries() const { return entries_; }
    const BarTape& tape() const { return tape_; }
    const TournamentConfig& config() const { return config_; }
    const ReplayStats& last_stats() const { return last_stats_; }
    const IntelligenceTape& intelligence_tape() const { return intelligence_tape_; }
    const ValidationSummary& validation_summary() const { return validation_summary_; }

private:
    TournamentConfig             config_;
    const BarTape&               tape_;
    std::vector<TournamentEntry> entries_;
    ReplayStats                  last_stats_;
    IntelligenceTape             intelligence_tape_;
    ValidationSummary            validation_summary_;

    // V3: Compute composite quality score for ranking
    static double compute_composite_score(const LeaderboardRow& row);
    static double compute_consistency(const Account& account, size_t tape_size);
};

} // namespace aphelion
