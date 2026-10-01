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
# Dependencies
# ---------------------------------------------------------------------------
find_package(SDL2 REQUIRED CONFIG)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(model1
    src/main.cpp
    src/core/bus.cpp
    src/core/input_manager.cpp
    src/core/motherboard.cpp
    src/core/tgp.cpp
    src/core/v60.cpp
    src/input/keyboard_input.cpp
    src/video/video_manager.cpp
)

target_include_directories(model1 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# SDL2main provides the WinMain shim on Windows; it is absent or a no-op elsewhere.
if(TARGET SDL2::SDL2main)
    target_link_libraries(model1 PRIVATE SDL2::SDL2main)
endif()
target_link_libraries(model1 PRIVATE SDL2::SDL2)

model1_set_warnings(model1)

# On Windows, copy SDL2.dll next to the executable so it runs from the build dir.
if(WIN32)
    add_custom_command(TARGET model1 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:model1> $<TARGET_FILE_DIR:model1>
        COMMAND_EXPAND_LISTS
    )
endif()

# Log every V60 interrupt request / acknowledgement (two lines per frame once
# a game enables interrupts).
option(MODEL1_TRACE_IRQ "Trace V60 interrupt requests and acknowledgements" ON)
target_compile_definitions(model1 PRIVATE MODEL1_TRACE_IRQ=$<BOOL:${MODEL1_TRACE_IRQ}>)

# Log every TGP function executed (very verbose once games run).
option(MODEL1_TRACE_TGP "Trace TGP function execution" OFF)
target_compile_definitions(model1 PRIVATE MODEL1_TRACE_TGP=$<BOOL:${MODEL1_TRACE_TGP}>)

# Print a confirmation to stdout when a coin or start input is pressed.
option(MODEL1_TRACE_INPUT "Log coin and start presses" ON)
target_compile_definitions(model1 PRIVATE MODEL1_TRACE_INPUT=$<BOOL:${MODEL1_TRACE_INPUT}>)
