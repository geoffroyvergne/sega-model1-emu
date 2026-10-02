#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace model1 {

// Host audio output (SDL2). Receives the emulator's stereo stream at its
// native rate, resamples it to 44.1 kHz and plays it.
//
// Design: SDL's queue API (SDL_QueueAudio) instead of a callback. The
// emulator thread pushes samples; SDL's audio thread only reads its own
// queue. No emulator state is shared with the audio thread, so there are no
// locks to deadlock on and nothing is allocated on the audio thread. All
// buffers here are allocated in open().
//
// Smooth playback: the emulator delivers one frame's audio (~735 frames)
// every 1/60 s, while the device pulls 512-frame buffers on its own clock.
// Without a margin the queue would run dry between deliveries and play
// gaps. So:
//  - the queue is kept near k_target_queued_frames (~50 ms): at start, and
//    whenever it has run (nearly) dry, it is topped up with silence;
//  - the resampling ratio is nudged by at most k_max_rate_adjust (0.5%,
//    inaudible) toward that target, which absorbs the drift between the
//    emulator's 60 Hz loop and the sound card's clock.
// Latency is bounded: when more than k_max_queued_frames are already waiting
// (audio ran ahead), new audio is dropped and counted instead of piling up.
// Underruns (the queue found empty) are counted and reported at close.
class AudioOutput {
public:
    static constexpr int k_output_rate = 44100;
    static constexpr int k_channels = 2;
    static constexpr int k_device_buffer_frames = 512;   // ~11.6 ms per SDL buffer
    static constexpr int k_target_queued_frames = 2205;  // 50 ms
    static constexpr int k_max_queued_frames = 4410;     // 100 ms
    static constexpr double k_max_rate_adjust = 0.005;   // +-0.5% resampling ratio

    AudioOutput() = default;
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // Opens the default playback device (44.1 kHz, signed 16-bit, stereo)
    // and starts it. Requires SDL_INIT_AUDIO. Returns false (and logs) if no
    // device is available; the emulator then runs silently.
    bool open(double source_rate);
    // Stops and closes the device. Safe to call more than once.
    void close();

    // Resamples and queues interleaved stereo frames at the source rate.
    void submit(std::span<const int16_t> interleaved);

    [[nodiscard]] bool is_open() const { return m_device != 0; }
    [[nodiscard]] std::size_t queued_frames() const;
    [[nodiscard]] uint64_t frames_queued_total() const { return m_frames_queued; }
    [[nodiscard]] uint64_t frames_dropped_total() const { return m_frames_dropped; }
    [[nodiscard]] uint64_t frames_padded_total() const { return m_frames_padded; }
    [[nodiscard]] uint64_t underruns() const { return m_underruns; }

    // Resampler state, exposed for tests: input frames consumed per output
    // frame (nominal, and as last adjusted).
    [[nodiscard]] double base_step() const { return m_base_step; }
    [[nodiscard]] double step() const { return m_step; }

    // Resampling ratio factor for a queue level: 1 at the target, up to
    // 1 + k_max_rate_adjust (consume input faster, produce less) when the
    // queue is twice the target or more, down to 1 - k_max_rate_adjust when
    // empty.
    [[nodiscard]] static double rate_factor(double queued, double target);

private:
    uint32_t m_device = 0; // SDL_AudioDeviceID
    double m_base_step = 1.0; // source frames per output frame, nominal
    double m_step = 1.0;      // ... adjusted toward the target queue level
    double m_smoothed_queue = 0.0;
    bool m_started = false;   // something has been queued since open()
    double m_position = 0.0;
    int16_t m_history_left = 0;
    int16_t m_history_right = 0;
    std::vector<int16_t> m_resampled; // sized in open()
    uint64_t m_frames_queued = 0;
    uint64_t m_frames_dropped = 0;
    uint64_t m_frames_padded = 0;
    uint64_t m_underruns = 0;
    std::vector<int16_t> m_silence; // sized in open()
};

} // namespace model1
