#include "audio/ym3438.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>

namespace model1 {

namespace {

constexpr uint32_t bits(uint32_t value, uint32_t start, uint32_t count = 1)
{
    return (value >> start) & ((1u << count) - 1);
}

// Envelope increments: for each 6-bit rate, eight 4-bit steps (one per
// 3-bit position of the envelope counter). From the YM2608 / YM2612 die
// (Nuked-OPN2), as in ymfm.
constexpr std::array<uint32_t, 64> k_increment_table = {
    0x00000000, 0x00000000, 0x10101010, 0x10101010,  // 0-3
    0x10101010, 0x10101010, 0x11101110, 0x11101110,  // 4-7
    0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 8-11
    0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 12-15
    0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 16-19
    0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 20-23
    0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 24-27
    0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 28-31
    0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 32-35
    0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 36-39
    0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 40-43
    0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 44-47
    0x11111111, 0x21112111, 0x21212121, 0x22212221,  // 48-51
    0x22222222, 0x42224222, 0x42424242, 0x44424442,  // 52-55
    0x44444444, 0x84448444, 0x84848484, 0x88848884,  // 56-59
    0x88888888, 0x88888888, 0x88888888, 0x88888888,  // 60-63
};

uint32_t attenuation_increment(uint32_t rate, uint32_t index)
{
    return bits(k_increment_table[rate], 4 * index, 4);
}

// Detune: phase step adjustment per 5-bit key code for detune 1-3 (0 has
// none; 5-7 are the negatives). The YM2608 manual's table.
constexpr std::array<std::array<uint8_t, 4>, 32> k_detune = {{
    {0, 0, 1, 2},  {0, 0, 1, 2},  {0, 0, 1, 2},  {0, 0, 1, 2},
    {0, 1, 2, 2},  {0, 1, 2, 3},  {0, 1, 2, 3},  {0, 1, 2, 3},
    {0, 1, 2, 4},  {0, 1, 3, 4},  {0, 1, 3, 4},  {0, 1, 3, 5},
    {0, 2, 4, 5},  {0, 2, 4, 6},  {0, 2, 4, 6},  {0, 2, 5, 7},
    {0, 2, 5, 8},  {0, 3, 6, 8},  {0, 3, 6, 9},  {0, 3, 7, 10},
    {0, 4, 8, 11}, {0, 4, 8, 12}, {0, 4, 9, 13}, {0, 5, 10, 14},
    {0, 5, 11, 16}, {0, 6, 12, 17}, {0, 6, 13, 19}, {0, 7, 14, 20},
    {0, 8, 16, 22}, {0, 8, 16, 22}, {0, 8, 16, 22}, {0, 8, 16, 22},
}};

int32_t detune_adjustment(uint32_t detune, uint32_t keycode)
{
    const auto value = static_cast<int32_t>(k_detune[keycode][detune & 3]);
    return bits(detune, 2) != 0 ? -value : value;
}

// LFO pitch modulation: two right shifts of the top 7 frequency bits per
// PM sensitivity and LFO step (a multiply by a 0-2 bit constant), as the
// chip computes it (Nuked-OPN2, ymfm).
constexpr std::array<std::array<uint8_t, 8>, 8> k_pm_shifts = {{
    {0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77},
    {0x77, 0x77, 0x77, 0x77, 0x72, 0x72, 0x72, 0x72},
    {0x77, 0x77, 0x77, 0x72, 0x72, 0x72, 0x17, 0x17},
    {0x77, 0x77, 0x72, 0x72, 0x17, 0x17, 0x12, 0x12},
    {0x77, 0x77, 0x72, 0x17, 0x17, 0x17, 0x12, 0x07},
    {0x77, 0x77, 0x17, 0x12, 0x07, 0x07, 0x02, 0x01},
    {0x77, 0x77, 0x17, 0x12, 0x07, 0x07, 0x02, 0x01},
    {0x77, 0x77, 0x17, 0x12, 0x07, 0x07, 0x02, 0x01},
}};

int32_t pm_adjustment(uint32_t fnum_bits, uint32_t sensitivity, int32_t lfo_pm)
{
    const auto magnitude = static_cast<uint32_t>(lfo_pm < 0 ? -lfo_pm : lfo_pm);
    const uint32_t shifts = k_pm_shifts[sensitivity][magnitude & 7];
    auto adjust = static_cast<int32_t>((fnum_bits >> bits(shifts, 0, 4)) + (fnum_bits >> bits(shifts, 4, 4)));
    if (sensitivity > 5) {
        adjust <<= sensitivity - 5;
    }
    adjust >>= 2;
    return lfo_pm < 0 ? -adjust : adjust;
}

// LFO: samples per step for each rate (3.98 to 72.2 Hz over 128 steps).
constexpr std::array<uint32_t, 8> k_lfo_max_count = {109, 78, 72, 68, 63, 45, 9, 6};

// Algorithms: which value feeds operators 2, 3 and 4, and which operators
// besides 4 reach the output. Inputs: 0 none, 1 O1, 2 O2, 3 O3, 5 O1+O2,
// 6 O1+O3, 7 O2+O3.
struct Algorithm {
    uint8_t op2_in, op3_in, op4_in;
    bool op1_out, op2_out, op3_out;
};
constexpr std::array<Algorithm, 8> k_algorithms = {{
    {1, 2, 3, false, false, false}, // 0: O1 -> O2 -> O3 -> O4
    {0, 5, 3, false, false, false}, // 1: (O1 + O2) -> O3 -> O4
    {0, 2, 6, false, false, false}, // 2: (O1 + (O2 -> O3)) -> O4
    {1, 0, 7, false, false, false}, // 3: ((O1 -> O2) + O3) -> O4
    {1, 0, 3, false, true, false},  // 4: (O1 -> O2) + (O3 -> O4)
    {1, 1, 1, false, true, true},   // 5: O1 -> each of O2, O3, O4
    {1, 0, 0, false, true, true},   // 6: (O1 -> O2) + O3 + O4
    {0, 0, 0, true, true, true},    // 7: O1 + O2 + O3 + O4
}};

constexpr uint32_t k_eg_quiet = 0x380;          // below this the operator is silent
constexpr int32_t k_channel_clip = 256;         // 9-bit channel output
constexpr uint32_t k_channel_shift = 5;         // 14-bit operator -> 9-bit channel
constexpr std::array<uint32_t, 4> k_slot_offset = {0, 8, 4, 12}; // S1, S2, S3, S4

} // namespace

Ym3438::Ym3438(std::string name)
    : m_name(std::move(name))
{
    // Log-sin table: -log2(sin(x)) in 4.8 fixed point over a quarter wave
    // (256 points, sampled at the middle of each step). Power table: the
    // 10-bit mantissa of 2^-(x/256) with its implied leading 1, << 2.
    // These reproduce the chip's internal ROMs (as read from the die).
    constexpr double k_pi = 3.14159265358979323846;
    for (std::size_t i = 0; i < 256; ++i) {
        const double angle = (2.0 * static_cast<double>(i) + 1.0) * k_pi / 1024.0;
        m_sin_table[i] = static_cast<uint16_t>(std::lround(-std::log2(std::sin(angle)) * 256.0));
        const double mantissa = (std::exp2(static_cast<double>(255 - i) / 256.0) - 1.0) * 1024.0;
        m_power_table[i] = static_cast<uint16_t>((static_cast<uint32_t>(std::lround(mantissa)) | 0x400u) << 2);
    }
    for (std::size_t ch = 0; ch < m_channels.size(); ++ch) {
        Channel& channel = m_channels[ch];
        channel.reg = static_cast<uint32_t>((ch % 3) + 0x100 * (ch / 3));
        for (std::size_t op = 0; op < 4; ++op) {
            channel.ops[op].reg = channel.reg + k_slot_offset[op];
        }
    }
    reset();
}

void Ym3438::reset()
{
    for (auto& part : m_regs) {
        part.fill(0);
    }
    // Both outputs enabled on every channel at power-on.
    for (uint32_t part = 0; part < 2; ++part) {
        for (uint32_t r = 0xB4; r <= 0xB6; ++r) {
            m_regs[part][r] = 0xC0;
        }
    }
    m_address.fill(0);
    m_freq_latch.fill(0);
    m_control = 0;
    m_status = 0;
    m_timer_a = 0;
    m_timer_b = 0;
    m_timer_b_prescaler = 0;
    m_clock_remainder = 0;
    for (Channel& channel : m_channels) {
        channel.feedback = {};
        channel.feedback_in = 0;
        for (Operator& op : channel.ops) {
            op.phase = 0;
            op.attenuation = 0x3FF;
            op.state = EnvelopeState::Release;
            op.ssg_inverted = false;
            op.key = false;
            op.key_live = false;
        }
    }
    m_caches_dirty = true;
    m_env_counter = 0;
    m_lfo_counter = 0;
    m_lfo_am = 0;
    m_lfo_pm = 0;
    m_csm_key = false;
    m_dac_data = 0;
    m_dac_enable = false;
    m_output_read = 0;
    m_output_count = 0;
}

uint8_t Ym3438::read(uint32_t /*port*/) const
{
    return m_status; // busy (bit 7) never set: writes complete immediately
}

void Ym3438::write(uint32_t port, uint8_t value)
{
    const uint32_t part = (port >> 1) & 1;
    if ((port & 1) == 0) {
        m_address[part] = value;
        return;
    }
    write_register(part, m_address[part], value);
}

void Ym3438::write_register(uint32_t part, uint8_t reg, uint8_t value)
{
    m_caches_dirty = true;

    // Frequency registers: the high byte (0xA4-0xA6, 0xAC-0xAE) is latched
    // and only takes effect with the following low-byte write.
    if ((reg & 0xF0) == 0xA0) {
        if ((reg & 3) == 3) {
            return;
        }
        const uint32_t latch = bits(reg, 3);
        if (bits(reg, 2) != 0) {
            m_freq_latch[latch] = static_cast<uint8_t>(value & 0x3F);
        } else {
            m_regs[part][reg] = value;
            m_regs[part][reg | 4u] = m_freq_latch[latch];
        }
        return;
    }

    m_regs[part][reg] = value;
    if (part != 0) {
        return;
    }
    switch (reg) {
    case 0x27:
        write_timer_control(value);
        break;
    case 0x28: { // key on / off
        uint32_t channel = bits(value, 0, 2);
        if (channel == 3) {
            break;
        }
        channel += bits(value, 2) * 3;
        for (uint32_t op = 0; op < 4; ++op) {
            m_channels[channel].ops[op].key_live = bits(value, 4 + op) != 0;
        }
        break;
    }
    case 0x2A: // DAC data, unsigned 8 bits -> signed 9 bits
        m_dac_data = static_cast<uint16_t>((m_dac_data & ~0x1FEu) | ((value ^ 0x80u) << 1));
        break;
    case 0x2B:
        m_dac_enable = bits(value, 7) != 0;
        break;
    case 0x2C: // test register: bit 3 is the DAC's low bit
        m_dac_data = static_cast<uint16_t>((m_dac_data & ~1u) | bits(value, 3));
        break;
    default:
        break;
    }
}

void Ym3438::write_timer_control(uint8_t value)
{
    // A timer reloads when it is started (run bit going from 0 to 1).
    if ((value & 0x01) != 0 && (m_control & 0x01) == 0) {
        m_timer_a = timer_a_value();
    }
    if ((value & 0x02) != 0 && (m_control & 0x02) == 0) {
        m_timer_b = m_regs[0][0x26];
        m_timer_b_prescaler = 0;
    }
    if ((value & 0x10) != 0) {
        m_status &= static_cast<uint8_t>(~k_status_timer_a);
    }
    if ((value & 0x20) != 0) {
        m_status &= static_cast<uint8_t>(~k_status_timer_b);
    }
    m_control = static_cast<uint8_t>(value & 0xCF); // reset bits are not stored
}

void Ym3438::clock(uint32_t clocks)
{
    m_clock_remainder += clocks;
    while (m_clock_remainder >= k_clocks_per_sample) {
        m_clock_remainder -= k_clocks_per_sample;
        sample_tick();
    }
}

bool Ym3438::pop_output(int16_t& left, int16_t& right)
{
    if (m_output_count == 0) {
        return false;
    }
    left = m_output[m_output_read * 2];
    right = m_output[m_output_read * 2 + 1];
    m_output_read = (m_output_read + 1) % k_output_buffer_frames;
    --m_output_count;
    return true;
}

void Ym3438::push_output(int32_t left, int32_t right)
{
    if (m_output_count == k_output_buffer_frames) {
        m_output_read = (m_output_read + 1) % k_output_buffer_frames; // drop the oldest
        --m_output_count;
        ++m_output_dropped;
    }
    const std::size_t write = (m_output_read + m_output_count) % k_output_buffer_frames;
    m_output[write * 2] = static_cast<int16_t>(std::clamp(left, -32768, 32767));
    m_output[write * 2 + 1] = static_cast<int16_t>(std::clamp(right, -32768, 32767));
    ++m_output_count;
}

void Ym3438::sample_tick()
{
    if ((m_control & 0x01) != 0 && ++m_timer_a >= 1024) {
        timer_a_overflow();
    }
    if ((m_control & 0x02) != 0 && ++m_timer_b_prescaler >= k_timer_b_prescale) {
        m_timer_b_prescaler = 0;
        if (++m_timer_b >= 256) {
            m_timer_b = m_regs[0][0x26];
            if ((m_control & 0x08) != 0) {
                m_status |= k_status_timer_b;
            }
        }
    }
    clock_fm();
}

void Ym3438::timer_a_overflow()
{
    m_timer_a = timer_a_value();
    if ((m_control & 0x04) != 0) {
        m_status |= k_status_timer_a;
    }
    // CSM mode: each overflow keys channel 3's operators on for one sample.
    if (bits(m_control, 6, 2) == 2) {
        m_csm_key = true;
    }
}

// ---------------------------------------------------------------------------
// FM engine
// ---------------------------------------------------------------------------

void Ym3438::update_caches()
{
    for (Channel& channel : m_channels) {
        for (Operator& op : channel.ops) {
            cache_operator(channel, op);
        }
    }
    m_caches_dirty = false;
}

void Ym3438::cache_operator(const Channel& channel, Operator& op)
{
    // Frequency: block (3 bits) and fnum (11 bits). In channel 3's special
    // mode, operators S1-S3 have their own (0xA8-0xAE).
    uint32_t block_freq = (static_cast<uint32_t>(reg(channel.reg + 0xA4) & 0x3F) << 8) | reg(channel.reg + 0xA0);
    if (bits(m_control, 6, 2) != 0 && channel.reg == 2) {
        int multi = -1;
        if (op.reg == 2) {
            multi = 1;      // S1: 0xA9 / 0xAD
        } else if (op.reg == 10) {
            multi = 2;      // S2: 0xAA / 0xAE
        } else if (op.reg == 6) {
            multi = 0;      // S3: 0xA8 / 0xAC
        }
        if (multi >= 0) {
            const auto m = static_cast<uint32_t>(multi);
            block_freq = (static_cast<uint32_t>(m_regs[0][0xAC + m] & 0x3F) << 8) | m_regs[0][0xA8 + m];
        }
    }
    op.block_freq = block_freq;

    // Key code: block and the top fnum bits (the YM2608 manual's rule for
    // the low bit: F11 & (F10 | F9 | F8) | !F11 & F10 & F9 & F8).
    const uint32_t keycode = (bits(block_freq, 10, 4) << 1) | bits(0xFE80u, bits(block_freq, 7, 4));

    const uint8_t r30 = reg(op.reg + 0x30);
    op.detune = detune_adjustment(bits(r30, 4, 3), keycode);
    op.multiple = bits(r30, 0, 4) * 2;
    if (op.multiple == 0) {
        op.multiple = 1; // multiple 0 = x0.5
    }
    op.phase_step = compute_phase_step(channel, op, 0);
    op.total_level = static_cast<uint32_t>(reg(op.reg + 0x40) & 0x7F) << 3;

    // Sustain level: 4 bits in 3 dB steps, with 15 meaning 93 dB.
    uint32_t sustain = bits(reg(op.reg + 0x80), 4, 4);
    sustain |= (sustain + 1) & 0x10;
    op.sustain = sustain << 5;

    const uint32_t key_scale = keycode >> (bits(reg(op.reg + 0x50), 6, 2) ^ 3);
    auto effective = [key_scale](uint32_t raw) { return raw == 0 ? 0u : std::min(raw + key_scale, 63u); };
    op.rate[static_cast<std::size_t>(EnvelopeState::Attack)] = effective(bits(reg(op.reg + 0x50), 0, 5) * 2);
    op.rate[static_cast<std::size_t>(EnvelopeState::Decay)] = effective(bits(reg(op.reg + 0x60), 0, 5) * 2);
    op.rate[static_cast<std::size_t>(EnvelopeState::Sustain)] = effective(bits(reg(op.reg + 0x70), 0, 5) * 2);
    op.rate[static_cast<std::size_t>(EnvelopeState::Release)] = effective(bits(reg(op.reg + 0x80), 0, 4) * 4 + 2);
}

uint32_t Ym3438::compute_phase_step(const Channel& channel, const Operator& op, int32_t lfo_pm) const
{
    uint32_t fnum = bits(op.block_freq, 0, 11) << 1;
    const uint32_t pm_sensitivity = bits(reg(channel.reg + 0xB4), 0, 3);
    if (pm_sensitivity != 0) {
        fnum = static_cast<uint32_t>(static_cast<int32_t>(fnum)
                                     + pm_adjustment(bits(op.block_freq, 4, 7), pm_sensitivity, lfo_pm));
        fnum &= 0xFFF;
    }
    const uint32_t block = bits(op.block_freq, 11, 3);
    uint32_t step = (fnum << block) >> 2;
    step = static_cast<uint32_t>(static_cast<int32_t>(step) + op.detune) & 0x1FFFF;
    return (step * op.multiple) >> 1;
}

// LFO: a 7-bit counter stepped every k_lfo_max_count samples. AM is a
// triangle (0-0x3F); PM a 5-bit signed triangle (-7..7).
void Ym3438::clock_lfo()
{
    if (bits(m_regs[0][0x22], 3) == 0) {
        m_lfo_counter = 0;
        m_lfo_am = 0x3F; // counter held at 0, where AM is at its maximum
        m_lfo_pm = 0;
        return;
    }
    const uint32_t subcount = m_lfo_counter & 0xFF;
    ++m_lfo_counter;
    if (subcount >= k_lfo_max_count[bits(m_regs[0][0x22], 0, 3)]) {
        m_lfo_counter += 0x101 - subcount;
    }
    m_lfo_am = bits(m_lfo_counter, 8, 6);
    if (bits(m_lfo_counter, 14) == 0) {
        m_lfo_am ^= 0x3F;
    }
    auto pm = static_cast<int32_t>(bits(m_lfo_counter, 10, 3));
    if (bits(m_lfo_counter, 13) != 0) {
        pm ^= 7;
    }
    m_lfo_pm = bits(m_lfo_counter, 14) != 0 ? -pm : pm;
}

void Ym3438::clock_keystate(Operator& op, bool key)
{
    if (key == op.key) {
        return;
    }
    op.key = key;
    if (key) {
        start_attack(op, false);
    } else if (op.state != EnvelopeState::Release) {
        op.state = EnvelopeState::Release;
        if (op.ssg_inverted) {
            op.attenuation = (0x200 - op.attenuation) & 0x3FF;
            op.ssg_inverted = false;
        }
    }
}

void Ym3438::start_attack(Operator& op, bool restart)
{
    if (op.state == EnvelopeState::Attack) {
        return;
    }
    op.state = EnvelopeState::Attack;
    const uint8_t ssg = reg(op.reg + 0x90);
    if (!restart) {
        op.ssg_inverted = bits(ssg, 3) != 0 && bits(ssg, 2) != 0;
        op.phase = 0; // a key-on restarts the waveform
    }
    if (op.rate[static_cast<std::size_t>(EnvelopeState::Attack)] >= 62) {
        op.attenuation = 0; // the fastest attack rates jump straight to full level
    }
}

// SSG-EG: once the attenuation passes the midpoint, repeat, alternate or
// hold the envelope (mode bits: 0 hold, 1 alternate, 2 invert).
void Ym3438::clock_ssg_eg(Operator& op)
{
    if (bits(op.attenuation, 9) == 0) {
        return;
    }
    const uint32_t mode = bits(reg(op.reg + 0x90), 0, 3);
    if (bits(mode, 0) != 0) {
        op.ssg_inverted = (bits(mode, 2) ^ bits(mode, 1)) != 0;
        if (op.state != EnvelopeState::Attack) {
            op.attenuation = op.ssg_inverted ? 0x200 : 0x3FF;
        }
    } else {
        op.ssg_inverted = op.ssg_inverted != (bits(mode, 1) != 0);
        if (op.state == EnvelopeState::Decay || op.state == EnvelopeState::Sustain) {
            start_attack(op, true);
        }
        if (bits(mode, 1) == 0) {
            op.phase = 0;
        }
    }
    if (op.state == EnvelopeState::Release) {
        op.attenuation = 0x3FF;
    }
}

void Ym3438::clock_envelope(Operator& op, uint32_t counter)
{
    if (op.state == EnvelopeState::Attack && op.attenuation == 0) {
        op.state = EnvelopeState::Decay;
    }
    if (op.state == EnvelopeState::Decay && op.attenuation >= op.sustain) {
        op.state = EnvelopeState::Sustain;
    }

    // A rate steps the envelope every 2^(11 - rate / 4) counter ticks (or
    // every tick with larger steps for rates above 47).
    const uint32_t rate = op.rate[static_cast<std::size_t>(op.state)];
    const uint32_t shift = rate >> 2;
    counter <<= shift;
    if (bits(counter, 0, 11) != 0) {
        return;
    }
    const uint32_t increment = attenuation_increment(rate, bits(counter, shift <= 11 ? 11 : shift, 3));

    if (op.state == EnvelopeState::Attack) {
        // Exponential: each step removes a fraction of the remaining
        // attenuation. Rates 62-63 only act at key-on (a chip quirk).
        if (rate < 62) {
            auto attenuation = static_cast<int32_t>(op.attenuation);
            attenuation += (~attenuation * static_cast<int32_t>(increment)) >> 4;
            op.attenuation = static_cast<uint32_t>(attenuation);
        }
        return;
    }
    if (bits(reg(op.reg + 0x90), 3) == 0) {
        op.attenuation += increment;
    } else if (op.attenuation < 0x200) {
        op.attenuation += 4 * increment; // SSG-EG runs 4x faster, up to the midpoint
    }
    if (op.attenuation >= 0x400) {
        op.attenuation = 0x3FF;
    }
}

uint32_t Ym3438::envelope_attenuation(const Operator& op, uint32_t am_offset) const
{
    uint32_t result = op.attenuation;
    if (op.ssg_inverted) {
        result = (0x200 - result) & 0x3FF;
    }
    if (bits(reg(op.reg + 0x60), 7) != 0) {
        result += am_offset;
    }
    result += op.total_level;
    return std::min(result, 0x3FFu);
}

// One operator's output: sin(phase) attenuated by the envelope, as a
// 14-bit signed value.
int32_t Ym3438::compute_volume(const Operator& op, uint32_t phase, uint32_t am_offset) const
{
    if (op.attenuation > k_eg_quiet) {
        return 0;
    }
    uint32_t index = phase & 0x3FF;
    const bool negative = bits(index, 9) != 0;
    if (bits(index, 8) != 0) {
        index = ~index; // second quarter mirrors the first
    }
    const uint32_t sin_attenuation = m_sin_table[index & 0xFF];
    const uint32_t total = sin_attenuation + (envelope_attenuation(op, am_offset) << 2); // 5.8 attenuation
    const auto volume = static_cast<int32_t>(m_power_table[total & 0xFF] >> (total >> 8));
    return negative ? -volume : volume;
}

int32_t Ym3438::channel_output(Channel& channel)
{
    const uint8_t b4 = reg(channel.reg + 0xB4);
    const uint32_t am_offset = (m_lfo_am << 1) >> ((1u << (bits(b4, 4, 2) ^ 3)) - 1);

    // Operator 1, with self-feedback from its last two outputs.
    int32_t modulation = 0;
    const uint32_t feedback = bits(reg(channel.reg + 0xB0), 3, 3);
    if (feedback != 0) {
        modulation = (channel.feedback[0] + channel.feedback[1]) >> (10 - feedback);
    }
    std::array<int32_t, 8> out{};
    out[1] = channel.feedback_in =
        compute_volume(channel.ops[0], (channel.ops[0].phase >> 10) + static_cast<uint32_t>(modulation), am_offset);
    if (bits(b4, 6, 2) == 0) {
        return 0; // no output enabled: only operator 1 runs (for its feedback)
    }

    const Algorithm& algorithm = k_algorithms[bits(reg(channel.reg + 0xB0), 0, 3)];
    out[2] = compute_volume(channel.ops[1], (channel.ops[1].phase >> 10) + static_cast<uint32_t>(out[algorithm.op2_in] >> 1),
                            am_offset);
    out[5] = out[1] + out[2];
    out[3] = compute_volume(channel.ops[2], (channel.ops[2].phase >> 10) + static_cast<uint32_t>(out[algorithm.op3_in] >> 1),
                            am_offset);
    out[6] = out[1] + out[3];
    out[7] = out[2] + out[3];
    int32_t result = compute_volume(channel.ops[3],
                                    (channel.ops[3].phase >> 10) + static_cast<uint32_t>(out[algorithm.op4_in] >> 1),
                                    am_offset) >> k_channel_shift;

    // Carriers are summed with 9-bit clipping.
    const int32_t low = -k_channel_clip - 1;
    if (algorithm.op1_out) {
        result = std::clamp(result + (out[1] >> k_channel_shift), low, k_channel_clip);
    }
    if (algorithm.op2_out) {
        result = std::clamp(result + (out[2] >> k_channel_shift), low, k_channel_clip);
    }
    if (algorithm.op3_out) {
        result = std::clamp(result + (out[3] >> k_channel_shift), low, k_channel_clip);
    }
    return result;
}

void Ym3438::clock_fm()
{
    if (m_caches_dirty) {
        update_caches();
    }

    // Key states (register 0x28, or a CSM pulse on channel 3).
    for (std::size_t ch = 0; ch < m_channels.size(); ++ch) {
        for (Operator& op : m_channels[ch].ops) {
            clock_keystate(op, op.key_live || (m_csm_key && ch == 2));
        }
    }
    m_csm_key = false;

    // Envelope counter: the low 2 bits count 0, 1, 2; the envelope steps
    // when they wrap (every third sample).
    if (bits(++m_env_counter, 0, 2) == 3) {
        ++m_env_counter;
    }
    clock_lfo();
    const bool lfo_on = bits(m_regs[0][0x22], 3) != 0;

    for (Channel& channel : m_channels) {
        channel.feedback[0] = channel.feedback[1];
        channel.feedback[1] = channel.feedback_in;
        const bool pm_active = lfo_on && bits(reg(channel.reg + 0xB4), 0, 3) != 0;
        for (Operator& op : channel.ops) {
            if (bits(reg(op.reg + 0x90), 3) != 0) {
                clock_ssg_eg(op);
            } else {
                op.ssg_inverted = false;
            }
            if (bits(m_env_counter, 0, 2) == 0) {
                clock_envelope(op, m_env_counter >> 2);
            }
            op.phase += pm_active ? compute_phase_step(channel, op, m_lfo_pm) : op.phase_step;
        }
    }

    // Mix: channel 6 plays the DAC instead when enabled. Each channel goes
    // to the left / right output per its register 0xB4 bits 7 / 6.
    int32_t left = 0;
    int32_t right = 0;
    for (std::size_t ch = 0; ch < m_channels.size(); ++ch) {
        Channel& channel = m_channels[ch];
        const uint8_t b4 = reg(channel.reg + 0xB4);
        const int32_t value = ch == 5 && m_dac_enable
                                  ? static_cast<int16_t>(static_cast<uint16_t>(m_dac_data << 7)) >> 7 // 9-bit signed
                                  : channel_output(channel);
        if (bits(b4, 7) != 0) {
            left += value;
        }
        if (bits(b4, 6) != 0) {
            right += value;
        }
    }
    // The chip time-multiplexes the six channels into its DAC; scale the
    // sum so that six full channels reach 16-bit full scale (as MAME).
    push_output(left * 128 / 6, right * 128 / 6);
}

} // namespace model1
