#pragma once
// ============================================================
// Aphelion Research — Tournament Orchestration (Layer G)
// Runs N simulations over one replay stream, ranks, outputs
// ============================================================

#include "aphelion/types.h"
#include "aphelion/account.h"
#include "aphelion/strategy.h"
#include "aphelion/data_ingest.h"
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
    // Strategy parameter ranges for variation
    int          fast_period_min  = 5;
    int          fast_period_max  = 50;
    int          slow_period_min  = 20;
    int          slow_period_max  = 200;
};

class Tournament {
public:
    Tournament(const TournamentConfig& config, const BarTape& tape);

    // Initialize all accounts with varied parameters.
    void initialize();

    // Run the full replay.
    void run();

    // Compute leaderboard after replay.
    std::vector<LeaderboardRow> leaderboard() const;

    // Access entries for reporting.
    const std::vector<TournamentEntry>& entries() const { return entries_; }
    const BarTape& tape() const { return tape_; }
    const TournamentConfig& config() const { return config_; }

private:
    TournamentConfig             config_;
    const BarTape&               tape_;
    std::vector<TournamentEntry> entries_;
};

} // namespace aphelion
