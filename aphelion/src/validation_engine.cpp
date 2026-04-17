// ============================================================
// Aphelion Research - Robustness / Validation Engine
// ============================================================

#include "aphelion/validation_engine.h"

#include "aphelion/execution.h"
#include "aphelion/replay_engine.h"
#include "aphelion/robustness_score.h"
#include "aphelion/strategy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace aphelion {

namespace {

struct TradeAccumulator {
    int    trades = 0;
    int    wins = 0;
    double gross_profit = 0.0;
    double gross_loss = 0.0;
    double net_pnl = 0.0;
};

struct TapeSlice {
    BarTape   tape;
    uint32_t  trade_start_bar = 0;
    size_t    source_begin = 0;
    size_t    source_end = 0;
};

struct SingleRunResult {
    Account          account;
    CandidateMetrics metrics;
    ReplayStats      stats;
};

double clamp01(double value) {
    return std::max(0.0, std::min(1.0, value));
}

double safe_div(double numerator, double denominator, double fallback = 0.0) {
    return std::fabs(denominator) > 1e-9 ? numerator / denominator : fallback;
}

double percentile(std::vector<double> values, double q) {
    if (values.empty()) return 0.0;
    q = clamp01(q);
    std::sort(values.begin(), values.end());
    if (values.size() == 1) return values.front();

    const double position = q * static_cast<double>(values.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(position));
    const size_t hi = static_cast<size_t>(std::ceil(position));
    if (lo == hi) return values[lo];

    const double weight = position - static_cast<double>(lo);
    return values[lo] * (1.0 - weight) + values[hi] * weight;
}

CandidateMetrics metrics_from_trade_sequence(
    double initial_balance,
    const std::vector<double>& trade_pnls,
    bool liquidated,
    double override_max_drawdown_pct = -1.0
) {
    CandidateMetrics metrics{};
    metrics.trade_count = static_cast<int>(trade_pnls.size());
    metrics.liquidated = liquidated;

    double balance = initial_balance;
    double peak_balance = initial_balance;
    double max_drawdown = 0.0;
    double gross_profit = 0.0;
    double gross_loss = 0.0;
    int wins = 0;
    int losses = 0;
    double win_sum = 0.0;
    double loss_sum = 0.0;
    std::vector<double> trade_returns_pct;
    trade_returns_pct.reserve(trade_pnls.size());

    for (double pnl : trade_pnls) {
        balance += pnl;
        peak_balance = std::max(peak_balance, balance);
        if (peak_balance > 0.0) {
            max_drawdown = std::max(max_drawdown, (peak_balance - balance) / peak_balance);
        }

        if (pnl > 0.0) {
            gross_profit += pnl;
            win_sum += pnl;
            wins++;
        } else if (pnl < 0.0) {
            gross_loss += std::fabs(pnl);
            loss_sum += std::fabs(pnl);
            losses++;
        }
        trade_returns_pct.push_back(safe_div(pnl, initial_balance, 0.0) * 100.0);
    }

    metrics.final_equity = balance;
    metrics.total_return_pct = safe_div(balance - initial_balance, initial_balance, 0.0) * 100.0;
    metrics.max_drawdown_pct = (override_max_drawdown_pct >= 0.0)
        ? override_max_drawdown_pct
        : max_drawdown * 100.0;
    metrics.profit_factor = (gross_loss > 0.0)
        ? (gross_profit / gross_loss)
        : (gross_profit > 0.0 ? 999.99 : 0.0);
    metrics.expectancy = trade_pnls.empty()
        ? 0.0
        : std::accumulate(trade_pnls.begin(), trade_pnls.end(), 0.0) /
          static_cast<double>(trade_pnls.size());
    metrics.win_rate_pct = trade_pnls.empty()
        ? 0.0
        : static_cast<double>(wins) / static_cast<double>(trade_pnls.size()) * 100.0;
    metrics.avg_win = wins > 0 ? win_sum / static_cast<double>(wins) : 0.0;
    metrics.avg_loss = losses > 0 ? loss_sum / static_cast<double>(losses) : 0.0;

    if (trade_pnls.size() >= 2) {
        const double mean_pnl = metrics.expectancy;
        double pnl_var = 0.0;
        double return_var = 0.0;
        const double mean_return = std::accumulate(
            trade_returns_pct.begin(), trade_returns_pct.end(), 0.0
        ) / static_cast<double>(trade_returns_pct.size());

        for (size_t i = 0; i < trade_pnls.size(); ++i) {
            const double pnl_diff = trade_pnls[i] - mean_pnl;
            pnl_var += pnl_diff * pnl_diff;

            const double ret_diff = trade_returns_pct[i] - mean_return;
            return_var += ret_diff * ret_diff;
        }
        pnl_var /= static_cast<double>(trade_pnls.size() - 1);
        return_var /= static_cast<double>(trade_returns_pct.size() - 1);
        metrics.pnl_stddev = std::sqrt(std::max(0.0, pnl_var));
        metrics.return_volatility = std::sqrt(std::max(0.0, return_var));
    }

    if (!trade_returns_pct.empty()) {
        std::sort(trade_returns_pct.begin(), trade_returns_pct.end());
        const size_t tail_count = std::max<size_t>(1, trade_returns_pct.size() / 10);
        metrics.tail_loss_pct = std::accumulate(
            trade_returns_pct.begin(),
            trade_returns_pct.begin() + tail_count,
            0.0
        ) / static_cast<double>(tail_count);
    }

    return metrics;
}

CandidateMetrics compute_metrics(const Account& account) {
    std::vector<double> trade_pnls;
    trade_pnls.reserve(account.trade_log.size());
    for (const auto& trade : account.trade_log) {
        trade_pnls.push_back(trade.net_pnl);
    }

    CandidateMetrics metrics = metrics_from_trade_sequence(
        account.state.initial_balance,
        trade_pnls,
        account.state.liquidated != 0,
        account.state.max_drawdown * 100.0
    );
    metrics.final_equity = account.state.equity;
    metrics.total_return_pct = safe_div(
        account.state.equity - account.state.initial_balance,
        account.state.initial_balance,
        0.0
    ) * 100.0;
    metrics.trade_count = account.state.total_trades;
    metrics.liquidated = account.state.liquidated != 0;
    return metrics;
}

size_t compute_warmup_bars(
    const ValidationCandidate& candidate,
    const ValidationExecutionConfig& execution_config
) {
    size_t warmup = static_cast<size_t>(std::max(candidate.params.slow_period, 0));
    warmup = std::max(warmup, static_cast<size_t>(execution_config.feature_config.trend_long_period));
    warmup = std::max(warmup, static_cast<size_t>(execution_config.feature_config.volatility_long_period));
    warmup = std::max(warmup, static_cast<size_t>(execution_config.ech_config.entropy_window * 2));
    warmup = std::max(warmup, static_cast<size_t>(execution_config.ech_config.structural_window * 2));
    return std::max<size_t>(warmup, 32);
}

TapeSlice build_eval_slice(
    const BarTape& source,
    size_t eval_begin,
    size_t eval_end,
    size_t warmup
) {
    TapeSlice slice{};
    if (source.bars.empty()) return slice;

    eval_begin = std::min(eval_begin, source.bars.size());
    eval_end = std::min(eval_end, source.bars.size());
    if (eval_begin >= eval_end) return slice;

    const size_t source_begin = (eval_begin > warmup) ? (eval_begin - warmup) : 0;
    slice.source_begin = source_begin;
    slice.source_end = eval_end;
    slice.trade_start_bar = static_cast<uint32_t>(eval_begin - source_begin);

    slice.tape.symbol = source.symbol;
    slice.tape.timeframe = source.timeframe;
    slice.tape.timeframe_seconds = source.timeframe_seconds;
    slice.tape.bars.assign(
        source.bars.begin() + static_cast<std::ptrdiff_t>(source_begin),
        source.bars.begin() + static_cast<std::ptrdiff_t>(eval_end)
    );
    if (!slice.tape.bars.empty()) {
        slice.tape.bars.front().delta_sec = 0;
        slice.tape.bars.front().flags = 0;
        slice.tape.min_time = slice.tape.bars.front().time_ms;
        slice.tape.max_time = slice.tape.bars.back().time_ms;
    }
    return slice;
}

BarTape make_noisy_tape(
    const BarTape& source,
    double sigma_bps,
    uint32_t seed
) {
    BarTape noisy = source;
    if (noisy.bars.empty() || sigma_bps <= 0.0) return noisy;

    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0.0, sigma_bps * 1e-4);

    for (auto& bar : noisy.bars) {
        const double shared = noise(rng);
        const double open_noise = shared + noise(rng) * 0.35;
        const double close_noise = shared + noise(rng) * 0.35;
        const double high_noise = std::max(open_noise, close_noise) + std::fabs(noise(rng)) * 0.20;
        const double low_noise = std::min(open_noise, close_noise) - std::fabs(noise(rng)) * 0.20;

        bar.open = std::max(0.01, bar.open * (1.0 + open_noise));
        bar.close = std::max(0.01, bar.close * (1.0 + close_noise));
        bar.high = std::max({bar.open, bar.close, std::max(0.01, bar.high * (1.0 + high_noise))});
        bar.low = std::min({bar.open, bar.close, std::max(0.01, bar.low * (1.0 + low_noise))});
    }

    return noisy;
}

const char* regime_name(Regime regime) {
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

bool is_trend_regime(Regime regime) {
    return regime == Regime::TRENDING_UP ||
           regime == Regime::TRENDING_DOWN ||
           regime == Regime::VOLATILE_EXPANSION;
}

bool is_range_regime(Regime regime) {
    return regime == Regime::RANGE_BOUND || regime == Regime::COMPRESSION;
}

bool stress_passed(const CandidateMetrics& metrics, const ValidationConfig& config) {
    return !metrics.liquidated &&
           metrics.total_return_pct >= config.acceptable_test_return_pct &&
           metrics.max_drawdown_pct <= config.max_stress_drawdown_pct;
}

SingleRunResult run_candidate_on_tape(
    const BarTape& tape,
    const ValidationCandidate& candidate,
    const ValidationExecutionConfig& execution_config,
    uint32_t trade_start_bar = 0
) {
    SingleRunResult result{};
    if (tape.bars.empty()) {
        return result;
    }

    SimulationParams params = candidate.params;
    params.trade_start_bar = trade_start_bar;

    result.account.init(candidate.account_id, params);
    auto strategy = create_strategy(params);

    EchConfig ech_config = execution_config.ech_config;
    ech_config.enabled = ech_config.enabled && (params.enable_ech != 0);

    IntelligenceTape intelligence;
    const IntelligenceState* intelligence_ptr = nullptr;
    try {
        intelligence = build_intelligence_tape(
            tape,
            nullptr,
            0,
            execution_config.feature_config,
            execution_config.regime_config,
            ech_config
        );
        intelligence_ptr = intelligence.empty() ? nullptr : intelligence.data();
    } catch (const std::exception& e) {
        std::cerr << "[validation] WARNING: intelligence build failed for account "
                  << candidate.account_id << ": " << e.what() << std::endl;
        intelligence_ptr = nullptr;
    }

    if (strategy->is_intelligence_aware() && intelligence_ptr != nullptr) {
        strategy->prepare_with_intelligence(
            tape.bars.data(), tape.bars.size(), intelligence_ptr
        );
    } else {
        strategy->prepare(tape.bars.data(), tape.bars.size());
    }

    std::vector<ReplayEntry> entries;
    entries.push_back({&result.account, strategy.get(), &params});

    result.stats = run_replay_v3(
        tape.bars.data(),
        tape.bars.size(),
        entries,
        RunMode::BENCHMARK,
        intelligence_ptr,
        execution_config.risk_config
    );

    MarketState final_market;
    final_market.bar_index = tape.bars.size() - 1;
    final_market.current_bar = &tape.bars.back();
    final_market.prev_bar = (tape.bars.size() > 1) ? &tape.bars[tape.bars.size() - 2] : nullptr;
    final_market.total_bars = tape.bars.size();
    final_market.tape_begin = tape.bars.data();

    for (int i = 0; i < MAX_POSITIONS_PER_ACCOUNT; ++i) {
        if (!result.account.positions[i].active) continue;
        const double exit_price = (result.account.positions[i].direction == Direction::LONG)
            ? final_market.bid()
            : final_market.ask();
        close_position(
            result.account,
            i,
            exit_price,
            ExitReason::END_OF_DATA,
            final_market,
            params.commission_per_lot
        );
    }
    result.account.mark_to_market(final_market);
    result.metrics = compute_metrics(result.account);
    return result;
}

HoldoutValidation run_holdout_validation(
    const BarTape& primary,
    const ValidationCandidate& candidate,
    const ValidationExecutionConfig& execution_config,
    const ValidationConfig& validation_config
) {
    HoldoutValidation holdout{};
    const size_t n = primary.bars.size();
    if (n < 32) return holdout;

    const size_t train_end = std::min(
        n,
        static_cast<size_t>(std::lround(
            static_cast<double>(n) * validation_config.holdout_train_percent / 100.0
        ))
    );
    const size_t validation_end = std::min(
        n,
        train_end + static_cast<size_t>(std::lround(
            static_cast<double>(n) * validation_config.holdout_validation_percent / 100.0
        ))
    );
    const size_t warmup = compute_warmup_bars(candidate, execution_config);

    if (train_end > 0) {
        TapeSlice train_slice = build_eval_slice(primary, 0, train_end, 0);
        if (!train_slice.tape.bars.empty()) {
            holdout.train = run_candidate_on_tape(
                train_slice.tape,
                candidate,
                execution_config,
                train_slice.trade_start_bar
            ).metrics;
        }
    }

    if (validation_end > train_end) {
        TapeSlice validation_slice = build_eval_slice(primary, train_end, validation_end, warmup);
        if (!validation_slice.tape.bars.empty()) {
            holdout.validation = run_candidate_on_tape(
                validation_slice.tape,
                candidate,
                execution_config,
                validation_slice.trade_start_bar
            ).metrics;
        }
    }

    if (n > validation_end) {
        TapeSlice test_slice = build_eval_slice(primary, validation_end, n, warmup);
        if (!test_slice.tape.bars.empty()) {
            holdout.test = run_candidate_on_tape(
                test_slice.tape,
                candidate,
                execution_config,
                test_slice.trade_start_bar
            ).metrics;
        }
    }

    return holdout;
}

std::vector<WalkForwardWindow> run_walkforward_validation(
    const BarTape& primary,
    const ValidationCandidate& candidate,
    const ValidationExecutionConfig& execution_config,
    const ValidationConfig& validation_config
) {
    std::vector<WalkForwardWindow> windows;
    const size_t n = primary.bars.size();
    const int segments = std::max(validation_config.walkforward_segments, 2);
    const int train_segments = std::max(1, validation_config.walkforward_train_segments);
    if (n < static_cast<size_t>(segments * 16) || train_segments >= segments) {
        return windows;
    }

    const size_t segment_size = n / static_cast<size_t>(segments);
    if (segment_size < 8) return windows;

    const size_t warmup = compute_warmup_bars(candidate, execution_config);
    int window_index = 0;

    for (int segment = train_segments; segment < segments; ++segment) {
        const size_t train_begin = 0;
        const size_t train_end = segment_size * static_cast<size_t>(segment);
        const size_t test_begin = train_end;
        const size_t test_end = (segment == segments - 1)
            ? n
            : std::min(n, segment_size * static_cast<size_t>(segment + 1));
        if (train_end <= train_begin || test_end <= test_begin) continue;

        WalkForwardWindow window{};
        window.index = window_index++;
        window.train_begin = train_begin;
        window.train_end = train_end;
        window.test_begin = test_begin;
        window.test_end = test_end;

        TapeSlice train_slice = build_eval_slice(primary, train_begin, train_end, 0);
        if (!train_slice.tape.bars.empty()) {
            window.train = run_candidate_on_tape(
                train_slice.tape,
                candidate,
                execution_config,
                train_slice.trade_start_bar
            ).metrics;
        }

        TapeSlice test_slice = build_eval_slice(primary, test_begin, test_end, warmup);
        if (!test_slice.tape.bars.empty()) {
            window.test = run_candidate_on_tape(
                test_slice.tape,
                candidate,
                execution_config,
                test_slice.trade_start_bar
            ).metrics;
        }

        window.generalization_ratio = safe_div(
            window.test.total_return_pct,
            std::max(std::fabs(window.train.total_return_pct), 1.0),
            0.0
        );
        windows.push_back(window);
    }

    return windows;
}

double compute_degradation_slope(const std::vector<WalkForwardWindow>& windows) {
    if (windows.size() < 2) return 0.0;

    double mean_x = 0.0;
    double mean_y = 0.0;
    for (const auto& window : windows) {
        mean_x += static_cast<double>(window.index);
        mean_y += window.test.total_return_pct;
    }
    mean_x /= static_cast<double>(windows.size());
    mean_y /= static_cast<double>(windows.size());

    double cov = 0.0;
    double var = 0.0;
    for (const auto& window : windows) {
        const double dx = static_cast<double>(window.index) - mean_x;
        const double dy = window.test.total_return_pct - mean_y;
        cov += dx * dy;
        var += dx * dx;
    }
    return safe_div(cov, var, 0.0);
}

std::vector<RegimeBreakdown> analyze_regimes(
    const Account& account,
    const IntelligenceTape& intelligence_tape,
    const ValidationConfig& validation_config
) {
    std::array<TradeAccumulator, 7> accumulators{};
    std::array<double, 7> bar_counts{};
    double positive_profit_sum = 0.0;

    for (const auto& state : intelligence_tape.states) {
        const size_t idx = static_cast<size_t>(state.regime);
        if (idx < bar_counts.size()) {
            bar_counts[idx] += 1.0;
        }
    }

    for (const auto& trade : account.trade_log) {
        const size_t regime_idx = (trade.entry_bar_idx < intelligence_tape.size())
            ? static_cast<size_t>(intelligence_tape.at(trade.entry_bar_idx).regime)
            : static_cast<size_t>(Regime::UNKNOWN);
        if (regime_idx >= accumulators.size()) continue;

        auto& acc = accumulators[regime_idx];
        acc.trades++;
        acc.net_pnl += trade.net_pnl;
        if (trade.net_pnl > 0.0) {
            acc.wins++;
            acc.gross_profit += trade.net_pnl;
            positive_profit_sum += trade.net_pnl;
        } else {
            acc.gross_loss += std::fabs(trade.net_pnl);
        }
    }

    const double total_bars = static_cast<double>(std::max<size_t>(intelligence_tape.size(), 1));
    std::vector<RegimeBreakdown> breakdown;
    breakdown.reserve(accumulators.size());

    for (size_t idx = 0; idx < accumulators.size(); ++idx) {
        RegimeBreakdown row{};
        row.regime = static_cast<Regime>(idx);
        row.trades = accumulators[idx].trades;
        row.bar_share = bar_counts[idx] / total_bars;
        row.profit_share = (accumulators[idx].net_pnl > 0.0 && positive_profit_sum > 0.0)
            ? accumulators[idx].net_pnl / positive_profit_sum
            : 0.0;
        row.net_pnl = accumulators[idx].net_pnl;
        row.expectancy = accumulators[idx].trades > 0
            ? accumulators[idx].net_pnl / static_cast<double>(accumulators[idx].trades)
            : 0.0;
        row.win_rate_pct = accumulators[idx].trades > 0
            ? static_cast<double>(accumulators[idx].wins) /
              static_cast<double>(accumulators[idx].trades) * 100.0
            : 0.0;
        row.profit_factor = accumulators[idx].gross_loss > 0.0
            ? accumulators[idx].gross_profit / accumulators[idx].gross_loss
            : (accumulators[idx].gross_profit > 0.0 ? 999.99 : 0.0);
        row.failing = row.trades >= validation_config.min_regime_trades && row.net_pnl < 0.0;
        breakdown.push_back(row);
    }

    return breakdown;
}

ConditionBreakdown build_condition_breakdown(
    const std::string& name,
    const TradeAccumulator& accumulator
) {
    ConditionBreakdown condition{};
    condition.name = name;
    condition.trades = accumulator.trades;
    condition.net_pnl = accumulator.net_pnl;
    condition.expectancy = accumulator.trades > 0
        ? accumulator.net_pnl / static_cast<double>(accumulator.trades)
        : 0.0;
    condition.win_rate_pct = accumulator.trades > 0
        ? static_cast<double>(accumulator.wins) / static_cast<double>(accumulator.trades) * 100.0
        : 0.0;
    return condition;
}

std::vector<ConditionBreakdown> analyze_conditions(
    const Account& account,
    const IntelligenceTape& intelligence_tape
) {
    TradeAccumulator high_vol{};
    TradeAccumulator low_vol{};
    TradeAccumulator trend{};
    TradeAccumulator range{};
    TradeAccumulator transition{};

    auto register_trade = [](TradeAccumulator& accumulator, const TradeRecord& trade) {
        accumulator.trades++;
        accumulator.net_pnl += trade.net_pnl;
        if (trade.net_pnl > 0.0) {
            accumulator.wins++;
            accumulator.gross_profit += trade.net_pnl;
        } else {
            accumulator.gross_loss += std::fabs(trade.net_pnl);
        }
    };

    for (const auto& trade : account.trade_log) {
        if (trade.entry_bar_idx >= intelligence_tape.size()) continue;
        const auto& state = intelligence_tape.at(trade.entry_bar_idx);

        if (state.features.volatility_percentile >= 0.75f) {
            register_trade(high_vol, trade);
        }
        if (state.features.volatility_percentile <= 0.25f) {
            register_trade(low_vol, trade);
        }
        if (is_trend_regime(state.regime)) {
            register_trade(trend, trade);
        }
        if (is_range_regime(state.regime)) {
            register_trade(range, trade);
        }
        if (state.regime == Regime::TRANSITION) {
            register_trade(transition, trade);
        }
    }

    std::vector<ConditionBreakdown> breakdown;
    breakdown.push_back(build_condition_breakdown("HIGH_VOL", high_vol));
    breakdown.push_back(build_condition_breakdown("LOW_VOL", low_vol));
    breakdown.push_back(build_condition_breakdown("TREND", trend));
    breakdown.push_back(build_condition_breakdown("RANGE", range));
    breakdown.push_back(build_condition_breakdown("TRANSITION", transition));
    return breakdown;
}

CandidateMetrics summarize_metric_samples(
    double initial_balance,
    const std::vector<CandidateMetrics>& samples
) {
    CandidateMetrics summary{};
    if (samples.empty()) {
        summary.final_equity = initial_balance;
        return summary;
    }

    std::vector<double> returns;
    std::vector<double> drawdowns;
    std::vector<double> pfs;
    std::vector<double> expectancies;
    std::vector<double> win_rates;
    std::vector<double> pnl_stdevs;
    std::vector<double> vols;
    std::vector<double> tails;
    std::vector<double> equities;
    int liquidations = 0;
    int trade_count_sum = 0;

    returns.reserve(samples.size());
    drawdowns.reserve(samples.size());
    for (const auto& sample : samples) {
        equities.push_back(sample.final_equity);
        returns.push_back(sample.total_return_pct);
        drawdowns.push_back(sample.max_drawdown_pct);
        pfs.push_back(sample.profit_factor);
        expectancies.push_back(sample.expectancy);
        win_rates.push_back(sample.win_rate_pct);
        pnl_stdevs.push_back(sample.pnl_stddev);
        vols.push_back(sample.return_volatility);
        tails.push_back(sample.tail_loss_pct);
        trade_count_sum += sample.trade_count;
        if (sample.liquidated) liquidations++;
    }

    summary.final_equity = percentile(equities, 0.10);
    summary.total_return_pct = percentile(returns, 0.10);
    summary.max_drawdown_pct = percentile(drawdowns, 0.90);
    summary.profit_factor = percentile(pfs, 0.25);
    summary.expectancy = percentile(expectancies, 0.25);
    summary.win_rate_pct = percentile(win_rates, 0.25);
    summary.pnl_stddev = percentile(pnl_stdevs, 0.75);
    summary.return_volatility = percentile(vols, 0.75);
    summary.tail_loss_pct = percentile(tails, 0.10);
    summary.trade_count = static_cast<int>(std::lround(
        static_cast<double>(trade_count_sum) / static_cast<double>(samples.size())
    ));
    summary.liquidated = liquidations > static_cast<int>(samples.size() / 5);
    return summary;
}

CandidateMetrics shuffled_trade_order_metrics(
    const Account& account,
    int iterations,
    uint32_t seed
) {
    std::vector<double> base_pnls;
    base_pnls.reserve(account.trade_log.size());
    for (const auto& trade : account.trade_log) {
        base_pnls.push_back(trade.net_pnl);
    }
    if (base_pnls.empty()) {
        return compute_metrics(account);
    }

    std::mt19937 rng(seed);
    std::vector<CandidateMetrics> samples;
    samples.reserve(static_cast<size_t>(iterations));

    for (int i = 0; i < iterations; ++i) {
        std::vector<double> shuffled = base_pnls;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        samples.push_back(metrics_from_trade_sequence(
            account.state.initial_balance,
            shuffled,
            false
        ));
    }

    return summarize_metric_samples(account.state.initial_balance, samples);
}

CandidateMetrics bootstrap_trade_path_metrics(
    const Account& account,
    int iterations,
    uint32_t seed
) {
    std::vector<double> base_pnls;
    base_pnls.reserve(account.trade_log.size());
    for (const auto& trade : account.trade_log) {
        base_pnls.push_back(trade.net_pnl);
    }
    if (base_pnls.empty()) {
        return compute_metrics(account);
    }

    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> pick(0, base_pnls.size() - 1);
    std::vector<CandidateMetrics> samples;
    samples.reserve(static_cast<size_t>(iterations));

    for (int i = 0; i < iterations; ++i) {
        std::vector<double> bootstrapped;
        bootstrapped.reserve(base_pnls.size());
        for (size_t j = 0; j < base_pnls.size(); ++j) {
            bootstrapped.push_back(base_pnls[pick(rng)]);
        }
        samples.push_back(metrics_from_trade_sequence(
            account.state.initial_balance,
            bootstrapped,
            false
        ));
    }

    return summarize_metric_samples(account.state.initial_balance, samples);
}

double compute_parameter_sensitivity(
    const CandidateMetrics& base_metrics,
    const std::vector<StressScenarioResult>& jitter_results,
    const ValidationConfig& validation_config
) {
    if (jitter_results.empty()) return 0.5;

    std::vector<double> returns;
    returns.reserve(jitter_results.size());
    int passes = 0;
    double downside = 0.0;

    for (const auto& result : jitter_results) {
        returns.push_back(result.metrics.total_return_pct);
        if (stress_passed(result.metrics, validation_config)) {
            passes++;
        }
        downside += std::max(0.0, base_metrics.total_return_pct - result.metrics.total_return_pct);
    }

    const double mean = std::accumulate(returns.begin(), returns.end(), 0.0) /
        static_cast<double>(returns.size());
    double var = 0.0;
    for (double value : returns) {
        const double diff = value - mean;
        var += diff * diff;
    }
    var /= static_cast<double>(std::max<size_t>(1, returns.size() - 1));

    const double stdev = std::sqrt(std::max(0.0, var));
    const double pass_ratio = static_cast<double>(passes) / static_cast<double>(jitter_results.size());
    const double stability = clamp01(1.0 - stdev / std::max(6.0, std::fabs(base_metrics.total_return_pct) + 3.0));
    const double downside_score = clamp01(
        1.0 - downside /
        (static_cast<double>(jitter_results.size()) * std::max(6.0, std::fabs(base_metrics.total_return_pct) + 3.0))
    );

    return clamp01(pass_ratio * 0.45 + stability * 0.35 + downside_score * 0.20);
}

double quick_rank_score(const CandidateMetrics& metrics) {
    const double return_score = metrics.total_return_pct * 0.55;
    const double drawdown_penalty = metrics.max_drawdown_pct * 0.35;
    const double pf_bonus = std::min(metrics.profit_factor, 4.0) * 5.0;
    const double trade_bonus = std::min(metrics.trade_count, 100) * 0.08;
    return return_score - drawdown_penalty + pf_bonus + trade_bonus;
}

} // namespace

ValidationSummary run_validation_suite(
    const BarTape& primary,
    const std::vector<const BarTape*>& validation_tapes,
    const IntelligenceTape& intelligence_tape,
    const std::vector<ValidationCandidate>& candidates,
    const ValidationExecutionConfig& execution_config,
    const ValidationConfig& validation_config
) {
    ValidationSummary summary{};
    if (!validation_config.enabled || primary.bars.empty() || candidates.empty()) {
        return summary;
    }

    IntelligenceTape primary_intelligence = intelligence_tape;
    if (primary_intelligence.empty()) {
        try {
            primary_intelligence = build_intelligence_tape(
                primary,
                nullptr,
                0,
                execution_config.feature_config,
                execution_config.regime_config,
                execution_config.ech_config
            );
        } catch (const std::exception& e) {
            std::cerr << "[validation] WARNING: primary intelligence rebuild failed: "
                      << e.what() << std::endl;
        }
    }

    summary.reports.reserve(candidates.size());
    std::vector<std::pair<size_t, double>> quick_survivors;
    quick_survivors.reserve(candidates.size());

    for (size_t candidate_idx = 0; candidate_idx < candidates.size(); ++candidate_idx) {
        const auto& candidate = candidates[candidate_idx];

        ValidationReport report{};
        report.account_id = candidate.account_id;
        report.strategy_name = candidate.strategy_name;
        report.fast_period = candidate.params.fast_period;
        report.slow_period = candidate.params.slow_period;

        if (candidate.base_account != nullptr) {
            report.base_metrics = compute_metrics(*candidate.base_account);
        } else {
            report.base_metrics = run_candidate_on_tape(primary, candidate, execution_config).metrics;
        }

        report.quick_filter_passed =
            !report.base_metrics.liquidated &&
            report.base_metrics.trade_count >= validation_config.quick_min_trades &&
            report.base_metrics.total_return_pct >= validation_config.quick_min_return_pct &&
            report.base_metrics.max_drawdown_pct <= validation_config.quick_max_drawdown_pct;

        summary.reports.push_back(std::move(report));
        if (summary.reports.back().quick_filter_passed) {
            quick_survivors.push_back({candidate_idx, quick_rank_score(summary.reports.back().base_metrics)});
        }
    }

    std::sort(
        quick_survivors.begin(),
        quick_survivors.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.second > rhs.second;
        }
    );

    const size_t heavy_budget = std::min(
        quick_survivors.size(),
        static_cast<size_t>(std::max(validation_config.heavy_validation_limit, 0))
    );

    for (size_t rank = 0; rank < quick_survivors.size(); ++rank) {
        const size_t candidate_idx = quick_survivors[rank].first;
        auto& report = summary.reports[candidate_idx];
        const auto& candidate = candidates[candidate_idx];

        if (rank >= heavy_budget) {
            report.rejection_reasons.push_back("did not survive pre-validation ranking budget");
            report.passed = false;
            continue;
        }

        report.heavy_validation_run = true;
        summary.heavy_validated++;

        report.holdout = run_holdout_validation(
            primary,
            candidate,
            execution_config,
            validation_config
        );
        report.walkforward = run_walkforward_validation(
            primary,
            candidate,
            execution_config,
            validation_config
        );
        report.degradation_slope = compute_degradation_slope(report.walkforward);

        if (candidate.base_account != nullptr && !primary_intelligence.empty()) {
            report.regime_breakdown = analyze_regimes(
                *candidate.base_account,
                primary_intelligence,
                validation_config
            );
            report.condition_breakdown = analyze_conditions(
                *candidate.base_account,
                primary_intelligence
            );
        }

        {
            SimulationParams stressed_params = candidate.params;
            stressed_params.slippage_points = std::max(
                stressed_params.slippage_points * validation_config.stress_slippage_multiplier,
                validation_config.stress_slippage_floor_points
            );
            ValidationCandidate stressed_candidate = candidate;
            stressed_candidate.params = stressed_params;
            const auto run = run_candidate_on_tape(primary, stressed_candidate, execution_config);
            report.stress_tests.push_back({
                "slippage_stress",
                run.metrics,
                stress_passed(run.metrics, validation_config)
            });
        }

        {
            BarTape noisy_tape = make_noisy_tape(
                primary,
                validation_config.stress_price_noise_sigma_bps,
                0xC0FFEEu + static_cast<uint32_t>(candidate.account_id)
            );
            const auto run = run_candidate_on_tape(noisy_tape, candidate, execution_config);
            report.stress_tests.push_back({
                "price_noise",
                run.metrics,
                stress_passed(run.metrics, validation_config)
            });
        }

        if (candidate.base_account != nullptr) {
            const auto shuffle_metrics = shuffled_trade_order_metrics(
                *candidate.base_account,
                validation_config.monte_carlo_iterations,
                0xA11CEu + static_cast<uint32_t>(candidate.account_id)
            );
            report.stress_tests.push_back({
                "trade_shuffle",
                shuffle_metrics,
                stress_passed(shuffle_metrics, validation_config)
            });

            const auto bootstrap_metrics = bootstrap_trade_path_metrics(
                *candidate.base_account,
                validation_config.bootstrap_iterations,
                0xB0075u + static_cast<uint32_t>(candidate.account_id)
            );
            report.stress_tests.push_back({
                "trade_bootstrap",
                bootstrap_metrics,
                stress_passed(bootstrap_metrics, validation_config)
            });
        }

        std::vector<StressScenarioResult> jitter_results;
        auto push_jitter = [&](const char* name, int fast_delta, int slow_delta) {
            SimulationParams jittered_params = candidate.params;
            jittered_params.fast_period = std::max(2, jittered_params.fast_period + fast_delta);
            jittered_params.slow_period = std::max(4, jittered_params.slow_period + slow_delta);
            if (jittered_params.fast_period >= jittered_params.slow_period) {
                jittered_params.slow_period = jittered_params.fast_period + 2;
            }

            ValidationCandidate jittered_candidate = candidate;
            jittered_candidate.params = jittered_params;
            const auto run = run_candidate_on_tape(primary, jittered_candidate, execution_config);
            StressScenarioResult scenario{
                name,
                run.metrics,
                stress_passed(run.metrics, validation_config)
            };
            jitter_results.push_back(scenario);
            report.stress_tests.push_back(scenario);
        };

        push_jitter("fast_period_up", validation_config.parameter_jitter_fast_step, 0);
        push_jitter("fast_period_down", -validation_config.parameter_jitter_fast_step, 0);
        push_jitter("slow_period_up", 0, validation_config.parameter_jitter_slow_step);
        push_jitter("slow_period_down", 0, -validation_config.parameter_jitter_slow_step);
        report.parameter_sensitivity = compute_parameter_sensitivity(
            report.base_metrics,
            jitter_results,
            validation_config
        );

        for (const BarTape* validation_tape : validation_tapes) {
            if (validation_tape == nullptr || validation_tape->bars.empty()) continue;
            const auto run = run_candidate_on_tape(
                *validation_tape,
                candidate,
                execution_config
            );
            report.timeframe_tests.push_back({
                validation_tape->timeframe,
                run.metrics,
                stress_passed(run.metrics, validation_config)
            });
        }

        const RobustnessScoreBreakdown scores = score_validation_report(report, validation_config);
        report.robust_score = scores.final_score;
        report.return_quality = scores.return_quality;
        report.drawdown_score = scores.drawdown_score;
        report.stability_score = scores.stability_score;
        report.robustness_component = scores.robustness_component;
        report.regime_consistency = scores.regime_consistency;
        report.monte_carlo_resilience = scores.monte_carlo_resilience;
        report.timeframe_consistency = scores.timeframe_consistency;
        report.overfit_penalty = scores.overfit_penalty;

        report.rejection_reasons = evaluate_rejection_rules(report, validation_config, scores);
        report.failure_modes = derive_failure_modes(report);
        report.passed = report.rejection_reasons.empty() &&
            report.robust_score >= validation_config.robust_pass_threshold;
        if (report.passed) {
            summary.passing_account_ids.push_back(report.account_id);
        }
    }

    for (auto& report : summary.reports) {
        if (!report.quick_filter_passed && report.rejection_reasons.empty()) {
            const RobustnessScoreBreakdown scores = score_validation_report(report, validation_config);
            report.robust_score = scores.final_score;
            report.return_quality = scores.return_quality;
            report.drawdown_score = scores.drawdown_score;
            report.stability_score = scores.stability_score;
            report.robustness_component = scores.robustness_component;
            report.regime_consistency = scores.regime_consistency;
            report.monte_carlo_resilience = scores.monte_carlo_resilience;
            report.timeframe_consistency = scores.timeframe_consistency;
            report.overfit_penalty = scores.overfit_penalty;
            report.rejection_reasons = evaluate_rejection_rules(report, validation_config, scores);
            report.failure_modes = derive_failure_modes(report);
        }
    }

    return summary;
}

} // namespace aphelion
