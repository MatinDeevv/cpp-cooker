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
#include "aphelion/multi_timeframe.h"
#include "aphelion/evolution_engine.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::vector<std::string> default_validation_timeframes(const std::string& primary_timeframe) {
    if (primary_timeframe == "M5") return {"M15", "H1"};
    if (primary_timeframe == "M15") return {"M5", "H1"};
    if (primary_timeframe == "H1") return {"M15", "H4"};
    if (primary_timeframe == "H4") return {"H1", "D1"};
    return {};
}

static void print_usage() {
    std::cout << R"(
Aphelion Research — Simulation Engine v3.0.0 (Intelligence Evolution)
=============================================
Usage: aphelion [options]

Options:
  --data-root <path>       Data root directory (default: C:\Users\marti\LLM\data_sim_ready)
  --symbol <sym>           Symbol to replay (default: XAUUSD)
  --timeframe <tf>         Primary replay timeframe (default: H1)
  --context-tf <tf>        Add secondary context timeframe (repeatable)
  --validation-tf <tf>     Add validation transfer timeframe (repeatable)
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
  --strategy <id>          Strategy: 0=SmaCrossover 1=ContextAwareSma (default: 1)
  --disable-ech            Disable ECH and run legacy intelligence behavior
  --disable-validation     Disable robustness validation gate
  --disable-live-safe      Disable conservative live safety caps and session rails
  --emergency-flat         Start with an emergency flatten / no-new-risk kill switch
  --live-risk-scale <x>    Risk scale applied in live-safe mode (default: 0.50)
  --live-max-lev <x>       Effective leverage cap in live-safe mode (default: 25)
  --max-position-notional <v>  Per-position live-safe notional cap (default: 5000)
  --max-total-notional <v>     Per-account live-safe total notional cap (default: 10000)
  --session-trade-limit <n>    Session entry cap before halting (default: 48)
  --session-dd-kill <x>        Session drawdown kill threshold (default: 0.03)
  --session-loss-kill <x>      Session realized loss kill threshold (default: 0.02)
  --output <path>          Output directory (default: output)
  --mode <full|bench|research>  Run mode (default: full)
  --help                   Show this help

Strategies:
  0  SmaCrossover       Classic SMA crossover (V2 compatible)
  1  ContextAwareSma    SMA + regime gating, confidence, dynamic risk (V3)

Run Modes:
  full       Full reporting: equity curves, trade logs, leaderboard
  bench      Benchmark: replay only, no equity curve collection
  research   Full data collection, minimal console output

Evolution Mode (--evolve):
  --evolve                  Run evolutionary strategy discovery instead of tournament
  --pop-size <n>            Population size (default: 500)
  --generations <n>         Number of generations (default: 100)
  --elite-count <n>         Elite count (default: 20)
  --mutation-rate <f>       Mutation rate 0.0-1.0 (default: 0.15)
  --crossover-rate <f>      Crossover rate 0.0-1.0 (default: 0.60)
  --min-monthly-return <f>  Monthly return threshold as decimal (default: 0.20)
  --max-drawdown <f>        Max drawdown limit as decimal (default: 0.30)
  --min-profit-factor <f>   Minimum profit factor (default: 1.3)
  --min-trades <n>          Minimum trade count (default: 20)
  --batch-size <n>          Candidates per batch evaluation (default: 50)
  --evolution-output <dir>  Evolution output directory (default: evolution_output)
  --evolution-seed <n>      Random seed for reproducibility (default: 42)
  --no-robustness           Skip robustness validation phase
)";
}

int main(int argc, char* argv[]) {
    // ── Defaults ────────────────────────────────────────────
    std::string data_root   = R"(C:\Users\marti\LLM\data_sim_ready)";
    std::string symbol      = "XAUUSD";
    std::string timeframe   = "H1";
    std::vector<std::string> context_timeframes;  // V2: secondary TFs
    std::vector<std::string> validation_timeframes;
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
    int    strategy_id      = 1;  // V3: default to ContextAwareSma
    bool   enable_ech       = true;
    bool   enable_validation = true;
    bool   live_safe_mode   = true;
    bool   emergency_flatten = false;
    double live_risk_scale  = 0.50;
    double live_max_lev     = 25.0;
    double max_position_notional = 5000.0;
    double max_total_notional    = 10000.0;
    int    session_trade_limit   = 48;
    double session_dd_kill       = 0.03;
    double session_loss_kill     = 0.02;
    std::string output_dir  = "output";
    aphelion::RunMode run_mode = aphelion::RunMode::FULL;

    // Evolution mode
    bool   evolve_mode      = false;
    int    pop_size          = 500;
    int    evo_generations   = 100;
    int    elite_count       = 20;
    float  mutation_rate     = 0.15f;
    float  crossover_rate    = 0.60f;
    double min_monthly_return = 0.20;
    double max_drawdown_limit = 0.30;
    double min_profit_factor = 1.3;
    int    min_trades        = 20;
    int    batch_size_evo    = 50;
    std::string evo_output   = "evolution_output";
    uint64_t evo_seed        = 42;
    bool   skip_robustness   = false;

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
        else if (arg == "--context-tf") context_timeframes.push_back(next());
        else if (arg == "--validation-tf") validation_timeframes.push_back(next());
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
        else if (arg == "--strategy")   strategy_id   = std::stoi(next());
        else if (arg == "--disable-ech") enable_ech   = false;
        else if (arg == "--disable-validation") enable_validation = false;
        else if (arg == "--disable-live-safe") live_safe_mode = false;
        else if (arg == "--emergency-flat") emergency_flatten = true;
        else if (arg == "--live-risk-scale") live_risk_scale = std::stod(next());
        else if (arg == "--live-max-lev") live_max_lev = std::stod(next());
        else if (arg == "--max-position-notional") max_position_notional = std::stod(next());
        else if (arg == "--max-total-notional") max_total_notional = std::stod(next());
        else if (arg == "--session-trade-limit") session_trade_limit = std::stoi(next());
        else if (arg == "--session-dd-kill") session_dd_kill = std::stod(next());
        else if (arg == "--session-loss-kill") session_loss_kill = std::stod(next());
        else if (arg == "--output")     output_dir    = next();
        else if (arg == "--mode") {
            std::string m = next();
            if (m == "bench" || m == "benchmark") run_mode = aphelion::RunMode::BENCHMARK;
            else if (m == "research")             run_mode = aphelion::RunMode::RESEARCH;
            else                                  run_mode = aphelion::RunMode::FULL;
        }
        // Evolution mode args
        else if (arg == "--evolve")              evolve_mode = true;
        else if (arg == "--pop-size")            pop_size = std::stoi(next());
        else if (arg == "--generations")         evo_generations = std::stoi(next());
        else if (arg == "--elite-count")         elite_count = std::stoi(next());
        else if (arg == "--mutation-rate")        mutation_rate = std::stof(next());
        else if (arg == "--crossover-rate")       crossover_rate = std::stof(next());
        else if (arg == "--min-monthly-return")   min_monthly_return = std::stod(next());
        else if (arg == "--max-drawdown")         max_drawdown_limit = std::stod(next());
        else if (arg == "--min-profit-factor")    min_profit_factor = std::stod(next());
        else if (arg == "--min-trades")           min_trades = std::stoi(next());
        else if (arg == "--batch-size")           batch_size_evo = std::stoi(next());
        else if (arg == "--evolution-output")     evo_output = next();
        else if (arg == "--evolution-seed")       evo_seed = std::stoull(next());
        else if (arg == "--no-robustness")        skip_robustness = true;
        else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            print_usage();
            return 1;
        }
    }

    if (run_mode == aphelion::RunMode::BENCHMARK && enable_validation) {
        std::cout << "[config] Disabling validation in benchmark mode to preserve timing." << std::endl;
        enable_validation = false;
    }

    auto total_start = std::chrono::high_resolution_clock::now();

    const char* mode_str = (run_mode == aphelion::RunMode::BENCHMARK) ? "BENCHMARK"
                         : (run_mode == aphelion::RunMode::RESEARCH) ? "RESEARCH"
                         : "FULL";

    std::cout << "================================================================" << std::endl;
    std::cout << " APHELION RESEARCH — Simulation Engine v3.0.0" << std::endl;
    std::cout << "================================================================" << std::endl;
    std::cout << " Symbol:       " << symbol << std::endl;
    std::cout << " Timeframe:    " << timeframe << std::endl;
    if (!context_timeframes.empty()) {
        std::cout << " Context TFs:  ";
        for (size_t i = 0; i < context_timeframes.size(); ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << context_timeframes[i];
        }
        std::cout << std::endl;
    }
    if (enable_validation) {
        const auto effective_validation_timeframes = validation_timeframes.empty()
            ? default_validation_timeframes(timeframe)
            : validation_timeframes;
        if (!effective_validation_timeframes.empty()) {
            std::cout << " Valid TFs:   ";
            for (size_t i = 0; i < effective_validation_timeframes.size(); ++i) {
                if (i > 0) std::cout << ", ";
                std::cout << effective_validation_timeframes[i];
            }
            std::cout << std::endl;
        }
    }
    std::cout << " Accounts:     " << num_accounts << std::endl;
    std::cout << " Max leverage: " << max_leverage << std::endl;
    std::cout << " Risk/trade:   " << risk_per_trade << std::endl;
    std::cout << " Stop-out:     " << stop_out_level << "%" << std::endl;
    const char* strat_str = (strategy_id == 1) ? "ContextAwareSma (V3)" : "SmaCrossover (V2)";
    std::cout << " Strategy:     " << strat_str << " (id=" << strategy_id << ")" << std::endl;
    std::cout << " ECH:          " << (enable_ech ? "enabled" : "disabled") << std::endl;
    std::cout << " Live-safe:    " << (live_safe_mode ? "enabled" : "disabled") << std::endl;
    if (live_safe_mode) {
        std::cout << " Live cap:     lev<=" << live_max_lev
                  << " risk_scale=" << live_risk_scale
                  << " pos_notional<=" << max_position_notional
                  << " total_notional<=" << max_total_notional
                  << std::endl;
        std::cout << " Session rails: trade_limit=" << session_trade_limit
                  << " dd_kill=" << session_dd_kill
                  << " loss_kill=" << session_loss_kill
                  << std::endl;
    }
    std::cout << " Mode:         " << mode_str << std::endl;
    std::cout << " Data root:    " << data_root << std::endl;
    std::cout << " Output:       " << output_dir << std::endl;
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

    // V2: Load secondary context timeframes
    std::vector<aphelion::BarTape> context_tapes;
    std::vector<aphelion::TimeframeAlignment> alignments;
    for (const auto& ctx_tf : context_timeframes) {
        try {
            auto ctx_tape = aphelion::load_bar_tape(fs::path(data_root), symbol, ctx_tf);
            auto alignment = aphelion::build_alignment(tape, ctx_tape);
            alignments.push_back(std::move(alignment));
            context_tapes.push_back(std::move(ctx_tape));
        } catch (const std::exception& e) {
            std::cerr << "WARNING: Failed to load context TF " << ctx_tf << ": " << e.what() << std::endl;
        }
    }

    std::vector<std::string> effective_validation_timeframes = enable_validation
        ? (validation_timeframes.empty()
            ? default_validation_timeframes(timeframe)
            : validation_timeframes)
        : std::vector<std::string>{};
    effective_validation_timeframes.erase(
        std::remove_if(
            effective_validation_timeframes.begin(),
            effective_validation_timeframes.end(),
            [&](const std::string& tf) { return tf == timeframe; }
        ),
        effective_validation_timeframes.end()
    );
    std::sort(effective_validation_timeframes.begin(), effective_validation_timeframes.end());
    effective_validation_timeframes.erase(
        std::unique(effective_validation_timeframes.begin(), effective_validation_timeframes.end()),
        effective_validation_timeframes.end()
    );

    std::vector<aphelion::BarTape> validation_tapes_storage;
    validation_tapes_storage.reserve(effective_validation_timeframes.size());
    std::vector<const aphelion::BarTape*> validation_tape_ptrs;
    validation_tape_ptrs.reserve(effective_validation_timeframes.size());

    for (const auto& validation_tf : effective_validation_timeframes) {
        bool reused_context = false;
        for (size_t i = 0; i < context_timeframes.size(); ++i) {
            if (context_timeframes[i] == validation_tf && i < context_tapes.size()) {
                validation_tape_ptrs.push_back(&context_tapes[i]);
                reused_context = true;
                break;
            }
        }
        if (reused_context) continue;

        try {
            validation_tapes_storage.push_back(
                aphelion::load_bar_tape(fs::path(data_root), symbol, validation_tf)
            );
            validation_tape_ptrs.push_back(&validation_tapes_storage.back());
        } catch (const std::exception& e) {
            std::cerr << "WARNING: Failed to load validation TF " << validation_tf
                      << ": " << e.what() << std::endl;
        }
    }

    auto load_end = std::chrono::high_resolution_clock::now();
    double load_secs = std::chrono::duration<double>(load_end - load_start).count();
    std::cout << "[phase 2] Data loaded in " << load_secs << " s" << std::endl;
    std::cout << "  Primary bars:  " << tape.bars.size() << std::endl;
    std::cout << "  Tape size:     " << (tape.bars.size() * sizeof(aphelion::Bar)) / (1024 * 1024) << " MB" << std::endl;
    if (!context_tapes.empty()) {
        std::cout << "  Context TFs:   " << context_tapes.size() << " loaded" << std::endl;
    }
    if (!validation_tape_ptrs.empty()) {
        std::cout << "  Valid TFs:     " << validation_tape_ptrs.size() << " loaded" << std::endl;
    }

    if (tape.bars.empty()) {
        std::cerr << "FATAL: No bars loaded." << std::endl;
        return 1;
    }

    std::vector<aphelion::MultiTimeframeInput> context_inputs;
    context_inputs.reserve(context_tapes.size());
    for (size_t i = 0; i < context_tapes.size(); ++i) {
        float weight = 1.0f;
        if (tape.timeframe_seconds > 0 && context_tapes[i].timeframe_seconds > 0) {
            float ratio = static_cast<float>(context_tapes[i].timeframe_seconds) /
                          static_cast<float>(tape.timeframe_seconds);
            weight = std::max(1.0f, std::min(3.0f, 1.0f + std::log2(std::max(1.0f, ratio)) * 0.35f));
        }
        context_inputs.push_back({&context_tapes[i], &alignments[i], weight});
    }

    // ──────────────────────────────────────────────────────
    //  EVOLUTION MODE
    // ──────────────────────────────────────────────────────
    if (evolve_mode) {
        std::cout << "\n[evolution] Launching evolutionary strategy discovery..." << std::endl;

        aphelion::EvolutionConfig evo_config;
        evo_config.population_size   = pop_size;
        evo_config.generations       = evo_generations;
        evo_config.elite_count       = elite_count;
        evo_config.mutation_rate     = mutation_rate;
        evo_config.crossover_rate    = crossover_rate;
        evo_config.min_monthly_return = min_monthly_return;
        evo_config.max_drawdown_limit = max_drawdown_limit;
        evo_config.min_profit_factor = min_profit_factor;
        evo_config.min_trade_count   = min_trades;
        evo_config.eval_batch_size   = batch_size_evo;
        evo_config.output_dir        = evo_output;
        evo_config.random_seed       = evo_seed;
        evo_config.checkpoint_enabled = true;
        evo_config.enable_robustness_eval = !skip_robustness;
        evo_config.enable_finalist_extraction = true;
        evo_config.initial_balance   = 10000.0;
        evo_config.max_leverage      = max_leverage;
        evo_config.stop_out_level    = stop_out_level;
        evo_config.commission        = commission;
        evo_config.slippage          = slippage;

        aphelion::FeatureConfig feat_cfg;
        aphelion::RegimeConfig  reg_cfg;
        aphelion::EchConfig     ech_cfg;

        aphelion::EvolutionEngine engine(
            evo_config, tape,
            context_inputs,
            feat_cfg, reg_cfg, ech_cfg);

        engine.run();
        return 0;
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
    tcfg.mode            = run_mode;
    tcfg.fast_period_min = fast_min;
    tcfg.fast_period_max = fast_max;
    tcfg.slow_period_min = slow_min;
    tcfg.slow_period_max = slow_max;
    tcfg.strategy_id     = strategy_id;
    tcfg.live_safe_mode  = live_safe_mode;
    tcfg.emergency_flatten = emergency_flatten;
    tcfg.live_reduced_risk_scale = live_risk_scale;
    tcfg.live_max_leverage_cap = live_max_lev;
    tcfg.max_position_notional = max_position_notional;
    tcfg.max_total_notional = max_total_notional;
    tcfg.session_trade_limit = session_trade_limit;
    tcfg.session_drawdown_kill = session_dd_kill;
    tcfg.session_loss_kill = session_loss_kill;
    tcfg.ech_config.enabled = enable_ech;
    tcfg.risk_config.enable_ech = enable_ech;
    tcfg.risk_config.live_safe_mode = live_safe_mode;
    tcfg.context_inputs  = context_inputs;
    tcfg.validation_config.enabled = enable_validation;
    tcfg.validation_tapes = validation_tape_ptrs;

    aphelion::Tournament tournament(tcfg, tape);
    tournament.initialize();

    // ── Phase 4: Run replay ─────────────────────────────────
    std::cout << "\n[phase 4] Running simulation..." << std::endl;
    auto sim_start = std::chrono::high_resolution_clock::now();

    tournament.run();

    auto sim_end = std::chrono::high_resolution_clock::now();
    double sim_secs = std::chrono::duration<double>(sim_end - sim_start).count();

    // ── Phase 5: Output ─────────────────────────────────────
    if (run_mode == aphelion::RunMode::BENCHMARK) {
        // Benchmark mode: minimal output, just timing
        auto total_end = std::chrono::high_resolution_clock::now();
        double total_secs = std::chrono::duration<double>(total_end - total_start).count();

        const auto& rs = tournament.last_stats();
        std::cout << "\n================================================================" << std::endl;
        std::cout << " BENCHMARK COMPLETE" << std::endl;
        std::cout << " Replay:           " << rs.elapsed_seconds << " s" << std::endl;
        std::cout << " Acct*Bars/sec:    " << static_cast<int64_t>(rs.acct_bars_per_sec) << std::endl;
        std::cout << " Total elapsed:    " << total_secs << " s" << std::endl;
        std::cout << " Data load:        " << load_secs << " s" << std::endl;
        std::cout << " Signals fired:    " << rs.total_signals << std::endl;
        std::cout << " Fills:            " << rs.total_fills << std::endl;
        std::cout << " SL/TP exits:      " << rs.total_sl_tp << std::endl;
        std::cout << " Session kills:    " << rs.total_session_kills << std::endl;
        std::cout << " Emergency flats:  " << rs.total_emergency_flats << std::endl;
        std::cout << "================================================================" << std::endl;
        return 0;
    }

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
    meta.strategy_id     = strategy_id;
    meta.enable_ech      = enable_ech;
    meta.live_safe_mode  = live_safe_mode;
    meta.live_max_leverage_cap = live_max_lev;
    meta.max_position_notional = max_position_notional;
    meta.max_total_notional = max_total_notional;
    meta.session_trade_limit = session_trade_limit;
    meta.session_drawdown_kill = session_dd_kill;
    meta.session_loss_kill = session_loss_kill;
    meta.fast_period_min = fast_min;
    meta.fast_period_max = fast_max;
    meta.slow_period_min = slow_min;
    meta.slow_period_max = slow_max;
    meta.total_bars      = tape.bars.size();
    meta.elapsed_seconds = sim_secs;

    aphelion::ReplayStats rstats = tournament.last_stats();

    aphelion::write_all_outputs(fs::path(output_dir), tournament, rstats, meta);

    auto total_end = std::chrono::high_resolution_clock::now();
    double total_secs = std::chrono::duration<double>(total_end - total_start).count();

    std::cout << "\n================================================================" << std::endl;
    std::cout << " COMPLETE — v3.0.0" << std::endl;
    std::cout << " Total elapsed:    " << total_secs << " s" << std::endl;
    std::cout << " Simulation:       " << sim_secs << " s" << std::endl;
    std::cout << " Data load:        " << load_secs << " s" << std::endl;
    std::cout << " Acct*Bars/sec:    " << static_cast<int64_t>(rstats.acct_bars_per_sec) << std::endl;
    std::cout << " Signals fired:    " << rstats.total_signals << std::endl;
    std::cout << " Session kills:    " << rstats.total_session_kills << std::endl;
    std::cout << " Emergency flats:  " << rstats.total_emergency_flats << std::endl;
    std::cout << "================================================================" << std::endl;

    return 0;
}
