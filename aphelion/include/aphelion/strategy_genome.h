#pragma once
// ============================================================
// Aphelion Research — Strategy Genome / Grammar / DSL
//
// Structured strategy representation for evolutionary search.
// Each genome encodes:
//   - Entry conditions (indicator-based, AND logic with min-match)
//   - Direction bias mode
//   - Regime filter
//   - Exit parameters (ATR-based stops/targets)
//   - Risk sizing
//   - Multi-timeframe filter
//
// Genomes are machine-generable, machine-mutable, and
// human-auditable. They compile into GenomeStrategy (IStrategy)
// for evaluation on the existing replay engine.
// ============================================================

#include "aphelion/indicator_library.h"
#include "aphelion/strategy.h"
#include <vector>
#include <string>
#include <random>
#include <cstdint>

namespace aphelion {

// ── Comparison operators ────────────────────────────────────
enum class CompareOp : uint8_t {
    GREATER_THAN  = 0,
    LESS_THAN     = 1,
    CROSSES_ABOVE = 2,   // prev < threshold, now >= threshold
    CROSSES_BELOW = 3,   // prev >= threshold, now < threshold
    BETWEEN       = 4,   // threshold <= value <= threshold2
    OUTSIDE       = 5    // value < threshold || value > threshold2
};

// ── Single condition in entry/exit logic ────────────────────
struct GenomeCondition {
    IndicatorParam indicator;
    CompareOp      op         = CompareOp::GREATER_THAN;
    float          threshold  = 0.0f;
    float          threshold2 = 0.0f;   // for BETWEEN / OUTSIDE
    bool           negate     = false;

    // Compare against another indicator instead of threshold
    bool           compare_to_indicator = false;
    IndicatorParam compare_indicator;
};

// ── Direction bias mode ─────────────────────────────────────
enum class BiasMode : uint8_t {
    TREND_FOLLOWING = 0,
    MEAN_REVERSION  = 1,
    MOMENTUM        = 2,
    ADAPTIVE        = 3
};

// ── Exit mode ───────────────────────────────────────────────
enum class ExitMode : uint8_t {
    ATR_MULTIPLE = 0,
    VOLATILITY_SCALED = 1,
    STRUCTURE_BASED = 2,
    COMPOSITE = 3
};

// ── Risk sizing mode ────────────────────────────────────────
enum class SizingMode : uint8_t {
    FIXED_RISK        = 0,
    VOL_SCALED        = 1,
    CONFIDENCE_SCALED = 2,
    REGIME_ADAPTIVE   = 3
};

// ── Strategy Genome: the complete DNA ───────────────────────
struct StrategyGenome {
    // === Identity ===
    uint64_t genome_id    = 0;
    uint64_t parent_id1   = 0;
    uint64_t parent_id2   = 0;
    uint32_t generation   = 0;
    uint32_t mutation_count = 0;

    // === Entry Logic ===
    std::vector<GenomeCondition> long_conditions;
    std::vector<GenomeCondition> short_conditions;
    uint8_t min_conditions_long  = 1;   // min conditions that must pass
    uint8_t min_conditions_short = 1;

    // === Direction Bias ===
    BiasMode       bias_mode = BiasMode::TREND_FOLLOWING;
    IndicatorParam bias_indicator;
    float          bias_threshold = 0.0f;

    // === Regime Filter ===
    bool    filter_regime   = false;
    uint8_t allowed_regimes = 0xFF;  // bitmask: bit i = Regime(i) allowed

    // === Exit Logic ===
    ExitMode exit_mode            = ExitMode::ATR_MULTIPLE;
    float    stop_atr_multiple    = 2.0f;
    float    target_atr_multiple  = 3.0f;
    int16_t  max_hold_bars        = 50;
    float    trailing_activation  = 1.5f;
    float    trailing_distance    = 1.0f;

    // === Risk Sizing ===
    SizingMode sizing_mode        = SizingMode::FIXED_RISK;
    float      base_risk_fraction = 0.01f;
    float      confidence_threshold = 0.4f;

    // === Multi-Timeframe Filter ===
    bool           use_htf_filter = false;
    IndicatorParam htf_indicator;
    CompareOp      htf_op         = CompareOp::GREATER_THAN;
    float          htf_threshold  = 0.0f;

    // === Metrics ===
    int complexity() const;
    std::string describe() const;
    std::string serialize_json() const;
    static StrategyGenome deserialize_json(const std::string& json);
};

// ── Genome operations ───────────────────────────────────────
StrategyGenome random_genome(std::mt19937& rng, uint64_t id);
StrategyGenome mutate(const StrategyGenome& parent, std::mt19937& rng, float rate = 0.15f);
StrategyGenome crossover(const StrategyGenome& p1, const StrategyGenome& p2, std::mt19937& rng);
StrategyGenome simplify(const StrategyGenome& genome);
bool validate_genome(StrategyGenome& genome);

// ── GenomeStrategy: IStrategy from genome ───────────────────
class GenomeStrategy : public IStrategy {
public:
    explicit GenomeStrategy(const StrategyGenome& genome);

    StrategyDecision decide(
        const MarketState& market,
        const AccountState& account,
        size_t bar_index
    ) override;

    StrategyDecision decide_with_intelligence(
        const MarketState& market,
        const AccountState& account,
        size_t bar_index,
        const IntelligenceState& intelligence
    ) override;

    const char* name() const override { return "Genome"; }

    void prepare(const Bar* tape, size_t tape_size) override;
    void prepare_with_intelligence(
        const Bar* tape, size_t tape_size,
        const IntelligenceState* intelligence
    ) override;

    Signal signal_at(size_t bar_index) const override {
        return (bar_index < signal_tape_.size())
            ? static_cast<Signal>(signal_tape_[bar_index])
            : Signal::NONE;
    }
    bool has_signal_tape() const override { return !signal_tape_.empty(); }
    size_t min_signal_bar() const override;
    bool is_intelligence_aware() const override { return true; }

    const StrategyGenome& genome() const { return genome_; }

private:
    StrategyGenome genome_;
    std::vector<uint8_t> signal_tape_;
    std::vector<IndicatorTape> indicator_tapes_;
    std::vector<float> atr_tape_;
    const IntelligenceState* intelligence_ = nullptr;
    size_t tape_size_ = 0;

    void compile_signal_tape(const Bar* bars, size_t n);
    bool evaluate_conditions(
        const std::vector<GenomeCondition>& conds,
        uint8_t min_match,
        size_t bar_idx
    ) const;
    float get_indicator_value(const IndicatorParam& p, size_t bar_idx) const;
    int find_tape_index(const IndicatorParam& p) const;
};

} // namespace aphelion
