// ============================================================
// Aphelion Research — Multi-Timeframe Implementation
// ============================================================

#include "aphelion/multi_timeframe.h"
#include <iostream>
#include <cstring>

namespace aphelion {

size_t MultiTimeframeContext::find_view(const char* tf_name) const {
    for (size_t i = 0; i < num_views; ++i) {
        if (views[i].timeframe_name && std::strcmp(views[i].timeframe_name, tf_name) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

TimeframeAlignment build_alignment(
    const BarTape& primary,
    const BarTape& secondary
) {
    TimeframeAlignment result;
    result.timeframe = secondary.timeframe;
    result.timeframe_seconds = secondary.timeframe_seconds;
    result.index.resize(primary.bars.size(), UINT32_MAX);

    if (secondary.bars.empty() || primary.bars.empty()) {
        return result;
    }

    const int64_t sec_duration_ms =
        static_cast<int64_t>(secondary.timeframe_seconds) * 1000;

    // Single linear scan: both tapes are sorted ascending by time.
    // For each primary bar, find the latest secondary bar that has
    // fully completed by the primary bar's time.
    //
    // A secondary bar at index j is "completed" at time:
    //   secondary.bars[j].time_ms + sec_duration_ms
    //
    // We want the largest j such that:
    //   secondary.bars[j].time_ms + sec_duration_ms <= primary.bars[i].time_ms

    size_t sec_cursor = 0;
    uint32_t last_valid = UINT32_MAX;

    for (size_t i = 0; i < primary.bars.size(); ++i) {
        const int64_t primary_time = primary.bars[i].time_ms;

        // Advance secondary cursor
        while (sec_cursor < secondary.bars.size() &&
               secondary.bars[sec_cursor].time_ms + sec_duration_ms <= primary_time) {
            last_valid = static_cast<uint32_t>(sec_cursor);
            ++sec_cursor;
        }

        result.index[i] = last_valid;
    }

    // Log alignment stats
    size_t aligned_count = 0;
    for (auto idx : result.index) {
        if (idx != UINT32_MAX) ++aligned_count;
    }

    std::cout << "[multi-tf] Aligned " << primary.timeframe
              << " -> " << secondary.timeframe
              << ": " << aligned_count << "/" << primary.bars.size()
              << " primary bars have secondary context" << std::endl;

    return result;
}

} // namespace aphelion
