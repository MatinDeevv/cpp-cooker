// ============================================================
// Aphelion Research — Tournament Orchestration V3
// Feature computation, regime classification, composite scoring
// ============================================================

#include "aphelion/tournament.h"
#include "aphelion/replay_engine.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numeric>

namespace aphelion {

Tournament::Tournament(const TournamentConfig& config, const BarTape& tape)
    : config_(config), tape_(tape) {}

void Tournament::initialize() {
    entries_.clear();
    entries_.resize(config_.num_accounts);

    int fast_range = config_.fast_period_max - config_.fast_period_min;
    int slow_range = config_.slow_period_max - config_.slow_period_min;

    for (int i = 0; i < config_.num_accounts; ++i) {
        auto& entry = entries_[i];

        double t = (config_.num_accounts > 1)
                   ? static_cast<double>(i) / (config_.num_accounts - 1)
                   : 0.5;

        entry.params.initial_balance    = config_.initial_balance;
        entry.params.max_leverage       = config_.max_leverage;
        entry.params.risk_per_trade     = config_.risk_per_trade;
        entry.params.stop_out_level     = config_.stop_out_level;
        entry.params.commission_per_lot = config_.commission;
        entry.params.slippage_points    = config_.slippage;
        entry.params.max_positions      = config_.max_positions;
        entry.params.strategy_id        = config_.strategy_id;

        entry.params.fast_period = config_.fast_period_min +
            static_cast<int>(t * fast_range);
        entry.params.slow_period = config_.slow_period_min +
            static_cast<int>(t * slow_range);

        if (entry.params.fast_period >= entry.params.slow_period) {
            entry.params.slow_period = entry.params.fast_period + 10;
        }

        entry.account.init(static_cast<uint32_t>(i), entry.params);
        entry.strategy = create_strategy(entry.params);
    }

    std::cout << "[tournament] Initialized " << config_.num_accounts
              << " accounts (strategy=" << config_.strategy_id
              << ") with SMA periods [" << config_.fast_period_min
              << "-" << config_.fast_period_max << "] / ["
              << config_.slow_period_min << "-" << config_.slow_period_max << "]"
              << std::endl;
}

void Tournament::run() {
    if (entries_.empty()) {
        std::cerr << "[tournament] No entries to run!" << std::endl;
        return;
    }
    if (tape_.bars.empty()) {
        std::cerr << "[tournament] No bars to replay!" << std::endl;
        return;
    }

    const Bar* tape_ptr = tape_.bars.data();
    size_t tape_size    = tape_.bars.size();

    std::cout << "[tournament] Building unified intelligence tape..." << std::flush;
    intelligence_tape_ = build_intelligence_tape(
        tape_,
        config_.context_inputs.empty() ? nullptr : config_.context_inputs.data(),
        config_.context_inputs.size(),
        config_.feature_config,
        config_.regime_config
    );
    std::cout << " done" << std::endl;

    // ── Prepare strategies with shared intelligence ─────────
    std::cout << "[tournament] Preparing strategies..." << std::flush;
    for (auto& entry : entries_) {
        if (entry.strategy->is_intelligence_aware() && !intelligence_tape_.empty()) {
            entry.strategy->prepare_with_intelligence(
                tape_ptr, tape_size, intelligence_tape_.data()
            );
        } else {
            entry.strategy->prepare(tape_ptr, tape_size);
        }
    }
    std::cout << " done" << std::endl;

    // Build replay entries
    std::vector<ReplayEntry> replay_entries;
    replay_entries.reserve(entries_.size());
    for (auto& entry : entries_) {
        replay_entries.push_back({
            &entry.account,
            entry.strategy.get(),
            &entry.params
        });
    }

    std::cout << "[tournament] Running replay over " << tape_size
              << " bars x " << entries_.size() << " accounts..." << std::endl;

    // ── V3: Use run_replay_v3 with features and risk config ─
    const IntelligenceState* intelligence_ptr = intelligence_tape_.empty() ? nullptr : intelligence_tape_.data();
    ReplayStats stats = run_replay_v3(
        tape_ptr, tape_size, replay_entries, config_.mode,
        intelligence_ptr, config_.risk_config
    );

    last_stats_ = stats;

    // Close remaining positions at last bar
    MarketState final_market;
    final_market.bar_index   = tape_size - 1;
    final_market.current_bar = &tape_ptr[tape_size - 1];
    final_market.prev_bar    = (tape_size > 1) ? &tape_ptr[tape_size - 2] : nullptr;
    final_market.total_bars  = tape_size;
    final_market.tape_begin  = tape_ptr;

    for (auto& entry : entries_) {
        for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
            if (!entry.account.positions[i].active) continue;
            double exit_price = (entry.account.positions[i].direction == Direction::LONG)
                                ? final_market.bid()
                                : final_market.ask();
            close_position(entry.account, i, exit_price, ExitReason::END_OF_DATA, final_market,
                           entry.params.commission_per_lot);
        }
        entry.account.mark_to_market(final_market);
    }

    double bars_per_sec = stats.bars_processed / std::max(stats.elapsed_seconds, 0.001);
    double acct_bars_per_sec = (stats.bars_processed * entries_.size()) / std::max(stats.elapsed_seconds, 0.001);

    if (config_.mode != RunMode::RESEARCH) {
        std::cout << "[tournament] Replay complete:" << std::endl
                  << "  Bars processed:   " << stats.bars_processed << std::endl
                  << "  Accounts:         " << entries_.size() << std::endl
                  << "  Total fills:      " << stats.total_fills << std::endl
                  << "  Total rejects:    " << stats.total_rejects << std::endl
                  << "  Total stop-outs:  " << stats.total_stopouts << std::endl
                  << "  Total SL/TP:      " << stats.total_sl_tp << std::endl
                  << "  Total signals:    " << stats.total_signals << std::endl
                  << "  Risk vetoes:      " << stats.total_risk_vetoes << std::endl
                  << "  Intelligence skips:" << stats.total_regime_skips << std::endl
                  << "  Skipped (liq):    " << stats.total_skipped_liq << std::endl
                  << "  Elapsed:          " << stats.elapsed_seconds << " s" << std::endl
                  << "  Bars/sec:         " << static_cast<int64_t>(bars_per_sec) << std::endl
                  << "  Acct*Bars/sec:    " << static_cast<int64_t>(acct_bars_per_sec) << std::endl;
    }
}

// ── Consistency score ───────────────────────────────────────
// Measures how smooth the equity curve is. 0 = extremely volatile,
// 1 = perfectly smooth. Computed as 1 - (rolling stdev of returns / mean).
double Tournament::compute_consistency(const Account& account, size_t tape_size) {
    const auto& curve = account.equity_curve;
    if (curve.size() < 100) return 0.5; // insufficient data

    // Sample the equity curve at intervals to compute return stability
    int sample_period = std::max(1, static_cast<int>(curve.size() / 50));
    std::vector<double> period_returns;

    for (size_t i = sample_period; i < curve.size(); i += sample_period) {
        double prev = curve[i - sample_period];
        double curr = curve[i];
        if (prev > 0) {
            period_returns.push_back((curr - prev) / prev);
        }
    }

    if (period_returns.size() < 5) return 0.5;

    double mean = std::accumulate(period_returns.begin(), period_returns.end(), 0.0)
                  / period_returns.size();
    double var = 0.0;
    for (double r : period_returns) {
        var += (r - mean) * (r - mean);
    }
    var /= (period_returns.size() - 1);
    double stdev = std::sqrt(var);

    // Score: 1 when stdev is 0, approaching 0 when very volatile
    // Normalize: stdev of 0.05 (5% per period) maps to ~0.5
    double score = std::exp(-stdev * 20.0);
    return std::max(0.0, std::min(1.0, score));
}

// ── Composite score ─────────────────────────────────────────
// Weighted combination of quality factors.
// This is what makes the tournament intelligent.
double Tournament::compute_composite_score(const LeaderboardRow& row) {
    if (row.liquidated) return -1000.0; // hard penalty

    // Components:
    double return_component = row.total_return;

    // Risk-adjusted return (similar to Calmar ratio)
    double risk_adj = (row.max_drawdown > 0.1)
        ? row.total_return / row.max_drawdown
        : row.total_return * 5.0; // bonus for very low drawdown

    // Trade quality: profit factor × expectancy
    double trade_quality = 0.0;
    if (row.trade_count > 10) {
        trade_quality = std::min(row.profit_factor, 3.0) * 10.0; // cap at 3.0 PF
    }

    // Win rate penalty/bonus (too low = unreliable, too high for trend = suspicious)
    double wr_component = 0.0;
    if (row.win_rate > 30.0 && row.win_rate < 70.0) {
        wr_component = 5.0; // reasonable range
    }

    // Drawdown penalty: exponential penalty for deep drawdowns
    double dd_penalty = -std::exp(row.max_drawdown * 0.05) + 1.0;

    // Trade count: bonus for sufficient sample size, penalty for too few
    double sample_bonus = 0.0;
    if (row.trade_count >= 30) sample_bonus = 5.0;
    else if (row.trade_count >= 10) sample_bonus = 2.0;

    // Consistency bonus
    double consistency_bonus = row.consistency_score * 15.0;

    // Weighted composite
    double composite = return_component * 0.25
                     + risk_adj * 0.30
                     + trade_quality * 0.15
                     + consistency_bonus * 0.15
                     + sample_bonus * 0.05
                     + wr_component * 0.05
                     + dd_penalty * 0.05;

    return composite;
}

std::vector<LeaderboardRow> Tournament::leaderboard() const {
    std::vector<LeaderboardRow> rows;
    rows.reserve(entries_.size());

    for (const auto& entry : entries_) {
        const auto& s = entry.account.state;
        LeaderboardRow row;

        row.account_id    = s.account_id;
        row.final_balance = s.balance;
        row.final_equity  = s.equity;
        row.total_return  = (s.initial_balance > 0)
                            ? (s.equity - s.initial_balance) / s.initial_balance * 100.0
                            : 0.0;
        row.max_drawdown  = s.max_drawdown * 100.0;
        row.win_rate      = (s.total_trades > 0)
                            ? static_cast<double>(s.winning_trades) / s.total_trades * 100.0
                            : 0.0;
        row.profit_factor = (s.gross_loss > 0)
                            ? s.gross_profit / s.gross_loss
                            : (s.gross_profit > 0 ? 999.99 : 0.0);
        row.expectancy    = (s.total_trades > 0)
                            ? s.realized_pnl / s.total_trades
                            : 0.0;
        row.trade_count   = s.total_trades;
        row.liquidated    = s.liquidated != 0;
        row.strategy_name = entry.strategy ? entry.strategy->name() : "unknown";
        row.fast_period   = entry.params.fast_period;
        row.slow_period   = entry.params.slow_period;

        // V3: Quality metrics
        row.risk_adjusted_return = (row.max_drawdown > 0.1)
            ? row.total_return / row.max_drawdown : row.total_return * 5.0;
        row.consistency_score = compute_consistency(entry.account, tape_.bars.size());
        row.composite_score = compute_composite_score(row);

        rows.push_back(row);
    }

    // V3: Sort by composite score (intelligence-ranked)
    std::sort(rows.begin(), rows.end(),
        [](const LeaderboardRow& a, const LeaderboardRow& b) {
            return a.composite_score > b.composite_score;
        }
    );

    return rows;
}

} // namespace aphelion
