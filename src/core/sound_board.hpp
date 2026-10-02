#pragma once

#include "audio/multipcm.hpp"
#include "audio/ym3438.hpp"
#include "core/i8251.hpp"
#include "core/m68000.hpp"
#include "core/sound_bus.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace model1 {

// Model 1 sound board ("Sega Model 1 sound board", MAME segam1audio): a
// 68000 at 10 MHz with its own address space, and the i8251 UART that
// receives commands from the main board over a 31.25 kbaud serial line.
// The UART's RxRDY output drives the 68000's interrupt level 2.
//
// Audio: a stereo stream at the MultiPCM output rate, 10 MHz / 224 =
// 44,642.86 Hz, mixing (with MAME's gains) the two MultiPCM chips at 0.5
// each and the YM3438 FM chip at 0.3. The YM3438 runs at 8 MHz from the
// 68000's cycle count and produces a sample every 144 clocks (55,555.6 Hz),
// exactly 56 FM samples for every 45 mixed samples; its stream is
// resampled by linear interpolation.
class SoundBoard {
public:
    static constexpr uint32_t k_cpu_clock_hz = 10'000'000;
    static constexpr int k_uart_irq_level = 2;
    static constexpr double k_audio_rate_hz = static_cast<double>(k_cpu_clock_hz) / MultiPCM::k_clock_divider;
    // FM samples per mixed sample: (8 MHz / 144) / (10 MHz / 224) = 56 / 45.
    static constexpr uint32_t k_fm_step = 56;
    static constexpr uint32_t k_fm_step_divisor = 45;
    // Mixing gains, in tenths (MAME: MultiPCM 0.5, YM3438 0.3).
    static constexpr int32_t k_pcm_gain_tenths = 5;
    static constexpr int32_t k_fm_gain_tenths = 3;

    // Capacity of the internal audio buffer, in stereo frames (~90 ms).
    static constexpr std::size_t k_audio_buffer_frames = 4096;

    SoundBoard();

    SoundBoard(const SoundBoard&) = delete;
    SoundBoard& operator=(const SoundBoard&) = delete;

    // Resets RAM, the UART and the CPU (which reads its reset vectors from
    // the sound ROM, so load the ROM first).
    void reset();

    // Runs one 68000 step with the interrupt line updated from the UART;
    // returns the cycles it took.
    uint32_t step();

    [[nodiscard]] I8251& uart() { return *m_uart; }
    [[nodiscard]] SoundBus& bus() { return *m_bus; }
    [[nodiscard]] M68000& cpu() { return *m_cpu; }
    [[nodiscard]] MultiPCM& pcm1() { return *m_pcm1; }
    [[nodiscard]] MultiPCM& pcm2() { return *m_pcm2; }
    [[nodiscard]] Ym3438& ym() { return *m_ym; }

    // Renders `frames` more stereo frames of mixed output into the internal
    // buffer (frames beyond its free space are dropped and counted).
    void generate_audio(std::size_t frames);

    // Rendered audio not yet taken (interleaved L, R at k_audio_rate_hz).
    [[nodiscard]] std::span<const int16_t> pending_audio() const;
    // Empties the buffer once the host has consumed it.
    void clear_audio() { m_audio_frames = 0; }
    [[nodiscard]] uint64_t audio_frames_generated() const { return m_audio_generated; }
    [[nodiscard]] uint64_t audio_frames_dropped() const { return m_audio_dropped; }
    // FM samples the mix needed before the chip had produced them (should
    // stay 0: the 68000, which clocks the chip, runs ahead of the mix).
    [[nodiscard]] uint64_t fm_underruns() const { return m_fm_underruns; }

private:
    std::unique_ptr<I8251> m_uart;
    std::unique_ptr<MultiPCM> m_pcm1;
    std::unique_ptr<MultiPCM> m_pcm2;
    std::unique_ptr<Ym3438> m_ym;
    uint32_t m_ym_clock_fraction = 0; // 68000 cycles * 8, below one YM clock (10)
    std::unique_ptr<SoundBus> m_bus;
    std::unique_ptr<M68000> m_cpu;

    // Mixed output waiting for the host, and per-chip scratch buffers.
    std::array<int16_t, k_audio_buffer_frames * 2> m_audio{};
    std::array<int16_t, k_audio_buffer_frames * 2> m_scratch1{};
    std::array<int16_t, k_audio_buffer_frames * 2> m_scratch2{};
    std::size_t m_audio_frames = 0;
    uint64_t m_audio_generated = 0;
    uint64_t m_audio_dropped = 0;

    // FM resampler: the two FM samples around the current position, and
    // the position between them in 45ths.
    std::array<int32_t, 2> m_fm_previous{};
    std::array<int32_t, 2> m_fm_next{};
    uint32_t m_fm_phase = 0;
    uint64_t m_fm_underruns = 0; // FM samples needed before the chip made them

    void next_fm_frame(int32_t& left, int32_t& right);
};

} // namespace model1
