// ============================================================
// Aphelion Research — QuantLib Integration (Layer B)
// Calendar / business-day logic — cold path only
// ============================================================

#include "aphelion/quantlib_layer.h"

#include <ql/time/calendar.hpp>
#include <ql/time/calendars/unitedstates.hpp>
#include <ql/time/calendars/unitedkingdom.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/calendars/jointcalendar.hpp>
#include <ql/time/date.hpp>

#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>

namespace aphelion {

// ── Global calendar instance ────────────────────────────────
// Forex/commodities: use a joint US+UK+TARGET calendar.
// Gold (XAUUSD) primarily trades on COMEX (US) and London (UK).

static std::unique_ptr<QuantLib::Calendar> g_calendar;
static std::once_flag g_calendar_init;

static QuantLib::Date timestamp_to_ql_date(Timestamp time_ms) {
    // Convert milliseconds since epoch → QuantLib::Date
    int64_t secs = time_ms / 1000;

    // Use C library to break down
    std::time_t t = static_cast<std::time_t>(secs);
    struct tm utc_tm;
#ifdef _WIN32
    gmtime_s(&utc_tm, &t);
#else
    gmtime_r(&t, &utc_tm);
#endif

    return QuantLib::Date(
        static_cast<QuantLib::Day>(utc_tm.tm_mday),
        static_cast<QuantLib::Month>(utc_tm.tm_mon + 1),
        static_cast<QuantLib::Year>(utc_tm.tm_year + 1900)
    );
}

void init_calendar(const std::string& symbol) {
    std::call_once(g_calendar_init, [&]() {
        // For gold/forex: joint US + UK calendar captures major closures
        auto us = QuantLib::UnitedStates(QuantLib::UnitedStates::NYSE);
        auto uk = QuantLib::UnitedKingdom(QuantLib::UnitedKingdom::Exchange);

        g_calendar = std::make_unique<QuantLib::JointCalendar>(us, uk);
    });
}

bool is_business_day(Timestamp time_ms) {
    if (!g_calendar) return true;

    try {
        auto d = timestamp_to_ql_date(time_ms);
        return g_calendar->isBusinessDay(d);
    } catch (...) {
        return true; // Fail open — don't block replay for calendar edge cases
    }
}

void classify_gap(Bar& bar, Timestamp prev_time_ms, const std::string& symbol) {
    // This function augments gap flags with QuantLib calendar awareness.
    // Called during data load, NOT in the replay loop.

    if (!(bar.flags & Bar::FLAG_GAP)) return;

    if (!g_calendar) {
        init_calendar(symbol);
    }

    try {
        auto prev_date = timestamp_to_ql_date(prev_time_ms);
        auto curr_date = timestamp_to_ql_date(bar.time_ms);

        // Count business days between
        int bdays = 0;
        auto d = prev_date + 1;
        while (d < curr_date) {
            if (g_calendar->isBusinessDay(d)) ++bdays;
            ++d;
        }

        // If all intermediate days are holidays/weekends, this is a calendar gap
        if (bdays == 0 && curr_date > prev_date + 1) {
            // Pure holiday/weekend gap — clear the unexpected flag
            bar.flags &= ~Bar::FLAG_UNEXPECTED_GAP;
        }
    } catch (...) {
        // QuantLib date range issues — leave flags as-is
    }
}

} // namespace aphelion
