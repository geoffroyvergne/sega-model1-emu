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
//      loads the 12-byte sample header, including registers 6-10.
//   2  pitch low bits (bits 7-2) | sample number bit 8 (bit 0)
//   3  octave (bits 7-4, signed -8..7) | pitch high bits (bits 3-0)
//   4  key: bit 7 = 1 starts the sample from the beginning (attack);
//      0 starts the release (or stops at once if the release rate is 15)
//   5  attenuation (bits 7-1, 0 = loudest, 0.375 dB per step); bit 0 = 1
//      sets it at once, 0 glides to it (about 78 ms per 48 dB down,
//      twice as long up)
//   6  LFO frequency (bits 5-3), vibrato depth (bits 2-0)
//   7  attack rate (bits 7-4), decay 1 rate (bits 3-0)
//   8  decay level (bits 7-4), decay 2 rate (bits 3-0)
//   9  key rate scaling (bits 7-4), release rate (bits 3-0)
//   10 tremolo depth (bits 2-0)
//
// Sample ROM (the chip's 22-bit address space): headers at 12 * n:
//   bytes 0-2  start address (bit 22 set = 12-bit samples, else 8-bit)
//   bytes 3-4  loop start, in samples
//   bytes 5-6  0x10000 - sample length (end), in samples
//   byte 7     -> register 6 (LFO frequency, vibrato)
//   bytes 8-10 -> registers 7-9 (envelope)
//   byte 11    -> register 10 (tremolo)
// 0x000000-0x0FFFFF reads the first megabyte of the 4 MB sample ROM;
// 0x100000-0x1FFFFF reads the megabyte selected by the bank register.
//
// Output: stereo, one sample per clock / 224 (10 MHz -> 44,642.86 Hz).
// Playback step = 2^(octave - 1) x (1 + pitch / 1024) ROM samples per
// output sample, with linear interpolation between ROM samples; reaching
// the end jumps back to the loop point.
//
// Envelope (as MAME's gew_pcm, from the YMF278B "OPL4" manual): attack ->
// decay 1 (down to the decay level) -> decay 2 -> release at key-off, on a
// 10-bit level mapped to 96 dB. Rates 1-14 are scaled by key rate scaling
// (octave and pitch); rate 0 holds, 15 is instant. The LFOs add vibrato
// (pitch, up to 79 cents) and tremolo (level, up to 24 dB) at 8 speeds.
//
// Output level as MAME: per voice, the sample x envelope (x4 at full level)
// x attenuation and pan (/4 at full level), summed and clamped to 16 bits.
//
// Not emulated: the effect send (to an external DSP the Model 1 board
// doesn't have). Octave -8 is taken as 2^-9 (MAME wraps it to 2^7).
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
    // Envelope level, 0 (silent, -96 dB) to 0x3FF (full).
    [[nodiscard]] int slot_envelope_level(int slot) const
    {
        return m_slots[static_cast<std::size_t>(slot)].envelope.volume >> k_eg_shift;
    }
    [[nodiscard]] int slot_attenuation(int slot) const
    {
        return m_slots[static_cast<std::size_t>(slot)].total_level >> k_fraction_bits;
    }
    [[nodiscard]] int active_slots() const;
    [[nodiscard]] uint8_t sample_rom_byte(std::size_t offset) const { return m_rom[offset % k_sample_rom_size]; }

private:
    struct Sample {
        uint32_t start = 0;
        uint32_t loop = 0;
        uint32_t end = 0;
        bool twelve_bit = false;
        uint8_t lfo_vibrato = 0;   // header byte 7
        uint8_t lfo_amplitude = 0; // header byte 11, bits 3-0
        uint32_t attack = 0, decay1 = 0, decay2 = 0, decay_level = 0, release = 0, key_rate_scale = 0;
    };

    enum class EnvelopeState : uint8_t { Attack, Decay1, Decay2, Release };

    struct Envelope {
        EnvelopeState state = EnvelopeState::Attack;
        int32_t volume = 0; // 10-bit level, k_eg_shift fraction bits
        uint32_t attack_rate = 0, decay1_rate = 0, decay2_rate = 0, release_rate = 0;
        uint32_t decay_level = 0;
    };

    struct Lfo {
        uint32_t phase = 0;
        uint32_t phase_step = 0;
        const int32_t* table = nullptr; // waveform: phase -> index into scale
        const int32_t* scale = nullptr; // index -> factor, k_lfo_shift fraction bits
    };

    struct Slot {
        std::array<uint8_t, k_register_count> regs{};
        Sample sample;
        bool playing = false;
        uint32_t pan = 0;
        int32_t total_level = 0;      // attenuation 0-127, 12 fraction bits (glides)
        int32_t dest_total_level = 0; // register 5 target
        int32_t total_level_step = 0;
        Envelope envelope;
        uint32_t vibrato = 0, tremolo = 0; // LFO depths (0 = off)
        Lfo pitch_lfo, amplitude_lfo;
        int32_t octave = 0;        // -8..7
        uint32_t pitch = 0;        // 0-1023
        uint32_t step = 0;         // ROM samples per output sample, 12-bit fraction
        uint32_t offset = 0;       // position in samples, 12-bit fraction
        int32_t previous = 0;      // previous ROM sample, for interpolation
    };

    static constexpr uint32_t k_eg_shift = 16;
    static constexpr uint32_t k_lfo_shift = 8;

    void write_slot(Slot& slot, int reg, uint8_t value);
    void retrigger(Slot& slot);
    void envelope_calc(Slot& slot);
    int32_t envelope_update(Slot& slot); // envelope gain, 12 fraction bits
    void lfo_compute_step(Lfo& lfo, uint32_t frequency, uint32_t depth, bool amplitude) const;
    static int32_t lfo_step(Lfo& lfo);
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

    // Envelope rates (per output sample, k_eg_shift fraction bits), the
    // 10-bit level -> gain curve, attenuation glide steps, LFO tables.
    std::array<uint32_t, 64> m_attack_step{};
    std::array<uint32_t, 64> m_decay_release_step{};
    std::array<int32_t, 0x400> m_level_to_gain{};
    std::array<int32_t, 2> m_total_level_steps{}; // down, up
    std::array<int32_t, 256> m_pitch_wave{};
    std::array<int32_t, 256> m_amplitude_wave{};
    std::array<std::array<int32_t, 256>, 8> m_pitch_scale{};
    std::array<std::array<int32_t, 256>, 8> m_amplitude_scale{};

    std::array<uint8_t, k_sample_rom_size> m_rom{};
};

} // namespace model1
