#include "audio/audio_output.hpp"

#include <SDL.h>

#include <algorithm>
#include <iostream>

namespace model1 {

namespace {

// Largest input chunk processed at once; bigger submissions are split.
constexpr std::size_t k_max_chunk_frames = 4096;

} // namespace

AudioOutput::~AudioOutput()
{
    close();
}

bool AudioOutput::open(double source_rate)
{
    close();

    SDL_AudioSpec wanted{};
    wanted.freq = k_output_rate;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = k_channels;
    wanted.samples = k_device_buffer_frames;
    wanted.callback = nullptr; // queue API

    SDL_AudioSpec obtained{};
    m_device = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0); // no format changes allowed
    if (m_device == 0) {
        std::cerr << "[Audio] No audio device (" << SDL_GetError() << "): running without sound\n";
        return false;
    }

    m_base_step = source_rate / static_cast<double>(k_output_rate);
    m_step = m_base_step;
    m_smoothed_queue = k_target_queued_frames;
    m_started = false;
    m_position = 0.0;
    m_history_left = 0;
    m_history_right = 0;
    // Room for one chunk of output (plus rounding), allocated once.
    // (The step can be k_max_rate_adjust below nominal.)
    m_resampled.assign(
        (static_cast<std::size_t>(static_cast<double>(k_max_chunk_frames) / (m_base_step * (1.0 - k_max_rate_adjust))) + 4)
            * k_channels,
        0);
    m_silence.assign(static_cast<std::size_t>(k_target_queued_frames) * k_channels, 0);
    m_frames_queued = 0;
    m_frames_dropped = 0;
    m_frames_padded = 0;
    m_underruns = 0;

    SDL_PauseAudioDevice(m_device, 0); // start playback
    std::cerr << "[Audio] Output open: " << obtained.freq << " Hz, 16-bit stereo, " << obtained.samples
              << "-frame buffer; source " << source_rate << " Hz resampled\n";
    return true;
}

void AudioOutput::close()
{
    if (m_device != 0) {
        std::cerr << "[Audio] Output closed: " << m_frames_queued << " frames played, " << m_frames_dropped
                  << " dropped, " << m_underruns << " underruns (" << m_frames_padded << " frames of silence inserted)\n";
        SDL_PauseAudioDevice(m_device, 1);
        SDL_ClearQueuedAudio(m_device);
        SDL_CloseAudioDevice(m_device);
        m_device = 0;
    }
}

std::size_t AudioOutput::queued_frames() const
{
    if (m_device == 0) {
        return 0;
    }
    return SDL_GetQueuedAudioSize(m_device) / (k_channels * sizeof(int16_t));
}

double AudioOutput::rate_factor(double queued, double target)
{
    const double error = std::clamp((queued - target) / target, -1.0, 1.0);
    return 1.0 + k_max_rate_adjust * error;
}

// Linear-interpolation resampler. The input is seen as a continuous stream:
// position 0 is the last frame of the previous submission (the "history"),
// positions 1..n are this submission's frames.
void AudioOutput::submit(std::span<const int16_t> interleaved)
{
    if (m_device == 0) {
        return;
    }

    // Keep a margin: top an (almost) empty queue up with silence, so that
    // this delivery plus the margin reach the target level.
    const std::size_t queued = queued_frames();
    if (m_started && queued == 0) {
        ++m_underruns;
    }
    if (queued < static_cast<std::size_t>(k_device_buffer_frames)) {
        const auto incoming = static_cast<std::size_t>(static_cast<double>(interleaved.size() / k_channels) / m_base_step);
        const std::size_t target = static_cast<std::size_t>(k_target_queued_frames);
        const std::size_t pad = target > queued + incoming ? target - queued - incoming : 0;
        if (pad > 0 && SDL_QueueAudio(m_device, m_silence.data(),
                                      static_cast<Uint32>(pad * k_channels * sizeof(int16_t))) == 0) {
            m_frames_padded += pad;
        }
        m_smoothed_queue = k_target_queued_frames;
    }
    m_started = true;

    // Steer the queue toward the target. The measured level saw-tooths with
    // each delivery and device read, so it is smoothed first.
    m_smoothed_queue += (static_cast<double>(queued_frames()) - m_smoothed_queue) * 0.05;
    m_step = m_base_step * rate_factor(m_smoothed_queue, k_target_queued_frames);

    std::size_t offset = 0;
    const std::size_t total = interleaved.size() / k_channels;
    while (offset < total) {
        const std::size_t n = std::min(total - offset, k_max_chunk_frames);
        const int16_t* in = interleaved.data() + offset * k_channels;
        auto sample = [&](std::size_t index, int channel) -> int32_t {
            if (index == 0) {
                return channel == 0 ? m_history_left : m_history_right;
            }
            return in[(index - 1) * k_channels + static_cast<std::size_t>(channel)];
        };

        std::size_t produced = 0;
        while (m_position < static_cast<double>(n) && produced * k_channels + 1 < m_resampled.size()) {
            const auto index = static_cast<std::size_t>(m_position);
            const double fraction = m_position - static_cast<double>(index);
            for (int channel = 0; channel < k_channels; ++channel) {
                const int32_t a = sample(index, channel);
                const int32_t b = sample(index + 1, channel);
                m_resampled[produced * k_channels + static_cast<std::size_t>(channel)] =
                    static_cast<int16_t>(a + static_cast<int32_t>(static_cast<double>(b - a) * fraction));
            }
            ++produced;
            m_position += m_step;
        }
        m_position -= static_cast<double>(n);
        m_history_left = in[(n - 1) * k_channels];
        m_history_right = in[(n - 1) * k_channels + 1];
        offset += n;

        if (queued_frames() > static_cast<std::size_t>(k_max_queued_frames)) {
            m_frames_dropped += produced; // keep latency bounded
            continue;
        }
        if (SDL_QueueAudio(m_device, m_resampled.data(),
                           static_cast<Uint32>(produced * k_channels * sizeof(int16_t))) != 0) {
            m_frames_dropped += produced;
            continue;
        }
        m_frames_queued += produced;
    }
}

} // namespace model1
