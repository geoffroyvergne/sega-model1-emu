// Host audio tests (SDL): the audio device opens and closes cleanly and
// repeatedly - alone and with the whole emulator running - without hanging,
// the resampler produces the right amount of output, and steady 60 Hz
// deliveries play without underruns.
//
// A watchdog aborts the process if a test hangs (e.g. a deadlock between
// the emulator and SDL's audio thread).

#include "test_framework.hpp"

#include "audio/audio_output.hpp"
#include "core/motherboard.hpp"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {

using model1::AudioOutput;
using model1::Motherboard;
using model1::SoundBoard;

// Aborts the process if not stopped within `limit`.
class Watchdog {
public:
    explicit Watchdog(std::chrono::seconds limit)
        : m_thread([this, limit] {
              const auto deadline = std::chrono::steady_clock::now() + limit;
              while (!m_done.load()) {
                  if (std::chrono::steady_clock::now() > deadline) {
                      std::fprintf(stderr, "WATCHDOG: test hung (possible deadlock), aborting\n");
                      std::abort();
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds(10));
              }
          })
    {
    }
    ~Watchdog()
    {
        m_done = true;
        m_thread.join();
    }

private:
    std::atomic<bool> m_done{false};
    std::thread m_thread;
};

// SDL audio subsystem for one test, with the given driver ("dummy" plays
// nothing but consumes audio in real time; nullptr = the default driver).
class SdlAudio {
public:
    explicit SdlAudio(const char* driver)
    {
        if (driver != nullptr) {
            SDL_setenv("SDL_AUDIODRIVER", driver, 1);
        } else {
            SDL_setenv("SDL_AUDIODRIVER", "", 1);
        }
        m_ok = SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;
    }
    ~SdlAudio()
    {
        if (m_ok) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }
    }
    [[nodiscard]] bool ok() const { return m_ok; }

private:
    bool m_ok = false;
};

} // namespace

TEST_CASE(audio_device_open_close_repeatedly)
{
    Watchdog watchdog(std::chrono::seconds(20));
    SdlAudio sdl("dummy");
    CHECK(sdl.ok());
    std::vector<int16_t> frame(744 * 2, 1000);
    for (int i = 0; i < 25; ++i) {
        AudioOutput audio;
        CHECK(audio.open(SoundBoard::k_audio_rate_hz));
        audio.submit(frame);
        CHECK(audio.frames_queued_total() > 0);
        audio.close();
        CHECK(!audio.is_open());
        audio.close(); // closing twice is safe
    } // destructor after an explicit close: also safe
}

TEST_CASE(audio_default_device_open_close_if_available)
{
    // The real output device, when the machine has one; skipped otherwise
    // (e.g. headless CI).
    Watchdog watchdog(std::chrono::seconds(20));
    SdlAudio sdl(nullptr);
    if (!sdl.ok()) {
        return;
    }
    AudioOutput probe;
    if (!probe.open(SoundBoard::k_audio_rate_hz)) {
        return;
    }
    probe.close();
    std::vector<int16_t> silence(744 * 2, 0);
    for (int i = 0; i < 10; ++i) {
        AudioOutput audio;
        CHECK(audio.open(SoundBoard::k_audio_rate_hz));
        audio.submit(silence);
    }
}

TEST_CASE(audio_emulator_with_sound_starts_and_stops_cleanly)
{
    // Whole emulator + audio, created and destroyed several times while the
    // sound demo plays.
    Watchdog watchdog(std::chrono::seconds(30));
    SdlAudio sdl("dummy");
    CHECK(sdl.ok());
    for (int run = 0; run < 3; ++run) {
        auto board = std::make_unique<Motherboard>();
        AudioOutput audio;
        CHECK(audio.open(SoundBoard::k_audio_rate_hz));
        board->reset();
        board->load_sound_demo();
        for (uint64_t frame = 0; frame < 45; ++frame) {
            board->update_sound_demo(frame);
            board->run_frame();
            audio.submit(board->sound().pending_audio());
            board->sound().clear_audio();
        }
        CHECK(audio.frames_queued_total() + audio.frames_dropped_total() > 30'000);
        audio.close();
    }
}

TEST_CASE(audio_resampler_output_count)
{
    // 44,642.86 Hz in -> 44,100 Hz out: 5 x 744 input frames -> ~3,675 output.
    Watchdog watchdog(std::chrono::seconds(20));
    SdlAudio sdl("dummy");
    AudioOutput audio;
    CHECK(audio.open(SoundBoard::k_audio_rate_hz));
    CHECK(audio.base_step() > 1.012 && audio.base_step() < 1.013);
    std::vector<int16_t> chunk(744 * 2, 500);
    for (int i = 0; i < 5; ++i) {
        audio.submit(chunk);
    }
    // Within the rate control's +-0.5% of nominal; the silence that
    // primes the queue is counted separately.
    const double expected = 5.0 * 744.0 / audio.base_step();
    const auto total = static_cast<double>(audio.frames_queued_total() + audio.frames_dropped_total());
    CHECK(total >= expected * 0.994 && total <= expected * 1.006);
    CHECK(audio.frames_padded_total() > 0);
}

TEST_CASE(audio_rate_factor_steers_toward_the_target)
{
    const double target = AudioOutput::k_target_queued_frames;
    CHECK_EQ(AudioOutput::rate_factor(target, target), 1.0);
    CHECK_EQ(AudioOutput::rate_factor(0, target), 1.0 - AudioOutput::k_max_rate_adjust);        // empty: stretch
    CHECK_EQ(AudioOutput::rate_factor(target * 2, target), 1.0 + AudioOutput::k_max_rate_adjust); // full: squeeze
    CHECK_EQ(AudioOutput::rate_factor(target * 10, target), 1.0 + AudioOutput::k_max_rate_adjust); // clamped
    CHECK(AudioOutput::rate_factor(target * 1.5, target) > 1.0);
}

TEST_CASE(audio_steady_deliveries_play_without_underruns)
{
    // One frame of emulator audio (744 frames) every 1/60 s, as the main
    // loop delivers it. Without a margin the queue starts empty and falls
    // under one device read (512 frames) between deliveries; with it, it
    // stays well above. Checked over the first 20 deliveries: the dummy
    // driver consumes a little slower than real time, so its queue grows
    // over longer runs. (On a real device the queue settles near the
    // target: measured on CoreAudio, 1,771-2,466 frames.)
    Watchdog watchdog(std::chrono::seconds(20));
    SdlAudio sdl("dummy");
    AudioOutput audio;
    CHECK(audio.open(SoundBoard::k_audio_rate_hz));
    std::vector<int16_t> chunk(744 * 2, 500);
    std::size_t lowest = SIZE_MAX; // queue level just before each delivery
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 20; ++frame) {
        std::this_thread::sleep_until(start + std::chrono::microseconds(16'667 * frame));
        if (frame > 0) {
            lowest = std::min(lowest, audio.queued_frames());
        }
        audio.submit(chunk);
    }
    // The queue never runs dry (without the margin it reaches 0 at the
    // second delivery). Its exact low point depends on how the dummy
    // driver's thread is scheduled, so only "never empty" is checked.
    CHECK(lowest > 0);
    CHECK_EQ(audio.underruns(), 0u);
    CHECK_EQ(audio.frames_dropped_total(), 0u);
}
