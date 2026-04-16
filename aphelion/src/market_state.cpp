// ============================================================
// Aphelion Research — Market State (Layer C)
// Read-only bar view for strategy/engine consumption
// ============================================================

#include "aphelion/market_state.h"

namespace aphelion {

bool advance_market(MarketState& state, const Bar* tape, size_t tape_size) {
    if (state.bar_index >= tape_size) return false;

    state.current_bar = &tape[state.bar_index];
    state.prev_bar    = (state.bar_index > 0) ? &tape[state.bar_index - 1] : nullptr;
    state.total_bars  = tape_size;
    state.tape_begin  = tape;

    return true;
}

} // namespace aphelion
