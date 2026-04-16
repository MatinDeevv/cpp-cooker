// ============================================================
// Aphelion Research — Main Entry Point
//
// PHYSICAL REALITY STATEMENT:
// This is a historical simulation engine, not a hardware-exchange
// execution engine. True nanosecond execution is not the goal.
// The correct goal is an architecture with nanosecond-level
// engineering discipline and extremely high throughput per
// replay run.
//
// This system is designed so that:
//   - research throughput is maximized
//   - replay cost per bar per account is minimized
//   - future acceleration paths remain open
// ============================================================

#include "aphelion/data_ingest.h"
#include "aphelion/quantlib_layer.h"
#include "aphelion/tournament.h"
#include "aphelion/reporting.h"
#include "aphelion/replay_engine.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static void print_usage() {
    std::cout << R"(
Aphelion Research — Simulation Engine v1.0.0
=============================================
Usage: aphelion [options]

Options:
  --data-root <path>       Data root directory (default: C:\Users\marti\LLM\data_sim_ready)
  --symbol <sym>           Symbol to replay (default: XAUUSD)
  --timeframe <tf>         Timeframe (default: H1)
  --accounts <n>           Number of simulation accounts (default: 100)
  --leverage <x>           Max leverage (default: 500)
  --risk <pct>             Risk per trade as decimal (default: 0.01)
  --stop-out <pct>         Stop-out margin level % (default: 50)
  --commission <val>       Commission per lot round-trip (default: 0)
  --slippage <pts>         Slippage in points (default: 0)
  --fast-min <n>           Fast SMA period min (default: 5)
  --fast-max <n>           Fast SMA period max (default: 50)
  --slow-min <n>           Slow SMA period min (default: 20)
  --slow-max <n>           Slow SMA period max (default: 200)
  --output <path>          Output directory (default: output)
  --help                   Show this help
)";
}

int main(int argc, char* argv[]) {
    // ── Defaults ────────────────────────────────────────────
    std::string data_root   = R"(C:\Users\marti\LLM\data_sim_ready)";
    std::string symbol      = "XAUUSD";
    std::string timeframe   = "H1";
    int    num_accounts     = 100;
    double max_leverage     = 500.0;
    double risk_per_trade   = 0.01;
    double stop_out_level   = 50.0;
    double commission       = 0.0;
    double slippage         = 0.0;
    int    fast_min         = 5;
    int    fast_max         = 50;
    int    slow_min         = 20;
    int    slow_max         = 200;
    std::string output_dir  = "output";

    // ── Parse args ──────────────────────────────────────────
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "Missing value for " << arg << std::endl;
            std::exit(1);
            return "";
        };

        if (arg == "--help")            { print_usage(); return 0; }
        else if (arg == "--data-root")  data_root     = next();
        else if (arg == "--symbol")     symbol        = next();
        else if (arg == "--timeframe")  timeframe     = next();
        else if (arg == "--accounts")   num_accounts  = std::stoi(next());
        else if (arg == "--leverage")   max_leverage  = std::stod(next());
        else if (arg == "--risk")       risk_per_trade= std::stod(next());
        else if (arg == "--stop-out")   stop_out_level= std::stod(next());
        else if (arg == "--commission") commission    = std::stod(next());
        else if (arg == "--slippage")   slippage      = std::stod(next());
        else if (arg == "--fast-min")   fast_min      = std::stoi(next());
        else if (arg == "--fast-max")   fast_max      = std::stoi(next());
        else if (arg == "--slow-min")   slow_min      = std::stoi(next());
        else if (arg == "--slow-max")   slow_max      = std::stoi(next());
        else if (arg == "--output")     output_dir    = next();
        else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            print_usage();
            return 1;
        }
    }

    auto total_start = std::chrono::high_resolution_clock::now();

    std::cout << "================================================================" << std::endl;
    std::cout << " APHELION RESEARCH — Simulation Engine v1.0.0" << std::endl;
    std::cout << "================================================================" << std::endl;
    std::cout << " Symbol:      " << symbol << std::endl;
    std::cout << " Timeframe:   " << timeframe << std::endl;
    std::cout << " Accounts:    " << num_accounts << std::endl;
    std::cout << " Max leverage: " << max_leverage << std::endl;
    std::cout << " Risk/trade:  " << risk_per_trade << std::endl;
    std::cout << " Stop-out:    " << stop_out_level << "%" << std::endl;
    std::cout << " Data root:   " << data_root << std::endl;
    std::cout << " Output:      " << output_dir << std::endl;
    std::cout << "================================================================" << std::endl;

    // ── Phase 1: Initialize QuantLib calendar ───────────────
    std::cout << "\n[phase 1] Initializing calendar..." << std::endl;
    aphelion::init_calendar(symbol);

    // ── Phase 2: Load data ──────────────────────────────────
    std::cout << "\n[phase 2] Loading market data..." << std::endl;
    auto load_start = std::chrono::high_resolution_clock::now();

    aphelion::BarTape tape;
    try {
        tape = aphelion::load_bar_tape(fs::path(data_root), symbol, timeframe);
    } catch (const std::exception& e) {
        std::cerr << "FATAL: Data load failed: " << e.what() << std::endl;
        return 1;
    }

    auto load_end = std::chrono::high_resolution_clock::now();
    double load_secs = std::chrono::duration<double>(load_end - load_start).count();
    std::cout << "[phase 2] Data loaded in " << load_secs << " s" << std::endl;
    std::cout << "  Bars:      " << tape.bars.size() << std::endl;
    std::cout << "  Tape size: " << (tape.bars.size() * sizeof(aphelion::Bar)) / (1024 * 1024) << " MB" << std::endl;

    if (tape.bars.empty()) {
        std::cerr << "FATAL: No bars loaded." << std::endl;
        return 1;
    }

    // ── Phase 3: Tournament setup ───────────────────────────
    std::cout << "\n[phase 3] Setting up tournament..." << std::endl;

    aphelion::TournamentConfig tcfg;
    tcfg.num_accounts    = num_accounts;
    tcfg.initial_balance = 10000.0;
    tcfg.max_leverage    = max_leverage;
    tcfg.stop_out_level  = stop_out_level;
    tcfg.risk_per_trade  = risk_per_trade;
    tcfg.commission      = commission;
    tcfg.slippage        = slippage;
    tcfg.max_positions   = 1;
    tcfg.fast_period_min = fast_min;
    tcfg.fast_period_max = fast_max;
    tcfg.slow_period_min = slow_min;
    tcfg.slow_period_max = slow_max;

    aphelion::Tournament tournament(tcfg, tape);
    tournament.initialize();

    // ── Phase 4: Run replay ─────────────────────────────────
    std::cout << "\n[phase 4] Running simulation..." << std::endl;
    auto sim_start = std::chrono::high_resolution_clock::now();

    tournament.run();

    auto sim_end = std::chrono::high_resolution_clock::now();
    double sim_secs = std::chrono::duration<double>(sim_end - sim_start).count();

    // ── Phase 5: Output ─────────────────────────────────────
    std::cout << "\n[phase 5] Writing outputs..." << std::endl;

    aphelion::RunMetadata meta;
    meta.symbol          = symbol;
    meta.timeframe       = timeframe;
    meta.leverage_cap    = max_leverage;
    meta.commission      = commission;
    meta.slippage        = slippage;
    meta.risk_per_trade  = risk_per_trade;
    meta.stop_out_level  = stop_out_level;
    meta.num_accounts    = num_accounts;
    meta.strategy_id     = 0;
    meta.fast_period_min = fast_min;
    meta.fast_period_max = fast_max;
    meta.slow_period_min = slow_min;
    meta.slow_period_max = slow_max;
    meta.total_bars      = tape.bars.size();
    meta.elapsed_seconds = sim_secs;

    aphelion::ReplayStats rstats;
    rstats.bars_processed  = tape.bars.size();
    rstats.elapsed_seconds = sim_secs;

    aphelion::write_all_outputs(fs::path(output_dir), tournament, rstats, meta);

    auto total_end = std::chrono::high_resolution_clock::now();
    double total_secs = std::chrono::duration<double>(total_end - total_start).count();

    std::cout << "\n================================================================" << std::endl;
    std::cout << " COMPLETE" << std::endl;
    std::cout << " Total elapsed: " << total_secs << " s" << std::endl;
    std::cout << " Simulation:    " << sim_secs << " s" << std::endl;
    std::cout << " Data load:     " << load_secs << " s" << std::endl;
    std::cout << "================================================================" << std::endl;

    return 0;
}
