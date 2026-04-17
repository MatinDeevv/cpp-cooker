// ============================================================
// Aphelion Research — Strategy Genome Implementation
//
// Genome operations (random, mutate, crossover, simplify)
// and GenomeStrategy (IStrategy from genome).
// ============================================================

#include "aphelion/strategy_genome.h"
#include "aphelion/intelligence.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <cassert>

namespace aphelion {

// ════════════════════════════════════════════════════════════
//  INDICATOR CATEGORY RANGES
// ════════════════════════════════════════════════════════════

namespace {

struct CategoryRange { uint16_t first; uint16_t last; };

constexpr CategoryRange CAT_TREND     = {0, 19};
constexpr CategoryRange CAT_VOLATILITY= {20, 34};
constexpr CategoryRange CAT_STRUCTURE = {35, 49};
constexpr CategoryRange CAT_REGIME    = {50, 59};
constexpr CategoryRange CAT_FRICTION  = {60, 64};
constexpr CategoryRange CAT_HTF       = {65, 74};
constexpr CategoryRange CAT_PATTERN   = {75, 99};

constexpr CategoryRange ALL_CATEGORIES[] = {
    CAT_TREND, CAT_VOLATILITY, CAT_STRUCTURE, CAT_REGIME,
    CAT_FRICTION, CAT_HTF, CAT_PATTERN
};

// Pick a random indicator from a category
IndicatorParam random_indicator_from_category(const CategoryRange& cat, std::mt19937& rng) {
    std::uniform_int_distribution<uint16_t> id_dist(cat.first, cat.last);
    IndicatorId id = static_cast<IndicatorId>(id_dist(rng));
    const auto& meta = get_indicator_meta(id);

    IndicatorParam p;
    p.id = id;
    std::uniform_int_distribution<int16_t> period_dist(meta.min_period, meta.max_period);
    p.period = period_dist(rng);

    if (meta.needs_period2) {
        int16_t p2_min = std::max(meta.min_period, static_cast<int16_t>(p.period + 2));
        int16_t p2_max = std::min(meta.max_period, static_cast<int16_t>(p.period * 3));
        if (p2_min > p2_max) p2_max = p2_min;
        std::uniform_int_distribution<int16_t> p2_dist(p2_min, p2_max);
        p.period2 = p2_dist(rng);
    }

    return p;
}

// Pick a random indicator from any category
IndicatorParam random_indicator(std::mt19937& rng) {
    // Weight toward trend/structure/pattern categories
    static const float weights[] = {0.25f, 0.12f, 0.18f, 0.08f, 0.04f, 0.08f, 0.25f};
    std::discrete_distribution<int> cat_dist({weights[0], weights[1], weights[2],
        weights[3], weights[4], weights[5], weights[6]});
    int cat_idx = cat_dist(rng);
    return random_indicator_from_category(ALL_CATEGORIES[cat_idx], rng);
}

// Random threshold based on indicator's output range
float random_threshold(const IndicatorMeta& meta, std::mt19937& rng) {
    float range = meta.output_max - meta.output_min;
    std::uniform_real_distribution<float> dist(
        meta.output_min + range * 0.1f,
        meta.output_max - range * 0.1f
    );
    return dist(rng);
}

// Random condition
GenomeCondition random_condition(std::mt19937& rng) {
    GenomeCondition c;
    c.indicator = random_indicator(rng);

    const auto& meta = get_indicator_meta(c.indicator.id);

    // 20% chance of cross-indicator comparison
    std::uniform_real_distribution<float> coin(0.0f, 1.0f);
    if (coin(rng) < 0.2f) {
        c.compare_to_indicator = true;
        // Pick a related indicator (same or adjacent category)
        c.compare_indicator = random_indicator(rng);
        c.op = CompareOp::GREATER_THAN;
    } else {
        c.compare_to_indicator = false;

        // Pick comparison op
        std::uniform_int_distribution<int> op_dist(0, 3); // favor simple ops
        c.op = static_cast<CompareOp>(op_dist(rng));

        c.threshold = random_threshold(meta, rng);

        if (c.op == CompareOp::BETWEEN || c.op == CompareOp::OUTSIDE) {
            float t2 = random_threshold(meta, rng);
            if (t2 < c.threshold) std::swap(c.threshold, t2);
            c.threshold2 = t2;
        }
    }

    c.negate = (coin(rng) < 0.1f); // 10% chance of negation

    return c;
}

// Perturb a float value by percentage
float perturb(float v, float pct, std::mt19937& rng) {
    std::uniform_real_distribution<float> dist(1.0f - pct, 1.0f + pct);
    return v * dist(rng);
}

int16_t perturb_period(int16_t v, float pct, std::mt19937& rng) {
    std::uniform_real_distribution<float> dist(1.0f - pct, 1.0f + pct);
    int16_t result = static_cast<int16_t>(std::round(v * dist(rng)));
    return std::max(static_cast<int16_t>(2), result);
}

} // anonymous namespace


// ════════════════════════════════════════════════════════════
//  GENOME METHODS
// ════════════════════════════════════════════════════════════

int StrategyGenome::complexity() const {
    int c = 0;
    c += static_cast<int>(long_conditions.size());
    c += static_cast<int>(short_conditions.size());
    for (const auto& cond : long_conditions) {
        if (cond.compare_to_indicator) c += 1;
        if (cond.negate) c += 1;
        if (cond.op == CompareOp::BETWEEN || cond.op == CompareOp::OUTSIDE) c += 1;
    }
    for (const auto& cond : short_conditions) {
        if (cond.compare_to_indicator) c += 1;
        if (cond.negate) c += 1;
        if (cond.op == CompareOp::BETWEEN || cond.op == CompareOp::OUTSIDE) c += 1;
    }
    if (filter_regime) c += 2;
    if (use_htf_filter) c += 2;
    if (exit_mode != ExitMode::ATR_MULTIPLE) c += 1;
    if (sizing_mode != SizingMode::FIXED_RISK) c += 1;
    return c;
}

std::string StrategyGenome::describe() const {
    std::ostringstream ss;
    ss << "Genome[" << genome_id << "] gen=" << generation << " mut=" << mutation_count;
    ss << " long=" << long_conditions.size() << "/" << (int)min_conditions_long;
    ss << " short=" << short_conditions.size() << "/" << (int)min_conditions_short;
    ss << " SL=" << stop_atr_multiple << "xATR TP=" << target_atr_multiple << "xATR";
    ss << " hold<=" << max_hold_bars;
    if (filter_regime) ss << " [regime-filtered]";
    if (use_htf_filter) ss << " [htf-filtered]";
    ss << " complexity=" << complexity();
    return ss.str();
}

std::string StrategyGenome::serialize_json() const {
    // Minimal JSON for checkpointing — enough to reconstruct
    std::ostringstream ss;
    ss << "{\"id\":" << genome_id
       << ",\"gen\":" << generation
       << ",\"long_n\":" << long_conditions.size()
       << ",\"short_n\":" << short_conditions.size()
       << ",\"stop\":" << stop_atr_multiple
       << ",\"target\":" << target_atr_multiple
       << ",\"hold\":" << max_hold_bars
       << ",\"complexity\":" << complexity()
       << "}";
    return ss.str();
}

StrategyGenome StrategyGenome::deserialize_json(const std::string& /*json*/) {
    // TODO: full deserialization for checkpoint loading
    return StrategyGenome{};
}


// ════════════════════════════════════════════════════════════
//  GENOME OPERATIONS
// ════════════════════════════════════════════════════════════

StrategyGenome random_genome(std::mt19937& rng, uint64_t id) {
    StrategyGenome g;
    g.genome_id = id;
    g.generation = 0;

    std::uniform_int_distribution<int> cond_count(2, 5);
    std::uniform_real_distribution<float> f01(0.0f, 1.0f);
    std::uniform_real_distribution<float> atr_dist(1.0f, 5.0f);
    std::uniform_int_distribution<int> hold_dist(10, 200);

    // Long conditions
    int n_long = cond_count(rng);
    for (int i = 0; i < n_long; ++i)
        g.long_conditions.push_back(random_condition(rng));
    g.min_conditions_long = static_cast<uint8_t>(
        std::max(1, static_cast<int>(std::ceil(n_long * 0.6f))));

    // Short conditions
    int n_short = cond_count(rng);
    for (int i = 0; i < n_short; ++i)
        g.short_conditions.push_back(random_condition(rng));
    g.min_conditions_short = static_cast<uint8_t>(
        std::max(1, static_cast<int>(std::ceil(n_short * 0.6f))));

    // Bias mode
    std::uniform_int_distribution<int> bias_dist(0, 3);
    g.bias_mode = static_cast<BiasMode>(bias_dist(rng));
    g.bias_indicator = random_indicator_from_category(CAT_TREND, rng);
    g.bias_threshold = 0.0f;

    // Regime filter (50% chance)
    g.filter_regime = (f01(rng) < 0.5f);
    if (g.filter_regime) {
        // Allow 2-4 regimes randomly
        g.allowed_regimes = 0;
        for (int r = 1; r <= 6; ++r) {
            if (f01(rng) < 0.5f) g.allowed_regimes |= (1 << r);
        }
        if (g.allowed_regimes == 0) g.allowed_regimes = 0xFF; // at least allow all
    }

    // Exit params
    g.stop_atr_multiple = atr_dist(rng);
    g.target_atr_multiple = g.stop_atr_multiple * (1.0f + f01(rng)); // target >= stop
    g.max_hold_bars = static_cast<int16_t>(hold_dist(rng));
    g.trailing_activation = 1.0f + f01(rng) * 2.0f;
    g.trailing_distance = 0.5f + f01(rng) * 1.5f;

    // Exit mode
    std::uniform_int_distribution<int> exit_dist(0, 3);
    g.exit_mode = static_cast<ExitMode>(exit_dist(rng));

    // Risk sizing
    std::uniform_int_distribution<int> sizing_dist(0, 3);
    g.sizing_mode = static_cast<SizingMode>(sizing_dist(rng));
    g.base_risk_fraction = 0.005f + f01(rng) * 0.025f;
    g.confidence_threshold = 0.3f + f01(rng) * 0.4f;

    // HTF filter (30% chance)
    g.use_htf_filter = (f01(rng) < 0.3f);
    if (g.use_htf_filter) {
        g.htf_indicator = random_indicator_from_category(CAT_HTF, rng);
        std::uniform_int_distribution<int> htf_op_dist(0, 1);
        g.htf_op = static_cast<CompareOp>(htf_op_dist(rng));
        g.htf_threshold = 0.0f;
    }

    validate_genome(g);
    return g;
}

StrategyGenome mutate(const StrategyGenome& parent, std::mt19937& rng, float rate) {
    StrategyGenome g = parent;
    g.mutation_count++;
    g.parent_id1 = parent.genome_id;

    std::uniform_real_distribution<float> coin(0.0f, 1.0f);

    // Mutate long conditions
    for (auto& c : g.long_conditions) {
        if (coin(rng) < rate) {
            // Perturb period
            c.indicator.period = perturb_period(c.indicator.period, 0.3f, rng);
            const auto& meta = get_indicator_meta(c.indicator.id);
            c.indicator.period = std::clamp(c.indicator.period, meta.min_period, meta.max_period);
        }
        if (coin(rng) < rate * 0.5f) {
            // Perturb threshold
            c.threshold = perturb(c.threshold, 0.3f, rng);
        }
        if (coin(rng) < rate * 0.3f) {
            // Swap to different indicator
            c.indicator = random_indicator(rng);
            c.threshold = random_threshold(get_indicator_meta(c.indicator.id), rng);
        }
        if (coin(rng) < rate * 0.2f) {
            // Change operator
            std::uniform_int_distribution<int> op_dist(0, 3);
            c.op = static_cast<CompareOp>(op_dist(rng));
        }
    }

    // Mutate short conditions (same process)
    for (auto& c : g.short_conditions) {
        if (coin(rng) < rate) {
            c.indicator.period = perturb_period(c.indicator.period, 0.3f, rng);
            const auto& meta = get_indicator_meta(c.indicator.id);
            c.indicator.period = std::clamp(c.indicator.period, meta.min_period, meta.max_period);
        }
        if (coin(rng) < rate * 0.5f) c.threshold = perturb(c.threshold, 0.3f, rng);
        if (coin(rng) < rate * 0.3f) {
            c.indicator = random_indicator(rng);
            c.threshold = random_threshold(get_indicator_meta(c.indicator.id), rng);
        }
        if (coin(rng) < rate * 0.2f) {
            std::uniform_int_distribution<int> op_dist(0, 3);
            c.op = static_cast<CompareOp>(op_dist(rng));
        }
    }

    // Add/remove conditions
    if (coin(rng) < rate * 0.3f && g.long_conditions.size() < 8) {
        g.long_conditions.push_back(random_condition(rng));
    }
    if (coin(rng) < rate * 0.2f && g.long_conditions.size() > 1) {
        std::uniform_int_distribution<size_t> idx_dist(0, g.long_conditions.size() - 1);
        g.long_conditions.erase(g.long_conditions.begin() + idx_dist(rng));
    }
    if (coin(rng) < rate * 0.3f && g.short_conditions.size() < 8) {
        g.short_conditions.push_back(random_condition(rng));
    }
    if (coin(rng) < rate * 0.2f && g.short_conditions.size() > 1) {
        std::uniform_int_distribution<size_t> idx_dist(0, g.short_conditions.size() - 1);
        g.short_conditions.erase(g.short_conditions.begin() + idx_dist(rng));
    }

    // Mutate exit params
    if (coin(rng) < rate) g.stop_atr_multiple = perturb(g.stop_atr_multiple, 0.2f, rng);
    if (coin(rng) < rate) g.target_atr_multiple = perturb(g.target_atr_multiple, 0.2f, rng);
    if (coin(rng) < rate) g.max_hold_bars = perturb_period(g.max_hold_bars, 0.3f, rng);

    // Mutate regime filter
    if (coin(rng) < rate * 0.2f) g.filter_regime = !g.filter_regime;
    if (coin(rng) < rate * 0.1f && g.filter_regime) {
        // Flip a random regime bit
        std::uniform_int_distribution<int> bit_dist(1, 6);
        g.allowed_regimes ^= (1 << bit_dist(rng));
    }

    // Mutate HTF filter
    if (coin(rng) < rate * 0.15f) g.use_htf_filter = !g.use_htf_filter;

    // Mutate risk
    if (coin(rng) < rate * 0.3f) g.base_risk_fraction = perturb(g.base_risk_fraction, 0.3f, rng);

    validate_genome(g);
    return g;
}

StrategyGenome crossover(const StrategyGenome& p1, const StrategyGenome& p2, std::mt19937& rng) {
    StrategyGenome g;
    g.parent_id1 = p1.genome_id;
    g.parent_id2 = p2.genome_id;

    std::uniform_real_distribution<float> coin(0.0f, 1.0f);

    // Conditions: pick from both parents
    // Long conditions: take some from p1, some from p2
    for (const auto& c : p1.long_conditions) {
        if (coin(rng) < 0.5f) g.long_conditions.push_back(c);
    }
    for (const auto& c : p2.long_conditions) {
        if (coin(rng) < 0.5f) g.long_conditions.push_back(c);
    }
    if (g.long_conditions.empty()) {
        // At least one condition from each parent
        if (!p1.long_conditions.empty())
            g.long_conditions.push_back(p1.long_conditions[0]);
        else if (!p2.long_conditions.empty())
            g.long_conditions.push_back(p2.long_conditions[0]);
        else
            g.long_conditions.push_back(random_condition(rng));
    }

    // Short conditions: same process
    for (const auto& c : p1.short_conditions) {
        if (coin(rng) < 0.5f) g.short_conditions.push_back(c);
    }
    for (const auto& c : p2.short_conditions) {
        if (coin(rng) < 0.5f) g.short_conditions.push_back(c);
    }
    if (g.short_conditions.empty()) {
        if (!p1.short_conditions.empty())
            g.short_conditions.push_back(p1.short_conditions[0]);
        else if (!p2.short_conditions.empty())
            g.short_conditions.push_back(p2.short_conditions[0]);
        else
            g.short_conditions.push_back(random_condition(rng));
    }

    // Min conditions: from either parent
    g.min_conditions_long = (coin(rng) < 0.5f) ? p1.min_conditions_long : p2.min_conditions_long;
    g.min_conditions_short = (coin(rng) < 0.5f) ? p1.min_conditions_short : p2.min_conditions_short;

    // Exit params: blend or pick
    g.stop_atr_multiple = (coin(rng) < 0.5f) ? p1.stop_atr_multiple : p2.stop_atr_multiple;
    g.target_atr_multiple = (coin(rng) < 0.5f) ? p1.target_atr_multiple : p2.target_atr_multiple;
    g.max_hold_bars = (coin(rng) < 0.5f) ? p1.max_hold_bars : p2.max_hold_bars;
    g.exit_mode = (coin(rng) < 0.5f) ? p1.exit_mode : p2.exit_mode;

    // Bias/regime/HTF: from either parent
    g.bias_mode = (coin(rng) < 0.5f) ? p1.bias_mode : p2.bias_mode;
    g.filter_regime = (coin(rng) < 0.5f) ? p1.filter_regime : p2.filter_regime;
    g.allowed_regimes = (coin(rng) < 0.5f) ? p1.allowed_regimes : p2.allowed_regimes;
    g.use_htf_filter = (coin(rng) < 0.5f) ? p1.use_htf_filter : p2.use_htf_filter;
    g.htf_indicator = (coin(rng) < 0.5f) ? p1.htf_indicator : p2.htf_indicator;
    g.htf_op = (coin(rng) < 0.5f) ? p1.htf_op : p2.htf_op;
    g.htf_threshold = (coin(rng) < 0.5f) ? p1.htf_threshold : p2.htf_threshold;

    // Risk: from either parent
    g.sizing_mode = (coin(rng) < 0.5f) ? p1.sizing_mode : p2.sizing_mode;
    g.base_risk_fraction = (coin(rng) < 0.5f) ? p1.base_risk_fraction : p2.base_risk_fraction;

    validate_genome(g);
    return g;
}

StrategyGenome simplify(const StrategyGenome& genome) {
    StrategyGenome g = genome;

    // Remove redundant conditions (duplicate indicators with same params)
    auto dedup = [](std::vector<GenomeCondition>& conds) {
        for (size_t i = 0; i < conds.size(); ++i) {
            for (size_t j = i + 1; j < conds.size();) {
                if (conds[i].indicator == conds[j].indicator &&
                    conds[i].op == conds[j].op) {
                    conds.erase(conds.begin() + j);
                } else {
                    ++j;
                }
            }
        }
    };
    dedup(g.long_conditions);
    dedup(g.short_conditions);

    // Cap conditions at 8
    while (g.long_conditions.size() > 8) g.long_conditions.pop_back();
    while (g.short_conditions.size() > 8) g.short_conditions.pop_back();

    validate_genome(g);
    return g;
}

bool validate_genome(StrategyGenome& g) {
    bool changed = false;

    // Ensure at least one condition per side
    if (g.long_conditions.empty()) {
        g.long_conditions.push_back(GenomeCondition{});
        g.long_conditions.back().indicator.id = IndicatorId::RSI;
        g.long_conditions.back().indicator.period = 14;
        g.long_conditions.back().op = CompareOp::LESS_THAN;
        g.long_conditions.back().threshold = 0.3f;
        changed = true;
    }
    if (g.short_conditions.empty()) {
        g.short_conditions.push_back(GenomeCondition{});
        g.short_conditions.back().indicator.id = IndicatorId::RSI;
        g.short_conditions.back().indicator.period = 14;
        g.short_conditions.back().op = CompareOp::GREATER_THAN;
        g.short_conditions.back().threshold = 0.7f;
        changed = true;
    }

    // Fix min_conditions bounds
    g.min_conditions_long = std::clamp(g.min_conditions_long,
        static_cast<uint8_t>(1),
        static_cast<uint8_t>(g.long_conditions.size()));
    g.min_conditions_short = std::clamp(g.min_conditions_short,
        static_cast<uint8_t>(1),
        static_cast<uint8_t>(g.short_conditions.size()));

    // Clamp exit params
    g.stop_atr_multiple = std::clamp(g.stop_atr_multiple, 0.5f, 10.0f);
    g.target_atr_multiple = std::clamp(g.target_atr_multiple, 0.5f, 15.0f);
    g.max_hold_bars = std::clamp(g.max_hold_bars, static_cast<int16_t>(5), static_cast<int16_t>(500));

    // Clamp risk
    g.base_risk_fraction = std::clamp(g.base_risk_fraction, 0.001f, 0.05f);
    g.confidence_threshold = std::clamp(g.confidence_threshold, 0.1f, 0.9f);

    // Ensure regime bitmask has at least one bit set
    if (g.filter_regime && g.allowed_regimes == 0)
        g.allowed_regimes = 0xFF;

    // Validate indicator periods
    for (auto* conds : {&g.long_conditions, &g.short_conditions}) {
        for (auto& c : *conds) {
            const auto& meta = get_indicator_meta(c.indicator.id);
            c.indicator.period = std::clamp(c.indicator.period, meta.min_period, meta.max_period);
            if (c.compare_to_indicator) {
                const auto& cmeta = get_indicator_meta(c.compare_indicator.id);
                c.compare_indicator.period = std::clamp(c.compare_indicator.period,
                    cmeta.min_period, cmeta.max_period);
            }
        }
    }

    return changed;
}


// ════════════════════════════════════════════════════════════
//  GENOME STRATEGY
// ════════════════════════════════════════════════════════════

GenomeStrategy::GenomeStrategy(const StrategyGenome& genome)
    : genome_(genome) {}

void GenomeStrategy::prepare(const Bar* tape, size_t tape_size) {
    prepare_with_intelligence(tape, tape_size, nullptr);
}

void GenomeStrategy::prepare_with_intelligence(
    const Bar* tape, size_t tape_size,
    const IntelligenceState* intelligence
) {
    tape_size_ = tape_size;
    intelligence_ = intelligence;

    // Collect all unique indicators needed
    std::vector<IndicatorParam> needed;
    auto add_unique = [&](const IndicatorParam& p) {
        for (const auto& existing : needed) {
            if (existing == p) return;
        }
        needed.push_back(p);
    };

    for (const auto& c : genome_.long_conditions) {
        add_unique(c.indicator);
        if (c.compare_to_indicator) add_unique(c.compare_indicator);
    }
    for (const auto& c : genome_.short_conditions) {
        add_unique(c.indicator);
        if (c.compare_to_indicator) add_unique(c.compare_indicator);
    }
    if (genome_.use_htf_filter) add_unique(genome_.htf_indicator);
    add_unique(genome_.bias_indicator);

    // Compute all indicator tapes
    indicator_tapes_ = compute_indicators_batch(tape, tape_size, needed, intelligence);

    // Compute ATR tape for stop/target calculation
    atr_tape_.resize(tape_size, 0.0f);
    if (tape_size > 0) {
        float alpha = 1.0f / 14.0f;
        atr_tape_[0] = static_cast<float>(tape[0].high - tape[0].low);
        for (size_t i = 1; i < tape_size; ++i) {
            double hl = tape[i].high - tape[i].low;
            double hc = std::fabs(tape[i].high - tape[i - 1].close);
            double lc = std::fabs(tape[i].low - tape[i - 1].close);
            float tr = static_cast<float>(std::max({hl, hc, lc}));
            atr_tape_[i] = atr_tape_[i - 1] + alpha * (tr - atr_tape_[i - 1]);
        }
    }

    // Compile signal tape
    compile_signal_tape(tape, tape_size);
}

int GenomeStrategy::find_tape_index(const IndicatorParam& p) const {
    for (size_t i = 0; i < indicator_tapes_.size(); ++i) {
        if (indicator_tapes_[i].param == p) return static_cast<int>(i);
    }
    return -1;
}

float GenomeStrategy::get_indicator_value(const IndicatorParam& p, size_t bar_idx) const {
    int idx = find_tape_index(p);
    if (idx < 0) return 0.0f;
    return indicator_tapes_[idx].at(bar_idx);
}

bool GenomeStrategy::evaluate_conditions(
    const std::vector<GenomeCondition>& conds,
    uint8_t min_match,
    size_t bar_idx
) const {
    if (conds.empty()) return false;

    int passing = 0;
    for (const auto& c : conds) {
        float value = get_indicator_value(c.indicator, bar_idx);
        float compare_val = c.threshold;

        if (c.compare_to_indicator) {
            compare_val = get_indicator_value(c.compare_indicator, bar_idx);
        }

        bool result = false;
        switch (c.op) {
            case CompareOp::GREATER_THAN:
                result = value > compare_val;
                break;
            case CompareOp::LESS_THAN:
                result = value < compare_val;
                break;
            case CompareOp::CROSSES_ABOVE:
                if (bar_idx > 0) {
                    float prev = get_indicator_value(c.indicator, bar_idx - 1);
                    float prev_cmp = c.compare_to_indicator ?
                        get_indicator_value(c.compare_indicator, bar_idx - 1) : compare_val;
                    result = (prev < prev_cmp) && (value >= compare_val);
                }
                break;
            case CompareOp::CROSSES_BELOW:
                if (bar_idx > 0) {
                    float prev = get_indicator_value(c.indicator, bar_idx - 1);
                    float prev_cmp = c.compare_to_indicator ?
                        get_indicator_value(c.compare_indicator, bar_idx - 1) : compare_val;
                    result = (prev >= prev_cmp) && (value < compare_val);
                }
                break;
            case CompareOp::BETWEEN:
                result = (value >= c.threshold && value <= c.threshold2);
                break;
            case CompareOp::OUTSIDE:
                result = (value < c.threshold || value > c.threshold2);
                break;
        }

        if (c.negate) result = !result;
        if (result) passing++;
    }

    return passing >= static_cast<int>(min_match);
}

void GenomeStrategy::compile_signal_tape(const Bar* bars, size_t n) {
    signal_tape_.resize(n, static_cast<uint8_t>(Signal::NONE));

    size_t warmup = min_signal_bar();
    bool prev_long = false, prev_short = false;

    for (size_t i = warmup; i < n; ++i) {
        // Regime filter
        if (genome_.filter_regime && intelligence_) {
            uint8_t regime_bit = 1 << static_cast<uint8_t>(intelligence_[i].regime);
            if (!(genome_.allowed_regimes & regime_bit)) {
                prev_long = false;
                prev_short = false;
                continue;
            }
        }

        // HTF filter
        if (genome_.use_htf_filter) {
            float htf_val = get_indicator_value(genome_.htf_indicator, i);
            bool htf_pass = false;
            switch (genome_.htf_op) {
                case CompareOp::GREATER_THAN: htf_pass = htf_val > genome_.htf_threshold; break;
                case CompareOp::LESS_THAN: htf_pass = htf_val < genome_.htf_threshold; break;
                default: htf_pass = true; break;
            }
            if (!htf_pass) {
                prev_long = false;
                prev_short = false;
                continue;
            }
        }

        bool curr_long = evaluate_conditions(genome_.long_conditions,
            genome_.min_conditions_long, i);
        bool curr_short = evaluate_conditions(genome_.short_conditions,
            genome_.min_conditions_short, i);

        // State-transition signals (only fire on edge)
        if (curr_long && !prev_long) {
            signal_tape_[i] = static_cast<uint8_t>(Signal::BULLISH_CROSS);
        } else if (curr_short && !prev_short) {
            signal_tape_[i] = static_cast<uint8_t>(Signal::BEARISH_CROSS);
        }

        prev_long = curr_long;
        prev_short = curr_short;
    }
}

size_t GenomeStrategy::min_signal_bar() const {
    size_t max_period = 50; // default minimum warmup
    auto check_param = [&](const IndicatorParam& p) {
        max_period = std::max(max_period, static_cast<size_t>(p.period));
        if (p.period2 > 0) max_period = std::max(max_period, static_cast<size_t>(p.period2));
    };
    for (const auto& c : genome_.long_conditions) {
        check_param(c.indicator);
        if (c.compare_to_indicator) check_param(c.compare_indicator);
    }
    for (const auto& c : genome_.short_conditions) {
        check_param(c.indicator);
        if (c.compare_to_indicator) check_param(c.compare_indicator);
    }
    return max_period + 1;
}

StrategyDecision GenomeStrategy::decide(
    const MarketState& market,
    const AccountState& account,
    size_t bar_index
) {
    // Fallback non-intelligence path
    Signal sig = signal_at(bar_index);
    return build_decision_from_signal(
        sig,
        market.current_bar->close,
        market.current_bar->high,
        market.current_bar->low,
        account.open_position_count, account.risk_per_trade);
}

StrategyDecision GenomeStrategy::decide_with_intelligence(
    const MarketState& market,
    const AccountState& account,
    size_t bar_index,
    const IntelligenceState& intelligence
) {
    StrategyDecision d;
    d.action = ActionType::HOLD;
    d.confidence = Confidence::NONE;

    Signal sig = signal_at(bar_index);
    if (sig == Signal::NONE) return d;

    bool has_position = (account.open_position_count > 0);
    float atr = (bar_index < atr_tape_.size()) ? atr_tape_[bar_index] : 0.0f;
    if (atr < 1e-6f) atr = static_cast<float>(market.current_bar->high - market.current_bar->low);
    if (atr < 1e-6f) atr = static_cast<float>(market.current_bar->close * 0.001);

    // Compute confidence from condition match quality
    int long_passing = 0, short_passing = 0;
    for (const auto& c : genome_.long_conditions) {
        float v = get_indicator_value(c.indicator, bar_index);
        // Simple check if value is on the "right side" of threshold
        bool ok = (c.op == CompareOp::GREATER_THAN) ? v > c.threshold : v < c.threshold;
        if (ok) long_passing++;
    }
    for (const auto& c : genome_.short_conditions) {
        float v = get_indicator_value(c.indicator, bar_index);
        bool ok = (c.op == CompareOp::GREATER_THAN) ? v > c.threshold : v < c.threshold;
        if (ok) short_passing++;
    }

    float match_ratio = 0.0f;
    if (sig == Signal::BULLISH_CROSS && !genome_.long_conditions.empty()) {
        match_ratio = static_cast<float>(long_passing) / genome_.long_conditions.size();
    } else if (sig == Signal::BEARISH_CROSS && !genome_.short_conditions.empty()) {
        match_ratio = static_cast<float>(short_passing) / genome_.short_conditions.size();
    }

    // Map match ratio to confidence
    if (match_ratio > 0.85f) d.confidence = Confidence::HIGH;
    else if (match_ratio > 0.6f) d.confidence = Confidence::MEDIUM;
    else d.confidence = Confidence::LOW;

    // Add intelligence context modulation
    if (intelligence.stability_score > 0.7f && intelligence.cross_timeframe_coherence > 0.6f) {
        d.confidence = static_cast<Confidence>(
            std::min(static_cast<int>(d.confidence) + 1,
                     static_cast<int>(Confidence::EXTREME)));
    }

    // Risk fraction
    d.risk_fraction = genome_.base_risk_fraction;

    // Regime preference
    d.preferred_regime = intelligence.regime;

    // Intelligence-based aggression
    d.entry_aggression = intelligence.entry_aggression;
    d.exit_urgency = intelligence.exit_urgency;
    d.context_pressure = (sig == Signal::BULLISH_CROSS) ?
        intelligence.long_pressure : intelligence.short_pressure;

    if (sig == Signal::BULLISH_CROSS) {
        if (!has_position) {
            d.action = ActionType::OPEN_LONG;
            d.stop_loss = market.current_bar->close - atr * genome_.stop_atr_multiple;
            d.take_profit = market.current_bar->close + atr * genome_.target_atr_multiple;
        } else {
            d.action = ActionType::CLOSE;
        }
    } else if (sig == Signal::BEARISH_CROSS) {
        if (!has_position) {
            d.action = ActionType::OPEN_SHORT;
            d.stop_loss = market.current_bar->close + atr * genome_.stop_atr_multiple;
            d.take_profit = market.current_bar->close - atr * genome_.target_atr_multiple;
        } else {
            d.action = ActionType::CLOSE;
        }
    }

    // Expected hold
    d.expected_hold_bars = static_cast<uint16_t>(std::min(
        static_cast<int>(genome_.max_hold_bars), 500));
    d.minimum_hold_bars = std::min(static_cast<uint16_t>(5), d.expected_hold_bars);

    // Stop/TP multipliers for risk manager
    d.stop_width_multiplier = genome_.stop_atr_multiple / 2.0f;
    d.take_profit_multiplier = genome_.target_atr_multiple / 3.0f;

    return d;
}

} // namespace aphelion
