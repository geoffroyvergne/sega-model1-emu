#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace model1 {

// Sega MultiPCM (Yamaha YMW-258-F, Sega part 315-5560): 28-voice sample
// playback chip. The Model 1 sound board has two, driven by its 68000.
// Behaviour follows MAME's multipcm / gew_pcm devices. Emulated hardware: no
// host (SDL) dependencies.
//
// CPU interface (byte ports):
//   0  data      write: value for the selected register of the selected slot
//   1  slot      select: 0-6, 8-14, 16-22, 24-30 -> slots 0-27
//   2  register  select: 0-10
//   reads return 0
//
// Slot registers:
//   0  pan (bits 7-4; 0 = centre, 1-7 = left louder, 8 = muted,
//      9-15 = right louder), effect send level (bits 3-0, not emulated)
//   1  sample number (low 8 bits; bit 8 is register 2 bit 0). Writing it
//      loads the 12-byte sample header.
//   2  pitch low bits (bits 7-2) | sample number bit 8 (bit 0)
//   3  octave (bits 7-4, signed -8..7) | pitch high bits (bits 3-0)
//   4  key: bit 7 = 1 starts the sample from the beginning, 0 stops it
//   5  attenuation (bits 7-1, 0 = loudest, 0.375 dB per step);
//      bit 0 = set immediately (interpolated changes not emulated)
//   6-10  LFO and envelope parameters (stored; not emulated yet)
//
// Sample ROM (the chip's 22-bit address space): headers at 12 * n:
//   bytes 0-2  start address (bit 22 set = 12-bit samples, else 8-bit)
//   bytes 3-4  loop start, in samples
//   bytes 5-6  0x10000 - sample length (end), in samples
//   bytes 7-11 LFO / envelope parameters
// 0x000000-0x0FFFFF reads the first megabyte of the 4 MB sample ROM;
// 0x100000-0x1FFFFF reads the megabyte selected by the bank register.
//
// Output: stereo, one sample per clock / 224 (10 MHz -> 44,642.86 Hz).
// Playback step = 2^(octave - 1) x (1 + pitch / 1024) ROM samples per
// output sample, with linear interpolation between ROM samples; reaching
// the end jumps back to the loop point.
//
// Not emulated yet: envelopes (a keyed-on voice plays at full envelope
// level and stops at key-off), LFOs, interpolated attenuation changes,
// reverse playback and the effect send.
class MultiPCM {
public:
    static constexpr int k_slot_count = 28;
    static constexpr int k_register_count = 11;
    static constexpr uint32_t k_clock_divider = 224;
    static constexpr std::size_t k_sample_rom_size = 0x400000; // 4 MB
    static constexpr uint32_t k_fraction_bits = 12;

    explicit MultiPCM(std::string name);

    MultiPCM(const MultiPCM&) = delete;
    MultiPCM& operator=(const MultiPCM&) = delete;

    void reset();

    // CPU port access.
    void    write(uint32_t port, uint8_t value);
    uint8_t read(uint32_t port) const;

    // Sound board bank register: selects the megabyte seen at 0x100000.
    void set_bank(uint8_t value) { m_bank = value & 3u; }

    // Copies sample data into the sample ROM; false if it does not fit.
    bool load_sample_rom(std::span<const uint8_t> data, std::size_t offset);

    // Renders `frames` stereo output samples (interleaved L, R) at the chip
    // rate. No allocation; safe to call with any frame count.
    void generate(std::span<int16_t> interleaved, std::size_t frames);

    // Inspection for tests and debugging.
    [[nodiscard]] bool slot_playing(int slot) const { return m_slots[static_cast<std::size_t>(slot)].playing; }
    [[nodiscard]] uint32_t slot_step(int slot) const { return m_slots[static_cast<std::size_t>(slot)].step; }
    [[nodiscard]] int active_slots() const;
    [[nodiscard]] uint8_t sample_rom_byte(std::size_t offset) const { return m_rom[offset % k_sample_rom_size]; }

private:
    struct Sample {
        uint32_t start = 0;
        uint32_t loop = 0;
        uint32_t end = 0;
        bool twelve_bit = false;
    };

    struct Slot {
        std::array<uint8_t, k_register_count> regs{};
        Sample sample;
        bool playing = false;
        uint32_t pan = 0;
        uint32_t attenuation = 0;  // 0-127
        int32_t octave = 0;        // -8..7
        uint32_t pitch = 0;        // 0-1023
        uint32_t step = 0;         // ROM samples per output sample, 12-bit fraction
        uint32_t offset = 0;       // position in samples, 12-bit fraction
        int32_t previous = 0;      // previous ROM sample, for interpolation
    };

    void write_slot(Slot& slot, int reg, uint8_t value);
    void load_sample_header(Sample& sample, uint32_t index) const;
    void update_step(Slot& slot);
    [[nodiscard]] uint8_t read_sample_byte(uint32_t address) const;
    [[nodiscard]] int32_t fetch(const Slot& slot, uint32_t position) const;

    std::string m_name;
    std::array<Slot, k_slot_count> m_slots{};
    int m_selected_slot = 0;        // -1 = invalid slot number written
    uint32_t m_selected_register = 0;
    uint32_t m_bank = 0;
    bool m_logged_invalid_slot = false;

    // (pan << 7 | attenuation) -> gain, 12-bit fraction.
    std::array<int32_t, 0x800> m_left_gain{};
    std::array<int32_t, 0x800> m_right_gain{};

    std::array<uint8_t, k_sample_rom_size> m_rom{};
};

} // namespace model1
