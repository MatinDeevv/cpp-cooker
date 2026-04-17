// ============================================================
// Aphelion Research — Dynamic Risk Manager
//
// Pure arithmetic risk modulation. No allocations.
// Takes precomputed features + runtime account state,
// produces a size multiplier and optional veto.
// ============================================================

#include "aphelion/risk_manager.h"
#include <cmath>
#include <algorithm>

namespace aphelion {

AccountRiskContext AccountRiskContext::from_account(
    const AccountState& state,
    const TradeRecord* recent_trades_ptr,
    int recent_trade_count,
    int lookback_trades
) {
    AccountRiskContext ctx;

    // Current drawdown
    ctx.current_drawdown = static_cast<float>(state.max_drawdown);

    // Compute from recent trades
    int start = std::max(0, recent_trade_count - lookback_trades);
    int wins = 0, losses = 0, consecutive_losses = 0;

    // Count backwards from most recent to find loss streak
    for (int i = recent_trade_count - 1; i >= start; --i) {
        if (recent_trades_ptr[i].net_pnl > 0) {
            wins++;
            break; // streak broken
        } else {
            losses++;
            consecutive_losses++;
        }
    }

    // Also count wins in the rest of the lookback
    for (int i = recent_trade_count - 1 - consecutive_losses; i >= start; --i) {
        if (recent_trades_ptr[i].net_pnl > 0) wins++;
        else losses++;
    }

    int total_recent = wins + losses;
    ctx.recent_loss_streak = consecutive_losses;
    ctx.recent_trades = total_recent;
    ctx.recent_win_rate = (total_recent > 0)
        ? static_cast<float>(wins) / static_cast<float>(total_recent)
        : 0.5f;

    return ctx;
}


RiskModulation compute_risk_modulation(
    const StrategyDecision& decision,
    const BarFeatures& features,
    const AccountRiskContext& account_ctx,
    const RiskConfig& config
) {
    RiskModulation mod;
    mod.size_scale = 1.0f;
    mod.veto = false;
    mod.effective_regime = static_cast<Regime>(features.regime);

    // ── 1. Confidence scaling ───────────────────────────────
    switch (decision.confidence) {
        case Confidence::LOW:     mod.size_scale *= config.confidence_low_scale; break;
        case Confidence::MEDIUM:  mod.size_scale *= config.confidence_medium_scale; break;
        case Confidence::HIGH:    mod.size_scale *= config.confidence_high_scale; break;
        case Confidence::EXTREME: mod.size_scale *= config.confidence_extreme_scale; break;
        case Confidence::NONE:    mod.size_scale *= config.confidence_medium_scale; break;
    }

    // ── 2. Drawdown throttling ──────────────────────────────
    // Linear ramp-down between start and severe thresholds
    if (account_ctx.current_drawdown > config.drawdown_throttle_start) {
        float dd_range = config.drawdown_throttle_severe - config.drawdown_throttle_start;
        float dd_progress = (account_ctx.current_drawdown - config.drawdown_throttle_start);

        if (dd_range > 1e-6f) {
            float t = std::min(1.0f, dd_progress / dd_range);
            float dd_scale = 1.0f - t * (1.0f - config.drawdown_min_scale);
            mod.size_scale *= dd_scale;
        } else {
            mod.size_scale *= config.drawdown_min_scale;
        }
    }

    // ── 3. Volatility modulation ────────────────────────────
    if (features.volatility_percentile > config.vol_high_threshold) {
        mod.size_scale *= config.vol_high_scale;
    } else if (features.volatility_percentile < config.vol_low_threshold) {
        mod.size_scale *= config.vol_low_scale;
    }

    // ── 4. Regime compatibility ─────────────────────────────
    Regime current_regime = static_cast<Regime>(features.regime);
    if (decision.preferred_regime != Regime::UNKNOWN &&
        decision.preferred_regime != current_regime) {
        if (config.reject_on_regime_mismatch) {
            mod.veto = true;
        } else {
            mod.size_scale *= config.regime_mismatch_scale;
        }
    }

    // ── 5. Loss streak throttling ───────────────────────────
    if (account_ctx.recent_loss_streak >= config.loss_streak_threshold) {
        mod.size_scale *= config.loss_streak_scale;
    }

    // ── 6. Regime-specific adjustments ──────────────────────
    // In transition regimes, reduce aggression
    if (current_regime == Regime::TRANSITION) {
        mod.size_scale *= 0.7f;
    }
    // In compression, reduce unless strategy specifically wants it
    if (current_regime == Regime::COMPRESSION &&
        decision.preferred_regime != Regime::COMPRESSION) {
        mod.size_scale *= 0.6f;
    }

    // Clamp to valid range
    mod.size_scale = std::max(0.1f, std::min(2.0f, mod.size_scale));

    return mod;
}

} // namespace aphelion
