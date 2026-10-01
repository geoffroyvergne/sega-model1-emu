cmake_minimum_required(VERSION 3.20)

project(SegaModel1Emu
    VERSION 0.1.0
    DESCRIPTION "Sega Model 1 arcade emulator"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Language standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# Log every V60 interrupt request / acknowledgement (two lines per frame once
# a game enables interrupts).
option(MODEL1_TRACE_IRQ "Trace V60 interrupt requests and acknowledgements" ON)
# Log every TGP function executed (very verbose once games run).
option(MODEL1_TRACE_TGP "Trace TGP function execution" OFF)
# Print a confirmation to stdout when a coin or start input is pressed.
option(MODEL1_TRACE_INPUT "Log coin and start presses" ON)
# Build the emulator_tests unit test executable.
option(MODEL1_BUILD_TESTS "Build the unit tests" ON)
# Instrument every target with AddressSanitizer + UndefinedBehaviorSanitizer,
# and enable the standard library's bounds checks on std::span / std::array /
# std::vector indexing (libc++ hardening, libstdc++ assertions). ASan alone
# cannot see an overrun from one memory array into the next inside the same
# object (e.g. tile RAM into character RAM within Bus); the library checks can.
# Any violation aborts with a report.
option(MODEL1_SANITIZE "Build with AddressSanitizer, UBSan and bounds-checked containers (GCC/Clang)" OFF)

if(MODEL1_SANITIZE)
    if(MSVC)
        message(WARNING "MODEL1_SANITIZE is only supported with GCC/Clang; ignored")
    else()
        add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
        add_compile_definitions(
            _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE
            _GLIBCXX_ASSERTIONS
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(SDL2 REQUIRED CONFIG)

# ---------------------------------------------------------------------------
# Emulator core: the emulated machine, no host dependencies (no SDL).
# Shared by the emulator executable and the unit tests.
# ---------------------------------------------------------------------------
add_library(model1_core STATIC
    src/audio/multipcm.cpp
    src/audio/ym3438.cpp
    src/core/bus.cpp
    src/core/eeprom_93c46.cpp
    src/core/i8251.cpp
    src/core/input_manager.cpp
    src/core/io_board.cpp
    src/core/m68000.cpp
    src/core/mb86233.cpp
    src/core/motherboard.cpp
    src/core/polygon_renderer.cpp
    src/core/rom_loader.cpp
    src/core/sound_board.cpp
    src/core/sound_bus.cpp
    src/core/tgp.cpp
    src/core/tgp_copro.cpp
    src/core/tilemap_renderer.cpp
    src/core/v60.cpp
    src/core/z80.cpp
)
target_include_directories(model1_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(model1_core PUBLIC
    MODEL1_TRACE_IRQ=$<BOOL:${MODEL1_TRACE_IRQ}>
    MODEL1_TRACE_TGP=$<BOOL:${MODEL1_TRACE_TGP}>
    MODEL1_TRACE_INPUT=$<BOOL:${MODEL1_TRACE_INPUT}>
)
model1_set_warnings(model1_core)

# ---------------------------------------------------------------------------
# Emulator executable: SDL host layer + core
# ---------------------------------------------------------------------------
add_executable(model1
    src/main.cpp
    src/audio/audio_output.cpp
    src/input/keyboard_input.cpp
    src/video/video_manager.cpp
)

# SDL2main provides the WinMain shim on Windows; it is absent or a no-op elsewhere.
if(TARGET SDL2::SDL2main)
    target_link_libraries(model1 PRIVATE SDL2::SDL2main)
endif()
target_link_libraries(model1 PRIVATE model1_core SDL2::SDL2)

model1_set_warnings(model1)

# On Windows, copy SDL2.dll next to the executable so it runs from the build dir.
if(WIN32)
    add_custom_command(TARGET model1 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:model1> $<TARGET_FILE_DIR:model1>
        COMMAND_EXPAND_LISTS
    )
endif()

# ---------------------------------------------------------------------------
# Unit tests: standalone runner, core only (no SDL, no window)
# ---------------------------------------------------------------------------
if(MODEL1_BUILD_TESTS)
    enable_testing()

    add_executable(emulator_tests
        tests/test_main.cpp
        tests/test_v60_core.cpp
        tests/test_v60_addressing_math.cpp
        tests/test_v60_boot.cpp
        tests/test_m68000.cpp
        tests/test_tilemap.cpp
        tests/test_polygons.cpp
        tests/test_sound.cpp
        tests/test_multipcm.cpp
        tests/test_ym3438.cpp
        tests/test_rom_loader.cpp
        tests/test_io_board.cpp
        tests/test_z80.cpp
        tests/test_tgp_copro.cpp
    )
    target_link_libraries(emulator_tests PRIVATE model1_core)
    model1_set_warnings(emulator_tests)

    add_test(NAME emulator_tests COMMAND emulator_tests)

    # Host-side tests: need SDL (audio device lifecycle), no window.
    add_executable(host_tests
        tests/test_main.cpp
        tests/test_audio_host.cpp
        src/audio/audio_output.cpp
    )
    target_link_libraries(host_tests PRIVATE model1_core SDL2::SDL2)
    model1_set_warnings(host_tests)
    add_test(NAME host_tests COMMAND host_tests)
endif()
