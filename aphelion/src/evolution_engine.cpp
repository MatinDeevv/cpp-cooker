// ============================================================
// Aphelion Research — Evolution Engine Implementation
//
// Population management, batch evaluation via existing replay
// infrastructure, selection, reproduction, reporting.
// ============================================================

#include "aphelion/evolution_engine.h"
#include "aphelion/replay_engine.h"
#include "aphelion/account.h"
#include "aphelion/execution.h"
#include "aphelion/reporting.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <filesystem>

namespace fs = std::filesystem;

namespace aphelion {

// ════════════════════════════════════════════════════════════
//  CONSTRUCTOR
// ════════════════════════════════════════════════════════════

EvolutionEngine::EvolutionEngine(
    const EvolutionConfig& config,
    const BarTape& primary_tape,
    const std::vector<MultiTimeframeInput>& context_inputs,
    const FeatureConfig& feature_config,
    const RegimeConfig& regime_config,
    const EchConfig& ech_config
)
    : config_(config)
    , primary_tape_(primary_tape)
    , context_inputs_(context_inputs)
    , feature_config_(feature_config)
    , regime_config_(regime_config)
    , ech_config_(ech_config)
    , rng_(config.random_seed)
{
    // Build intelligence tape once — shared by all candidates
    std::cout << "[evolution] Building intelligence tape..." << std::endl;
    auto t0 = std::chrono::high_resolution_clock::now();

    intelligence_tape_ = build_intelligence_tape(
        primary_tape_,
        context_inputs_.empty() ? nullptr : context_inputs_.data(),
        context_inputs_.size(),
        feature_config_,
        regime_config_,
        ech_config_
    );

    auto t1 = std::chrono::high_resolution_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "[evolution] Intelligence tape built: "
              << intelligence_tape_.size() << " states in "
              << secs << "s" << std::endl;

    // Create output directory
    if (config_.checkpoint_enabled) {
        fs::create_directories(config_.output_dir);
    }
}


// ════════════════════════════════════════════════════════════
//  MAIN EVOLUTION LOOP
// ════════════════════════════════════════════════════════════

void EvolutionEngine::run() {
    auto evolution_start = std::chrono::high_resolution_clock::now();

    std::cout << "\n================================================================" << std::endl;
    std::cout << " EVOLUTIONARY STRATEGY DISCOVERY" << std::endl;
    std::cout << " Population: " << config_.population_size << std::endl;
    std::cout << " Generations: " << config_.generations << std::endl;
    std::cout << " Elite count: " << config_.elite_count << std::endl;
    std::cout << " Mutation rate: " << config_.mutation_rate << std::endl;
    std::cout << " Crossover rate: " << config_.crossover_rate << std::endl;
    std::cout << " Min monthly return: " << (config_.min_monthly_return * 100) << "%" << std::endl;
    std::cout << " Max drawdown limit: " << (config_.max_drawdown_limit * 100) << "%" << std::endl;
    std::cout << " Bars: " << primary_tape_.bars.size() << std::endl;
    std::cout << "================================================================\n" << std::endl;

    // Initialize random population
    initialize_population();

    // Run generations
    for (int gen = 0; gen < config_.generations; ++gen) {
        run_generation(gen);

        // Checkpoint
        if (config_.checkpoint_enabled && gen > 0 &&
            gen % config_.checkpoint_interval == 0) {
            auto cp_path = config_.output_dir / ("checkpoint_gen_" + std::to_string(gen) + ".json");
            save_checkpoint(cp_path);
        }
    }

    // Robustness evaluation on top-k
    if (config_.enable_robustness_eval && !scores_.empty()) {
        std::cout << "\n[evolution] Robustness evaluation on top "
                  << config_.robustness_top_k << " candidates..." << std::endl;

        // Sort by composite and take top-k
        std::sort(scores_.begin(), scores_.end(),
            [](const auto& a, const auto& b) { return a.composite_score > b.composite_score; });

        int k = std::min(config_.robustness_top_k, static_cast<int>(scores_.size()));
        for (int i = 0; i < k; ++i) {
            auto result = validate_robustness(
                scores_[i].genome,
                primary_tape_,
                intelligence_tape_.data(),
                primary_tape_.bars.size(),
                config_.robustness_config
            );
            scores_[i].robustness = result;
            scores_[i].robustness_score = result.composite_robustness;
            scores_[i].passes_threshold = scores_[i].passes_threshold && result.passes;
            // Recompute composite with robustness
            scores_[i].composite_score = compute_composite(scores_[i]);
        }
    }

    // Extract finalists
    if (config_.enable_finalist_extraction) {
        std::sort(scores_.begin(), scores_.end(),
            [](const auto& a, const auto& b) { return a.composite_score > b.composite_score; });

        for (const auto& s : scores_) {
            if (s.passes_threshold && !s.liquidated) {
                finalists_.push_back(s);
                if (finalists_.size() >= static_cast<size_t>(config_.elite_count * 2))
                    break;
            }
        }
        write_finalist_report();
    }

    auto evolution_end = std::chrono::high_resolution_clock::now();
    double total_secs = std::chrono::duration<double>(evolution_end - evolution_start).count();

    std::cout << "\n================================================================" << std::endl;
    std::cout << " EVOLUTION COMPLETE" << std::endl;
    std::cout << " Total time: " << total_secs << "s" << std::endl;
    std::cout << " Finalists: " << finalists_.size() << std::endl;
    if (!finalists_.empty()) {
        std::cout << " Best composite: " << finalists_[0].composite_score << std::endl;
        std::cout << " Best monthly return: " << (finalists_[0].monthly_return * 100) << "%" << std::endl;
        std::cout << " Best drawdown: " << (finalists_[0].max_drawdown * 100) << "%" << std::endl;
        std::cout << " Best trades: " << finalists_[0].trade_count << std::endl;
        std::cout << " Best genome: " << finalists_[0].genome.describe() << std::endl;
    }
    std::cout << "================================================================" << std::endl;
}


// ════════════════════════════════════════════════════════════
//  SINGLE GENERATION
// ════════════════════════════════════════════════════════════

void EvolutionEngine::run_generation(int gen) {
    auto gen_start = std::chrono::high_resolution_clock::now();

    // Evaluate population
    evaluate_population();

    // Compute composites and threshold flags
    for (auto& s : scores_) {
        s.composite_score = compute_composite(s);
        s.passes_threshold =
            !s.liquidated &&
            s.monthly_return >= config_.min_monthly_return &&
            s.max_drawdown <= config_.max_drawdown_limit &&
            s.profit_factor >= config_.min_profit_factor &&
            s.trade_count >= config_.min_trade_count &&
            s.win_rate >= config_.min_win_rate;
    }

    // Track best
    for (const auto& s : scores_) {
        if (s.composite_score > best_composite_ever_) {
            best_composite_ever_ = s.composite_score;
        }
    }

    // Report
    write_generation_summary(gen);

    auto gen_end = std::chrono::high_resolution_clock::now();
    double gen_secs = std::chrono::duration<double>(gen_end - gen_start).count();

    // Console summary
    int passing = 0;
    double best_return = -1e9, best_composite = -1e9;
    for (const auto& s : scores_) {
        if (s.passes_threshold) passing++;
        best_return = std::max(best_return, s.monthly_return);
        best_composite = std::max(best_composite, s.composite_score);
    }

    std::cout << "[gen " << std::setw(4) << gen << "] "
              << gen_secs << "s  "
              << "best_ret=" << std::fixed << std::setprecision(1) << (best_return * 100) << "% "
              << "best_comp=" << std::setprecision(3) << best_composite << " "
              << "passing=" << passing << "/" << scores_.size()
              << std::endl;

    // Selection and reproduction for next generation
    if (gen < config_.generations - 1) {
        select_and_reproduce();
    }
}


// ════════════════════════════════════════════════════════════
//  POPULATION MANAGEMENT
// ════════════════════════════════════════════════════════════

void EvolutionEngine::initialize_population() {
    std::cout << "[evolution] Initializing population of " << config_.population_size << std::endl;
    population_.clear();
    population_.reserve(config_.population_size);
    for (int i = 0; i < config_.population_size; ++i) {
        population_.push_back(random_genome(rng_, next_id_++));
    }
}

void EvolutionEngine::select_and_reproduce() {
    // Sort by composite score descending
    std::vector<size_t> indices(scores_.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(),
        [this](size_t a, size_t b) {
            return scores_[a].composite_score > scores_[b].composite_score;
        });

    std::vector<StrategyGenome> next_gen;
    next_gen.reserve(config_.population_size);

    // Elitism: carry top-k unchanged
    int elite = std::min(config_.elite_count, static_cast<int>(indices.size()));
    for (int i = 0; i < elite; ++i) {
        auto g = scores_[indices[i]].genome;
        g.genome_id = next_id_++;
        g.generation++;
        next_gen.push_back(std::move(g));
    }

    // Tournament selection + reproduction for the rest
    std::uniform_int_distribution<int> pop_dist(0, static_cast<int>(scores_.size()) - 1);
    std::uniform_real_distribution<float> coin(0.0f, 1.0f);

    while (static_cast<int>(next_gen.size()) < config_.population_size) {
        // Tournament selection: pick best from k random candidates
        auto tournament_select = [&]() -> size_t {
            size_t best = pop_dist(rng_);
            for (int j = 1; j < config_.tournament_k; ++j) {
                size_t challenger = pop_dist(rng_);
                if (scores_[challenger].composite_score > scores_[best].composite_score)
                    best = challenger;
            }
            return best;
        };

        size_t p1_idx = tournament_select();
        const auto& parent1 = scores_[p1_idx].genome;

        StrategyGenome child;

        if (coin(rng_) < config_.crossover_rate) {
            // Crossover
            size_t p2_idx = tournament_select();
            const auto& parent2 = scores_[p2_idx].genome;
            child = crossover(parent1, parent2, rng_);
        } else {
            child = parent1;
        }

        // Mutation
        if (coin(rng_) < config_.mutation_rate + 0.5f) { // always some mutation chance
            child = mutate(child, rng_, config_.mutation_rate);
        }

        child.genome_id = next_id_++;
        child.generation = parent1.generation + 1;
        next_gen.push_back(std::move(child));
    }

    population_ = std::move(next_gen);
}


// ════════════════════════════════════════════════════════════
//  EVALUATION
// ════════════════════════════════════════════════════════════

void EvolutionEngine::evaluate_population() {
    const Bar* bars = primary_tape_.bars.data();
    size_t num_bars = primary_tape_.bars.size();
    const IntelligenceState* intel = intelligence_tape_.data();

    scores_.clear();
    scores_.reserve(population_.size());

    // Evaluate in batches for throughput
    int batch_size = config_.eval_batch_size;
    for (size_t start = 0; start < population_.size(); start += batch_size) {
        size_t end = std::min(start + batch_size, population_.size());
        std::vector<StrategyGenome> batch(
            population_.begin() + start, population_.begin() + end);

        std::vector<CandidateScore> batch_scores;
        evaluate_batch(batch, bars, num_bars, intel, batch_scores);

        for (auto& s : batch_scores) scores_.push_back(std::move(s));
    }
}

void EvolutionEngine::evaluate_batch(
    const std::vector<StrategyGenome>& genomes,
    const Bar* bars, size_t num_bars,
    const IntelligenceState* intelligence,
    std::vector<CandidateScore>& out_scores
) {
    size_t n = genomes.size();
    out_scores.resize(n);

    // Create strategies, accounts, and replay entries
    std::vector<std::unique_ptr<GenomeStrategy>> strategies;
    std::vector<Account> accounts(n);
    std::vector<SimulationParams> params(n);
    std::vector<ReplayEntry> entries;

    strategies.reserve(n);
    entries.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        strategies.push_back(std::make_unique<GenomeStrategy>(genomes[i]));

        // Initialize account with evolution config parameters
        params[i].initial_balance = config_.initial_balance;
        params[i].max_leverage = config_.max_leverage;
        params[i].risk_per_trade = genomes[i].base_risk_fraction;
        params[i].stop_out_level = config_.stop_out_level;
        params[i].commission_per_lot = config_.commission;
        params[i].slippage_points = config_.slippage;
        params[i].max_positions = 1;
        params[i].enable_ech = config_.risk_config.enable_ech ? 1 : 0;
        params[i].live_safe_mode = 0; // no live-safe in evolution

        accounts[i].init(static_cast<uint32_t>(i), params[i]);

        // Prepare strategy with intelligence
        strategies[i]->prepare_with_intelligence(bars, num_bars, intelligence);

        entries.push_back({&accounts[i], strategies[i].get(), &params[i]});
    }

    // Single replay pass for entire batch
    run_replay_v3(bars, num_bars, entries, RunMode::BENCHMARK,
                  intelligence, config_.risk_config);

    // Extract results
    for (size_t i = 0; i < n; ++i) {
        auto& s = out_scores[i];
        s.genome = genomes[i];
        s.genome_id = genomes[i].genome_id;

        const auto& acct = accounts[i].state;
        s.liquidated = (acct.liquidated != 0);
        s.total_return = (acct.equity - acct.initial_balance) / acct.initial_balance;
        s.max_drawdown = acct.max_drawdown;
        s.trade_count = acct.total_trades;
        s.win_rate = (acct.total_trades > 0) ?
            static_cast<double>(acct.winning_trades) / acct.total_trades : 0.0;

        s.profit_factor = (acct.gross_loss > 0) ?
            acct.gross_profit / std::fabs(acct.gross_loss) : 0.0;

        s.expectancy = (acct.total_trades > 0) ?
            (acct.gross_profit + acct.gross_loss) / acct.total_trades : 0.0;

        // Monthly return approximation
        // Assume data spans ~years, compute monthly rate
        double num_bars_d = static_cast<double>(num_bars);
        double bars_per_month = (primary_tape_.timeframe_seconds > 0) ?
            (30.0 * 24.0 * 3600.0) / primary_tape_.timeframe_seconds : 720.0;
        double months = num_bars_d / bars_per_month;
        if (months > 0 && s.total_return > -1.0 && !s.liquidated) {
            s.monthly_return = std::pow(1.0 + s.total_return, 1.0 / months) - 1.0;
        } else {
            s.monthly_return = -1.0;
        }

        // Consistency: compute from equity curve
        s.consistency = 0.0;
        if (!accounts[i].equity_curve.empty() && accounts[i].equity_curve.size() > 10) {
            const auto& ec = accounts[i].equity_curve;
            // Divide into segments, compute return per segment
            size_t seg_size = ec.size() / 10;
            int positive_segs = 0;
            for (int seg = 0; seg < 10; ++seg) {
                size_t start_idx = seg * seg_size;
                size_t end_idx = std::min(start_idx + seg_size, ec.size()) - 1;
                if (ec[end_idx] > ec[start_idx]) positive_segs++;
            }
            s.consistency = static_cast<double>(positive_segs) / 10.0;
        }

        s.composite_score = compute_composite(s);
    }
}

CandidateScore EvolutionEngine::evaluate_candidate(
    const StrategyGenome& genome,
    const Bar* bars, size_t num_bars,
    const IntelligenceState* intelligence
) {
    std::vector<StrategyGenome> batch = {genome};
    std::vector<CandidateScore> results;
    evaluate_batch(batch, bars, num_bars, intelligence, results);
    return results[0];
}


// ════════════════════════════════════════════════════════════
//  COMPOSITE SCORING
// ════════════════════════════════════════════════════════════

double EvolutionEngine::compute_composite(const CandidateScore& s) const {
    if (s.liquidated) return -1e6;
    if (s.trade_count < 5) return -1e5;

    // Return component (capped at 2x target)
    double return_comp = std::min(2.0,
        std::max(0.0, s.monthly_return / std::max(config_.min_monthly_return, 0.01)));

    // Risk-adjusted return
    double risk_adj = (s.max_drawdown > 0.001) ?
        s.monthly_return / s.max_drawdown : 0.0;
    risk_adj = std::min(5.0, std::max(0.0, risk_adj));

    // Trade quality
    double trade_quality = s.profit_factor * std::min(1.0,
        static_cast<double>(s.trade_count) / std::max(config_.min_trade_count, 1));

    // Consistency
    double consistency = std::max(0.0, s.consistency);

    // Robustness (0 if not yet tested)
    double robustness = std::max(0.0, s.robustness_score);

    // Complexity penalty
    double complexity = s.genome.complexity() * config_.complexity_penalty;

    // Weighted composite
    double composite = return_comp * 0.25
                     + risk_adj * 0.10       // normalized weight
                     + trade_quality * 0.20
                     + consistency * 0.15
                     + robustness * 0.15
                     - complexity * 0.05;

    // Bonus for meeting ALL thresholds
    if (s.monthly_return >= config_.min_monthly_return &&
        s.max_drawdown <= config_.max_drawdown_limit &&
        s.profit_factor >= config_.min_profit_factor &&
        s.trade_count >= config_.min_trade_count) {
        composite += 0.5;
    }

    return composite;
}


// ════════════════════════════════════════════════════════════
//  REPORTING
// ════════════════════════════════════════════════════════════

void EvolutionEngine::write_generation_summary(int gen) {
    if (!config_.checkpoint_enabled) return;

    auto path = config_.output_dir / ("gen_" + std::to_string(gen) + "_summary.txt");
    std::ofstream out(path);
    if (!out) return;

    // Sort for reporting
    std::vector<size_t> idx(scores_.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(),
        [this](size_t a, size_t b) {
            return scores_[a].composite_score > scores_[b].composite_score;
        });

    out << "Generation " << gen << " — " << scores_.size() << " candidates\n";
    out << std::string(70, '=') << "\n\n";

    int passing = 0;
    for (const auto& s : scores_) if (s.passes_threshold) passing++;
    out << "Passing threshold: " << passing << "\n\n";

    // Top 20
    out << "Top 20:\n";
    out << std::setw(8) << "Rank" << " "
        << std::setw(10) << "ID" << " "
        << std::setw(10) << "MonthRet" << " "
        << std::setw(10) << "MaxDD" << " "
        << std::setw(8) << "PF" << " "
        << std::setw(8) << "Trades" << " "
        << std::setw(8) << "WinRate" << " "
        << std::setw(10) << "Composite" << " "
        << std::setw(6) << "Pass" << "\n";
    out << std::string(90, '-') << "\n";

    int show = std::min(20, static_cast<int>(idx.size()));
    for (int i = 0; i < show; ++i) {
        const auto& s = scores_[idx[i]];
        out << std::setw(8) << (i + 1) << " "
            << std::setw(10) << s.genome_id << " "
            << std::setw(9) << std::fixed << std::setprecision(1) << (s.monthly_return * 100) << "% "
            << std::setw(9) << (s.max_drawdown * 100) << "% "
            << std::setw(8) << std::setprecision(2) << s.profit_factor << " "
            << std::setw(8) << s.trade_count << " "
            << std::setw(7) << std::setprecision(1) << (s.win_rate * 100) << "% "
            << std::setw(10) << std::setprecision(3) << s.composite_score << " "
            << std::setw(6) << (s.passes_threshold ? "YES" : "no") << "\n";
    }
    out << "\n";

    // Top candidate details
    if (!idx.empty()) {
        out << "Best genome: " << scores_[idx[0]].genome.describe() << "\n";
    }
}

void EvolutionEngine::write_finalist_report() {
    if (finalists_.empty()) return;

    auto path = config_.output_dir / "finalists.txt";
    std::ofstream out(path);
    if (!out) return;

    out << "EVOLUTIONARY SEARCH FINALISTS\n";
    out << std::string(70, '=') << "\n\n";
    out << "Total finalists: " << finalists_.size() << "\n\n";

    for (size_t i = 0; i < finalists_.size(); ++i) {
        const auto& f = finalists_[i];
        out << "--- Finalist #" << (i + 1) << " ---\n";
        out << "  Genome: " << f.genome.describe() << "\n";
        out << "  Monthly return: " << (f.monthly_return * 100) << "%\n";
        out << "  Total return: " << (f.total_return * 100) << "%\n";
        out << "  Max drawdown: " << (f.max_drawdown * 100) << "%\n";
        out << "  Profit factor: " << f.profit_factor << "\n";
        out << "  Win rate: " << (f.win_rate * 100) << "%\n";
        out << "  Trade count: " << f.trade_count << "\n";
        out << "  Consistency: " << f.consistency << "\n";
        out << "  Composite score: " << f.composite_score << "\n";
        if (f.robustness_score > 0) {
            out << "  Robustness: " << f.robustness_score << "\n";
            out << "  OOS degradation: " << f.robustness.degradation_ratio << "\n";
            out << "  Window variance: " << f.robustness.window_variance << "\n";
        }
        out << "\n";
    }

    // Also write JSON for machine parsing
    auto json_path = config_.output_dir / "finalists.json";
    std::ofstream json(json_path);
    if (json) {
        json << "[\n";
        for (size_t i = 0; i < finalists_.size(); ++i) {
            if (i > 0) json << ",\n";
            json << "  " << finalists_[i].genome.serialize_json();
        }
        json << "\n]\n";
    }

    std::cout << "[evolution] Finalists written to " << path.string() << std::endl;
}


// ════════════════════════════════════════════════════════════
//  CHECKPOINT
// ════════════════════════════════════════════════════════════

void EvolutionEngine::save_checkpoint(const fs::path& path) const {
    std::ofstream out(path);
    if (!out) return;

    out << "{\"population_size\":" << population_.size()
        << ",\"next_id\":" << next_id_
        << ",\"best_composite\":" << best_composite_ever_
        << ",\"genomes\":[\n";

    for (size_t i = 0; i < population_.size(); ++i) {
        if (i > 0) out << ",\n";
        out << "  " << population_[i].serialize_json();
    }
    out << "\n]}\n";
}

void EvolutionEngine::load_checkpoint(const fs::path& /*path*/) {
    // TODO: full checkpoint loading for resume
}

} // namespace aphelion
