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
// Latency is bounded: when more than k_max_queued_frames are already waiting
// (the host fell behind, or audio ran ahead), new audio is dropped and
// counted instead of piling up.
class AudioOutput {
public:
    static constexpr int k_output_rate = 44100;
    static constexpr int k_channels = 2;
    static constexpr int k_device_buffer_frames = 512;   // ~11.6 ms per SDL buffer
    static constexpr int k_max_queued_frames = 4410;     // 100 ms

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

    // Resampler state, exposed for tests: input frames consumed per output
    // frame.
    [[nodiscard]] double step() const { return m_step; }

private:
    uint32_t m_device = 0; // SDL_AudioDeviceID
    double m_step = 1.0;   // source frames per output frame
    double m_position = 0.0;
    int16_t m_history_left = 0;
    int16_t m_history_right = 0;
    std::vector<int16_t> m_resampled; // sized in open()
    uint64_t m_frames_queued = 0;
    uint64_t m_frames_dropped = 0;
};

} // namespace model1
