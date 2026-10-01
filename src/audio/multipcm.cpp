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

int32_t to_fixed(float value)
{
    return static_cast<int32_t>(value * static_cast<float>(1u << MultiPCM::k_fraction_bits));
}

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
        load_sample_header(slot.sample, slot.regs[1] | ((slot.regs[2] & 1u) << 8));
        if (slot.playing) {
            slot.offset = 0; // a new sample on a sounding voice retriggers it
            slot.previous = 0;
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
            slot.offset = 0;
            slot.previous = 0;
        } else {
            slot.playing = false; // release envelope not emulated: stop now
        }
        break;
    case 5:
        slot.attenuation = (value >> 1) & 0x7F;
        break;
    default:
        break; // LFO / envelope parameters: stored only
    }
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
            const uint32_t position = slot.offset >> k_fraction_bits;
            const auto fraction = static_cast<int32_t>(slot.offset & (k_one - 1));
            const int32_t current = fetch(slot, position);
            const int32_t value = (current * fraction + slot.previous * (static_cast<int32_t>(k_one) - fraction))
                                  >> k_fraction_bits;

            slot.offset += slot.step;
            if (position != (slot.offset >> k_fraction_bits)) {
                slot.previous = current;
            }
            if ((slot.offset >> k_fraction_bits) >= slot.sample.end) {
                const uint32_t loop_length = slot.sample.end - std::min(slot.sample.loop, slot.sample.end - 1);
                slot.offset -= loop_length << k_fraction_bits;
            }

            const std::size_t gain = (slot.pan << 7) | slot.attenuation;
            left += (m_left_gain[gain] * value) >> k_fraction_bits;
            right += (m_right_gain[gain] * value) >> k_fraction_bits;
        }
        interleaved[2 * i] = static_cast<int16_t>(std::clamp(left, -32768, 32767));
        interleaved[2 * i + 1] = static_cast<int16_t>(std::clamp(right, -32768, 32767));
    }
}

} // namespace model1
