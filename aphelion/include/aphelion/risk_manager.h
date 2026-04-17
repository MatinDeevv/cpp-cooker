#pragma once
// ============================================================
// Aphelion Research — Dynamic Risk Manager (Layer E)
//
// Modulates position sizing and trade acceptance based on:
//   - Signal confidence
//   - Market regime
//   - Account drawdown state
//   - Recent trade performance
//   - Volatility conditions
//
// The risk manager sits between strategy output and execution.
// It does NOT veto — it MODULATES. The execution layer remains
// the final authority for margin/lot validation.
//
// This is precompute-safe: regime and features are already
// computed before replay. Only the account-state-dependent
// modulation happens at runtime (cheap arithmetic).
// ============================================================

#include "aphelion/types.h"
#include "aphelion/features.h"

namespace aphelion {

// ── Risk adaptation configuration ───────────────────────────
struct RiskConfig {
    // Confidence scaling
    float confidence_low_scale     = 0.5f;   // LOW confidence → 50% size
    float confidence_medium_scale  = 1.0f;   // MEDIUM → 100% (baseline)
    float confidence_high_scale    = 1.25f;  // HIGH → 125%
    float confidence_extreme_scale = 1.5f;   // EXTREME → 150%

    // Drawdown throttling
    float drawdown_throttle_start  = 0.10f;  // start reducing at 10% DD
    float drawdown_throttle_severe = 0.25f;  // severe reduction at 25% DD
    float drawdown_min_scale       = 0.25f;  // minimum scale at severe DD

    // Volatility scaling
    float vol_high_threshold       = 0.80f;  // vol percentile above this → reduce
    float vol_high_scale           = 0.7f;   // scale factor in high vol
    float vol_low_threshold        = 0.20f;  // vol percentile below this → reduce (no edge)
    float vol_low_scale            = 0.8f;   // scale factor in low vol

    // Regime compatibility
    bool  reject_on_regime_mismatch = false;  // if true, skip trade entirely
    float regime_mismatch_scale    = 0.5f;   // if false, reduce size on mismatch

    // Loss streak throttling
    int   loss_streak_threshold    = 3;       // consecutive losses to trigger
    float loss_streak_scale        = 0.5f;    // scale after streak
};

// ── Account performance context (computed at runtime) ───────
// Cheap to compute from AccountState + recent trades.
struct AccountRiskContext {
    float  current_drawdown;     // 0.0-1.0
    int    recent_loss_streak;   // consecutive recent losses
    int    recent_trades;        // trades in recent window
    float  recent_win_rate;      // recent window win rate

    static AccountRiskContext from_account(
        const AccountState& state,
        const TradeRecord* recent_trades_ptr,
        int recent_trade_count,
        int lookback_trades = 10
    );
};

// ── Risk modulation result ──────────────────────────────────
struct RiskModulation {
    float  size_scale;      // multiplier on base risk_fraction (0.0-2.0)
    bool   veto;            // true = do not take this trade
    Regime effective_regime; // regime used for this decision
};

// ── Compute risk modulation ─────────────────────────────────
// Called per signal bar per account. Pure arithmetic, no alloc.
RiskModulation compute_risk_modulation(
    const StrategyDecision& decision,
    const BarFeatures& features,
    const AccountRiskContext& account_ctx,
    const RiskConfig& config = RiskConfig{}
);

} // namespace aphelion
