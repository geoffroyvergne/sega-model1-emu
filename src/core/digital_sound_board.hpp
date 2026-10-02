#pragma once

#include "audio/mp2_decoder.hpp"
#include "core/i8251.hpp"
#include "core/z80.hpp"

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace model1 {

// Sega Digital Sound Board (837-10084 "DSB"), as in MAME's dsbz80 device:
// the music hardware of Star Wars Arcade (and of Model 2 / 3 games). A Z80
// at 4 MHz takes commands over a serial line and drives an MPEG audio
// decoder that plays MPEG-1 Layer II music straight from ROM. Emulated
// hardware: no host dependencies.
//
// Serial input: the board's i8251 receives what the Model 1 sound board's
// UART transmits (the 68000 forwards the music commands); its RxRDY output
// is the Z80's INT line.
//
// Z80 memory map: 0x0000-0x7FFF program ROM (the first 32 KB of the EPROM),
// 0x8000-0xFFFF RAM.
// Z80 I/O ports (low address byte):
//   0xE0        write: 0 stop, 1 play once, 2 play and loop (from the start
//               position)
//   0xE2-0xE4   write: start byte address (high, middle, low; the low byte
//               commits it); read: byte position being played
//   0xE5-0xE7   write: end byte address, likewise
//   0xE8        write: volume (inverted: 0 = loudest; bit 7 unknown)
//   0xE9        write: 0 stereo, 1 left on both sides, 2 right on both
//   0xEA, 0xEB  written by Star Wars Arcade at boot; purpose unknown,
//               ignored (MAME maps nothing there)
//   0xF0 / 0xF1 i8251 data / control
// Start and end written while music plays are latched as the loop points:
// at the end of the stream, a looping piece restarts from the latched start
// (with the latched end, if one was written), as MAME does.
//
// Audio: the decoder's samples (32 kHz for Star Wars Arcade), scaled by the
// volume, resampled to the sound board's output rate by linear
// interpolation and added to its mix at full level (MAME's "mpeg" speaker,
// gain 1.0).
class DigitalSoundBoard final : public Z80Bus {
public:
    static constexpr uint32_t k_clock_hz = 4'000'000;
    static constexpr std::size_t k_program_window = 0x8000;
    static constexpr std::size_t k_program_rom_size = 0x20000;
    static constexpr std::size_t k_mpeg_rom_size = 0x800000;
    static constexpr uint16_t k_ram_base = 0x8000;

    explicit DigitalSoundBoard(double output_rate_hz);

    DigitalSoundBoard(const DigitalSoundBoard&) = delete;
    DigitalSoundBoard& operator=(const DigitalSoundBoard&) = delete;

    bool load_program(std::span<const uint8_t> data);
    bool load_mpeg_rom(std::span<const uint8_t> data, std::size_t offset);
    // The board runs only once its program and MPEG data are loaded.
    [[nodiscard]] bool present() const { return m_has_program && m_has_mpeg; }

    void reset();

    // Runs one Z80 instruction (INT follows the UART's RxRDY); returns the
    // T-states it took.
    uint32_t step();

    // Renders `out.size() / 2` stereo frames of music at the output rate.
    void render(std::span<int16_t> out);

    [[nodiscard]] I8251& uart() { return *m_uart; }
    [[nodiscard]] Z80& cpu() { return *m_cpu; }
    [[nodiscard]] bool playing() const { return m_state != 0; }
    [[nodiscard]] uint64_t frames_decoded() const { return m_frames_decoded; }
    [[nodiscard]] uint32_t position_bytes() const { return static_cast<uint32_t>(m_pos >> 3); }

    // Z80Bus
    uint8_t read(uint16_t address) override;
    void write(uint16_t address, uint8_t value) override;
    uint8_t in(uint16_t port) override;
    void out(uint16_t port, uint8_t value) override;

private:
    void trigger(uint8_t value);
    // The next decoded sample pair (before resampling), as the board
    // outputs it; silence when stopped.
    void next_source_frame(int32_t& left, int32_t& right);

    double m_output_rate;
    std::unique_ptr<I8251> m_uart;
    std::unique_ptr<Z80> m_cpu;
    bool m_has_program = false;
    bool m_has_mpeg = false;
    std::array<uint8_t, k_program_window> m_program{};
    std::array<uint8_t, 0x8000> m_ram{};
    std::vector<uint8_t> m_mpeg;

    // MPEG control (MAME's names): playback start / end and the latched
    // loop points, in bytes; the address being assembled by register writes.
    uint32_t m_mp_start = 0, m_mp_end = 0, m_lp_start = 0, m_lp_end = 0;
    uint32_t m_start = 0, m_end = 0;
    uint32_t m_volume = 0x7F, m_pan = 0;
    uint8_t m_state = 0;  // 0 stopped, 1 playing once, 2 looping
    uint64_t m_pos = 0;   // decoder position, in bits

    Mp2Decoder m_decoder;
    std::array<int16_t, Mp2Decoder::k_samples_per_frame * 2> m_decoded{};
    std::size_t m_decoded_pos = 0, m_decoded_count = 0;
    int m_source_rate = 32000;
    uint64_t m_frames_decoded = 0;

    // Resampler: the source frames around the output position, which is
    // m_phase (0..1) of the way from m_previous to m_next.
    std::array<int32_t, 2> m_previous{}, m_next{};
    double m_phase = 0.0;

    std::bitset<256> m_logged_ports;
};

} // namespace model1
