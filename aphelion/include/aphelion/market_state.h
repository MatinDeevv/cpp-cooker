#pragma once
// ============================================================
// Aphelion Research — Market State (Layer C)
// Read-only normalized view of current bar for strategy/engine
// No account mutation, no strategy logic
// ============================================================

#include "aphelion/types.h"

namespace aphelion {

struct MarketState {
    const Bar*  current_bar   = nullptr;
    const Bar*  prev_bar      = nullptr;
    size_t      bar_index     = 0;
    size_t      total_bars    = 0;
    const Bar*  tape_begin    = nullptr;  // for lookback access

    double bid() const { return current_bar->close; }
    double ask() const { return current_bar->close + current_bar->spread_price(); }
    double mid() const { return (bid() + ask()) * 0.5; }
    double spread() const { return current_bar->spread_price(); }
};

// Advance market state to the next bar. Returns false at end.
bool advance_market(MarketState& state, const Bar* tape, size_t tape_size);

} // namespace aphelion
