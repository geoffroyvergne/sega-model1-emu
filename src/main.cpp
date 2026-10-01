#include "core/motherboard.hpp"
#include "input/keyboard_input.hpp"
#include "video/video_manager.hpp"

#include <SDL.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

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
bool poll_events(model1::KeyboardInput& keyboard)
{
    SDL_Event event;
    while (SDL_PollEvent(&event) != 0) {
        if (event.type == SDL_QUIT) {
            return false;
        }
        keyboard.process_sdl_event(event);
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    // Optional first argument: path to a program ROM image. Until real ROM set
    // handling exists, fall back to a placeholder path.
    const std::string rom_path = (argc > 1) ? argv[1] : "roms/dummy_program.bin";

    SdlContext sdl(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
    if (!sdl.ok()) {
        return EXIT_FAILURE;
    }

    model1::VideoManager video;
    if (!video.initialize("Sega Model 1")) {
        return EXIT_FAILURE;
    }

    model1::Motherboard motherboard;
    motherboard.reset();

    model1::KeyboardInput keyboard(motherboard.inputs());

    // Verify the TGP command path and float math before running anything.
    motherboard.run_tgp_self_test();

    // A missing ROM is not fatal yet. The image goes into the boot ROM
    // region, which holds the V60 reset address (0xFFFFF0).
    if (motherboard.bus().load_rom(rom_path, model1::Bus::k_boot_rom_base)) {
        std::cout << "[Main] ROM loaded: " << rom_path << '\n';
    } else {
        std::cout << "[Main] ROM load FAILED: " << rom_path << " (continuing without program)\n";
    }

    // Fixed 60 Hz pacing driven by the high-resolution counter.
    const uint64_t counter_freq = SDL_GetPerformanceFrequency();
    const uint64_t ticks_per_frame = counter_freq / model1::Motherboard::k_refresh_rate_hz;
    uint64_t next_frame = SDL_GetPerformanceCounter() + ticks_per_frame;

    bool running = true;
    while (running) {
        running = poll_events(keyboard);

        // Emulate one frame (CPU cycles, then VBlank IRQ), then render during
        // VBlank: VRAM -> pixel buffer -> window.
        motherboard.run_frame();
        video.update_framebuffer(motherboard.bus().vram());
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

        // If we fell far behind (debugger, window drag), resync instead of fast-forwarding.
        next_frame += ticks_per_frame;
        if (now > next_frame + ticks_per_frame) {
            next_frame = now + ticks_per_frame;
        }
    }

    std::cerr << "[Main] Quit requested, shutting down\n";
    return EXIT_SUCCESS;
}
