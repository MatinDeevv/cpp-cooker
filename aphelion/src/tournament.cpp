// ============================================================
// Aphelion Research — Tournament Orchestration (Layer G)
// Runs N simulations, varied parameters, ranks
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

        // Deterministic parameter variation across population
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
        entry.params.strategy_id        = 0;

        // Vary SMA periods across the population
        entry.params.fast_period = config_.fast_period_min +
            static_cast<int>(t * fast_range);
        entry.params.slow_period = config_.slow_period_min +
            static_cast<int>(t * slow_range);

        // Ensure fast < slow
        if (entry.params.fast_period >= entry.params.slow_period) {
            entry.params.slow_period = entry.params.fast_period + 10;
        }

        entry.account.init(static_cast<uint32_t>(i), entry.params);
        entry.strategy = create_strategy(entry.params);
    }

    std::cout << "[tournament] Initialized " << config_.num_accounts
              << " accounts with SMA periods [" << config_.fast_period_min
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

    // Prepare all strategies (pre-compute indicators)
    std::cout << "[tournament] Preparing strategies..." << std::flush;
    for (auto& entry : entries_) {
        entry.strategy->prepare(tape_ptr, tape_size);
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

    // Close any remaining positions at end of data
    std::cout << "[tournament] Running replay over " << tape_size
              << " bars x " << entries_.size() << " accounts..." << std::endl;

    ReplayStats stats = run_replay(tape_ptr, tape_size, replay_entries);

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
        // Final mark-to-market
        entry.account.mark_to_market(final_market);
    }

    double bars_per_sec = stats.bars_processed / std::max(stats.elapsed_seconds, 0.001);
    double acct_bars_per_sec = (stats.bars_processed * entries_.size()) / std::max(stats.elapsed_seconds, 0.001);

    std::cout << "[tournament] Replay complete:" << std::endl
              << "  Bars processed:   " << stats.bars_processed << std::endl
              << "  Accounts:         " << entries_.size() << std::endl
              << "  Total fills:      " << stats.total_fills << std::endl
              << "  Total rejects:    " << stats.total_rejects << std::endl
              << "  Total stop-outs:  " << stats.total_stopouts << std::endl
              << "  Total SL/TP:      " << stats.total_sl_tp << std::endl
              << "  Elapsed:          " << stats.elapsed_seconds << " s" << std::endl
              << "  Bars/sec:         " << static_cast<int64_t>(bars_per_sec) << std::endl
              << "  Acct*Bars/sec:    " << static_cast<int64_t>(acct_bars_per_sec) << std::endl;
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

        rows.push_back(row);
    }

    // Sort by total return descending
    std::sort(rows.begin(), rows.end(),
        [](const LeaderboardRow& a, const LeaderboardRow& b) {
            return a.total_return > b.total_return;
        }
    );

    return rows;
}

} // namespace aphelion
