#pragma once
// ============================================================
// Aphelion Research — Data Ingestion (Layer A)
// Parquet → contiguous Bar tape, sorted ascending by time
// ============================================================

#include "aphelion/types.h"
#include <vector>
#include <string>
#include <filesystem>

namespace aphelion {

struct BarTape {
    std::vector<Bar> bars;
    std::string      symbol;
    std::string      timeframe;
    int32_t          timeframe_seconds = 0;
    Timestamp        min_time = 0;
    Timestamp        max_time = 0;
};

// Load all parquet parts for a given symbol/timeframe from data_sim_ready,
// sort ascending, compute gap flags, return a contiguous bar tape.
BarTape load_bar_tape(
    const std::filesystem::path& data_root,
    const std::string& symbol,
    const std::string& timeframe
);

// Resolve timeframe string → seconds
int32_t timeframe_to_seconds(const std::string& tf);

} // namespace aphelion
