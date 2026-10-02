#include "audio/audio_output.hpp"
#include "core/motherboard.hpp"
#include "core/rom_loader.hpp"
#include "input/gamepad_input.hpp"
#include "input/keyboard_input.hpp"
#include "input/shared_presses.hpp"
#include "video/video_manager.hpp"

#include <SDL.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

// RAII guard for SDL_Init / SDL_Quit.
class SdlContext {
public:
    explicit SdlContext(uint32_t flags) : m_ok(SDL_Init(flags) == 0)
    {
        if (!m_ok) {
            std::cerr << "[Main] SDL_Init failed: " << SDL_GetError() << '\n';
        }
    }
    ~SdlContext()
    {
        if (m_ok) {
            SDL_Quit();
        }
    }
    SdlContext(const SdlContext&) = delete;
    SdlContext& operator=(const SdlContext&) = delete;

    [[nodiscard]] bool ok() const { return m_ok; }

private:
    bool m_ok;
};

// Drains all pending events without blocking. Returns false when the
// application should exit.
bool poll_events(model1::KeyboardInput& keyboard, model1::GamepadInput& gamepads)
{
    SDL_Event event;
    while (SDL_PollEvent(&event) != 0) {
        if (event.type == SDL_QUIT) {
            return false;
        }
        keyboard.process_sdl_event(event);
        gamepads.process_sdl_event(event);
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    // Usage: model1 [--romdir <folder> [--game <id>]] [--tile-demo] [--poly-demo] [--sound-demo]
    //               [--no-demo] [--no-audio] [<folder> | boot_rom.bin]
    // --romdir (or a bare folder path) loads an unzipped MAME ROM set; a bare
    // file is loaded raw into the boot ROM (for test programs). Without either, a placeholder path
    // is tried and the built-in demo runs.
    std::string rom_path = "roms/dummy_program.bin";
    std::string rom_dir;
    std::string game_id;
    bool tile_demo = false;
    bool poly_demo = false;
    bool no_demo = false;
    bool sound_demo = false;
    bool no_audio = false;
    uint64_t trace_count = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--tile-demo") {
            tile_demo = true;
        } else if (arg == "--poly-demo") {
            poly_demo = true;
        } else if (arg == "--no-demo") {
            no_demo = true;
        } else if (arg == "--sound-demo") {
            sound_demo = true;
        } else if (arg == "--no-audio") {
            no_audio = true;
        } else if (arg == "--trace" && i + 1 < argc) {
            trace_count = std::strtoull(argv[++i], nullptr, 10);
        } else if ((arg == "--romdir" || arg == "--game") && i + 1 < argc) {
            (arg == "--romdir" ? rom_dir : game_id) = argv[++i];
        } else {
            // A folder is a ROM set; a file is a raw boot ROM image.
            std::error_code ec;
            if (std::filesystem::is_directory(arg, ec)) {
                rom_dir = arg;
            } else {
                rom_path = arg;
            }
        }
    }

    SdlContext sdl(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER);
    if (!sdl.ok()) {
        return EXIT_FAILURE;
    }

    model1::VideoManager video;
    if (!video.initialize("Sega Model 1")) {
        return EXIT_FAILURE;
    }

    model1::Motherboard motherboard;
    motherboard.reset();

    // Declared after the SDL context, so it is destroyed (device closed)
    // before SDL_Quit.
    model1::AudioOutput audio;
    if (!no_audio) {
        audio.open(model1::SoundBoard::k_audio_rate_hz);
    }

    // Keyboard and game controllers press the same inputs. Controllers
    // already plugged in arrive as "device added" events on the first poll.
    model1::SharedPresses presses(motherboard.inputs());
    model1::KeyboardInput keyboard(presses);
    model1::GamepadInput gamepads(presses);
    // Extra controller mappings (SDL's gamecontrollerdb.txt format): from
    // the working directory, then next to the executable.
    if (model1::GamepadInput::load_mappings("gamecontrollerdb.txt") == 0) {
        if (char* base = SDL_GetBasePath(); base != nullptr) {
            model1::GamepadInput::load_mappings(std::string(base) + "gamecontrollerdb.txt");
            SDL_free(base);
        }
    }

    // Verify the TGP command path and float math before running anything.
    motherboard.run_tgp_self_test();

    bool rom_loaded = false;
    if (!rom_dir.empty()) {
        // A ROM set was asked for: anything missing is fatal.
        if (!model1::load_game_directory(rom_dir, motherboard, game_id)) {
            std::cerr << "[Main] Could not load the ROM set in '" << rom_dir << "', exiting\n";
            return EXIT_FAILURE;
        }
        motherboard.reset(); // CPUs fetch their reset vectors from the new ROMs
        rom_loaded = true;
    } else {
        // A missing raw ROM is not fatal. The image goes into the boot ROM
        // region, which holds the V60 reset address (0xFFFFF0).
        rom_loaded = motherboard.bus().load_rom(rom_path, model1::Bus::k_boot_rom_base);
        if (rom_loaded) {
            std::cout << "[Main] ROM loaded: " << rom_path << '\n';
        } else {
            std::cout << "[Main] ROM load FAILED: " << rom_path << " (continuing without program)\n";
        }
    }

    // Without a program, video memory stays empty and the screen shows only
    // palette entry 0 (black at power-on). Start the 3D demo instead, unless
    // a demo was chosen explicitly or --no-demo asks for a blank machine.
    if (!rom_loaded && !tile_demo && !poly_demo && !no_demo) {
        std::cout << "[Main] No program running: starting the 3D demo (use --no-demo for a blank screen)\n";
        poly_demo = true;
    }
    if (tile_demo) {
        motherboard.load_tile_demo();
    }
    if (poly_demo) {
        motherboard.load_polygon_demo();
    }
    if (sound_demo) {
        motherboard.load_sound_demo();
    }

    // --trace N: log each CPU's next N instructions (boot code mapping).
    motherboard.cpu().set_trace(trace_count);
    motherboard.sound().cpu().set_trace(trace_count);

    // Fixed 60 Hz pacing driven by the high-resolution counter.
    const uint64_t counter_freq = SDL_GetPerformanceFrequency();
    const uint64_t ticks_per_frame = counter_freq / model1::Motherboard::k_refresh_rate_hz;
    uint64_t next_frame = SDL_GetPerformanceCounter() + ticks_per_frame;

    // Speed check: if the machine can't be emulated in real time, the game
    // runs slow and the audio queue runs dry (stuttering sound). Measured
    // over 2-second windows, reported once.
    constexpr uint64_t k_speed_window_frames = 2 * model1::Motherboard::k_refresh_rate_hz;
    uint64_t speed_window_start = SDL_GetPerformanceCounter();
    uint64_t speed_window_frames = 0;
    bool speed_warned = false;

    bool running = true;
    while (running) {
        running = poll_events(keyboard, gamepads);

        // Emulate one frame (CPU cycles, 2D layer composition, VBlank IRQ),
        // then present it.
        // The 3D demo plays the part of game code: it spins the cube by
        // re-running its TGP geometry before each frame.
        if (poly_demo) {
            const auto frame = static_cast<float>(motherboard.frame_count());
            motherboard.update_polygon_demo(0.6f + frame * 0.021f, 0.45f + frame * 0.013f);
        }
        if (sound_demo) {
            motherboard.update_sound_demo(motherboard.frame_count());
        }
        motherboard.run_frame();

        // This frame's sound, produced in lockstep with the CPUs.
        audio.submit(motherboard.sound().pending_audio());
        motherboard.sound().clear_audio();
        video.update_framebuffer(motherboard.frame());
        video.present_frame();

        // Coarse sleep, then spin for the last millisecond for accuracy.
        uint64_t now = SDL_GetPerformanceCounter();
        while (now < next_frame) {
            const uint64_t remaining_ms = ((next_frame - now) * 1000) / counter_freq;
            if (remaining_ms > 1) {
                SDL_Delay(static_cast<uint32_t>(remaining_ms - 1));
            }
            now = SDL_GetPerformanceCounter();
        }

        if (++speed_window_frames == k_speed_window_frames) {
            const double seconds = static_cast<double>(now - speed_window_start) / static_cast<double>(counter_freq);
            const double fps = static_cast<double>(k_speed_window_frames) / seconds;
            if (!speed_warned && fps < 0.95 * model1::Motherboard::k_refresh_rate_hz) {
                speed_warned = true;
                std::cerr << "[Main] WARNING: emulating at " << static_cast<int>(fps) << " FPS ("
                          << static_cast<int>(100.0 * fps / model1::Motherboard::k_refresh_rate_hz)
                          << "% of real time): the game runs slow and the sound stutters.";
#ifndef NDEBUG
                std::cerr << " This is a Debug build; a Release build is about 7x faster"
                             " (cmake -DCMAKE_BUILD_TYPE=Release, see README).";
#endif
                std::cerr << '\n';
            }
            speed_window_start = now;
            speed_window_frames = 0;
        }

        // If we fell far behind (debugger, window drag), resync instead of fast-forwarding.
        next_frame += ticks_per_frame;
        if (now > next_frame + ticks_per_frame) {
            next_frame = now + ticks_per_frame;
        }
    }

    std::cerr << "[Main] Quit requested, shutting down\n";
    return EXIT_SUCCESS;
}
