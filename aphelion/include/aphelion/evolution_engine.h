#pragma once
// ============================================================
// Aphelion Research — Evolution Engine
//
// Large-scale strategy discovery via evolutionary search.
// Generates, evaluates, selects, mutates, and recombines
// strategy genomes using the existing replay infrastructure.
//
// Phases:
//   A. Broad Discovery    — random init, shallow eval, kill garbage
//   B. Local Refinement   — mutate survivors, refine params
//   C. Robustness Eval    — walk-forward on top-k
//   D. Finalist Extraction — preserve shortlist for review
// ============================================================

#include "aphelion/strategy_genome.h"
#include "aphelion/robustness_validator.h"
#include "aphelion/data_ingest.h"
#include "aphelion/intelligence.h"
#include "aphelion/risk_manager.h"
#include <vector>
#include <string>
#include <random>
#include <filesystem>

namespace aphelion {

// ── Candidate score ─────────────────────────────────────────
struct CandidateScore {
    uint64_t        genome_id       = 0;
    double          total_return    = 0.0;
    double          max_drawdown    = 0.0;
    double          profit_factor   = 0.0;
    double          expectancy      = 0.0;
    double          win_rate        = 0.0;
    int             trade_count     = 0;
    double          consistency     = 0.0;
    double          monthly_return  = 0.0;
    double          composite_score = 0.0;
    double          robustness_score = 0.0;
    bool            liquidated      = false;
    bool            passes_threshold = false;

    StrategyGenome  genome;
    RobustnessResult robustness;
};

// ── Evolution configuration ─────────────────────────────────
struct EvolutionConfig {
    // Population
    int      population_size   = 500;
    int      elite_count       = 20;
    int      generations       = 100;

    // Genetic operators
    float    mutation_rate      = 0.15f;
    float    crossover_rate     = 0.60f;
    int      tournament_k       = 5;

    // Acceptance thresholds
    double   min_monthly_return = 0.20;
    double   max_drawdown_limit = 0.30;
    double   min_profit_factor  = 1.3;
    double   min_expectancy     = 0.0;
    double   min_win_rate       = 0.25;
    int      min_trade_count    = 20;
    double   min_consistency    = 0.3;

    // Complexity control
    int      max_conditions     = 8;
    float    complexity_penalty = 0.05f;

    // Robustness
    int      robustness_top_k   = 50;
    RobustnessConfig robustness_config;
    bool     enable_robustness_eval = true;
    bool     enable_finalist_extraction = true;

    // Performance
    int      eval_batch_size    = 50;

    // Simulation
    double   initial_balance    = 10000.0;
    double   max_leverage       = 500.0;
    double   risk_per_trade     = 0.01;
    double   stop_out_level     = 50.0;
    double   commission         = 0.0;
    double   slippage           = 0.0;
    RiskConfig risk_config;

    // Output
    std::filesystem::path output_dir = "evolution_output";
    bool     checkpoint_enabled = true;
    int      checkpoint_interval = 10;
    bool     verbose            = false;

    // Seed
    uint64_t random_seed        = 42;
};

// ── Evolution Engine ────────────────────────────────────────
class EvolutionEngine {
public:
    EvolutionEngine(
        const EvolutionConfig& config,
        const BarTape& primary_tape,
        const std::vector<MultiTimeframeInput>& context_inputs,
        const FeatureConfig& feature_config = FeatureConfig{},
        const RegimeConfig& regime_config = RegimeConfig{},
        const EchConfig& ech_config = EchConfig{}
    );

    void run();
    void run_generation(int gen);

    const std::vector<CandidateScore>& population_scores() const { return scores_; }
    const std::vector<CandidateScore>& finalists() const { return finalists_; }

    void save_checkpoint(const std::filesystem::path& path) const;
    void load_checkpoint(const std::filesystem::path& path);

private:
    EvolutionConfig                config_;
    const BarTape&                 primary_tape_;
    std::vector<MultiTimeframeInput> context_inputs_;
    FeatureConfig                  feature_config_;
    RegimeConfig                   regime_config_;
    EchConfig                      ech_config_;

    IntelligenceTape               intelligence_tape_;
    std::vector<StrategyGenome>    population_;
    std::vector<CandidateScore>    scores_;
    std::vector<CandidateScore>    finalists_;
    std::mt19937                   rng_;
    uint64_t                       next_id_ = 1;

    // Best score tracking
    double                         best_composite_ever_ = -1e9;

    void initialize_population();
    void evaluate_population();
    CandidateScore evaluate_candidate(
        const StrategyGenome& genome,
        const Bar* bars, size_t num_bars,
        const IntelligenceState* intelligence
    );
    void evaluate_batch(
        const std::vector<StrategyGenome>& genomes,
        const Bar* bars, size_t num_bars,
        const IntelligenceState* intelligence,
        std::vector<CandidateScore>& out_scores
    );
    void select_and_reproduce();
    double compute_composite(const CandidateScore& s) const;
    void write_generation_summary(int gen);
    void write_finalist_report();
};

} // namespace aphelion
