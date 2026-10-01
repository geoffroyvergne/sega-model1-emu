// YM3438 timer tests: Timer A / B periods, status flag enable and reset,
// stopping and restarting, and Virtua Racing's sound program waiting on
// Timer A through the sound bus.

#include "test_framework.hpp"

#include "audio/ym3438.hpp"
#include "core/sound_board.hpp"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

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
