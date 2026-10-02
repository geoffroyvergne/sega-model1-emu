#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace model1 {

// Yamaha YM3438 (OPN2C, the CMOS YM2612) FM chip on the Model 1 sound
// board, clocked at 8 MHz. Emulated hardware: no host (SDL) dependencies.
// Behaviour follows MAME's ymfm library (Aaron Giles, verified against
// Nuked-OPN2's die analysis); the sine and power tables are generated from
// their formulas here.
//
// CPU interface (byte ports):
//   0  address, part 1 (registers 0x21-0xB6, channels 1-3)  read: status
//   1  data, part 1
//   2  address, part 2 (channels 4-6)                        read: status
//   3  data, part 2
// Status: bit 0 = Timer A overflowed, bit 1 = Timer B overflowed,
// bit 7 = busy (always 0 here: writes take effect immediately).
//
// Global registers (part 1):
//   0x22  bit 3 LFO enable, bits 2-0 LFO rate (3.98 - 72.2 Hz)
//   0x24 / 0x25  Timer A (10 bits)     0x26  Timer B (8 bits)
//   0x27  bit 0 / 1  run Timer A / B (reloaded when started)
//         bit 2 / 3  let Timer A / B set its status flag on overflow
//         bit 4 / 5  clear Timer A / B status flag (not stored)
//         bits 7-6   channel 3 mode: 0 normal, 1 per-operator
//                    frequencies (0xA8-0xAE), 2 also CSM (Timer A
//                    overflows key channel 3 on for one sample)
//   0x28  key on/off: bits 2-0 channel (0-2, 4-6), bits 7-4 operators 4-1
//   0x2A  DAC data (8 bits, unsigned)   0x2B  bit 7: channel 6 plays the DAC
//   0x2C  bit 3: DAC low (9th) bit
// Per-operator registers (base + channel 0-2 + operator offset: S1 +0,
// S3 +4, S2 +8, S4 +12; part 2 for channels 4-6):
//   0x30  detune (6-4), multiple (3-0)    0x40  total level (6-0)
//   0x50  key scale (7-6), attack rate (4-0)
//   0x60  AM enable (7), decay rate (4-0) 0x70  sustain rate (4-0)
//   0x80  sustain level (7-4), release rate (3-0)
//   0x90  SSG-EG: enable (3), mode (2-0)
// Per-channel registers:
//   0xA0 / 0xA4  frequency number low 8 bits / block (5-3) and high 3
//                bits (2-0); 0xA4 is latched and applied by the 0xA0 write
//   0xB0  feedback (5-3), algorithm (2-0)
//   0xB4  left (7), right (6), AM sensitivity (5-4), PM sensitivity (2-0)
//
// Timers, counted in FM samples (one per 144 chip clocks, 55.6 kHz):
// Timer A overflows every (1024 - A) samples, Timer B every
// (256 - B) * 16 samples; each then reloads and keeps running.
//
// Synthesis, per sample: each operator's phase advances by
// (fnum << block) >> 1 (+ detune) x multiple, in 10.10 fixed point; its
// output is sin(phase + modulation) attenuated by the envelope, total
// level and LFO AM, through the chip's log-sin / power tables (14-bit
// signed). The algorithm wires the four operators; operator 1 can feed
// back on itself. Each channel's output is clipped to 9 bits and the six
// are summed (on the chip they are time-multiplexed into one DAC).
// Envelopes: attack (exponential), decay to the sustain level, sustain
// (decay at the sustain rate), release; rates are 0-63 after key scaling,
// with the envelope clocked every third sample.
class Ym3438 {
public:
    static constexpr uint32_t k_clock_hz = 8'000'000;
    static constexpr uint32_t k_clocks_per_sample = 144;
    static constexpr uint32_t k_timer_b_prescale = 16; // samples per Timer B count
    static constexpr uint8_t k_status_timer_a = 0x01;
    static constexpr uint8_t k_status_timer_b = 0x02;
    static constexpr std::size_t k_output_buffer_frames = 4096;

    explicit Ym3438(std::string name);

    void reset();

    // Port access (0-3, see above).
    [[nodiscard]] uint8_t read(uint32_t port) const;
    void write(uint32_t port, uint8_t value);

    // Advances the chip by `clocks` input clocks (8 MHz): timers and one
    // stereo output sample per 144 clocks, appended to the output buffer.
    void clock(uint32_t clocks);

    // Output samples produced so far and not yet taken, oldest first. Each
    // sample is about +-32768 at full scale (six channels at full volume),
    // as MAME's YM3438 output. The buffer holds k_output_buffer_frames;
    // when full, the oldest samples are dropped.
    [[nodiscard]] std::size_t output_frames() const { return m_output_count; }
    // Removes the oldest sample; false if none.
    bool pop_output(int16_t& left, int16_t& right);

    [[nodiscard]] uint8_t status() const { return m_status; }
    // Interrupt output: asserted while a status flag is set. Not wired to
    // the 68000 on this board model.
    [[nodiscard]] bool irq() const { return m_status != 0; }
    [[nodiscard]] uint8_t register_value(uint32_t part, uint32_t reg) const
    {
        return m_regs[part & 1][reg & 0xFF];
    }
    [[nodiscard]] uint32_t timer_a_period() const { return 1024 - timer_a_value(); }        // in samples
    [[nodiscard]] uint32_t timer_b_period() const { return (256 - m_regs[0][0x26]) * k_timer_b_prescale; }

    // Inspection for tests: channel 0-5, operator 0-3 (S1, S2, S3, S4).
    [[nodiscard]] uint32_t envelope_attenuation(int channel, int op) const
    {
        return m_channels[static_cast<std::size_t>(channel)].ops[static_cast<std::size_t>(op)].attenuation;
    }
    [[nodiscard]] uint32_t phase_step(int channel, int op) const
    {
        return m_channels[static_cast<std::size_t>(channel)].ops[static_cast<std::size_t>(op)].phase_step;
    }

private:
    enum class EnvelopeState : uint8_t { Attack, Decay, Sustain, Release };

    struct Operator {
        uint32_t reg = 0;          // register offset: part * 0x100 + channel + slot offset
        uint32_t phase = 0;        // 10.10 fixed point
        uint32_t attenuation = 0x3FF; // 10 bits, 0 = loudest
        EnvelopeState state = EnvelopeState::Release;
        bool ssg_inverted = false;
        bool key = false;          // key state as last clocked
        bool key_live = false;     // from register 0x28
        // Cached from the registers.
        uint32_t block_freq = 0;
        int32_t detune = 0;
        uint32_t multiple = 1;     // x.1 (0 means 0.5)
        uint32_t phase_step = 0;
        uint32_t total_level = 0;  // << 3
        uint32_t sustain = 0;      // attenuation where decay stops
        std::array<uint32_t, 4> rate{}; // effective 6-bit rates per state
    };

    struct Channel {
        uint32_t reg = 0;          // register offset: part * 0x100 + channel
        std::array<Operator, 4> ops{}; // S1, S2, S3, S4
        std::array<int32_t, 2> feedback{};
        int32_t feedback_in = 0;
    };

    void write_register(uint32_t part, uint8_t reg, uint8_t value);
    void write_timer_control(uint8_t value);
    void sample_tick();
    void timer_a_overflow();
    [[nodiscard]] uint32_t timer_a_value() const
    {
        return (static_cast<uint32_t>(m_regs[0][0x24]) << 2) | (m_regs[0][0x25] & 3u);
    }

    // FM engine
    [[nodiscard]] uint8_t reg(uint32_t offset) const { return m_regs[(offset >> 8) & 1][offset & 0xFF]; }
    void update_caches();
    void cache_operator(const Channel& channel, Operator& op);
    [[nodiscard]] uint32_t compute_phase_step(const Channel& channel, const Operator& op, int32_t lfo_pm) const;
    void clock_lfo();
    void clock_fm();
    void clock_keystate(Operator& op, bool key);
    void start_attack(Operator& op, bool restart);
    void clock_ssg_eg(Operator& op);
    void clock_envelope(Operator& op, uint32_t counter);
    [[nodiscard]] uint32_t envelope_attenuation(const Operator& op, uint32_t am_offset) const;
    [[nodiscard]] int32_t compute_volume(const Operator& op, uint32_t phase, uint32_t am_offset) const;
    [[nodiscard]] int32_t channel_output(Channel& channel);
    void push_output(int32_t left, int32_t right);

    std::string m_name;
    std::array<std::array<uint8_t, 256>, 2> m_regs{};
    std::array<uint8_t, 2> m_address{}; // latched register number per part
    std::array<uint8_t, 2> m_freq_latch{};  // 0xA4-0xA6 / 0xAC-0xAE high bits, per register group
    uint8_t  m_control = 0;             // register 0x27 (reset bits excluded)
    uint8_t  m_status = 0;
    uint32_t m_timer_a = 0;             // counts up to 1024
    uint32_t m_timer_b = 0;             // counts up to 256
    uint32_t m_timer_b_prescaler = 0;
    uint32_t m_clock_remainder = 0;     // clocks toward the next sample

    std::array<Channel, 6> m_channels{};
    bool m_caches_dirty = true;
    uint32_t m_env_counter = 0;         // x.2: low 2 bits count samples, envelope every 3rd
    uint32_t m_lfo_counter = 0;
    uint32_t m_lfo_am = 0;              // 0-0x3F
    int32_t  m_lfo_pm = 0;              // -7..7
    bool     m_csm_key = false;         // channel 3 keyed on by CSM for this sample
    uint16_t m_dac_data = 0;            // 9 bits, signed (MSB inverted from the register)
    bool     m_dac_enable = false;

    // Output ring buffer (interleaved stereo).
    std::array<int16_t, k_output_buffer_frames * 2> m_output{};
    std::size_t m_output_read = 0;
    std::size_t m_output_count = 0;
    uint64_t m_output_dropped = 0;

    // Chip tables: log-sin (quarter wave, 4.8 attenuation) and power
    // (attenuation fraction -> 11-bit mantissa).
    std::array<uint16_t, 256> m_sin_table{};
    std::array<uint16_t, 256> m_power_table{};
};

} // namespace model1
