#pragma once
// ============================================================
// Aphelion Research — Reporting / Export (Layer H)
// Summary, trade logs, equity curves, metadata
// All I/O happens AFTER replay completes
// ============================================================

#include "aphelion/types.h"
#include "aphelion/tournament.h"
#include "aphelion/replay_engine.h"
#include <filesystem>
#include <string>

namespace aphelion {

struct RunMetadata {
    std::string symbol;
    std::string timeframe;
    double      leverage_cap;
    double      commission;
    double      slippage;
    double      risk_per_trade;
    double      stop_out_level;
    int         num_accounts;
    int         strategy_id;
    int         fast_period_min;
    int         fast_period_max;
    int         slow_period_min;
    int         slow_period_max;
    size_t      total_bars;
    double      elapsed_seconds;
};

void write_summary_csv(
    const std::filesystem::path& output_dir,
    const std::vector<LeaderboardRow>& leaderboard
);

void write_trade_log(
    const std::filesystem::path& output_dir,
    uint32_t account_id,
    const std::vector<TradeRecord>& trades
);

void write_equity_curve(
    const std::filesystem::path& output_dir,
    uint32_t account_id,
    const std::vector<float>& equity_curve
);

void write_run_metadata(
    const std::filesystem::path& output_dir,
    const RunMetadata& meta
);

// Write all outputs for a tournament.
void write_all_outputs(
    const std::filesystem::path& output_dir,
    const Tournament& tournament,
    const ReplayStats& stats,
    const RunMetadata& meta
);

} // namespace aphelion
