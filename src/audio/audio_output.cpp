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

    m_step = source_rate / static_cast<double>(k_output_rate);
    m_position = 0.0;
    m_history_left = 0;
    m_history_right = 0;
    // Room for one chunk of output (plus rounding), allocated once.
    m_resampled.assign((static_cast<std::size_t>(static_cast<double>(k_max_chunk_frames) / m_step) + 4) * k_channels, 0);
    m_frames_queued = 0;
    m_frames_dropped = 0;

    SDL_PauseAudioDevice(m_device, 0); // start playback
    std::cerr << "[Audio] Output open: " << obtained.freq << " Hz, 16-bit stereo, " << obtained.samples
              << "-frame buffer; source " << source_rate << " Hz resampled\n";
    return true;
}

void AudioOutput::close()
{
    if (m_device != 0) {
        std::cerr << "[Audio] Output closed: " << m_frames_queued << " frames played, " << m_frames_dropped
                  << " dropped\n";
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

// Linear-interpolation resampler. The input is seen as a continuous stream:
// position 0 is the last frame of the previous submission (the "history"),
// positions 1..n are this submission's frames.
void AudioOutput::submit(std::span<const int16_t> interleaved)
{
    if (m_device == 0) {
        return;
    }
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
