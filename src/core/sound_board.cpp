#include "core/sound_board.hpp"

#include <algorithm>

namespace model1 {

SoundBoard::SoundBoard()
    : m_uart(std::make_unique<I8251>("UART sound"))
    , m_pcm1(std::make_unique<MultiPCM>("MultiPCM 1"))
    , m_pcm2(std::make_unique<MultiPCM>("MultiPCM 2"))
    , m_ym(std::make_unique<Ym3438>("YM3438"))
    , m_bus(std::make_unique<SoundBus>(*m_uart, *m_pcm1, *m_pcm2, *m_ym))
    , m_cpu(std::make_unique<M68000>(*m_bus))
{
}

void SoundBoard::reset()
{
    m_bus->reset();
    m_uart->reset();
    m_pcm1->reset();
    m_pcm2->reset();
    m_ym->reset();
    m_ym_clock_fraction = 0;
    m_cpu->reset();
    m_audio_frames = 0;
    m_audio_generated = 0;
    m_audio_dropped = 0;
    m_fm_previous = {};
    m_fm_next = {};
    m_fm_phase = 0;
    m_fm_underruns = 0;
}

// The FM chip's output at the next mixed-sample time: advance 56/45 of an
// FM sample and interpolate. The chip is clocked by the 68000 ahead of the
// mix, so its samples are normally waiting; if not, the last one is held.
void SoundBoard::next_fm_frame(int32_t& left, int32_t& right)
{
    m_fm_phase += k_fm_step;
    while (m_fm_phase >= k_fm_step_divisor) {
        m_fm_phase -= k_fm_step_divisor;
        m_fm_previous = m_fm_next;
        int16_t l = 0;
        int16_t r = 0;
        if (m_ym->pop_output(l, r)) {
            m_fm_next = {l, r};
        } else {
            ++m_fm_underruns;
        }
    }
    const auto fraction = static_cast<int32_t>(m_fm_phase);
    const auto divisor = static_cast<int32_t>(k_fm_step_divisor);
    left = m_fm_previous[0] + (m_fm_next[0] - m_fm_previous[0]) * fraction / divisor;
    right = m_fm_previous[1] + (m_fm_next[1] - m_fm_previous[1]) * fraction / divisor;
}

uint32_t SoundBoard::step()
{
    // Level-sensitive: asserted while a received byte waits to be read.
    m_cpu->set_irq_level(m_uart->rx_ready() ? k_uart_irq_level : 0);
    const uint32_t cycles = m_cpu->execute_cycle();
    // YM3438 clock = 8/10 of the CPU clock; the remainder carries over.
    m_ym_clock_fraction += cycles * (Ym3438::k_clock_hz / 1'000'000);
    m_ym->clock(m_ym_clock_fraction / (k_cpu_clock_hz / 1'000'000));
    m_ym_clock_fraction %= k_cpu_clock_hz / 1'000'000;
    return cycles;
}

void SoundBoard::generate_audio(std::size_t frames)
{
    m_audio_generated += frames;
    const std::size_t room = k_audio_buffer_frames - m_audio_frames;
    const std::size_t kept = std::min(frames, room);
    m_audio_dropped += frames - kept;

    // The chips always advance by the full amount, even if the host is not
    // taking audio, so playback timing stays correct.
    std::size_t done = 0;
    while (done < frames) {
        const std::size_t chunk = std::min(frames - done, k_audio_buffer_frames);
        m_pcm1->generate(m_scratch1, chunk);
        m_pcm2->generate(m_scratch2, chunk);
        if (m_music) {
            m_music(std::span<int16_t>(m_scratch_music.data(), chunk * 2));
        }
        // Mix with MAME's routing gains. The FM stream advances for every
        // frame; only frames that fit in the buffer are stored.
        const std::size_t stored = done < kept ? std::min(chunk, kept - done) : 0;
        for (std::size_t frame = 0; frame < chunk; ++frame) {
            std::array<int32_t, 2> fm{};
            next_fm_frame(fm[0], fm[1]);
            if (frame >= stored) {
                continue;
            }
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const std::size_t in = frame * 2 + channel;
                int32_t mixed = ((static_cast<int32_t>(m_scratch1[in]) + m_scratch2[in]) * k_pcm_gain_tenths
                                 + fm[channel] * k_fm_gain_tenths)
                                / 10;
                if (m_music) {
                    mixed += m_scratch_music[in];
                }
                m_audio[(m_audio_frames + done + frame) * 2 + channel] =
                    static_cast<int16_t>(std::clamp(mixed, -32768, 32767));
            }
        }
        done += chunk;
    }
    m_audio_frames += kept;
}

std::span<const int16_t> SoundBoard::pending_audio() const
{
    return std::span<const int16_t>(m_audio.data(), m_audio_frames * 2);
}

} // namespace model1
