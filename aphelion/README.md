# Aphelion Research — Simulation Engine v1.0.0

## Physical Reality Statement

This is a **historical simulation engine**, not a hardware-exchange execution engine.
True nanosecond execution is not the goal. The correct goal is an architecture with
nanosecond-level engineering discipline and extremely high throughput per replay run.

## Architecture

```
Layer A — Data Ingestion     : Parquet → contiguous sorted Bar tape
Layer B — QuantLib Calendar   : Business-day / session gap classification (cold path)
Layer C — Market State        : Read-only normalized bar view
Layer D — Account / Risk Core : Balance, equity, margin, stop-out, position sizing
Layer E — Strategy Interface  : Pluggable decision generators
Layer F — Execution Engine    : Fill/reject authority, spread/slippage/commission
Layer G — Tournament          : N-account parameter sweep over shared replay stream
Layer H — Reporting / Export  : CSV/JSON outputs, leaderboard, trade logs, equity curves
```

### Memory Layout Decisions

- **Bar**: 80 bytes, `alignas(16)`. Two bars fit one 128-byte prefetch window.
  Contiguous `std::vector<Bar>` for sequential streaming.
- **AccountState**: 128 bytes, `alignas(64)`. Hot fields (balance, equity, margin)
  packed into first cache line. One mark-to-market pass per bar per account.
- **Position**: 80 bytes, `alignas(16)`. Fixed-size array `[MAX_POSITIONS_PER_ACCOUNT]`
  per account. No heap allocation for position management.
- **Equity curves**: `std::vector<float>` pre-reserved to tape size. Append-only.
- **Trade logs**: `std::vector<TradeRecord>` per account. Cold-path append on close.

### Hot Loop Discipline

The replay loop (Layer core) processes `bars × accounts` iterations.
Per iteration: mark-to-market (pure FP arithmetic), SL/TP check (branch on active
positions), stop-out check (single branch), strategy decision (one virtual call),
execution (conditional fill), equity snapshot (one float append).

**Zero heap allocations in the hot loop.** All containers pre-sized before replay.

## Prerequisites

- Visual Studio 2022+ (MSVC)
- vcpkg
- CMake 3.20+

## Build

```powershell
# From the aphelion directory:

# 1. Install dependencies via vcpkg (one-time, takes ~10-20 min)
vcpkg install --triplet x64-windows --allow-unsupported

# 2. Configure CMake
$cmake = "cmake"  # or full path to cmake
$vcpkg_toolchain = "$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
# If using VS-bundled vcpkg:
# $vcpkg_toolchain = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg\scripts\buildsystems\vcpkg.cmake"

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$vcpkg_toolchain" -DCMAKE_BUILD_TYPE=Release

# 3. Build
cmake --build build --config Release
```

## Run

```powershell
.\build\Release\aphelion.exe --help

# Default run: 100 accounts, XAUUSD H1, SMA crossover parameter sweep
.\build\Release\aphelion.exe

# Custom run
.\build\Release\aphelion.exe `
    --symbol XAUUSD `
    --timeframe H1 `
    --accounts 1000 `
    --leverage 500 `
    --risk 0.02 `
    --fast-min 5 --fast-max 100 `
    --slow-min 20 --slow-max 400 `
    --output results
```

## Output

```
output/
  summary.csv           — Leaderboard: all accounts ranked by return
  run_metadata.json     — Full run parameters and engine metadata
  trades/
    trades_account_0.csv  — Per-account trade log
    trades_account_1.csv
    ...
  equity/
    equity_account_0.csv  — Per-account equity curve
    equity_account_1.csv
    ...
```

## Performance Notes

- Bar tape loaded once into contiguous memory; sorted ascending by time
- All SMA indicators pre-computed in O(n) before replay (running sum)
- Replay loop is single-threaded (deterministic), with future parallelization path
  via independent account partitions (no shared mutable state between accounts)
- For 93K bars (H1) × 100 accounts: expect sub-second replay on modern hardware
- For 5M bars (M1) × 1000 accounts: expect single-digit seconds
- Memory: ~380 MB for M1 bar tape + ~20 MB equity curves for 1000 accounts

## Known V1 Limitations

1. Single instrument only (XAUUSD). Multi-symbol requires data interleaving.
2. SMA Crossover is the only baseline strategy. Framework supports any `IStrategy`.
3. No multi-timeframe within a single replay pass.
4. No GA/evolutionary optimization loop yet (tournament is a parameter sweep).
5. No GUI yet (Dear ImGui integration planned for V2).
6. Single-threaded replay (determinism first; parallelization path is clean).
7. Point value / contract size hardcoded for XAUUSD.

## V2 Roadmap

1. **Multi-instrument replay** — Interleaved bar tape from multiple symbols
2. **Dear ImGui GUI** — Chart, equity curve, leaderboard, trade markers
3. **GA/Tournament evolution** — Genetic algorithm evaluation loop
4. **Multi-timeframe** — Strategy access to multiple timeframes per bar
5. **Parallel replay** — Partition accounts across threads (no shared state)
6. **Strategy library** — Breakout, mean-reversion, volatility, regime systems
7. **Feature engine** — Configurable indicator/feature computation layer
8. **Distributed simulation** — Work distribution across machines
9. **Symbol-aware sizing** — Per-instrument contract specs, point values
10. **Risk overlay** — Portfolio-level risk constraints across positions
