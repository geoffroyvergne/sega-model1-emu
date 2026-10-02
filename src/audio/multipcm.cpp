#include "audio/multipcm.hpp"

#include "core/log.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>

namespace model1 {

namespace {

// Slot-select values: groups of 7 slots, every 8th value unused.
constexpr std::array<int, 32> k_value_to_slot = {
    0, 1, 2, 3, 4, 5, 6, -1,
    7, 8, 9, 10, 11, 12, 13, -1,
    14, 15, 16, 17, 18, 19, 20, -1,
    21, 22, 23, 24, 25, 26, 27, -1,
};

int32_t to_fixed(float value, uint32_t bits = MultiPCM::k_fraction_bits)
{
    return static_cast<int32_t>(value * static_cast<float>(1u << bits));
}

// Envelope times in ms for rates 0-63 (attack from silence to full; decay
// and release take k_decay_ratio times longer), on a 44.1 kHz timebase.
// From MAME's gew_pcm (YMF278B manual values).
constexpr std::array<double, 64> k_base_times = {
    0,       0,       0,       0,
    6222.95, 4978.37, 4148.66, 3556.01,
    3111.47, 2489.21, 2074.33, 1778.00,
    1555.74, 1244.63, 1037.19, 889.02,
    777.87,  622.31,  518.59,  444.54,
    388.93,  311.16,  259.32,  222.27,
    194.47,  155.60,  129.66,  111.16,
    97.23,   77.82,   64.85,   55.60,
    48.62,   38.91,   32.43,   27.80,
    24.31,   19.46,   16.24,   13.92,
    12.15,   9.75,    8.12,    6.98,
    6.08,    4.90,    4.08,    3.49,
    3.04,    2.49,    2.13,    1.90,
    1.72,    1.41,    1.18,    1.04,
    0.91,    0.73,    0.59,    0.50,
    0.45,    0.45,    0.45,    0.45,
};
constexpr double k_decay_ratio = 14.32833;

// LFO speeds (Hz), vibrato depths (cents) and tremolo depths (dB).
constexpr std::array<float, 8> k_lfo_freq = {0.168f, 2.019f, 3.196f, 4.206f, 5.215f, 5.888f, 6.224f, 7.066f};
constexpr std::array<float, 8> k_vibrato_cents = {0.0f, 3.378f, 5.065f, 6.750f, 10.114f, 20.170f, 40.180f, 79.307f};
constexpr std::array<float, 8> k_tremolo_db = {0.0f, 0.4f, 0.8f, 1.5f, 3.0f, 6.0f, 12.0f, 24.0f};

constexpr float k_output_rate = 10'000'000.0f / static_cast<float>(MultiPCM::k_clock_divider);

} // namespace

MultiPCM::MultiPCM(std::string name)
    : m_name(std::move(name))
{
    // Gain tables (MAME gew_pcm): attenuation 0.375 dB per step with an
    // overall /4 for mixing headroom; pan attenuates one side 3 dB per step.
    for (int level = 0; level < 0x80; ++level) {
        const float total = std::pow(10.0f, (static_cast<float>(level) * -24.0f / 64.0f) / 20.0f) / 4.0f;
        for (int pan = 0; pan < 0x10; ++pan) {
            float left = 1.0f;
            float right = 1.0f;
            if (pan == 0x8) {
                left = right = 0.0f;
            } else if ((pan & 0x8) != 0) {
                const int inverted = 0x10 - pan;
                right = (inverted & 7) == 7 ? 0.0f : std::pow(10.0f, (static_cast<float>(inverted) * -12.0f / 4.0f) / 20.0f);
            } else if (pan != 0) {
                left = (pan & 7) == 7 ? 0.0f : std::pow(10.0f, (static_cast<float>(pan) * -12.0f / 4.0f) / 20.0f);
            }
            m_left_gain[static_cast<std::size_t>((pan << 7) | level)] = to_fixed(left * total);
            m_right_gain[static_cast<std::size_t>((pan << 7) | level)] = to_fixed(right * total);
        }
    }

    // Envelope steps: the full 0x400 range over the rate's time.
    for (std::size_t i = 4; i < 64; ++i) {
        const auto full = static_cast<float>(0x400u << k_eg_shift);
        m_attack_step[i] = static_cast<uint32_t>(full / static_cast<float>(k_base_times[i] * 44100.0 / 1000.0));
        m_decay_release_step[i] =
            static_cast<uint32_t>(full / static_cast<float>(k_base_times[i] * k_decay_ratio * 44100.0 / 1000.0));
    }
    m_attack_step[0x3F] = 0x400u << k_eg_shift; // rate 63: instant

    // Envelope level -> gain: 0x400 steps over 96 dB.
    for (int i = 0; i < 0x400; ++i) {
        const float db = -(96.0f - 96.0f * static_cast<float>(i) / 1024.0f);
        m_level_to_gain[static_cast<std::size_t>(i)] = to_fixed(std::pow(10.0f, db / 20.0f));
    }

    // Attenuation glide: 128 steps in 78.2 ms down, twice as long up.
    const auto range = static_cast<float>(0x80 << k_fraction_bits);
    m_total_level_steps[0] = static_cast<int32_t>(-range / (78.2f * 44100.0f / 1000.0f));
    m_total_level_steps[1] = static_cast<int32_t>(range / (78.2f * 2 * 44100.0f / 1000.0f));

    // LFO waveforms: a triangle for pitch (centred on 128), a falling then
    // rising ramp for amplitude.
    for (int i = 0; i < 256; ++i) {
        const auto index = static_cast<std::size_t>(i);
        if (i < 64) {
            m_pitch_wave[index] = i * 2 + 128;
        } else if (i < 128) {
            m_pitch_wave[index] = 383 - i * 2;
        } else if (i < 192) {
            m_pitch_wave[index] = 384 - i * 2;
        } else {
            m_pitch_wave[index] = i * 2 - 383;
        }
        m_amplitude_wave[index] = i < 128 ? 255 - i * 2 : i * 2 - 256;
    }
    for (std::size_t depth = 0; depth < 8; ++depth) {
        for (int i = -128; i < 128; ++i) {
            const float cents = k_vibrato_cents[depth] * static_cast<float>(i) / 128.0f;
            m_pitch_scale[depth][static_cast<std::size_t>(i + 128)] =
                to_fixed(std::pow(2.0f, cents / 1200.0f), k_lfo_shift);
        }
        for (int i = 0; i < 256; ++i) {
            const float db = -k_tremolo_db[depth] * static_cast<float>(i) / 256.0f;
            m_amplitude_scale[depth][static_cast<std::size_t>(i)] = to_fixed(std::pow(10.0f, db / 20.0f), k_lfo_shift);
        }
    }
}

void MultiPCM::reset()
{
    m_slots = {};
    m_selected_slot = 0;
    m_selected_register = 0;
    m_bank = 0;
}

int MultiPCM::active_slots() const
{
    return static_cast<int>(std::count_if(m_slots.begin(), m_slots.end(), [](const Slot& s) { return s.playing; }));
}

bool MultiPCM::load_sample_rom(std::span<const uint8_t> data, std::size_t offset)
{
    if (offset > k_sample_rom_size || data.size() > k_sample_rom_size - offset) {
        std::cerr << "[" << m_name << "] load_sample_rom: " << data.size() << " bytes do not fit at offset "
                  << Hex{static_cast<uint32_t>(offset)} << '\n';
        return false;
    }
    std::copy(data.begin(), data.end(), m_rom.begin() + static_cast<std::ptrdiff_t>(offset));
    return true;
}

// ---------------------------------------------------------------------------
// Register interface
// ---------------------------------------------------------------------------

uint8_t MultiPCM::read(uint32_t /*port*/) const
{
    return 0;
}

void MultiPCM::write(uint32_t port, uint8_t value)
{
    switch (port) {
    case 0:
        if (m_selected_slot < 0) {
            if (!m_logged_invalid_slot) {
                m_logged_invalid_slot = true;
                std::cerr << "[" << m_name << "] WARNING: register write to an invalid slot number, ignored\n";
            }
            return;
        }
        if (m_selected_register < k_register_count) {
            write_slot(m_slots[static_cast<std::size_t>(m_selected_slot)], static_cast<int>(m_selected_register), value);
        }
        return;
    case 1:
        m_selected_slot = k_value_to_slot[value & 0x1F];
        return;
    case 2:
        m_selected_register = value;
        return;
    default:
        return; // effect DSP control (port 0xD) not emulated
    }
}

void MultiPCM::write_slot(Slot& slot, int reg, uint8_t value)
{
    slot.regs[static_cast<std::size_t>(reg)] = value;
    switch (reg) {
    case 0:
        slot.pan = (value >> 4) & 0xF;
        break;
    case 1: {
        // The header also loads the LFO and envelope registers.
        load_sample_header(slot.sample, slot.regs[1] | ((slot.regs[2] & 1u) << 8));
        slot.regs[7] = static_cast<uint8_t>((slot.sample.attack << 4) | slot.sample.decay1);
        slot.regs[8] = static_cast<uint8_t>((slot.sample.decay_level << 4) | slot.sample.decay2);
        slot.regs[9] = static_cast<uint8_t>((slot.sample.key_rate_scale << 4) | slot.sample.release);
        write_slot(slot, 6, slot.sample.lfo_vibrato);
        write_slot(slot, 10, slot.sample.lfo_amplitude);
        if (slot.playing) {
            retrigger(slot); // a new sample on a sounding voice restarts it
        }
        break;
    }
    case 2:
    case 3:
        slot.octave = static_cast<int8_t>(slot.regs[3]) >> 4; // signed 4-bit
        slot.pitch = ((slot.regs[3] & 0xFu) << 6) | (slot.regs[2] >> 2);
        update_step(slot);
        break;
    case 4:
        if ((value & 0x80) != 0) {
            slot.playing = true;
            retrigger(slot);
        } else if (slot.playing) {
            if (slot.sample.release != 0xF) {
                slot.envelope.state = EnvelopeState::Release;
            } else {
                slot.playing = false; // release rate 15: stop at once
            }
        }
        break;
    case 5:
        slot.dest_total_level = (value >> 1) & 0x7F;
        if ((value & 1) == 0) {
            // Glide toward the new attenuation.
            slot.total_level_step = (slot.total_level >> k_fraction_bits) > slot.dest_total_level
                                        ? m_total_level_steps[0]
                                        : m_total_level_steps[1];
        } else {
            slot.total_level = slot.dest_total_level << k_fraction_bits;
        }
        break;
    case 6:
    case 10: {
        const uint32_t frequency = (slot.regs[6] >> 3) & 7u;
        slot.vibrato = slot.regs[6] & 7u;
        slot.tremolo = slot.regs[10] & 7u;
        if (value != 0) {
            lfo_compute_step(slot.pitch_lfo, frequency, slot.vibrato, false);
            lfo_compute_step(slot.amplitude_lfo, frequency, slot.tremolo, true);
        }
        break;
    }
    case 7:
    case 8:
    case 9:
        slot.sample.attack = slot.regs[7] >> 4;
        slot.sample.decay1 = slot.regs[7] & 0xFu;
        slot.sample.decay_level = slot.regs[8] >> 4;
        slot.sample.decay2 = slot.regs[8] & 0xFu;
        slot.sample.key_rate_scale = slot.regs[9] >> 4;
        slot.sample.release = slot.regs[9] & 0xFu;
        envelope_calc(slot);
        break;
    default:
        break;
    }
}

void MultiPCM::retrigger(Slot& slot)
{
    slot.offset = 0;
    slot.previous = 0;
    slot.total_level = slot.dest_total_level << k_fraction_bits;
    envelope_calc(slot);
    slot.envelope.state = EnvelopeState::Attack;
    slot.envelope.volume = (0x3FF - 0x2A0) << k_eg_shift; // the attack starts at -63 dB
}

// Rates: register value x 4 plus the key rate scaling offset (from the
// octave and pitch); 0 holds, 15 is the fastest.
void MultiPCM::envelope_calc(Slot& slot)
{
    int32_t rate = 0;
    if (slot.sample.key_rate_scale != 0xF) {
        rate = (slot.octave + static_cast<int32_t>(slot.sample.key_rate_scale)) * 2
             + static_cast<int32_t>((slot.pitch >> 9) & 1u);
    }
    auto get_rate = [rate](const std::array<uint32_t, 64>& steps, uint32_t value) {
        if (value == 0) {
            return steps[0];
        }
        if (value == 0xF) {
            return steps[0x3F];
        }
        return steps[static_cast<std::size_t>(std::clamp(4 * static_cast<int32_t>(value) + rate, 0, 0x3F))];
    };
    Envelope& eg = slot.envelope;
    eg.attack_rate = get_rate(m_attack_step, slot.sample.attack);
    eg.decay1_rate = get_rate(m_decay_release_step, slot.sample.decay1);
    eg.decay2_rate = get_rate(m_decay_release_step, slot.sample.decay2);
    eg.release_rate = get_rate(m_decay_release_step, slot.sample.release);
    eg.decay_level = 0xF - slot.sample.decay_level;
}

int32_t MultiPCM::envelope_update(Slot& slot)
{
    Envelope& eg = slot.envelope;
    switch (eg.state) {
    case EnvelopeState::Attack:
        // Exponential approach toward a point beyond full level.
        eg.volume += static_cast<int32_t>(
            (static_cast<int64_t>((0x817 << (k_eg_shift - 1)) - eg.volume) * eg.attack_rate) >> 24);
        if (eg.volume >= (0x3FF << k_eg_shift)) {
            eg.state = eg.decay1_rate >= (0x400u << k_eg_shift) ? EnvelopeState::Decay2 : EnvelopeState::Decay1;
            eg.volume = 0x3FF << k_eg_shift;
        }
        break;
    case EnvelopeState::Decay1:
        eg.volume = std::max(eg.volume - static_cast<int32_t>(eg.decay1_rate), 0);
        if (static_cast<uint32_t>(eg.volume >> (k_eg_shift + 6)) <= eg.decay_level) {
            eg.state = EnvelopeState::Decay2;
        }
        break;
    case EnvelopeState::Decay2:
        eg.volume = std::max(eg.volume - static_cast<int32_t>(eg.decay2_rate), 0);
        break;
    case EnvelopeState::Release:
        eg.volume -= static_cast<int32_t>(eg.release_rate);
        if (eg.volume <= 0) {
            eg.volume = 0;
            slot.playing = false;
        }
        break;
    }
    return m_level_to_gain[static_cast<std::size_t>(eg.volume >> k_eg_shift)];
}

void MultiPCM::lfo_compute_step(Lfo& lfo, uint32_t frequency, uint32_t depth, bool amplitude) const
{
    const float step = k_lfo_freq[frequency] * 256.0f / k_output_rate;
    lfo.phase_step = static_cast<uint32_t>(static_cast<float>(1u << k_lfo_shift) * step);
    lfo.table = amplitude ? m_amplitude_wave.data() : m_pitch_wave.data();
    lfo.scale = amplitude ? m_amplitude_scale[depth].data() : m_pitch_scale[depth].data();
}

// Advances the LFO and returns its factor, k_fraction_bits fraction bits.
int32_t MultiPCM::lfo_step(Lfo& lfo)
{
    lfo.phase += lfo.phase_step;
    const int32_t index = lfo.table[(lfo.phase >> k_lfo_shift) & 0xFF];
    return lfo.scale[static_cast<std::size_t>(index)] << (k_fraction_bits - k_lfo_shift);
}

void MultiPCM::load_sample_header(Sample& sample, uint32_t index) const
{
    const uint32_t a = index * 12;
    const uint32_t start = (static_cast<uint32_t>(read_sample_byte(a)) << 16)
                         | (static_cast<uint32_t>(read_sample_byte(a + 1)) << 8) | read_sample_byte(a + 2);
    sample.twelve_bit = (start & 0x400000) != 0;
    sample.start = start & 0x3FFFFF;
    sample.loop = (static_cast<uint32_t>(read_sample_byte(a + 3)) << 8) | read_sample_byte(a + 4);
    sample.end = 0x10000 - ((static_cast<uint32_t>(read_sample_byte(a + 5)) << 8) | read_sample_byte(a + 6));
    sample.lfo_vibrato = read_sample_byte(a + 7);
    sample.attack = read_sample_byte(a + 8) >> 4;
    sample.decay1 = read_sample_byte(a + 8) & 0xFu;
    sample.decay_level = read_sample_byte(a + 9) >> 4;
    sample.decay2 = read_sample_byte(a + 9) & 0xFu;
    sample.key_rate_scale = read_sample_byte(a + 10) >> 4;
    sample.release = read_sample_byte(a + 10) & 0xFu;
    sample.lfo_amplitude = static_cast<uint8_t>(read_sample_byte(a + 11) & 0xFu);
}

void MultiPCM::update_step(Slot& slot)
{
    // (1 + pitch / 1024) in 12-bit fixed point, scaled by 2^(octave - 1).
    const uint32_t base = (1024 + slot.pitch) << 2;
    const int shift = slot.octave - 1;
    slot.step = shift >= 0 ? base << shift : base >> -shift;
}

uint8_t MultiPCM::read_sample_byte(uint32_t address) const
{
    address &= 0x3FFFFF; // 22 address pins
    if (address < 0x100000) {
        return m_rom[address];
    }
    if (address < 0x200000) {
        return m_rom[m_bank * 0x100000 + (address - 0x100000)];
    }
    return 0; // nothing mapped above 2 MB on the sound board
}

// ROM sample at `position` as a signed 16-bit value.
int32_t MultiPCM::fetch(const Slot& slot, uint32_t position) const
{
    if (slot.sample.twelve_bit) {
        // Two 12-bit samples packed in 3 bytes: ab.c ..  /  ..C. AB
        const uint32_t address = slot.sample.start + (position >> 1) * 3;
        if ((position & 1) == 0) {
            return static_cast<int16_t>((read_sample_byte(address) << 8) | ((read_sample_byte(address + 1) & 0x0F) << 4));
        }
        return static_cast<int16_t>((read_sample_byte(address + 2) << 8) | (read_sample_byte(address + 1) & 0xF0));
    }
    return static_cast<int16_t>(read_sample_byte(slot.sample.start + position) << 8);
}

// ---------------------------------------------------------------------------
// Sample generation
// ---------------------------------------------------------------------------

void MultiPCM::generate(std::span<int16_t> interleaved, std::size_t frames)
{
    frames = std::min(frames, interleaved.size() / 2);
    constexpr uint32_t k_one = 1u << k_fraction_bits;

    for (std::size_t i = 0; i < frames; ++i) {
        int32_t left = 0;
        int32_t right = 0;
        for (Slot& slot : m_slots) {
            if (!slot.playing || slot.sample.end == 0) {
                continue;
            }
            const auto gain = static_cast<std::size_t>((slot.pan << 7) | static_cast<uint32_t>(slot.total_level >> k_fraction_bits));
            const uint32_t position = slot.offset >> k_fraction_bits;
            const auto fraction = static_cast<int32_t>(slot.offset & (k_one - 1));
            const int32_t current = fetch(slot, position);
            int32_t value = (current * fraction + slot.previous * (static_cast<int32_t>(k_one) - fraction))
                            >> k_fraction_bits;

            uint32_t step = slot.step;
            if (slot.vibrato != 0) {
                step = static_cast<uint32_t>((static_cast<uint64_t>(step) * static_cast<uint32_t>(lfo_step(slot.pitch_lfo)))
                                             >> k_fraction_bits);
            }
            slot.offset += step;
            if (position != (slot.offset >> k_fraction_bits)) {
                slot.previous = current;
            }
            if ((slot.offset >> k_fraction_bits) >= slot.sample.end) {
                const uint32_t loop_length = slot.sample.end - std::min(slot.sample.loop, slot.sample.end - 1);
                slot.offset -= loop_length << k_fraction_bits;
            }

            if ((slot.total_level >> k_fraction_bits) != slot.dest_total_level) {
                slot.total_level += slot.total_level_step;
            }
            if (slot.tremolo != 0) {
                value = (value * lfo_step(slot.amplitude_lfo)) >> k_fraction_bits;
            }
            // Envelope gain is 12-bit fixed point; >> 10 leaves a x4 at full
            // level, which the /4 in the attenuation table cancels (as MAME).
            value = (value * envelope_update(slot)) >> 10;

            left += (m_left_gain[gain] * value) >> k_fraction_bits;
            right += (m_right_gain[gain] * value) >> k_fraction_bits;
        }
        interleaved[2 * i] = static_cast<int16_t>(std::clamp(left, -32768, 32767));
        interleaved[2 * i + 1] = static_cast<int16_t>(std::clamp(right, -32768, 32767));
    }
}

} // namespace model1
