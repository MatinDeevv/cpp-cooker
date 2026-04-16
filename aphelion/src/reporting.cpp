// ============================================================
// Aphelion Research — Reporting / Export (Layer H)
// All I/O after replay.  No I/O during replay.
// ============================================================

#include "aphelion/reporting.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <ctime>

namespace aphelion {

static std::string timestamp_to_iso(Timestamp ms) {
    int64_t secs = ms / 1000;
    std::time_t t = static_cast<std::time_t>(secs);
    struct tm utc;
#ifdef _WIN32
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string(buf);
}

static std::string exit_reason_str(ExitReason r) {
    switch (r) {
        case ExitReason::STOP_LOSS:   return "stop_loss";
        case ExitReason::TAKE_PROFIT: return "take_profit";
        case ExitReason::STRATEGY:    return "strategy";
        case ExitReason::LIQUIDATION: return "liquidation";
        case ExitReason::END_OF_DATA: return "end_of_data";
        default:                      return "none";
    }
}

static std::string direction_str(Direction d) {
    return d == Direction::LONG ? "LONG" : "SHORT";
}

void write_summary_csv(
    const std::filesystem::path& output_dir,
    const std::vector<LeaderboardRow>& leaderboard
) {
    auto path = output_dir / "summary.csv";
    std::ofstream f(path);
    if (!f) {
        std::cerr << "[report] Failed to write " << path << std::endl;
        return;
    }

    f << "account_id,final_balance,final_equity,total_return,max_drawdown,"
         "win_rate,profit_factor,expectancy,trade_count,liquidated,strategy\n";

    f << std::fixed << std::setprecision(2);
    for (const auto& r : leaderboard) {
        f << r.account_id << ","
          << r.final_balance << ","
          << r.final_equity << ","
          << r.total_return << ","
          << r.max_drawdown << ","
          << r.win_rate << ","
          << r.profit_factor << ","
          << r.expectancy << ","
          << r.trade_count << ","
          << (r.liquidated ? "true" : "false") << ","
          << r.strategy_name << "\n";
    }

    std::cout << "[report] Wrote " << path << " (" << leaderboard.size() << " rows)" << std::endl;
}

void write_trade_log(
    const std::filesystem::path& output_dir,
    uint32_t account_id,
    const std::vector<TradeRecord>& trades
) {
    std::ostringstream fname;
    fname << "trades_account_" << account_id << ".csv";
    auto path = output_dir / fname.str();

    std::ofstream f(path);
    if (!f) return;

    f << "direction,entry_bar_index,exit_bar_index,entry_time,exit_time,"
         "entry_price,exit_price,quantity,gross_pnl,net_pnl,exit_reason\n";

    f << std::fixed << std::setprecision(5);
    for (const auto& t : trades) {
        f << direction_str(t.direction) << ","
          << t.entry_bar_idx << ","
          << t.exit_bar_idx << ","
          << timestamp_to_iso(t.entry_time_ms) << ","
          << timestamp_to_iso(t.exit_time_ms) << ","
          << t.entry_price << ","
          << t.exit_price << ","
          << t.quantity << ","
          << t.gross_pnl << ","
          << t.net_pnl << ","
          << exit_reason_str(t.exit_reason) << "\n";
    }
}

void write_equity_curve(
    const std::filesystem::path& output_dir,
    uint32_t account_id,
    const std::vector<float>& equity_curve
) {
    std::ostringstream fname;
    fname << "equity_account_" << account_id << ".csv";
    auto path = output_dir / fname.str();

    std::ofstream f(path);
    if (!f) return;

    f << "step,equity\n";
    f << std::fixed << std::setprecision(2);
    for (size_t i = 0; i < equity_curve.size(); ++i) {
        f << i << "," << equity_curve[i] << "\n";
    }
}

void write_run_metadata(
    const std::filesystem::path& output_dir,
    const RunMetadata& meta
) {
    auto path = output_dir / "run_metadata.json";
    std::ofstream f(path);
    if (!f) return;

    f << "{\n"
      << "  \"symbol\": \"" << meta.symbol << "\",\n"
      << "  \"timeframe\": \"" << meta.timeframe << "\",\n"
      << "  \"leverage_cap\": " << meta.leverage_cap << ",\n"
      << "  \"commission\": " << meta.commission << ",\n"
      << "  \"slippage\": " << meta.slippage << ",\n"
      << "  \"risk_per_trade\": " << meta.risk_per_trade << ",\n"
      << "  \"stop_out_level\": " << meta.stop_out_level << ",\n"
      << "  \"num_accounts\": " << meta.num_accounts << ",\n"
      << "  \"strategy_id\": " << meta.strategy_id << ",\n"
      << "  \"fast_period_range\": [" << meta.fast_period_min << ", " << meta.fast_period_max << "],\n"
      << "  \"slow_period_range\": [" << meta.slow_period_min << ", " << meta.slow_period_max << "],\n"
      << "  \"total_bars\": " << meta.total_bars << ",\n"
      << "  \"elapsed_seconds\": " << std::fixed << std::setprecision(3) << meta.elapsed_seconds << ",\n"
      << "  \"engine\": \"Aphelion Research v1.0.0\",\n"
      << "  \"physical_reality_note\": \"This is a historical simulation engine. "
         "True nanosecond execution is not claimed. Architecture uses nanosecond-level "
         "engineering discipline for maximum research throughput.\"\n"
      << "}\n";

    std::cout << "[report] Wrote " << path << std::endl;
}

void write_all_outputs(
    const std::filesystem::path& output_dir,
    const Tournament& tournament,
    const ReplayStats& stats,
    const RunMetadata& meta
) {
    namespace fs = std::filesystem;
    fs::create_directories(output_dir);

    auto lb = tournament.leaderboard();

    // Summary
    write_summary_csv(output_dir, lb);

    // Metadata
    write_run_metadata(output_dir, meta);

    // Per-account outputs: trade logs and equity curves
    // Write top 10 + bottom 10 + all liquidated accounts
    // (for large tournaments, writing ALL equity curves would be O(bars * accounts) disk)
    size_t n = tournament.entries().size();

    std::vector<size_t> detail_indices;

    // Top accounts by rank
    for (size_t i = 0; i < std::min(n, static_cast<size_t>(10)); ++i) {
        detail_indices.push_back(lb[i].account_id);
    }
    // Bottom accounts by rank
    for (size_t i = (n > 10 ? n - 10 : 0); i < n; ++i) {
        if (lb[i].account_id < tournament.entries().size())
            detail_indices.push_back(lb[i].account_id);
    }
    // Liquidated accounts
    for (const auto& row : lb) {
        if (row.liquidated) detail_indices.push_back(row.account_id);
    }

    // Deduplicate
    std::sort(detail_indices.begin(), detail_indices.end());
    detail_indices.erase(std::unique(detail_indices.begin(), detail_indices.end()), detail_indices.end());

    auto trades_dir = output_dir / "trades";
    auto equity_dir = output_dir / "equity";
    fs::create_directories(trades_dir);
    fs::create_directories(equity_dir);

    for (size_t aid : detail_indices) {
        if (aid >= tournament.entries().size()) continue;
        const auto& entry = tournament.entries()[aid];
        write_trade_log(trades_dir, static_cast<uint32_t>(aid), entry.account.trade_log);
        write_equity_curve(equity_dir, static_cast<uint32_t>(aid), entry.account.equity_curve);
    }

    // Print leaderboard summary to console
    std::cout << "\n==================== LEADERBOARD (top 20) ====================\n";
    std::cout << std::left
              << std::setw(6)  << "Rank"
              << std::setw(8)  << "AcctID"
              << std::setw(14) << "Equity"
              << std::setw(12) << "Return%"
              << std::setw(12) << "MaxDD%"
              << std::setw(10) << "WinRate"
              << std::setw(10) << "PF"
              << std::setw(10) << "Trades"
              << std::setw(10) << "Liq"
              << "\n";

    size_t show = std::min(lb.size(), static_cast<size_t>(20));
    for (size_t i = 0; i < show; ++i) {
        const auto& r = lb[i];
        std::cout << std::left
                  << std::setw(6)  << (i + 1)
                  << std::setw(8)  << r.account_id
                  << std::setw(14) << std::fixed << std::setprecision(2) << r.final_equity
                  << std::setw(12) << std::fixed << std::setprecision(2) << r.total_return
                  << std::setw(12) << std::fixed << std::setprecision(2) << r.max_drawdown
                  << std::setw(10) << std::fixed << std::setprecision(1) << r.win_rate
                  << std::setw(10) << std::fixed << std::setprecision(2) << r.profit_factor
                  << std::setw(10) << r.trade_count
                  << std::setw(10) << (r.liquidated ? "YES" : "no")
                  << "\n";
    }
    std::cout << "================================================================\n\n";
}

} // namespace aphelion
