// ============================================================
// Aphelion Research - Dynamic Risk Manager
//
// Pure arithmetic risk modulation. No allocations.
// Takes precomputed intelligence + runtime account state,
// produces a size multiplier and optional veto.
// ============================================================

#include "aphelion/risk_manager.h"

#include <algorithm>
#include <cmath>

namespace aphelion {

AccountRiskContext AccountRiskContext::from_account(
    const AccountState& state,
    const TradeRecord* recent_trades_ptr,
    int recent_trade_count,
    int lookback_trades
) {
    AccountRiskContext ctx{};
    ctx.current_drawdown = static_cast<float>(state.max_drawdown);

    const int start = std::max(0, recent_trade_count - lookback_trades);
    int wins = 0;
    int losses = 0;
    int consecutive_losses = 0;

    for (int i = recent_trade_count - 1; i >= start; --i) {
        if (recent_trades_ptr[i].net_pnl > 0) {
            wins++;
            break;
        }
        losses++;
        consecutive_losses++;
    }

    for (int i = recent_trade_count - 1 - consecutive_losses; i >= start; --i) {
        if (recent_trades_ptr[i].net_pnl > 0) wins++;
        else losses++;
    }

    const int total_recent = wins + losses;
    ctx.recent_loss_streak = consecutive_losses;
    ctx.recent_trades = total_recent;
    ctx.recent_win_rate = (total_recent > 0)
        ? static_cast<float>(wins) / static_cast<float>(total_recent)
        : 0.5f;

    return ctx;
}

RiskModulation compute_risk_modulation(
    const StrategyDecision& decision,
    const IntelligenceState& intelligence,
    const AccountRiskContext& account_ctx,
    const RiskConfig& config
) {
    RiskModulation mod{};
    mod.size_scale = std::max(0.1f, decision.size_multiplier);
    mod.veto = false;
    mod.effective_regime = intelligence.regime;

    switch (decision.confidence) {
        case Confidence::LOW:     mod.size_scale *= config.confidence_low_scale; break;
        case Confidence::MEDIUM:  mod.size_scale *= config.confidence_medium_scale; break;
        case Confidence::HIGH:    mod.size_scale *= config.confidence_high_scale; break;
        case Confidence::EXTREME: mod.size_scale *= config.confidence_extreme_scale; break;
        case Confidence::NONE:    break;
    }

    if (account_ctx.current_drawdown > config.drawdown_throttle_start) {
        const float dd_range = config.drawdown_throttle_severe - config.drawdown_throttle_start;
        const float dd_progress = account_ctx.current_drawdown - config.drawdown_throttle_start;

        if (dd_range > 1e-6f) {
            const float t = std::min(1.0f, dd_progress / dd_range);
            const float dd_scale = 1.0f - t * (1.0f - config.drawdown_min_scale);
            mod.size_scale *= dd_scale;
        } else {
            mod.size_scale *= config.drawdown_min_scale;
        }
    }

    if (intelligence.features.volatility_percentile > config.vol_high_threshold) {
        mod.size_scale *= config.vol_high_scale;
    } else if (intelligence.features.volatility_percentile < config.vol_low_threshold) {
        mod.size_scale *= config.vol_low_scale;
    }

    if (decision.preferred_regime != Regime::UNKNOWN &&
        decision.preferred_regime != intelligence.regime) {
        if (config.reject_on_regime_mismatch) {
            mod.veto = true;
        } else {
            mod.size_scale *= config.regime_mismatch_scale;
        }
    }

    if (account_ctx.recent_loss_streak >= config.loss_streak_threshold) {
        mod.size_scale *= config.loss_streak_scale;
    }

    if (intelligence.context_validity < config.context_validity_floor) {
        mod.veto = config.live_safe_mode;
        mod.size_scale *= 0.35f;
    } else if (intelligence.context_validity < 0.45f) {
        mod.size_scale *= config.low_validity_scale;
    }

    if (config.enable_ech) {
        float direction_sign = 0.0f;
        if (decision.action == ActionType::OPEN_LONG) direction_sign = 1.0f;
        else if (decision.action == ActionType::OPEN_SHORT) direction_sign = -1.0f;

        const float ech_alignment = intelligence.entropy_collapse * direction_sign;
        const float energy_alignment = intelligence.directional_energy * direction_sign;

        mod.size_scale *= 1.0f + std::max(0.0f, ech_alignment) * config.ech_positive_bonus;
        mod.size_scale *= 1.0f - std::max(0.0f, -ech_alignment) * config.ech_negative_penalty;
        mod.size_scale *= 1.0f + std::max(0.0f, intelligence.temporal_stability - 0.5f) * config.stability_bonus;
        mod.size_scale *= 1.0f + std::max(0.0f, energy_alignment) * config.energy_bonus;
        mod.size_scale *= 1.0f - intelligence.failure_memory * config.failure_memory_penalty;
        mod.size_scale *= 0.90f + intelligence.cross_timeframe_coherence * config.cross_timeframe_bonus;
    }

    if (intelligence.regime == Regime::TRANSITION) {
        mod.size_scale *= 0.75f;
    }
    if (intelligence.regime == Regime::COMPRESSION &&
        decision.preferred_regime != Regime::COMPRESSION &&
        decision.preferred_regime != Regime::VOLATILE_EXPANSION) {
        mod.size_scale *= 0.70f;
    }

    if (config.live_safe_mode) {
        mod.size_scale = std::min(mod.size_scale, config.live_safe_size_cap);
        if (account_ctx.current_drawdown > config.drawdown_throttle_start * 0.5f) {
            mod.size_scale *= 0.85f;
        }
    }

    mod.size_scale = std::clamp(mod.size_scale, config.min_size_scale, config.max_size_scale);
    return mod;
}

} // namespace aphelion
