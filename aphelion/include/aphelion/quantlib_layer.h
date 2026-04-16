#pragma once
// ============================================================
// Aphelion Research — QuantLib Integration (Layer B)
// Calendar, business-day, session logic
// Cold path only — never called inside the replay loop
// ============================================================

#include "aphelion/types.h"
#include <string>

namespace aphelion {

// Classify a gap between two bars using QuantLib calendar logic.
// Sets FLAG_WEEKEND_GAP, FLAG_SESSION_GAP, FLAG_UNEXPECTED_GAP on the bar.
void classify_gap(Bar& bar, Timestamp prev_time_ms, const std::string& symbol);

// Check if a given timestamp falls on a business day.
bool is_business_day(Timestamp time_ms);

// Initialize QuantLib calendars for the given symbol.
// Call once at startup.
void init_calendar(const std::string& symbol);

} // namespace aphelion
