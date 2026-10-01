// Sound subsystem tests: the i8251 UART link, the 68000 sound CPU skeleton,
// and the main-board -> sound-board command path running inside the
// Motherboard's interleaved frame loop.

#include "test_framework.hpp"

#include "core/i8251.hpp"
#include "core/m68000.hpp"
#include "core/motherboard.hpp"
#include "core/sound_board.hpp"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace {

using model1::I8251;
using model1::M68000;
using model1::Motherboard;
using model1::SoundBoard;
using model1::SoundBus;

// Mode 0x4E: asynchronous, x16 clock, 8 data bits, no parity, 1 stop bit:
// 10 bits x 16 = 160 clock ticks per character.
constexpr uint8_t k_mode_8n1_x16 = 0x4E;
constexpr uint8_t k_cmd_tx_rx = 0x05;      // transmit + receive enable
constexpr uint8_t k_cmd_tx_rx_err = 0x15;  // + reset error flags

// Two cross-connected UARTs, as on the board.
struct LinkRig {
    std::unique_ptr<I8251> a = std::make_unique<I8251>("UART A");
    std::unique_ptr<I8251> b = std::make_unique<I8251>("UART B");
    LinkRig()
    {
        a->connect_transmitter_to(*b);
        b->connect_transmitter_to(*a);
        for (I8251* u : {a.get(), b.get()}) {
            u->write(1, k_mode_8n1_x16);
            u->write(1, k_cmd_tx_rx);
        }
    }
};

// Big-endian 68000 program bytes.
std::vector<uint8_t> words(std::initializer_list<uint16_t> list)
{
    std::vector<uint8_t> bytes;
    for (uint16_t w : list) {
        bytes.push_back(static_cast<uint8_t>(w >> 8));
        bytes.push_back(static_cast<uint8_t>(w));
    }
    return bytes;
}

// Sound program used by several tests:
//   vectors: SSP = 0xF01000, PC = 0x400, level 2 autovector -> 0x500
//   0x400: MOVEQ #$4E,D0 ; MOVE.B D0,$C20003     UART mode 8N1 x16
//          MOVEQ #$15,D0 ; MOVE.B D0,$C20003     command: Tx, Rx, reset errors
//   loop:  STOP #$2000 ; BRA loop                wait for interrupts
//   0x500: MOVE.B $C20001,D1                     read the command byte
//          MOVE.B D1,$F00100                     store it in sound RAM
//          RTE
void load_sound_program(SoundBus& bus)
{
    bus.load_rom(words({0x00F0, 0x1000, 0x0000, 0x0400}), 0x000);
    bus.load_rom(words({0x0000, 0x0500}), 0x068);
    bus.load_rom(words({0x704E, 0x13C0, 0x00C2, 0x0003,
                        0x7015, 0x13C0, 0x00C2, 0x0003,
                        0x4E72, 0x2000, 0x60FA}), 0x400);
    bus.load_rom(words({0x1239, 0x00C2, 0x0001,
                        0x13C1, 0x00F0, 0x0100,
                        0x4E73}), 0x500);
}

bool log_contains(const char* text)
{
    return model1_test::captured_log().find(text) != std::string::npos;
}

} // namespace

// ---------------------------------------------------------------------------
// i8251 UART
// ---------------------------------------------------------------------------

TEST_CASE(uart_reset_state_and_mode_timing)
{
    I8251 uart("UART");
    CHECK_EQ(uart.status(), 0x05u); // TxRDY | TxEMPTY
    CHECK_EQ(uart.character_ticks(), 0u);
    uart.write(1, k_mode_8n1_x16);
    CHECK_EQ(uart.character_ticks(), 160u); // 1 start + 8 data + 1 stop, x16

    I8251 slow("UART"); // 0xFB: x64, 7 data bits, even parity, 2 stop bits
    slow.write(1, 0xFB);
    CHECK_EQ(slow.character_ticks(), (1u + 7 + 1 + 2) * 64);

    I8251 odd("UART"); // 0x8D: x1, 8 data bits, 1.5 stop bits -> 10.5, rounded up
    odd.write(1, 0x8D);
    CHECK_EQ(odd.character_ticks(), 11u);
}

TEST_CASE(uart_byte_arrives_after_one_character_time)
{
    LinkRig link;
    link.a->write(0, 0x42);
    CHECK(!link.a->tx_ready() || link.a->status() == (I8251::k_status_tx_ready)); // shifting; buffer free again
    link.a->tick(159);
    CHECK(!link.b->rx_ready());          // not there yet
    link.a->tick(1);
    CHECK(link.b->rx_ready());           // exactly 160 ticks
    CHECK_EQ(link.b->read(0), 0x42u);
    CHECK(!link.b->rx_ready());          // reading clears RxRDY
    CHECK((link.a->status() & I8251::k_status_tx_empty) != 0);
}

TEST_CASE(uart_back_to_back_bytes_and_overrun)
{
    LinkRig link;
    link.a->write(0, 0x11);              // goes straight to the shift register
    CHECK(link.a->tx_ready());
    link.a->write(0, 0x22);              // waits in the holding buffer
    CHECK(!link.a->tx_ready());
    link.a->tick(160);
    CHECK(link.b->rx_ready());
    CHECK(link.a->tx_ready());           // 0x22 moved to the shift register
    link.a->tick(160);                   // 0x22 arrives before 0x11 was read
    CHECK((link.b->status() & I8251::k_status_overrun_error) != 0);
    CHECK_EQ(link.b->read(0), 0x22u);
    link.b->write(1, k_cmd_tx_rx_err);   // error reset command
    CHECK((link.b->status() & I8251::k_status_overrun_error) == 0);
}

TEST_CASE(uart_receiver_disabled_drops_bytes)
{
    LinkRig link;
    link.b->write(1, 0x01); // transmit only: receiver disabled
    link.a->write(0, 0x99);
    link.a->tick(160);
    CHECK(!link.b->rx_ready());
    CHECK(log_contains("arrived while the receiver is disabled"));
}

TEST_CASE(uart_internal_reset_returns_to_mode)
{
    LinkRig link;
    link.a->write(1, 0x40);  // command: internal reset
    link.a->write(1, 0x4F);  // so this is a mode byte (x64)
    CHECK_EQ(link.a->character_ticks(), 640u);
}

// ---------------------------------------------------------------------------
// 68000 skeleton
// ---------------------------------------------------------------------------

TEST_CASE(m68000_reset_reads_vectors)
{
    SoundBoard board;
    load_sound_program(board.bus());
    board.reset();
    CHECK_EQ(board.cpu().a(7), 0x00F01000u);
    CHECK_EQ(board.cpu().pc(), 0x400u);
    CHECK_EQ(board.cpu().sr(), 0x2700u); // supervisor, interrupts masked
}

TEST_CASE(m68000_instructions_and_cycle_counts)
{
    SoundBoard board;
    board.bus().load_rom(words({0x00F0, 0x1000, 0x0000, 0x0400}), 0);
    board.bus().load_rom(words({0x4E71,                              // NOP
                                0x72FF,                              // MOVEQ #-1, D1
                                0x13C1, 0x00F0, 0x0010,              // MOVE.B D1, $F00010
                                0x1439, 0x00F0, 0x0010,              // MOVE.B $F00010, D2
                                0x6002,                              // BRA.B +2 (skip NOP)
                                0x4E71,
                                0x6000, 0xFFFE}),                    // BRA.W -2 (to itself)
                         0x400);
    board.reset();
    M68000& cpu = board.cpu();
    CHECK_EQ(board.step(), 4u);
    CHECK_EQ(board.step(), 4u);
    CHECK_EQ(cpu.d(1), 0xFFFFFFFFu);
    CHECK((cpu.sr() & M68000::k_sr_negative) != 0);
    CHECK_EQ(board.step(), 16u);
    CHECK_EQ(board.bus().read_byte(0xF00010), 0xFFu);
    CHECK_EQ(board.step(), 16u);
    CHECK_EQ(cpu.d(2) & 0xFF, 0xFFu);
    CHECK_EQ(cpu.d(2) & 0xFFFFFF00u, 0u); // MOVE.B keeps the upper bytes
    CHECK_EQ(board.step(), 10u);
    CHECK_EQ(cpu.pc(), 0x414u);           // BRA.B at 0x410: 0x410 + 2 + 2
    CHECK_EQ(board.step(), 10u);
    CHECK_EQ(cpu.pc(), 0x414u);           // BRA.W -2 at 0x414 loops on itself
    CHECK_EQ(cpu.cycle_count(), 4u + 4 + 16 + 16 + 10 + 10);
}

TEST_CASE(m68000_stop_interrupt_and_rte)
{
    SoundBoard board;
    load_sound_program(board.bus());
    board.reset();
    M68000& cpu = board.cpu();
    for (int i = 0; i < 5; ++i) {
        board.step(); // UART setup, then STOP
    }
    CHECK(cpu.is_stopped());
    CHECK_EQ(cpu.sr(), 0x2000u);
    CHECK_EQ(board.step(), M68000::k_idle_cycles); // nothing pending: stays stopped

    board.uart().receive_character(0x5A); // a command byte arrives
    CHECK_EQ(board.step(), 44u);          // interrupt entry
    CHECK(!cpu.is_stopped());
    CHECK_EQ(cpu.pc(), 0x500u);
    CHECK_EQ(cpu.sr(), 0x2200u);          // supervisor, mask raised to 2
    CHECK_EQ(cpu.a(7), 0x00F01000u - 6);  // PC (4 bytes) + SR (2 bytes)
    CHECK_EQ(board.bus().read_word(0xF01000 - 6), 0x2000u);  // saved SR
    CHECK_EQ(board.bus().read_long(0xF01000 - 4), 0x414u);   // return PC: the BRA after STOP (at 0x410)

    board.step(); // MOVE.B $C20001, D1 (clears RxRDY, so the line drops)
    board.step(); // MOVE.B D1, $F00100
    CHECK_EQ(board.bus().read_byte(0xF00100), 0x5Au);
    CHECK_EQ(board.step(), 20u); // RTE
    CHECK_EQ(cpu.pc(), 0x414u);
    CHECK_EQ(cpu.sr(), 0x2000u);
    CHECK_EQ(cpu.a(7), 0x00F01000u);
    CHECK_EQ(cpu.interrupt_count(), 1u);
}

TEST_CASE(m68000_interrupt_masked_until_mask_lowered)
{
    SoundBoard board;
    load_sound_program(board.bus());
    board.reset(); // SR = 0x2700: level 2 is masked
    board.uart().write(1, k_mode_8n1_x16);
    board.uart().write(1, k_cmd_tx_rx);
    board.uart().receive_character(0x01);
    board.step(); // MOVEQ: the pending interrupt does not preempt it
    CHECK_EQ(board.cpu().interrupt_count(), 0u);
    CHECK_EQ(board.cpu().pc(), 0x402u);
}

TEST_CASE(m68000_unimplemented_opcode_halts)
{
    SoundBoard board;
    board.bus().load_rom(words({0x00F0, 0x1000, 0x0000, 0x0400}), 0);
    board.bus().load_rom(words({0x4E71, 0x4AFC}), 0x400); // NOP ; ILLEGAL (exceptions not emulated)
    board.reset();
    board.step();
    board.step();
    CHECK(board.cpu().is_halted());
    CHECK_EQ(board.cpu().pc(), 0x402u);
    CHECK(log_contains("[68000] CRITICAL: unimplemented opcode 0x4AFC at PC=0x00000402"));
    CHECK_EQ(board.step(), M68000::k_idle_cycles);
}

TEST_CASE(m68000_erased_rom_halts_cleanly)
{
    // No sound ROM: the vectors read 0xFFFFFFFF, an odd PC.
    SoundBoard board;
    board.reset();
    board.step();
    CHECK(board.cpu().is_halted());
    CHECK(log_contains("address error"));
}

// ---------------------------------------------------------------------------
// Main board -> sound board, inside the Motherboard frame loop
// ---------------------------------------------------------------------------

TEST_CASE(sound_command_from_v60_reaches_sound_cpu)
{
    auto board = std::make_unique<Motherboard>();
    load_sound_program(board->sound().bus());
    board->reset();

    // V60 program (work RAM B): program the main UART and send command 0x42,
    // with byte stores as game code does.
    //   MOV.B #0x4E, /0xC40002   mode
    //   MOV.B #0x05, /0xC40002   command: Tx + Rx enable
    //   MOV.B #0x42, /0xC40000   the sound command
    //   BR $
    const uint32_t program = 0x501000;
    const std::vector<uint8_t> v60 = {
        0x09, 0x80, 0xF4, 0x4E, 0xF3, 0x02, 0x00, 0xC4, 0x00,
        0x09, 0x80, 0xF4, 0x05, 0xF3, 0x02, 0x00, 0xC4, 0x00,
        0x09, 0x80, 0xF4, 0x42, 0xF3, 0x00, 0x00, 0xC4, 0x00,
        0x6A, 0x00,
    };
    for (uint32_t i = 0; i < v60.size(); ++i) {
        board->bus().write_byte(program + i, v60[i]);
    }
    board->cpu().set_pc(program);

    board->run_frame();
    SoundBoard& sound = board->sound();
    CHECK_EQ(sound.bus().read_byte(0xF00100), 0x42u);   // stored by the 68000's handler
    CHECK_EQ(sound.cpu().d(1) & 0xFF, 0x42u);
    CHECK_EQ(sound.cpu().interrupt_count(), 1u);
    CHECK(sound.cpu().is_stopped());                     // back to waiting
    CHECK(!sound.cpu().is_halted());
    CHECK(!board->cpu().is_halted());
}

TEST_CASE(sound_reply_reaches_main_uart)
{
    auto board = std::make_unique<Motherboard>();
    board->reset();
    I8251& main_uart = board->sound_uart();
    I8251& sound_uart = board->sound().uart();
    // Main side via the bus (byte stores, as the V60 would); sound side directly.
    board->bus().write_byte(0xC40002, k_mode_8n1_x16);
    board->bus().write_byte(0xC40002, k_cmd_tx_rx);
    sound_uart.write(1, k_mode_8n1_x16);
    sound_uart.write(1, k_cmd_tx_rx);
    sound_uart.write(0, 0xA5); // reply from the sound board
    board->run_frame();
    CHECK((board->bus().read_byte(0xC40002) & I8251::k_status_rx_ready) != 0);
    CHECK_EQ(board->bus().read_byte(0xC40000), 0xA5u);
    CHECK(!main_uart.rx_ready());
}

TEST_CASE(sound_cpu_runs_in_lockstep_with_main_cpu)
{
    auto board = std::make_unique<Motherboard>();
    load_sound_program(board->sound().bus());
    board->reset();
    // Keep the V60 busy with a branch-to-self in RAM so both CPUs run.
    board->bus().write_byte(0x501000, 0x6A);
    board->bus().write_byte(0x501001, 0x00);
    board->cpu().set_pc(0x501000);

    for (int frame = 1; frame <= 10; ++frame) {
        board->run_frame();
        const uint64_t main = board->main_cpu_cycles();
        const uint64_t sound = board->sound_cpu_cycles();
        // V60: 266,666 cycles per frame (overshoot under one instruction).
        CHECK(main >= 266'666ull * static_cast<uint64_t>(frame));
        CHECK(main < 266'666ull * static_cast<uint64_t>(frame) + 8);
        // 68000: exactly 10/16 of the V60's cycles, give or take its last
        // step (at most an interrupt entry, 44 cycles).
        const uint64_t expected = main * 5 / 8;
        CHECK(sound + 44 >= expected);
        CHECK(sound <= expected + 44);
    }
    // ~166,666 68000 cycles per frame.
    CHECK(board->sound_cpu_cycles() / 10 >= 166'600u);
    CHECK(board->sound_cpu_cycles() / 10 <= 166'700u);
}

TEST_CASE(uart_software_reset_sequence_is_silent_and_ends_asynchronous)
{
    // The usual i8251 software reset, as written by Virtua Racing:
    // 00 (mode: synchronous), 00 00 (its two SYNC characters), 40 (internal
    // reset), then the real mode 4E and a command enabling the transmitter.
    I8251 uart("UART test");
    uart.reset();
    for (int value : {0x00, 0x00, 0x00, 0x40, 0x4E, 0x37}) {
        uart.write(1, static_cast<uint8_t>(value));
    }
    CHECK_EQ(uart.character_ticks(), 160u); // 8N1 at x16: asynchronous again
    CHECK(uart.tx_ready());
    CHECK(!log_contains("synchronous mode"));

    // Actually using synchronous mode is still reported.
    I8251 sync("UART sync");
    sync.reset();
    for (int value : {0x80, 0x16, 0x37}) { // mode: single SYNC; SYNC 0x16; command TxEN
        sync.write(1, static_cast<uint8_t>(value));
    }
    CHECK(log_contains("[UART sync] WARNING: synchronous mode (0x80) not emulated"));
}

TEST_CASE(sound_bus_logs_each_unmapped_address_once)
{
    // VR's sound program writes 0xC10001 (unmapped, in MAME too) constantly.
    SoundBoard board;
    for (int i = 0; i < 5; ++i) {
        board.bus().write_byte(0xC10001, static_cast<uint8_t>(i));
    }
    board.bus().write_byte(0xC10003, 0);
    const std::string log = model1_test::captured_log();
    const auto count = [&log](const std::string& text) {
        std::size_t n = 0;
        for (std::size_t at = log.find(text); at != std::string::npos; at = log.find(text, at + 1)) {
            ++n;
        }
        return n;
    };
    CHECK_EQ(count("unmapped write8 at 0x00C10001"), 1u);
    CHECK_EQ(count("unmapped write8 at 0x00C10003"), 1u);
}
