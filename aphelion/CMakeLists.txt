cmake_minimum_required(VERSION 3.20)
project(AphelionEngine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Performance flags ────────────────────────────────────────
if(MSVC)
    add_compile_options(/O2 /Ob2 /Oi /Ot /GL /DNDEBUG /EHsc /fp:fast /arch:AVX2)
    add_link_options(/LTCG)
else()
    add_compile_options(-O3 -march=native -flto -ffast-math -DNDEBUG)
    add_link_options(-flto)
endif()

# ── vcpkg / find packages ───────────────────────────────────
find_package(Arrow CONFIG REQUIRED)
find_package(Parquet CONFIG REQUIRED)
find_package(QuantLib CONFIG REQUIRED)

# ── Source files ─────────────────────────────────────────────
set(ENGINE_SOURCES
    src/main.cpp
    src/data_ingest.cpp
    src/quantlib_layer.cpp
    src/market_state.cpp
    src/account.cpp
    src/execution.cpp
    src/strategy.cpp
    src/tournament.cpp
    src/reporting.cpp
    src/replay_engine.cpp
    src/multi_timeframe.cpp
    src/features.cpp
    src/regime.cpp
    src/risk_manager.cpp
)

add_executable(aphelion ${ENGINE_SOURCES})

target_include_directories(aphelion PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(aphelion PRIVATE
    Arrow::arrow_shared
    Parquet::parquet_shared
    QuantLib::QuantLib
)

# ── Install ──────────────────────────────────────────────────
install(TARGETS aphelion RUNTIME DESTINATION bin)
