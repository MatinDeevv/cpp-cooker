// ============================================================
// Aphelion Research — Reporting / Export (Layer H)
// All I/O after replay.  No I/O during replay.
// ============================================================

#include "aphelion/reporting.h"

#include <algorithm>
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
        case ExitReason::ADAPTIVE_EXIT: return "adaptive_exit";
        case ExitReason::KILL_SWITCH: return "kill_switch";
        default:                      return "none";
    }
}

static std::string direction_str(Direction d) {
    return d == Direction::LONG ? "LONG" : "SHORT";
}

static std::string regime_str(Regime regime) {
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

static std::string csv_escape(const std::string& value) {
    if (value.find_first_of(",\"\n") == std::string::npos) {
        return value;
    }
    std::string escaped;
    escaped.reserve(value.size() + 4);
    escaped.push_back('"');
    for (char c : value) {
        if (c == '"') escaped.push_back('"');
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

static std::string join_strings(const std::vector<std::string>& values) {
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out << " | ";
        out << values[i];
    }
    return out.str();
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
         "win_rate,profit_factor,expectancy,trade_count,liquidated,strategy,"
         "risk_adjusted_return,consistency_score,composite_score,"
         "validation_passed,heavy_validation_run,robust_score,stability_score,"
         "monte_carlo_resilience,regime_consistency,overfit_penalty,rejection_reason\n";

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
          << r.strategy_name << ","
          << r.risk_adjusted_return << ","
          << r.consistency_score << ","
          << r.composite_score << ","
          << (r.validation_passed ? "true" : "false") << ","
          << (r.heavy_validation_run ? "true" : "false") << ","
          << r.robust_score << ","
          << r.stability_score << ","
          << r.monte_carlo_resilience << ","
          << r.regime_consistency << ","
          << r.overfit_penalty << ","
          << csv_escape(r.rejection_reason) << "\n";
    }

    std::cout << "[report] Wrote " << path << " (" << leaderboard.size() << " rows)" << std::endl;
}

void write_validation_summary_csv(
    const std::filesystem::path& output_dir,
    const ValidationSummary& summary
) {
    auto path = output_dir / "validation_summary.csv";
    std::ofstream f(path);
    if (!f) return;

    f << "account_id,strategy,fast_period,slow_period,quick_filter_passed,"
         "heavy_validation_run,validation_passed,base_return_pct,base_max_drawdown_pct,"
         "base_profit_factor,base_expectancy,base_trade_count,holdout_validation_return_pct,"
         "holdout_test_return_pct,robust_score,return_quality,drawdown_score,stability_score,"
         "robustness_component,regime_consistency,monte_carlo_resilience,timeframe_consistency,"
         "parameter_sensitivity,overfit_penalty,degradation_slope,rejection_reasons,failure_modes\n";

    f << std::fixed << std::setprecision(4);
    for (const auto& report : summary.reports) {
        f << report.account_id << ","
          << csv_escape(report.strategy_name) << ","
          << report.fast_period << ","
          << report.slow_period << ","
          << (report.quick_filter_passed ? "true" : "false") << ","
          << (report.heavy_validation_run ? "true" : "false") << ","
          << (report.passed ? "true" : "false") << ","
          << report.base_metrics.total_return_pct << ","
          << report.base_metrics.max_drawdown_pct << ","
          << report.base_metrics.profit_factor << ","
          << report.base_metrics.expectancy << ","
          << report.base_metrics.trade_count << ","
          << report.holdout.validation.total_return_pct << ","
          << report.holdout.test.total_return_pct << ","
          << report.robust_score << ","
          << report.return_quality << ","
          << report.drawdown_score << ","
          << report.stability_score << ","
          << report.robustness_component << ","
          << report.regime_consistency << ","
          << report.monte_carlo_resilience << ","
          << report.timeframe_consistency << ","
          << report.parameter_sensitivity << ","
          << report.overfit_penalty << ","
          << report.degradation_slope << ","
          << csv_escape(join_strings(report.rejection_reasons)) << ","
          << csv_escape(join_strings(report.failure_modes)) << "\n";
    }
}

void write_validation_walkforward_csv(
    const std::filesystem::path& output_dir,
    const ValidationSummary& summary
) {
    auto path = output_dir / "validation_walkforward.csv";
    std::ofstream f(path);
    if (!f) return;

    f << "account_id,window_index,train_begin,train_end,test_begin,test_end,"
         "train_return_pct,test_return_pct,train_max_drawdown_pct,test_max_drawdown_pct,"
         "generalization_ratio\n";

    f << std::fixed << std::setprecision(4);
    for (const auto& report : summary.reports) {
        for (const auto& window : report.walkforward) {
            f << report.account_id << ","
              << window.index << ","
              << window.train_begin << ","
              << window.train_end << ","
              << window.test_begin << ","
              << window.test_end << ","
              << window.train.total_return_pct << ","
              << window.test.total_return_pct << ","
              << window.train.max_drawdown_pct << ","
              << window.test.max_drawdown_pct << ","
              << window.generalization_ratio << "\n";
        }
    }
}

void write_validation_regime_csv(
    const std::filesystem::path& output_dir,
    const ValidationSummary& summary
) {
    auto path = output_dir / "validation_regimes.csv";
    std::ofstream f(path);
    if (!f) return;

    f << "account_id,regime,trades,bar_share,profit_share,net_pnl,expectancy,"
         "win_rate_pct,profit_factor,failing\n";

    f << std::fixed << std::setprecision(4);
    for (const auto& report : summary.reports) {
        for (const auto& regime : report.regime_breakdown) {
            f << report.account_id << ","
              << regime_str(regime.regime) << ","
              << regime.trades << ","
              << regime.bar_share << ","
              << regime.profit_share << ","
              << regime.net_pnl << ","
              << regime.expectancy << ","
              << regime.win_rate_pct << ","
              << regime.profit_factor << ","
              << (regime.failing ? "true" : "false") << "\n";
        }
    }
}

void write_validation_stress_csv(
    const std::filesystem::path& output_dir,
    const ValidationSummary& summary
) {
    auto path = output_dir / "validation_stress.csv";
    std::ofstream f(path);
    if (!f) return;

    f << "account_id,category,name,total_return_pct,max_drawdown_pct,profit_factor,"
         "expectancy,trade_count,passed\n";

    f << std::fixed << std::setprecision(4);
    for (const auto& report : summary.reports) {
        for (const auto& stress : report.stress_tests) {
            f << report.account_id << ",stress,"
              << csv_escape(stress.name) << ","
              << stress.metrics.total_return_pct << ","
              << stress.metrics.max_drawdown_pct << ","
              << stress.metrics.profit_factor << ","
              << stress.metrics.expectancy << ","
              << stress.metrics.trade_count << ","
              << (stress.passed ? "true" : "false") << "\n";
        }
        for (const auto& test : report.timeframe_tests) {
            f << report.account_id << ",timeframe,"
              << csv_escape(test.name) << ","
              << test.metrics.total_return_pct << ","
              << test.metrics.max_drawdown_pct << ","
              << test.metrics.profit_factor << ","
              << test.metrics.expectancy << ","
              << test.metrics.trade_count << ","
              << (test.passed ? "true" : "false") << "\n";
        }
    }
}

void write_validation_conditions_csv(
    const std::filesystem::path& output_dir,
    const ValidationSummary& summary
) {
    auto path = output_dir / "validation_conditions.csv";
    std::ofstream f(path);
    if (!f) return;

    f << "account_id,condition,trades,net_pnl,expectancy,win_rate_pct\n";
    f << std::fixed << std::setprecision(4);

    for (const auto& report : summary.reports) {
        for (const auto& condition : report.condition_breakdown) {
            f << report.account_id << ","
              << csv_escape(condition.name) << ","
              << condition.trades << ","
              << condition.net_pnl << ","
              << condition.expectancy << ","
              << condition.win_rate_pct << "\n";
        }
    }
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
      << "  \"enable_ech\": " << (meta.enable_ech ? "true" : "false") << ",\n"
      << "  \"live_safe_mode\": " << (meta.live_safe_mode ? "true" : "false") << ",\n"
      << "  \"live_max_leverage_cap\": " << meta.live_max_leverage_cap << ",\n"
      << "  \"max_position_notional\": " << meta.max_position_notional << ",\n"
      << "  \"max_total_notional\": " << meta.max_total_notional << ",\n"
      << "  \"session_trade_limit\": " << meta.session_trade_limit << ",\n"
      << "  \"session_drawdown_kill\": " << meta.session_drawdown_kill << ",\n"
      << "  \"session_loss_kill\": " << meta.session_loss_kill << ",\n"
      << "  \"fast_period_range\": [" << meta.fast_period_min << ", " << meta.fast_period_max << "],\n"
      << "  \"slow_period_range\": [" << meta.slow_period_min << ", " << meta.slow_period_max << "],\n"
      << "  \"total_bars\": " << meta.total_bars << ",\n"
      << "  \"elapsed_seconds\": " << std::fixed << std::setprecision(3) << meta.elapsed_seconds << ",\n"
      << "  \"engine\": \"Aphelion Research v3.0.0\",\n"
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
    if (!tournament.validation_summary().reports.empty()) {
        write_validation_summary_csv(output_dir, tournament.validation_summary());
        write_validation_walkforward_csv(output_dir, tournament.validation_summary());
        write_validation_regime_csv(output_dir, tournament.validation_summary());
        write_validation_stress_csv(output_dir, tournament.validation_summary());
        write_validation_conditions_csv(output_dir, tournament.validation_summary());
    }

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
    std::cout << "\n==================== LEADERBOARD (top 20) — ranked by composite quality ====================\n";
    std::cout << std::left
              << std::setw(5)  << "Rank"
              << std::setw(6)  << "Acct"
              << std::setw(12) << "Equity"
              << std::setw(10) << "Ret%"
              << std::setw(9)  << "MaxDD%"
              << std::setw(8)  << "WR%"
              << std::setw(8)  << "PF"
              << std::setw(7)  << "Trd"
              << std::setw(8)  << "RiskAdj"
              << std::setw(7)  << "Cons"
              << std::setw(9)  << "Composite"
              << std::setw(6)  << "F"
              << std::setw(6)  << "S"
              << std::setw(5)  << "Liq"
              << "\n";

    size_t show = std::min(lb.size(), static_cast<size_t>(20));
    for (size_t i = 0; i < show; ++i) {
        const auto& r = lb[i];
        std::cout << std::left
                  << std::setw(5)  << (i + 1)
                  << std::setw(6)  << r.account_id
                  << std::setw(12) << std::fixed << std::setprecision(1) << r.final_equity
                  << std::setw(10) << std::fixed << std::setprecision(2) << r.total_return
                  << std::setw(9)  << std::fixed << std::setprecision(1) << r.max_drawdown
                  << std::setw(8)  << std::fixed << std::setprecision(1) << r.win_rate
                  << std::setw(8)  << std::fixed << std::setprecision(2) << r.profit_factor
                  << std::setw(7)  << r.trade_count
                  << std::setw(8)  << std::fixed << std::setprecision(2) << r.risk_adjusted_return
                  << std::setw(7)  << std::fixed << std::setprecision(2) << r.consistency_score
                  << std::setw(9)  << std::fixed << std::setprecision(2) << r.composite_score
                  << std::setw(6)  << r.fast_period
                  << std::setw(6)  << r.slow_period
                  << std::setw(5)  << (r.liquidated ? "YES" : "no")
                  << "\n";
    }
    std::cout << "================================================================\n\n";
}

} // namespace aphelion
