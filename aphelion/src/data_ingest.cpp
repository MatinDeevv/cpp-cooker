// ============================================================
// Aphelion Research — Data Ingestion (Layer A)
// Parquet → contiguous sorted Bar tape
// ============================================================

#include "aphelion/data_ingest.h"
#include "aphelion/quantlib_layer.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <cmath>

namespace aphelion {

// ── Timeframe resolution ────────────────────────────────────

static const std::unordered_map<std::string, int32_t> TF_MAP = {
    {"M1", 60}, {"M2", 120}, {"M3", 180}, {"M4", 240}, {"M5", 300},
    {"M6", 360}, {"M10", 600}, {"M12", 720}, {"M15", 900}, {"M20", 1200},
    {"M30", 1800}, {"H1", 3600}, {"H2", 7200}, {"H3", 10800}, {"H4", 14400},
    {"H6", 21600}, {"H8", 28800}, {"H12", 43200}, {"D1", 86400},
    {"W1", 604800}, {"MN1", 2592000}
};

int32_t timeframe_to_seconds(const std::string& tf) {
    auto it = TF_MAP.find(tf);
    if (it == TF_MAP.end())
        throw std::runtime_error("Unknown timeframe: " + tf);
    return it->second;
}

// ── Parquet column extraction helpers ───────────────────────

static void check_arrow(const arrow::Status& s, const char* ctx) {
    if (!s.ok())
        throw std::runtime_error(std::string(ctx) + ": " + s.ToString());
}

static std::shared_ptr<arrow::Table> read_parquet_file(const std::filesystem::path& path) {
    auto infile_result = arrow::io::ReadableFile::Open(path.string());
    if (!infile_result.ok())
        throw std::runtime_error("Cannot open " + path.string() + ": " + infile_result.status().ToString());

    auto reader_result = parquet::arrow::OpenFile(*infile_result, arrow::default_memory_pool());
    if (!reader_result.ok())
        throw std::runtime_error(std::string("OpenFile: ") + reader_result.status().ToString());
    auto reader = std::move(*reader_result);

    std::shared_ptr<arrow::Table> table;
    auto st = reader->ReadTable(&table);
    check_arrow(st, "ReadTable");
    return table;
}

// Extract a column as a flat vector of doubles from a chunked array
static std::vector<double> extract_double_column(
    const std::shared_ptr<arrow::Table>& table,
    const std::string& name
) {
    auto col = table->GetColumnByName(name);
    if (!col)
        throw std::runtime_error("Column not found: " + name);

    std::vector<double> out;
    out.reserve(static_cast<size_t>(table->num_rows()));

    for (int c = 0; c < col->num_chunks(); ++c) {
        auto chunk = col->chunk(c);
        if (auto d64 = std::dynamic_pointer_cast<arrow::DoubleArray>(chunk)) {
            for (int64_t i = 0; i < d64->length(); ++i)
                out.push_back(d64->Value(i));
        } else if (auto f32 = std::dynamic_pointer_cast<arrow::FloatArray>(chunk)) {
            for (int64_t i = 0; i < f32->length(); ++i)
                out.push_back(static_cast<double>(f32->Value(i)));
        } else {
            throw std::runtime_error("Unexpected type for column " + name);
        }
    }
    return out;
}

static std::vector<int64_t> extract_timestamp_column(
    const std::shared_ptr<arrow::Table>& table,
    const std::string& name
) {
    auto col = table->GetColumnByName(name);
    if (!col)
        throw std::runtime_error("Column not found: " + name);

    std::vector<int64_t> out;
    out.reserve(static_cast<size_t>(table->num_rows()));

    for (int c = 0; c < col->num_chunks(); ++c) {
        auto chunk = col->chunk(c);
        if (auto ts = std::dynamic_pointer_cast<arrow::TimestampArray>(chunk)) {
            auto ts_type = std::static_pointer_cast<arrow::TimestampType>(ts->type());
            for (int64_t i = 0; i < ts->length(); ++i) {
                int64_t val = ts->Value(i);
                switch (ts_type->unit()) {
                    case arrow::TimeUnit::SECOND: val *= 1000; break;
                    case arrow::TimeUnit::MILLI:  break;
                    case arrow::TimeUnit::MICRO:  val /= 1000; break;
                    case arrow::TimeUnit::NANO:   val /= 1000000; break;
                }
                out.push_back(val);
            }
        } else if (auto i64 = std::dynamic_pointer_cast<arrow::Int64Array>(chunk)) {
            // Assume seconds if stored as raw int64
            for (int64_t i = 0; i < i64->length(); ++i)
                out.push_back(i64->Value(i) * 1000);
        } else {
            throw std::runtime_error("Unexpected type for timestamp column " + name);
        }
    }
    return out;
}

static std::vector<uint64_t> extract_uint64_column(
    const std::shared_ptr<arrow::Table>& table,
    const std::string& name
) {
    auto col = table->GetColumnByName(name);
    if (!col)
        throw std::runtime_error("Column not found: " + name);

    std::vector<uint64_t> out;
    out.reserve(static_cast<size_t>(table->num_rows()));

    for (int c = 0; c < col->num_chunks(); ++c) {
        auto chunk = col->chunk(c);
        if (auto u64 = std::dynamic_pointer_cast<arrow::UInt64Array>(chunk)) {
            for (int64_t i = 0; i < u64->length(); ++i)
                out.push_back(u64->Value(i));
        } else if (auto i64 = std::dynamic_pointer_cast<arrow::Int64Array>(chunk)) {
            for (int64_t i = 0; i < i64->length(); ++i)
                out.push_back(static_cast<uint64_t>(i64->Value(i)));
        } else if (auto i32 = std::dynamic_pointer_cast<arrow::Int32Array>(chunk)) {
            for (int64_t i = 0; i < i32->length(); ++i)
                out.push_back(static_cast<uint64_t>(i32->Value(i)));
        } else {
            // Try zero-filling if column type is unexpected
            for (int64_t i = 0; i < chunk->length(); ++i)
                out.push_back(0);
        }
    }
    return out;
}

static std::vector<int32_t> extract_int32_column(
    const std::shared_ptr<arrow::Table>& table,
    const std::string& name
) {
    auto col = table->GetColumnByName(name);
    if (!col)
        throw std::runtime_error("Column not found: " + name);

    std::vector<int32_t> out;
    out.reserve(static_cast<size_t>(table->num_rows()));

    for (int c = 0; c < col->num_chunks(); ++c) {
        auto chunk = col->chunk(c);
        if (auto i32 = std::dynamic_pointer_cast<arrow::Int32Array>(chunk)) {
            for (int64_t i = 0; i < i32->length(); ++i)
                out.push_back(i32->Value(i));
        } else if (auto i64 = std::dynamic_pointer_cast<arrow::Int64Array>(chunk)) {
            for (int64_t i = 0; i < i64->length(); ++i)
                out.push_back(static_cast<int32_t>(i64->Value(i)));
        } else if (auto u32 = std::dynamic_pointer_cast<arrow::UInt32Array>(chunk)) {
            for (int64_t i = 0; i < u32->length(); ++i)
                out.push_back(static_cast<int32_t>(u32->Value(i)));
        } else {
            for (int64_t i = 0; i < chunk->length(); ++i)
                out.push_back(0);
        }
    }
    return out;
}

// ── Gap classification ──────────────────────────────────────

static constexpr int64_t WEEKEND_GAP_THRESHOLD_MS = 2 * 86400 * 1000LL; // >2 days

static uint8_t classify_gap_flags(
    int64_t delta_ms,
    int64_t expected_tf_ms,
    [[maybe_unused]] Timestamp prev_time_ms
) {
    uint8_t flags = 0;

    if (delta_ms <= expected_tf_ms)
        return 0;

    flags |= Bar::FLAG_GAP;

    if (delta_ms > WEEKEND_GAP_THRESHOLD_MS) {
        flags |= Bar::FLAG_WEEKEND_GAP;
    }

    // Session gap: delta > expected but <= weekend threshold
    if (delta_ms > expected_tf_ms && delta_ms <= WEEKEND_GAP_THRESHOLD_MS) {
        flags |= Bar::FLAG_SESSION_GAP;
    }

    // Missing bars estimation
    int64_t expected_count = delta_ms / expected_tf_ms;
    if (expected_count > 1) {
        flags |= Bar::FLAG_MISSING_BARS;
    }

    // Unexpected if not a clean multiple of expected
    double ratio = static_cast<double>(delta_ms) / expected_tf_ms;
    if (std::fabs(ratio - std::round(ratio)) > 0.1) {
        flags |= Bar::FLAG_UNEXPECTED_GAP;
    }

    return flags;
}

// ── Main load function ──────────────────────────────────────

BarTape load_bar_tape(
    const std::filesystem::path& data_root,
    const std::string& symbol,
    const std::string& timeframe
) {
    namespace fs = std::filesystem;

    int32_t tf_sec = timeframe_to_seconds(timeframe);
    int64_t tf_ms  = static_cast<int64_t>(tf_sec) * 1000;

    fs::path parts_path = data_root / symbol / timeframe / "bars" / "parts";

    if (!fs::exists(parts_path))
        throw std::runtime_error("Parts directory not found: " + parts_path.string());

    // Collect all parquet files
    std::vector<fs::path> parquet_files;
    for (auto& entry : fs::directory_iterator(parts_path)) {
        if (entry.path().extension() == ".parquet")
            parquet_files.push_back(entry.path());
    }
    std::sort(parquet_files.begin(), parquet_files.end());

    if (parquet_files.empty())
        throw std::runtime_error("No parquet files found in " + parts_path.string());

    std::cout << "[ingest] Loading " << parquet_files.size() << " parquet files for "
              << symbol << " " << timeframe << "..." << std::flush;

    // Phase 1: Read all files and accumulate raw columns
    std::vector<int64_t>  all_times;
    std::vector<double>   all_open, all_high, all_low, all_close;
    std::vector<uint64_t> all_tick_vol, all_real_vol;
    std::vector<int32_t>  all_spread;

    for (auto& fp : parquet_files) {
        auto table = read_parquet_file(fp);
        auto times     = extract_timestamp_column(table, "time");
        auto opens     = extract_double_column(table, "open");
        auto highs     = extract_double_column(table, "high");
        auto lows      = extract_double_column(table, "low");
        auto closes    = extract_double_column(table, "close");
        auto tick_vols = extract_uint64_column(table, "tick_volume");
        auto spreads   = extract_int32_column(table, "spread");
        auto real_vols = extract_uint64_column(table, "real_volume");
        all_times.insert(all_times.end(), times.begin(), times.end());
        all_open.insert(all_open.end(), opens.begin(), opens.end());
        all_high.insert(all_high.end(), highs.begin(), highs.end());
        all_low.insert(all_low.end(), lows.begin(), lows.end());
        all_close.insert(all_close.end(), closes.begin(), closes.end());
        all_tick_vol.insert(all_tick_vol.end(), tick_vols.begin(), tick_vols.end());
        all_spread.insert(all_spread.end(), spreads.begin(), spreads.end());
        all_real_vol.insert(all_real_vol.end(), real_vols.begin(), real_vols.end());
    }

    size_t total = all_times.size();

    // Phase 2: Sort by time (data arrives newest-first per position ordering)
    std::vector<size_t> sort_idx(total);
    for (size_t i = 0; i < total; ++i) sort_idx[i] = i;
    std::sort(sort_idx.begin(), sort_idx.end(),
        [&](size_t a, size_t b) { return all_times[a] < all_times[b]; }
    );

    // Phase 3: Deduplicate by time
    std::vector<size_t> deduped;
    deduped.reserve(total);
    int64_t last_time = std::numeric_limits<int64_t>::min();
    for (size_t i = 0; i < total; ++i) {
        size_t idx = sort_idx[i];
        if (all_times[idx] != last_time) {
            deduped.push_back(idx);
            last_time = all_times[idx];
        }
    }

    size_t n = deduped.size();

    // Phase 4: Build contiguous bar tape
    BarTape tape;
    tape.symbol    = symbol;
    tape.timeframe = timeframe;
    tape.timeframe_seconds = tf_sec;
    tape.bars.resize(n);

    for (size_t i = 0; i < n; ++i) {
        size_t si = deduped[i];
        Bar& b = tape.bars[i];

        b.time_ms       = all_times[si];
        b.open          = all_open[si];
        b.high          = all_high[si];
        b.low           = all_low[si];
        b.close         = all_close[si];
        b.tick_volume   = all_tick_vol[si];
        b.spread        = all_spread[si];
        b.real_volume_lo= static_cast<uint32_t>(all_real_vol[si] & 0xFFFFFFFF);
        b.timeframe_sec = tf_sec;
        b.flags         = 0;

        if (i == 0) {
            b.delta_sec = 0;
        } else {
            int64_t delta_ms = b.time_ms - tape.bars[i - 1].time_ms;
            b.delta_sec = static_cast<int32_t>(delta_ms / 1000);
            b.flags = classify_gap_flags(delta_ms, tf_ms, tape.bars[i - 1].time_ms);
        }

        std::memset(b._pad, 0, sizeof(b._pad));
    }

    if (n > 0) {
        tape.min_time = tape.bars[0].time_ms;
        tape.max_time = tape.bars[n - 1].time_ms;
    }

    std::cout << " " << n << " bars loaded ("
              << (n * sizeof(Bar)) / (1024 * 1024) << " MB tape)" << std::endl;

    return tape;
}

} // namespace aphelion
