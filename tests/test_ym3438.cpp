// YM3438 tests: Timer A / B periods, status flag enable and reset,
// stopping and restarting, Virtua Racing's sound program waiting on Timer A
// through the sound bus; FM synthesis (pitch, level, pan, envelopes,
// algorithms, feedback, frequency latch, DAC, LFO) and the FM stream mixed
// into the sound board's output.

#include "test_framework.hpp"

#include "audio/ym3438.hpp"
#include "core/motherboard.hpp"
#include "core/sound_board.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>
#include <array>

namespace {

using model1::SoundBoard;
using model1::SoundBus;
using model1::Ym3438;

constexpr uint32_t k_sample = Ym3438::k_clocks_per_sample;

void write_reg(Ym3438& ym, uint8_t reg, uint8_t value)
{
    ym.write(0, reg);
    ym.write(1, value);
}

// Timer A value for an overflow every `samples` samples.
void set_timer_a(Ym3438& ym, uint32_t samples)
{
    const uint32_t value = 1024 - samples;
    write_reg(ym, 0x24, static_cast<uint8_t>(value >> 2));
    write_reg(ym, 0x25, static_cast<uint8_t>(value & 3));
}

} // namespace

TEST_CASE(ym3438_timer_a_overflows_after_its_period)
{
    Ym3438 ym("YM test");
    ym.reset();
    set_timer_a(ym, 10);
    CHECK_EQ(ym.timer_a_period(), 10u);
    write_reg(ym, 0x27, 0x15); // run A, flag A enabled, clear flag A (VR's value)
    ym.clock(k_sample * 9 + k_sample - 1);
    CHECK_EQ(ym.status(), 0u); // one clock short of 10 samples
    ym.clock(1);
    CHECK_EQ(ym.status(), Ym3438::k_status_timer_a);
    CHECK_EQ(ym.read(0), Ym3438::k_status_timer_a); // status on every read port, busy clear
    CHECK_EQ(ym.read(2), Ym3438::k_status_timer_a);
    CHECK(ym.irq());

    // The flag stays until cleared; rewriting 0x27 with the run bit still
    // set clears it without restarting the count.
    ym.clock(k_sample * 5);
    write_reg(ym, 0x27, 0x15);
    CHECK_EQ(ym.status(), 0u);
    ym.clock(k_sample * 5); // 5 + 5 = the next overflow
    CHECK_EQ(ym.status(), Ym3438::k_status_timer_a);
}

TEST_CASE(ym3438_timer_b_and_flag_enables)
{
    Ym3438 ym("YM test");
    ym.reset();
    write_reg(ym, 0x26, 254); // (256 - 254) * 16 = 32 samples
    CHECK_EQ(ym.timer_b_period(), 32u);
    write_reg(ym, 0x27, 0x2A); // run B, flag B enabled, clear flag B
    ym.clock(k_sample * 31);
    CHECK_EQ(ym.status(), 0u);
    ym.clock(k_sample);
    CHECK_EQ(ym.status(), Ym3438::k_status_timer_b);

    // Running without the flag enable: overflows are not reported.
    Ym3438 quiet("YM quiet");
    quiet.reset();
    set_timer_a(quiet, 4);
    write_reg(quiet, 0x27, 0x01);
    quiet.clock(k_sample * 20);
    CHECK_EQ(quiet.status(), 0u);
}

TEST_CASE(ym3438_stopped_timer_reloads_when_restarted)
{
    Ym3438 ym("YM test");
    ym.reset();
    set_timer_a(ym, 10);
    write_reg(ym, 0x27, 0x05); // run A
    ym.clock(k_sample * 6);
    write_reg(ym, 0x27, 0x04); // stop A: the count freezes
    ym.clock(k_sample * 100);
    CHECK_EQ(ym.status(), 0u);
    write_reg(ym, 0x27, 0x05); // restart: reloaded, a full period again
    ym.clock(k_sample * 9);
    CHECK_EQ(ym.status(), 0u);
    ym.clock(k_sample);
    CHECK_EQ(ym.status(), Ym3438::k_status_timer_a);

    // Part 2 registers are stored separately and do not touch the timers.
    ym.write(2, 0x27);
    ym.write(3, 0x00);
    CHECK_EQ(ym.register_value(1, 0x27), 0u);
    CHECK_EQ(ym.register_value(0, 0x27), 0x05u);
}

TEST_CASE(ym3438_vr_sound_program_waits_for_timer_a)
{
    // VR sound program at 0x1164: poll YM status bit 0 until Timer A fires.
    //   MOVEA.L #$D00001,A0 ; MOVE.B (A0),D0 ; LSR.B #1,D0 ; BCC -> 0x1164 ; NOP
    auto board = std::make_unique<SoundBoard>();
    std::vector<uint8_t> rom = {0x00, 0xF0, 0x10, 0x00, 0x00, 0x00, 0x04, 0x00};
    board->bus().load_rom(rom, 0);
    rom.clear();
    for (int w : {0x207C, 0x00D0, 0x0001, 0x1010, 0xE248, 0x64F4, 0x4E71}) {
        rom.push_back(static_cast<uint8_t>(w >> 8));
        rom.push_back(static_cast<uint8_t>(w));
    }
    board->bus().load_rom(rom, 0x400);
    board->reset();

    // Program Timer A for 10 samples through the bus, as the game does.
    SoundBus& bus = board->bus();
    for (const auto [reg, value] : {std::pair<uint8_t, uint8_t>{0x24, 0xFD}, {0x25, 0x02}, {0x27, 0x15}}) {
        bus.write_byte(SoundBus::k_ym_base + 1, reg);
        bus.write_byte(SoundBus::k_ym_base + 3, value);
    }

    uint64_t cycles = 0;
    int steps = 0;
    while (board->cpu().pc() != 0x40Cu && steps < 10'000) { // 0x40C: the NOP after the loop
        cycles += board->step();
        ++steps;
    }
    CHECK_EQ(board->cpu().pc(), 0x40Cu);
    // 10 samples = 10 * 144 YM clocks = 1800 CPU cycles (8 MHz vs 10 MHz).
    // One pass of the loop is 12 + 8 + 8 + 10 = 38 cycles: the read at
    // cycle 1798 just misses the overflow, the next pass (from 1824) sees it
    // and leaves at 1824 + 12 + 8 + 8 + 8 (branch not taken) = 1860.
    CHECK_EQ(cycles, 1860u);
    CHECK(!board->cpu().is_halted());
}

// ---------------------------------------------------------------------------
// FM synthesis
// ---------------------------------------------------------------------------

namespace {

// Operator register offsets within a channel: S1, S2, S3, S4.
constexpr std::array<uint8_t, 4> k_slot = {0, 8, 4, 12};

// Channel 0 as a plain sine: algorithm 7 (all four operators are
// carriers), only S4 audible (the others at total level 127), multiple 1,
// attack rate 31 (instant), release rate 15, at block 4 / fnum 1038
// (1038 x 2^3 x 55,555.6 / 2^20 = 440.0 Hz).
void setup_sine(Ym3438& ym, uint8_t algorithm = 7)
{
    for (int op = 0; op < 4; ++op) {
        const auto slot = k_slot[static_cast<std::size_t>(op)];
        write_reg(ym, static_cast<uint8_t>(0x30 + slot), 0x01);               // detune 0, multiple 1
        write_reg(ym, static_cast<uint8_t>(0x40 + slot), op == 3 ? 0 : 127);   // total level
        write_reg(ym, static_cast<uint8_t>(0x50 + slot), 0x1F);               // attack 31
        write_reg(ym, static_cast<uint8_t>(0x60 + slot), 0x00);               // no decay
        write_reg(ym, static_cast<uint8_t>(0x70 + slot), 0x00);
        write_reg(ym, static_cast<uint8_t>(0x80 + slot), 0x0F);               // sustain 0, release 15
    }
    write_reg(ym, 0xB0, algorithm);
    write_reg(ym, 0xA4, (4 << 3) | (1038 >> 8)); // block 4, fnum high bits (latched)
    write_reg(ym, 0xA0, 1038 & 0xFF);            // fnum low bits: applies the latch
}

std::vector<int16_t> render(Ym3438& ym, std::size_t samples)
{
    std::vector<int16_t> out;
    out.reserve(samples * 2);
    for (std::size_t i = 0; i < samples; ++i) {
        ym.clock(k_sample);
        int16_t l = 0;
        int16_t r = 0;
        CHECK(ym.pop_output(l, r));
        out.push_back(l);
        out.push_back(r);
    }
    return out;
}

int rising_zero_crossings(const std::vector<int16_t>& out, std::size_t channel = 0)
{
    int crossings = 0;
    for (std::size_t i = 2 + channel; i < out.size(); i += 2) {
        if (out[i - 2] < 0 && out[i] >= 0) {
            ++crossings;
        }
    }
    return crossings;
}

int peak(const std::vector<int16_t>& out, std::size_t channel = 0)
{
    int p = 0;
    for (std::size_t i = channel; i < out.size(); i += 2) {
        p = std::max(p, std::abs(static_cast<int>(out[i])));
    }
    return p;
}

constexpr std::size_t k_one_second = 55'556;
constexpr int k_full = 256 * 128 / 6; // peak of one channel at full volume

} // namespace

TEST_CASE(ym3438_fm_sine_pitch_and_level)
{
    auto ym = std::make_unique<Ym3438>("YM test");
    setup_sine(*ym);
    auto silent = render(*ym, 100);
    CHECK_EQ(peak(silent), 0); // nothing keyed on
    write_reg(*ym, 0x28, 0x80); // key on S4 of channel 0
    auto out = render(*ym, k_one_second);
    const int crossings = rising_zero_crossings(out);
    CHECK(crossings >= 438 && crossings <= 442);
    // Full volume: power table maximum 0x1FE8 (8168) >> 5 = 255 per
    // channel on the positive half, -256 on the negative half (the shift
    // rounds down), x 128 / 6 (six channels share the output).
    CHECK_EQ(*std::max_element(out.begin(), out.end()), 255 * 128 / 6);
    CHECK_EQ(*std::min_element(out.begin(), out.end()), -256 * 128 / 6);
    CHECK_EQ(peak(out, 1), k_full); // both outputs on after reset
    // Total level 0x10 = -12 dB (0.75 dB per step): a quarter.
    write_reg(*ym, 0x4C, 0x10);
    out = render(*ym, 2000);
    CHECK(peak(out) >= 1300 && peak(out) <= 1400);
}

TEST_CASE(ym3438_fm_pan_and_channel_select)
{
    auto ym = std::make_unique<Ym3438>("YM test");
    setup_sine(*ym);
    write_reg(*ym, 0xB4, 0x80); // left only
    write_reg(*ym, 0x28, 0x80);
    auto out = render(*ym, 2000);
    CHECK_EQ(peak(out, 0), k_full);
    CHECK_EQ(peak(out, 1), 0);
    // Key-on of channel 4 (0x28 value 4 = channel 4, part 2) leaves
    // channel 0 alone; channel 4's registers are untouched, so it stays
    // silent (total level 0 but attack rate 0).
    write_reg(*ym, 0x28, 0xF4);
    out = render(*ym, 2000);
    CHECK_EQ(peak(out, 0), k_full);
}

TEST_CASE(ym3438_fm_envelope_attack_and_release)
{
    auto ym = std::make_unique<Ym3438>("YM test");
    setup_sine(*ym);
    write_reg(*ym, 0x5C, 0x0A); // S4 attack rate 10: a slow exponential rise
    write_reg(*ym, 0x8C, 0x0A); // release rate 10 (effective 43: about 0.1 s)
    write_reg(*ym, 0x28, 0x80);
    render(*ym, 1);
    uint32_t previous = ym->envelope_attenuation(0, 3);
    CHECK_EQ(previous, 0x3FFu); // starts silent
    bool monotonic = true;
    for (int i = 0; i < 40; ++i) {
        render(*ym, 500);
        const uint32_t now = ym->envelope_attenuation(0, 3);
        monotonic = monotonic && now <= previous;
        previous = now;
    }
    CHECK(monotonic);
    CHECK_EQ(previous, 0u); // reached full level within 20,000 samples

    write_reg(*ym, 0x28, 0x00); // key off: release
    render(*ym, 2000);
    const uint32_t releasing = ym->envelope_attenuation(0, 3);
    CHECK(releasing > 0 && releasing < 0x3FF);
    render(*ym, k_one_second);
    CHECK_EQ(ym->envelope_attenuation(0, 3), 0x3FFu);
    auto out = render(*ym, 100);
    CHECK_EQ(peak(out), 0);
}

TEST_CASE(ym3438_fm_algorithm_modulation_and_feedback)
{
    // Algorithm 0: S1 -> S2 -> S3 -> S4. With S3 audible at full level it
    // modulates S4: the output is no longer a pure 440 Hz sine (more zero
    // crossings), but keeps the fundamental's period.
    auto pure = std::make_unique<Ym3438>("YM test");
    setup_sine(*pure, 0);
    write_reg(*pure, 0x28, 0xF0);
    const int pure_crossings = rising_zero_crossings(render(*pure, k_one_second / 4));

    auto modulated = std::make_unique<Ym3438>("YM test");
    setup_sine(*modulated, 0);
    write_reg(*modulated, 0x44, 0);    // S3 total level 0: full modulation of S4
    write_reg(*modulated, 0x28, 0xF0);
    const auto out = render(*modulated, k_one_second / 4);
    CHECK(pure_crossings >= 109 && pure_crossings <= 111);
    CHECK(rising_zero_crossings(out) > pure_crossings);
    CHECK(peak(out) <= k_full);

    // Feedback on S1 (algorithm 7, S1 the only carrier) also distorts it.
    auto feedback = std::make_unique<Ym3438>("YM test");
    setup_sine(*feedback, 7);
    write_reg(*feedback, 0x4C, 127); // S4 off
    write_reg(*feedback, 0x40, 0);   // S1 on
    write_reg(*feedback, 0x28, 0xF0);
    const int plain = rising_zero_crossings(render(*feedback, k_one_second / 4));
    write_reg(*feedback, 0xB0, (7 << 3) | 7); // feedback 7
    const int with_feedback = rising_zero_crossings(render(*feedback, k_one_second / 4));
    CHECK(plain >= 109 && plain <= 111);
    CHECK(with_feedback != plain);
}

TEST_CASE(ym3438_fm_frequency_high_byte_is_latched)
{
    // Writing 0xA4 alone changes nothing; the following 0xA0 write applies
    // both bytes.
    auto ym = std::make_unique<Ym3438>("YM test");
    setup_sine(*ym);
    render(*ym, 1);
    const uint32_t step = ym->phase_step(0, 3);
    CHECK_EQ(step, ((1038u << 1) << 4) >> 2); // ((fnum << 1) << block) >> 2 = 8304
    write_reg(*ym, 0xA4, (5 << 3) | (1038 >> 8)); // block 5
    render(*ym, 1);
    CHECK_EQ(ym->phase_step(0, 3), step);
    write_reg(*ym, 0xA0, 1038 & 0xFF);
    render(*ym, 1);
    CHECK_EQ(ym->phase_step(0, 3), step * 2); // one octave up
}

TEST_CASE(ym3438_dac_replaces_channel_6)
{
    auto ym = std::make_unique<Ym3438>("YM test");
    write_reg(*ym, 0x2B, 0x80); // DAC on
    write_reg(*ym, 0x2A, 0xFF); // 0xFF unsigned -> +127 (x 2 in 9 bits = 254)
    auto out = render(*ym, 4);
    CHECK_EQ(out[6], 254 * 128 / 6);
    CHECK_EQ(out[7], 254 * 128 / 6);
    write_reg(*ym, 0x2A, 0x00); // -128 -> -256
    out = render(*ym, 1);
    CHECK_EQ(out[0], -256 * 128 / 6);
    write_reg(*ym, 0x2B, 0x00); // DAC off: channel 6 silent again
    out = render(*ym, 1);
    CHECK_EQ(out[0], 0);
}

TEST_CASE(ym3438_lfo_tremolo)
{
    // LFO on at rate 7 (72 Hz), channel 0 AM sensitivity 3 (11.8 dB), S4
    // AM enabled: the level swings at the LFO rate.
    auto ym = std::make_unique<Ym3438>("YM test");
    setup_sine(*ym);
    write_reg(*ym, 0x22, 0x08 | 7);
    write_reg(*ym, 0xB4, 0xC0 | (3 << 4));
    write_reg(*ym, 0x6C, 0x80);
    write_reg(*ym, 0x28, 0x80);
    const auto out = render(*ym, k_one_second / 10);
    int lowest_peak = 32767;
    int highest_peak = 0;
    for (std::size_t start = 0; start + 2 * 126 <= out.size(); start += 2 * 126) { // one 440 Hz period
        const std::vector<int16_t> period(out.begin() + static_cast<std::ptrdiff_t>(start),
                                          out.begin() + static_cast<std::ptrdiff_t>(start + 2 * 126));
        lowest_peak = std::min(lowest_peak, peak(period));
        highest_peak = std::max(highest_peak, peak(period));
    }
    CHECK(highest_peak > 2 * lowest_peak);
}

TEST_CASE(ym3438_fm_stream_mixed_in_lockstep)
{
    // The sound board consumes 56 FM samples per 45 mixed samples, and the
    // 68000 (which clocks the chip) keeps ahead of the mix: no FM sample is
    // ever missing and the chip's backlog stays small.
    auto board = std::make_unique<model1::Motherboard>();
    board->reset();
    for (int frame = 0; frame < 120; ++frame) {
        board->run_frame();
        board->sound().clear_audio();
        CHECK(board->sound().ym().output_frames() < 8);
    }
    CHECK_EQ(board->sound().fm_underruns(), 0u);
}
