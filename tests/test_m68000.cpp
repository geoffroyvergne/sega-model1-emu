// 68000 instruction tests: MOVE to SR (the first instruction of the Virtua
// Racing sound program), CLR, general MOVE/MOVEA with the addressing modes,
// LEA, DBcc, branches and subroutines, address errors, and the bit
// operations BTST/BCHG/BCLR/BSET, ADDQ/SUBQ, shifts and rotates, SWAP, the
// immediate ALU group, ADD/SUB/ADDA/SUBA/ADDX/SUBX, CMP/CMPA/CMPM/EOR, and the sound control latch written by VR's sound program.

#include "test_framework.hpp"

#include "core/m68000.hpp"
#include "core/sound_board.hpp"

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace {

using model1::M68000;
using model1::SoundBoard;

constexpr uint32_t k_start = 0x400;     // program start (ROM)
constexpr uint32_t k_stack = 0xF01000;  // initial supervisor stack (sound RAM)
constexpr uint32_t k_ram = 0xF00000;

constexpr uint16_t k_c = M68000::k_sr_carry;
constexpr uint16_t k_v = M68000::k_sr_overflow;
constexpr uint16_t k_z = M68000::k_sr_zero;
constexpr uint16_t k_n = M68000::k_sr_negative;
constexpr uint16_t k_x = M68000::k_sr_extend;

// A sound board whose ROM holds the reset vectors and `program` at 0x400.
struct Rig {
    std::unique_ptr<SoundBoard> board = std::make_unique<SoundBoard>();
    explicit Rig(std::initializer_list<uint16_t> program)
    {
        std::vector<uint8_t> bytes = {0x00, 0xF0, 0x10, 0x00, 0x00, 0x00, 0x04, 0x00}; // SSP, PC
        board->bus().load_rom(bytes, 0);
        bytes.clear();
        for (uint16_t w : program) {
            bytes.push_back(static_cast<uint8_t>(w >> 8));
            bytes.push_back(static_cast<uint8_t>(w));
        }
        board->bus().load_rom(bytes, k_start);
        board->reset();
    }
    M68000& cpu() { return board->cpu(); }
    uint32_t step() { return board->step(); }
    uint8_t ram8(uint32_t a) { return board->bus().read_byte(a); }
    uint16_t ram16(uint32_t a) { return board->bus().read_word(a); }
    bool z() { return (cpu().sr() & k_z) != 0; }
};

bool log_contains(const char* text)
{
    return model1_test::captured_log().find(text) != std::string::npos;
}

} // namespace

// ---------------------------------------------------------------------------
// MOVE to SR (0x46FC = MOVE #imm, SR)
// ---------------------------------------------------------------------------

TEST_CASE(m68000_move_immediate_to_sr_sets_flags_and_mask)
{
    // VR sound program at 0x200: MOVE #$2700, SR (supervisor, all interrupts masked).
    Rig rig({0x46FC, 0x2700, 0x46FC, 0x271F, 0x46FC, 0x2300});
    rig.cpu().set_sr(0x2000);
    CHECK_EQ(rig.step(), 16u);
    CHECK_EQ(rig.cpu().sr(), 0x2700u);
    CHECK_EQ(rig.cpu().pc(), k_start + 4u);
    rig.step(); // #$271F: X N Z V C all set
    CHECK_EQ(rig.cpu().sr() & (k_x | k_n | k_z | k_v | k_c), k_x | k_n | k_z | k_v | k_c);
    rig.step(); // #$2300: mask 3
    CHECK_EQ(static_cast<uint32_t>((rig.cpu().sr() & M68000::k_sr_mask) >> M68000::k_sr_mask_shift), 3u);
}

TEST_CASE(m68000_move_to_sr_leaving_supervisor_switches_stack)
{
    Rig rig({0x46FC, 0x0700, 0x46FC, 0x2700});
    rig.cpu().set_a(7, k_stack); // supervisor stack
    rig.step();                  // clear S: A7 becomes the user stack pointer
    CHECK_EQ(rig.cpu().sr(), 0x0700u);
    CHECK_EQ(rig.cpu().a(7), 0u);
    rig.step(); // now in user mode: MOVE to SR is privileged
    CHECK(rig.cpu().is_halted());
    CHECK(log_contains("privilege violation: MOVE to SR in user mode"));
}

TEST_CASE(m68000_move_to_sr_unmasks_pending_interrupt)
{
    Rig rig({0x46FC, 0x2100, 0x4E71});
    rig.board->bus().load_rom(std::vector<uint8_t>{0x00, 0x00, 0x05, 0x00}, 0x68); // level 2 vector -> 0x500
    rig.board->uart().write(1, 0x4E);
    rig.board->uart().write(1, 0x05);
    rig.board->uart().receive_character(0x42); // level 2 pending, masked by 7
    rig.step();                                 // mask 1: level 2 now allowed
    rig.step();                                 // taken before the NOP
    CHECK_EQ(rig.cpu().interrupt_count(), 1u);
    CHECK_EQ(rig.cpu().pc(), 0x500u);
}

// ---------------------------------------------------------------------------
// CLR, MOVE / MOVEA and addressing modes
// ---------------------------------------------------------------------------

TEST_CASE(m68000_clr_sizes_and_flags)
{
    Rig rig({0x4280, 0x4241, 0x4202, 0x4290});
    rig.cpu().set_d(0, 0xFFFFFFFF);
    rig.cpu().set_d(1, 0x12345678);
    rig.cpu().set_d(2, 0x12345678);
    rig.cpu().set_a(0, k_ram);
    rig.board->bus().write_long(k_ram, 0xDEADBEEF);
    rig.cpu().set_sr(0x2000 | k_n | k_c);
    CHECK_EQ(rig.step(), 6u); // CLR.L D0
    CHECK_EQ(rig.cpu().d(0), 0u);
    CHECK_EQ(rig.cpu().sr() & (k_n | k_z | k_c), k_z);
    rig.step(); // CLR.W D1
    CHECK_EQ(rig.cpu().d(1), 0x12340000u);
    rig.step(); // CLR.B D2
    CHECK_EQ(rig.cpu().d(2), 0x12345600u);
    CHECK_EQ(rig.step(), 20u); // CLR.L (A0): 12 + 8
    CHECK_EQ(rig.board->bus().read_long(k_ram), 0u);
}

TEST_CASE(m68000_move_addressing_modes)
{
    Rig rig({
        0x41F9, 0x00F0, 0x0000,       // LEA $F00000, A0
        0x30FC, 0x1234,               // MOVE.W #$1234, (A0)+
        0x30FC, 0x5678,               // MOVE.W #$5678, (A0)+
        0x3220,                       // MOVE.W -(A0), D1
        0x3428, 0xFFFE,               // MOVE.W -2(A0), D2
        0x7604,                       // MOVEQ #4, D3
        0x1630, 0x3000,               // MOVE.B 0(A0,D3.W), D3   (index)
        0x13FC, 0x0001, 0x00F0, 0x0010, // MOVE.B #1, $F00010 (VR: same form writes the PCM bank)
        0x207C, 0x00F0, 0x0020,       // MOVEA.L #$F00020, A0
        0x3270, 0x0000,               // MOVEA.W 0(A0,D0.W), A1 (sign-extended)
    });
    rig.step();
    CHECK_EQ(rig.cpu().a(0), k_ram);
    CHECK_EQ(rig.step(), 12u); // MOVE.W #imm,(An)+ = 4 + 4 + 4
    rig.step();
    CHECK_EQ(rig.cpu().a(0), k_ram + 4);
    CHECK_EQ(rig.ram16(k_ram), 0x1234u);
    rig.step();
    CHECK_EQ(rig.cpu().d(1) & 0xFFFF, 0x5678u);
    CHECK_EQ(rig.cpu().a(0), k_ram + 2);
    rig.step();
    CHECK_EQ(rig.cpu().d(2) & 0xFFFF, 0x1234u);
    rig.step();
    rig.board->bus().write_byte(k_ram + 6, 0x99);
    rig.step();
    CHECK_EQ(rig.cpu().d(3) & 0xFF, 0x99u);
    CHECK_EQ(rig.step(), 20u); // MOVE.B #imm, abs.L = 4 + 4 + 12
    CHECK_EQ(rig.ram8(k_ram + 0x10), 1u);
    rig.step();
    rig.board->bus().write_word(k_ram + 0x20, 0x8000);
    rig.cpu().set_d(0, 0);
    rig.step();
    CHECK_EQ(rig.cpu().a(1), 0xFFFF8000u);
}

TEST_CASE(m68000_word_access_at_odd_address_halts)
{
    Rig rig({0x3010}); // MOVE.W (A0), D0
    rig.cpu().set_a(0, k_ram + 1);
    rig.step();
    CHECK(rig.cpu().is_halted());
    CHECK(log_contains("address error"));
}

// ---------------------------------------------------------------------------
// LEA, DBcc, branches, subroutines
// ---------------------------------------------------------------------------

TEST_CASE(m68000_dbra_loop_clears_memory)
{
    Rig rig({
        0x41F9, 0x00F0, 0x0000, // LEA $F00000, A0
        0x7203,                 // MOVEQ #3, D1
        0x10FC, 0x0000,         // loop: MOVE.B #0, (A0)+
        0x51C9, 0xFFFA,         //       DBRA D1, loop
        0x4E71,
    });
    for (uint32_t i = 0; i < 6; ++i) {
        rig.board->bus().write_byte(k_ram + i, 0xEE);
    }
    for (int i = 0; i < 2 + 4 * 2; ++i) {
        rig.step();
    }
    CHECK_EQ(rig.ram8(k_ram + 3), 0u);
    CHECK_EQ(rig.ram8(k_ram + 4), 0xEEu); // exactly 4 iterations
    CHECK_EQ(rig.cpu().d(1) & 0xFFFF, 0xFFFFu);
    CHECK_EQ(rig.cpu().pc(), k_start + 16u);
}

TEST_CASE(m68000_bsr_rts_jsr_and_conditional_branches)
{
    Rig rig({
        0x6104,                 // 0x400: BSR.B +4 -> 0x406
        0x4E71,                 // 0x402: NOP (return lands here)
        0x6008,                 // 0x404: BRA +8 -> 0x40E
        0x4E75,                 // 0x406: RTS
        0x0000, 0x0000, 0x0000, // 0x408-0x40D: padding
        0x4EB9, 0x0000, 0x0406, // 0x40E: JSR $406
        0x6702,                 // 0x414: BEQ +2 (Z clear: not taken)
        0x6602,                 // 0x416: BNE +2 -> 0x41A
    });
    CHECK_EQ(rig.step(), 18u); // BSR
    CHECK_EQ(rig.cpu().pc(), 0x406u);
    CHECK_EQ(rig.board->bus().read_long(k_stack - 4), 0x402u);
    CHECK_EQ(rig.step(), 16u); // RTS
    CHECK_EQ(rig.cpu().pc(), 0x402u);
    rig.step(); // NOP
    rig.step(); // BRA
    CHECK_EQ(rig.cpu().pc(), 0x40Eu);
    CHECK_EQ(rig.step(), 20u); // JSR abs.L
    CHECK_EQ(rig.cpu().pc(), 0x406u);
    rig.step(); // RTS -> 0x414
    rig.cpu().set_sr(0x2700);
    CHECK_EQ(rig.step(), 8u); // BEQ not taken
    CHECK_EQ(rig.cpu().pc(), 0x416u);
    CHECK_EQ(rig.step(), 10u); // BNE taken
    CHECK_EQ(rig.cpu().pc(), 0x41Au);
}

// ---------------------------------------------------------------------------
// BTST / BCHG / BCLR / BSET: Z = the tested bit was 0
// ---------------------------------------------------------------------------

TEST_CASE(m68000_btst_immediate_on_data_register_sets_z_from_bit)
{
    // VR sound program at 0x17F2 uses 0x0807: BTST #n, D7.
    Rig rig({0x0807, 0x0003,   // BTST #3, D7
             0x0807, 0x0003,   // BTST #3, D7
             0x0807, 0x0023}); // BTST #35, D7: modulo 32 on a register = bit 3
    rig.cpu().set_sr(0x2000 | k_n | k_v | k_c | k_x);
    rig.cpu().set_d(7, 0x00000008); // bit 3 set
    CHECK_EQ(rig.step(), 10u);
    CHECK(!rig.z());
    CHECK_EQ(rig.cpu().pc(), k_start + 4u);
    // Only Z changes; the other flags and the register are untouched.
    CHECK_EQ(rig.cpu().sr() & (k_n | k_v | k_c | k_x), k_n | k_v | k_c | k_x);
    CHECK_EQ(rig.cpu().d(7), 0x00000008u);

    rig.cpu().set_d(7, 0xFFFFFFF7); // bit 3 clear, every other bit set
    rig.step();
    CHECK(rig.z());

    rig.cpu().set_d(7, 0x00000008);
    rig.step();
    CHECK(!rig.z());
}

TEST_CASE(m68000_btst_dynamic_and_on_memory)
{
    Rig rig({0x0300,           // BTST D1, D0
             0x0310,           // BTST D1, (A0): memory is a byte, bit modulo 8
             0x0810, 0x0009,   // BTST #9, (A0)  -> bit 1
             0x033C, 0x00F0}); // BTST D1, #$F0  (dynamic BTST allows #imm)
    rig.cpu().set_d(0, 0x80000000);
    rig.cpu().set_d(1, 31);
    rig.cpu().set_a(0, k_ram);
    rig.board->bus().write_byte(k_ram, 0x02);
    CHECK_EQ(rig.step(), 6u);
    CHECK(!rig.z()); // bit 31 of D0 set
    CHECK_EQ(rig.step(), 8u);           // 4 + (An) 4
    CHECK(rig.z()); // 31 & 7 = bit 7 of 0x02: clear
    CHECK_EQ(rig.step(), 12u);          // 8 + (An) 4
    CHECK(!rig.z()); // bit 1 of 0x02: set
    rig.cpu().set_d(1, 4);
    rig.step();
    CHECK(!rig.z()); // bit 4 of 0xF0: set
    CHECK(!rig.cpu().is_halted());
}

TEST_CASE(m68000_bset_bclr_bchg_modify_after_testing)
{
    Rig rig({0x08C2, 0x001F,   // BSET #31, D2
             0x0390,           // BCLR D1, (A0)
             0x0843, 0x0000,   // BCHG #0, D3
             0x0843, 0x0000}); // BCHG #0, D3
    rig.cpu().set_d(1, 2);
    rig.cpu().set_d(2, 0);
    rig.cpu().set_d(3, 0);
    rig.cpu().set_a(0, k_ram);
    rig.board->bus().write_byte(k_ram, 0xFF);

    CHECK_EQ(rig.step(), 12u);
    CHECK_EQ(rig.cpu().d(2), 0x80000000u);
    CHECK(rig.z()); // was clear before setting

    CHECK_EQ(rig.step(), 12u); // 8 + (An) 4
    CHECK_EQ(rig.ram8(k_ram), 0xFBu);
    CHECK(!rig.z()); // was set before clearing

    rig.step();
    CHECK_EQ(rig.cpu().d(3), 1u);
    CHECK(rig.z());
    rig.step();
    CHECK_EQ(rig.cpu().d(3), 0u);
    CHECK(!rig.z());
}

TEST_CASE(m68000_bit_operation_invalid_modes_halt)
{
    Rig bset_immediate({0x08FC, 0x0001, 0x0000}); // BSET #1, #imm: not alterable
    bset_immediate.step();
    CHECK(bset_immediate.cpu().is_halted());
    CHECK(log_contains("invalid addressing mode"));

    Rig movep({0x0108, 0x0000}); // MOVEP.W 0(A0), D0: shares the dynamic bit-op pattern
    movep.step();
    CHECK(movep.cpu().is_halted());
    CHECK(log_contains("unimplemented opcode 0x0108"));
}

// ---------------------------------------------------------------------------
// ADDQ / SUBQ #1-8, <ea>
// ---------------------------------------------------------------------------

TEST_CASE(m68000_addq_byte_wraps_and_sets_zero_carry_extend)
{
    // VR sound program at 0x196E: ADDQ.B #1, D0 (0x5200).
    Rig rig({0x5200, 0x5200});
    rig.cpu().set_sr(0x2000 | k_n | k_v);
    rig.cpu().set_d(0, 0x123456FF);
    CHECK_EQ(rig.step(), 4u);
    CHECK_EQ(rig.cpu().d(0), 0x12345600u); // only the low byte changes
    CHECK_EQ(rig.cpu().sr() & (k_x | k_n | k_z | k_v | k_c), k_x | k_z | k_c);
    CHECK_EQ(rig.cpu().pc(), k_start + 2u);

    rig.cpu().set_d(0, 0x7F); // +1 crosses into negative: overflow
    rig.step();
    CHECK_EQ(rig.cpu().d(0), 0x80u);
    CHECK_EQ(rig.cpu().sr() & (k_x | k_n | k_z | k_v | k_c), k_n | k_v);
}

TEST_CASE(m68000_subq_and_addq_sizes)
{
    Rig rig({0x5141,   // SUBQ.W #8, D1
             0x5382,   // SUBQ.L #1, D2
             0x5083}); // ADDQ.L #8, D3 (data field 0 means 8)
    rig.cpu().set_d(1, 0xAAAA0005);
    rig.cpu().set_d(2, 0x80000000);
    rig.cpu().set_d(3, 0xFFFFFFF8);
    CHECK_EQ(rig.step(), 4u);
    CHECK_EQ(rig.cpu().d(1), 0xAAAAFFFDu); // 5 - 8 borrows
    CHECK_EQ(rig.cpu().sr() & (k_x | k_n | k_z | k_v | k_c), k_x | k_n | k_c);
    CHECK_EQ(rig.step(), 8u);
    CHECK_EQ(rig.cpu().d(2), 0x7FFFFFFFu); // most negative - 1: overflow
    CHECK_EQ(rig.cpu().sr() & (k_x | k_n | k_z | k_v | k_c), k_v);
    CHECK_EQ(rig.step(), 8u);
    CHECK_EQ(rig.cpu().d(3), 0u);
    CHECK_EQ(rig.cpu().sr() & (k_x | k_n | k_z | k_v | k_c), k_x | k_z | k_c);
}

TEST_CASE(m68000_addq_address_register_and_memory)
{
    Rig rig({0x5448,   // ADDQ.W #2, A0: whole 32 bits, flags untouched
             0x5319,   // SUBQ.B #1, (A1)+
             0x5208}); // ADDQ.B #1, A0: byte size on An is invalid
    rig.cpu().set_sr(0x2000 | k_z);
    rig.cpu().set_a(0, 0x0000FFFF);
    rig.cpu().set_a(1, k_ram);
    rig.board->bus().write_byte(k_ram, 0x00);
    CHECK_EQ(rig.step(), 8u);
    CHECK_EQ(rig.cpu().a(0), 0x00010001u);
    CHECK(rig.z());
    CHECK_EQ(rig.step(), 12u); // 8 + (An)+ 4
    CHECK_EQ(rig.ram8(k_ram), 0xFFu);
    CHECK_EQ(rig.cpu().a(1), k_ram + 1);
    CHECK_EQ(rig.cpu().sr() & (k_x | k_n | k_c), k_x | k_n | k_c);
    rig.step();
    CHECK(rig.cpu().is_halted());
    CHECK(log_contains("invalid addressing mode 0x5208"));
}

// ---------------------------------------------------------------------------
// Shifts and rotates: ASL/ASR, LSL/LSR, ROXL/ROXR, ROL/ROR
// ---------------------------------------------------------------------------

namespace {
uint16_t flags(Rig& rig) { return static_cast<uint16_t>(rig.cpu().sr() & (k_x | k_n | k_z | k_v | k_c)); }
} // namespace

TEST_CASE(m68000_ror_long_immediate_like_vr_sound_program)
{
    // VR sound program at 0x185E: ROR.L #8, D0 (0xE098).
    Rig rig({0xE098, 0xE098});
    rig.cpu().set_sr(0x2000 | k_x | k_v);
    rig.cpu().set_d(0, 0x00123456);
    CHECK_EQ(rig.step(), 24u); // 8 + 2 * 8
    CHECK_EQ(rig.cpu().d(0), 0x56001234u);
    CHECK_EQ(flags(rig), k_x); // C = bit 7 of 0x56 = 0; V cleared; X untouched by ROR
    rig.cpu().set_d(0, 0x000000FF);
    rig.step();
    CHECK_EQ(rig.cpu().d(0), 0xFF000000u);
    CHECK_EQ(flags(rig), k_x | k_n | k_c);
}

TEST_CASE(m68000_logical_shifts_move_bits_into_carry_and_extend)
{
    Rig rig({0xE288,   // LSR.L #1, D0
             0xE909,   // LSL.B #4, D1
             0xE2D0}); // LSR.W (A0): memory form, one bit
    rig.cpu().set_d(0, 0x00000003);
    rig.cpu().set_d(1, 0x123456F1);
    rig.cpu().set_a(0, k_ram);
    rig.board->bus().write_word(k_ram, 0x0003);
    CHECK_EQ(rig.step(), 10u); // 8 + 2
    CHECK_EQ(rig.cpu().d(0), 1u);
    CHECK_EQ(flags(rig), k_x | k_c);
    CHECK_EQ(rig.step(), 14u); // 6 + 2 * 4
    CHECK_EQ(rig.cpu().d(1), 0x12345610u); // only the low byte shifts
    CHECK_EQ(flags(rig), k_x | k_c);       // last bit out = bit 4 of 0xF1
    CHECK_EQ(rig.step(), 12u);             // 8 + (An) 4
    CHECK_EQ(rig.ram16(k_ram), 0x0001u);
    CHECK_EQ(flags(rig), k_x | k_c);
}

TEST_CASE(m68000_arithmetic_shifts_keep_sign_and_report_overflow)
{
    Rig rig({0xE442,   // ASR.W #2, D2
             0xE303,   // ASL.B #1, D3: 0x40 -> 0x80, sign changes
             0xE503}); // ASL.B #2, D3 on 0xC0: sign changes on the 2nd step
    rig.cpu().set_d(2, 0x8004);
    rig.cpu().set_d(3, 0x40);
    rig.step();
    CHECK_EQ(rig.cpu().d(2), 0xE001u); // sign bit copied in
    CHECK_EQ(flags(rig), k_n);
    rig.step();
    CHECK_EQ(rig.cpu().d(3), 0x80u);
    CHECK_EQ(flags(rig), k_n | k_v);
    rig.cpu().set_d(3, 0xC0);
    rig.step();
    CHECK_EQ(rig.cpu().d(3), 0u);
    CHECK_EQ(flags(rig), k_x | k_z | k_v | k_c);
}

TEST_CASE(m68000_shift_count_in_register_and_rotate_through_extend)
{
    Rig rig({0xE9AD,   // LSL.L D4, D5
             0xE9AD,   // LSL.L D4, D5
             0xE356,   // ROXL.W #1, D6
             0xE8B6}); // ROXR.L D4, D6 with D4 = 0: C = X
    rig.cpu().set_sr(0x2000 | k_x | k_c);
    rig.cpu().set_d(4, 0); // count 0: C cleared, X kept, value unchanged
    rig.cpu().set_d(5, 0x80000001);
    CHECK_EQ(rig.step(), 8u);
    CHECK_EQ(rig.cpu().d(5), 0x80000001u);
    CHECK_EQ(flags(rig), k_x | k_n);

    rig.cpu().set_d(4, 64 + 32); // modulo 64 = 32: every bit leaves
    rig.cpu().set_d(5, 0x00000001);
    rig.cpu().set_sr(0x2000);
    CHECK_EQ(rig.step(), 72u); // 8 + 2 * 32
    CHECK_EQ(rig.cpu().d(5), 0u);
    CHECK_EQ(flags(rig), k_x | k_z | k_c); // the last bit out was the original bit 0

    rig.cpu().set_d(6, 0x00008000);
    rig.step(); // X (1) enters at bit 0, bit 15 goes to X and C
    CHECK_EQ(rig.cpu().d(6), 0x00000001u);
    CHECK_EQ(flags(rig), k_x | k_c);

    rig.cpu().set_d(4, 0);
    rig.step();
    CHECK_EQ(flags(rig), k_x | k_c); // count 0: C copies X
}

TEST_CASE(m68000_bit_field_and_invalid_shift_modes_halt)
{
    Rig bit_field({0xE8C0, 0x0000}); // BFTST (68020+), shares the memory-shift pattern
    bit_field.step();
    CHECK(bit_field.cpu().is_halted());
    Rig on_register({0xE0C0}); // memory-form shift on Dn
    on_register.step();
    CHECK(on_register.cpu().is_halted());
    CHECK(log_contains("invalid addressing mode"));
}

// ---------------------------------------------------------------------------
// Sound control latches: 0xC4xxxx / 0xC6xxxx outside the MultiPCM ports
// ---------------------------------------------------------------------------

TEST_CASE(m68000_vr_routine_stores_24_bit_value_in_sound_control_latch)
{
    // VR sound program at 0x1858, with A2 = 0xC40007 and A5 = the value.
    Rig rig({0x200D,           // MOVE.L A5, D0
             0x1540, 0x0004,   // MOVE.B D0, 4(A2)   -> 0xC4000B
             0xE098,           // ROR.L  #8, D0
             0x1540, 0x0002,   // MOVE.B D0, 2(A2)   -> 0xC40009
             0xE098,           // ROR.L  #8, D0
             0x1480});         // MOVE.B D0, (A2)    -> 0xC40007 (MultiPCM port 3)
    rig.cpu().set_a(2, 0xC40007);
    rig.cpu().set_a(5, 0x00123456);
    for (int i = 0; i < 6; ++i) {
        rig.step();
    }
    model1::SoundBus& bus = rig.board->bus();
    CHECK_EQ(bus.control_latch(0xC4000B), 0x56u);
    CHECK_EQ(bus.control_latch(0xC40009), 0x34u);
    CHECK_EQ(bus.read_byte(0xC4000B), 0x56u); // reads back
    CHECK_EQ(bus.control_latch(0xC40007), 0u); // a chip port, not latched
    CHECK(!rig.cpu().is_halted());

    bus.write_byte(0xC60013, 0x03); // VR's write to the second window
    CHECK_EQ(bus.read_byte(0xC60013), 0x03u);
    CHECK(log_contains("Sound control write8 at 0x00C4000B = 0x56"));
    CHECK(!log_contains("unmapped"));

    rig.board->reset();
    CHECK_EQ(bus.read_byte(0xC4000B), 0u);
}

// ---------------------------------------------------------------------------
// SWAP Dn
// ---------------------------------------------------------------------------

TEST_CASE(m68000_swap_exchanges_halves_and_sets_flags)
{
    // VR sound program at 0x187C: SWAP D0 (0x4840).
    Rig rig({0x4840, 0x4841});
    rig.cpu().set_sr(0x2000 | k_x | k_v | k_c);
    rig.cpu().set_d(0, 0x12348000);
    rig.cpu().set_d(1, 0);
    CHECK_EQ(rig.step(), 4u);
    CHECK_EQ(rig.cpu().d(0), 0x80001234u);
    CHECK_EQ(flags(rig), k_x | k_n); // V and C cleared, X kept
    rig.step();
    CHECK_EQ(rig.cpu().d(1), 0u);
    CHECK_EQ(flags(rig), k_x | k_z);
}

// ---------------------------------------------------------------------------
// ORI / ANDI / SUBI / ADDI / EORI / CMPI #imm, and logic to CCR / SR
// ---------------------------------------------------------------------------

TEST_CASE(m68000_andi_cmpi_vr_sound_dispatch)
{
    // VR sound program at 0x11DA: MOVE.B D7,D1; ANDI.W #7,D1; CMPI.B #3,D1; BCC.
    for (const auto& [d7, taken] : {std::pair{0x0Bu, true}, std::pair{0x02u, false}, std::pair{0xFBu, true}}) {
        Rig rig({0x1207, 0x0241, 0x0007, 0x0C01, 0x0003, 0x6400, 0x0030});
        rig.cpu().set_sr(0x2000 | k_x);
        rig.cpu().set_d(7, d7);
        rig.cpu().set_d(1, 0x12345678);
        rig.step();
        CHECK_EQ(rig.step(), 8u); // ANDI.W #imm, Dn
        CHECK_EQ(rig.cpu().d(1), 0x12340000u | (d7 & 7));
        CHECK_EQ(rig.step(), 8u); // CMPI.B #imm, Dn
        CHECK_EQ(rig.cpu().d(1) & 0xFF, d7 & 7); // not written
        CHECK_EQ(rig.cpu().sr() & k_x, k_x);    // CMPI leaves X alone
        rig.step();
        CHECK_EQ(rig.cpu().pc(), taken ? k_start + 12u + 0x30u : k_start + 14u);
    }
}

TEST_CASE(m68000_immediate_logic_and_arithmetic)
{
    Rig rig({0x0000, 0x0080,                 // ORI.B  #$80, D0
             0x0A82, 0xFFFF, 0xFFFF,         // EORI.L #-1, D2
             0x0650, 0x0001,                 // ADDI.W #1, (A0)
             0x0483, 0x0000, 0x0001,         // SUBI.L #1, D3
             0x0C44, 0x0001});               // CMPI.W #1, D4
    rig.cpu().set_sr(0x2000 | k_v | k_c);
    rig.cpu().set_d(0, 0x00000001);
    rig.cpu().set_d(2, 0x0F0F0F0F);
    rig.cpu().set_d(3, 0);
    rig.cpu().set_d(4, 0);
    rig.cpu().set_a(0, k_ram);
    rig.board->bus().write_word(k_ram, 0xFFFF);

    CHECK_EQ(rig.step(), 8u);
    CHECK_EQ(rig.cpu().d(0), 0x81u);
    CHECK_EQ(flags(rig), k_n); // V and C cleared
    CHECK_EQ(rig.step(), 16u);
    CHECK_EQ(rig.cpu().d(2), 0xF0F0F0F0u);
    CHECK_EQ(rig.step(), 16u); // 12 + (An) 4
    CHECK_EQ(rig.ram16(k_ram), 0u);
    CHECK_EQ(flags(rig), k_x | k_z | k_c);
    CHECK_EQ(rig.step(), 16u);
    CHECK_EQ(rig.cpu().d(3), 0xFFFFFFFFu);
    CHECK_EQ(flags(rig), k_x | k_n | k_c);
    rig.cpu().set_sr(0x2000);
    rig.step(); // 0 - 1: borrow, negative; X untouched by CMPI
    CHECK_EQ(flags(rig), k_n | k_c);
    CHECK_EQ(rig.cpu().d(4), 0u);
}

TEST_CASE(m68000_logic_immediate_to_ccr_and_sr)
{
    Rig rig({0x023C, 0x00FE,   // ANDI #$FE, CCR: clear C
             0x0A3C, 0x0004,   // EORI #$04, CCR: toggle Z
             0x007C, 0x0700,   // ORI  #$0700, SR: mask all interrupts
             0x027C, 0xDFFF,   // ANDI #$DFFF, SR: leave supervisor mode
             0x007C, 0x0700}); // ORI to SR in user mode: privileged
    rig.cpu().set_sr(0x2000 | k_x | k_c);
    CHECK_EQ(rig.step(), 20u);
    CHECK_EQ(flags(rig), k_x);
    rig.step();
    CHECK_EQ(flags(rig), k_x | k_z);
    rig.step();
    CHECK_EQ(rig.cpu().sr() & M68000::k_sr_mask, M68000::k_sr_mask);
    rig.step();
    CHECK_EQ(rig.cpu().sr() & M68000::k_sr_supervisor, 0);
    rig.step();
    CHECK(rig.cpu().is_halted());
    CHECK(log_contains("privilege violation: logic immediate to SR"));
}

TEST_CASE(m68000_immediate_group_rejects_invalid_forms)
{
    Rig to_address_reg({0x0648, 0x0001}); // ADDI.W #1, A0: not data-alterable
    to_address_reg.step();
    CHECK(to_address_reg.cpu().is_halted());
    CHECK(log_contains("invalid addressing mode"));
    Rig size_3({0x00C0, 0x0000}); // CMP2/CHK2 (68020+)
    size_3.step();
    CHECK(size_3.cpu().is_halted());
    CHECK(log_contains("unimplemented opcode 0x00C0"));
}

// ---------------------------------------------------------------------------
// ADD / SUB, ADDA / SUBA, ADDX / SUBX
// ---------------------------------------------------------------------------

TEST_CASE(m68000_add_to_register_like_vr_sound_program)
{
    // VR sound program at 0x1228: ADD.B D1, D5 (0xDA01).
    Rig rig({0xDA01,           // ADD.B D1, D5
             0xD090,           // ADD.L (A0), D0
             0xD081});         // ADD.L D1, D0
    rig.cpu().set_d(1, 1);
    rig.cpu().set_d(5, 0x120000FF);
    rig.cpu().set_d(0, 0x7FFFFFFF);
    rig.cpu().set_a(0, k_ram);
    rig.board->bus().write_long(k_ram, 1);
    CHECK_EQ(rig.step(), 4u);
    CHECK_EQ(rig.cpu().d(5), 0x12000000u); // only the low byte changes
    CHECK_EQ(flags(rig), k_x | k_z | k_c);
    CHECK_EQ(rig.step(), 14u); // 6 + (An) 8 for a long
    CHECK_EQ(rig.cpu().d(0), 0x80000000u);
    CHECK_EQ(flags(rig), k_n | k_v);
    CHECK_EQ(rig.step(), 8u); // long with a register source
    CHECK_EQ(rig.cpu().d(0), 0x80000001u);
}

TEST_CASE(m68000_sub_to_memory_and_address_arithmetic)
{
    Rig rig({0x9150,           // SUB.W D0, (A0)
             0xD2FC, 0xFFFE,   // ADDA.W #-2, A1 (sign-extended)
             0x95C2});         // SUBA.L D2, A2
    rig.cpu().set_d(0, 1);
    rig.cpu().set_a(0, k_ram);
    rig.board->bus().write_word(k_ram, 0);
    rig.cpu().set_a(1, 0x100);
    rig.cpu().set_a(2, 0x1000);
    rig.cpu().set_d(2, 0x10);
    CHECK_EQ(rig.step(), 12u); // 8 + (An) 4
    CHECK_EQ(rig.ram16(k_ram), 0xFFFFu);
    CHECK_EQ(flags(rig), k_x | k_n | k_c);
    CHECK_EQ(rig.step(), 12u); // 8 + #imm 4
    CHECK_EQ(rig.cpu().a(1), 0xFEu);
    CHECK_EQ(flags(rig), k_x | k_n | k_c); // ADDA leaves the flags alone
    CHECK_EQ(rig.step(), 8u);
    CHECK_EQ(rig.cpu().a(2), 0xFF0u);
}

TEST_CASE(m68000_addx_chains_64_bit_add_and_keeps_zero)
{
    Rig rig({0xD283,   // ADD.L  D3, D1   (low words)
             0xD182,   // ADDX.L D2, D0   (high words + X)
             0xD102}); // ADDX.B D2, D0 with a zero result: Z stays set
    rig.cpu().set_d(1, 0xFFFFFFFF);
    rig.cpu().set_d(3, 1);
    rig.cpu().set_d(0, 1);
    rig.cpu().set_d(2, 2);
    rig.step();
    CHECK_EQ(rig.cpu().d(1), 0u);
    CHECK_EQ(flags(rig), k_x | k_z | k_c);
    CHECK_EQ(rig.step(), 8u);
    CHECK_EQ(rig.cpu().d(0), 4u); // 1 + 2 + X
    CHECK_EQ(flags(rig), 0);       // non-zero result clears Z

    rig.cpu().set_sr(0x2000 | k_z);
    rig.cpu().set_d(0, 0);
    rig.cpu().set_d(2, 0);
    CHECK_EQ(rig.step(), 4u);
    CHECK_EQ(flags(rig), k_z); // zero result leaves Z as it was
}

TEST_CASE(m68000_subx_memory_predecrement)
{
    Rig rig({0x9109}); // SUBX.B -(A1), -(A0)
    rig.cpu().set_sr(0x2000 | k_x);
    rig.cpu().set_a(0, k_ram + 2);
    rig.cpu().set_a(1, k_ram + 4);
    rig.board->bus().write_byte(k_ram + 1, 0x10);
    rig.board->bus().write_byte(k_ram + 3, 0x01);
    CHECK_EQ(rig.step(), 18u);
    CHECK_EQ(rig.ram8(k_ram + 1), 0x0Eu); // 0x10 - 0x01 - X
    CHECK_EQ(rig.cpu().a(0), k_ram + 1);
    CHECK_EQ(rig.cpu().a(1), k_ram + 3);
    CHECK_EQ(flags(rig), 0);

    Rig byte_from_an({0xD008}); // ADD.B A0, D0: byte access to An is invalid
    byte_from_an.step();
    CHECK(byte_from_an.cpu().is_halted());
}

// ---------------------------------------------------------------------------
// CMP / CMPA / CMPM / EOR (1011 group)
// ---------------------------------------------------------------------------

TEST_CASE(m68000_cmpa_wraps_command_queue_like_vr_sound_handler)
{
    // VR sound program at 0x1B4: MOVE.B D3,(A6)+ ; CMPA.L #$F01300,A6 ;
    // BNE +8 ; MOVEA.L #$F01200,A6 - the UART command queue's wrap.
    for (const auto& [a6, wraps] : {std::pair{0xF012FFu, true}, std::pair{0xF01250u, false}}) {
        Rig rig({0x1CC3,                 // MOVE.B D3, (A6)+
                 0xBDFC, 0x00F0, 0x1300, // CMPA.L #$F01300, A6
                 0x6600, 0x0008,         // BNE +8
                 0x2C7C, 0x00F0, 0x1200, // MOVEA.L #$F01200, A6
                 0x4E71});               // NOP
        rig.cpu().set_sr(0x2000 | k_x);
        rig.cpu().set_a(6, a6);
        rig.cpu().set_d(3, 0x81);
        rig.step();
        CHECK_EQ(rig.step(), 14u); // 6 + #imm.L 8
        CHECK_EQ(rig.cpu().sr() & k_x, k_x); // X untouched
        if (wraps) {
            CHECK(rig.z());
            CHECK_EQ(rig.cpu().sr() & (k_n | k_c), 0);
        } else {
            CHECK(!rig.z());
            CHECK_EQ(rig.cpu().sr() & (k_n | k_c), k_n | k_c); // A6 below the end: negative, borrow
        }
        rig.step();
        rig.step();
        CHECK_EQ(rig.cpu().a(6), wraps ? 0xF01200u : a6 + 1);
        CHECK_EQ(rig.ram8(a6), 0x81u);
    }
}

TEST_CASE(m68000_cmp_sizes_and_flags)
{
    Rig rig({0xB001,                 // CMP.B D1, D0
             0xB450,                 // CMP.W (A0), D2
             0xB6BC, 0x8000, 0x0000, // CMP.L #$80000000, D3
             0xB2FC, 0x8000});       // CMPA.W #$8000, A1 (sign-extended)
    rig.cpu().set_d(0, 0x12345642);
    rig.cpu().set_d(1, 0xABCDEF42);
    rig.cpu().set_d(2, 0x0005);
    rig.cpu().set_d(3, 0x00000001);
    rig.cpu().set_a(0, k_ram);
    rig.cpu().set_a(1, 0xFFFF8000);
    rig.board->bus().write_word(k_ram, 0x0007);
    CHECK_EQ(rig.step(), 4u);
    CHECK_EQ(flags(rig), k_z); // low bytes equal
    CHECK_EQ(rig.cpu().d(0), 0x12345642u);
    CHECK_EQ(rig.step(), 8u); // 4 + (An) 4
    CHECK_EQ(flags(rig), k_n | k_c); // 5 - 7
    CHECK_EQ(rig.step(), 14u); // 6 + #imm.L 8
    CHECK_EQ(flags(rig), k_n | k_v | k_c); // 1 - most negative overflows
    rig.step();
    CHECK(rig.z());
}

TEST_CASE(m68000_cmpm_and_eor)
{
    Rig rig({0xB308,   // CMPM.B (A0)+, (A1)+
             0xB141,   // EOR.W D0, D1
             0xB190}); // EOR.L D0, (A0)
    rig.cpu().set_a(0, k_ram);
    rig.cpu().set_a(1, k_ram + 0x10);
    rig.board->bus().write_byte(k_ram, 0x20);
    rig.board->bus().write_byte(k_ram + 0x10, 0x20);
    rig.cpu().set_d(0, 0x0000FFFF);
    rig.cpu().set_d(1, 0x12340F0F);
    rig.cpu().set_sr(0x2000 | k_v | k_c);
    CHECK_EQ(rig.step(), 12u);
    CHECK(rig.z());
    CHECK_EQ(rig.cpu().a(0), k_ram + 1);
    CHECK_EQ(rig.cpu().a(1), k_ram + 0x11);
    CHECK_EQ(rig.step(), 4u);
    CHECK_EQ(rig.cpu().d(1), 0x1234F0F0u);
    CHECK_EQ(flags(rig), k_n); // V and C cleared
    rig.cpu().set_a(0, k_ram + 0x20);
    rig.board->bus().write_long(k_ram + 0x20, 0x0000FFFF);
    CHECK_EQ(rig.step(), 20u); // 12 + (An) long 8
    CHECK_EQ(rig.board->bus().read_long(k_ram + 0x20), 0u);
    CHECK(rig.z());
}

// ---------------------------------------------------------------------------
// OR / AND group: OR, AND, MULU / MULS, DIVU / DIVS, ABCD / SBCD, EXG
// ---------------------------------------------------------------------------

TEST_CASE(m68000_or_and_flags_like_vr)
{
    // VR sound program at 0xD50: OR.B D0, D0 (0x8000) - a zero test.
    Rig rig({0x8000, 0x8000,
             0xC27C, 0x0F0F,   // AND.W #$0F0F, D1
             0x8590});         // OR.L D2, (A0)
    rig.cpu().set_sr(0x2000 | k_x | k_v | k_c);
    rig.cpu().set_d(0, 0x12345600);
    CHECK_EQ(rig.step(), 4u);
    CHECK_EQ(flags(rig), k_x | k_z); // V, C cleared; X kept
    rig.cpu().set_d(0, 0x80);
    rig.step();
    CHECK_EQ(flags(rig), k_x | k_n);
    rig.cpu().set_d(1, 0xAAAA3CF0);
    CHECK_EQ(rig.step(), 8u); // 4 + #imm 4
    CHECK_EQ(rig.cpu().d(1), 0xAAAA0C00u);
    rig.cpu().set_a(0, k_ram);
    rig.cpu().set_d(2, 0x00FF00FF);
    rig.board->bus().write_long(k_ram, 0x0F000F00);
    CHECK_EQ(rig.step(), 20u); // 12 + (An) long 8
    CHECK_EQ(rig.board->bus().read_long(k_ram), 0x0FFF0FFFu);
}

TEST_CASE(m68000_multiply)
{
    Rig rig({0xC0FC, 0x0003,   // MULU.W #3, D0
             0xC3FC, 0xFFFE}); // MULS.W #-2, D1
    rig.cpu().set_d(0, 0xABCDFFFF); // only the low word is used
    rig.cpu().set_d(1, 5);
    CHECK_EQ(rig.step(), 38u + 2 * 2 + 4); // two ones in the multiplier
    CHECK_EQ(rig.cpu().d(0), 0x0002FFFDu);
    rig.step();
    CHECK_EQ(rig.cpu().d(1), static_cast<uint32_t>(-10));
    CHECK_EQ(flags(rig) & (k_n | k_v | k_c), k_n);
}

TEST_CASE(m68000_divide)
{
    Rig rig({0x84FC, 0x0007,   // DIVU.W #7, D2
             0x84FC, 0x0001,   // DIVU.W #1, D2: quotient too big
             0x87FC, 0xFFFD,   // DIVS.W #-3, D3
             0x84FC, 0x0000}); // DIVU.W #0: trap
    rig.cpu().set_d(2, 100);
    rig.cpu().set_d(3, 10);
    rig.step();
    CHECK_EQ(rig.cpu().d(2), 0x0002000Eu); // remainder 2, quotient 14
    rig.cpu().set_d(2, 0x00100000);
    rig.step();
    CHECK_EQ(rig.cpu().d(2), 0x00100000u); // unchanged on overflow
    CHECK(rig.cpu().sr() & k_v);
    rig.step();
    CHECK_EQ(rig.cpu().d(3), 0x0001FFFDu); // remainder 1, quotient -3
    CHECK(rig.cpu().sr() & k_n);
    rig.step();
    CHECK(rig.cpu().is_halted());
    CHECK(log_contains("division by zero"));
}

TEST_CASE(m68000_bcd_and_exchange)
{
    Rig rig({0xC101,   // ABCD D1, D0
             0xC101,   // ABCD D1, D0 (99 + 01 -> 00, carry)
             0x8101,   // SBCD D1, D0
             0xC141,   // EXG D0, D1
             0xC149,   // EXG A0, A1
             0xC58B}); // EXG D2, A3
    rig.cpu().set_sr(0x2000 | k_z);
    rig.cpu().set_d(0, 0x19);
    rig.cpu().set_d(1, 0x28);
    CHECK_EQ(rig.step(), 6u);
    CHECK_EQ(rig.cpu().d(0), 0x47u);
    CHECK(!rig.z()); // non-zero result clears Z
    rig.cpu().set_d(0, 0x99);
    rig.cpu().set_d(1, 0x01);
    rig.cpu().set_sr(0x2000 | k_z);
    rig.step();
    CHECK_EQ(rig.cpu().d(0), 0x00u);
    CHECK_EQ(flags(rig) & (k_x | k_c | k_z), k_x | k_c | k_z); // zero keeps Z
    rig.cpu().set_d(0, 0x42);
    rig.cpu().set_d(1, 0x15);
    rig.cpu().set_sr(0x2000);
    rig.step();
    CHECK_EQ(rig.cpu().d(0), 0x27u);

    rig.cpu().set_a(0, 0xA0);
    rig.cpu().set_a(1, 0xA1);
    rig.cpu().set_d(2, 0xD2);
    rig.cpu().set_a(3, 0xA3);
    CHECK_EQ(rig.step(), 6u);
    CHECK_EQ(rig.cpu().d(0), 0x15u);
    CHECK_EQ(rig.cpu().d(1), 0x27u);
    rig.step();
    CHECK_EQ(rig.cpu().a(0), 0xA1u);
    rig.step();
    CHECK_EQ(rig.cpu().d(2), 0xA3u);
    CHECK_EQ(rig.cpu().a(3), 0xD2u);
}

TEST_CASE(m68000_tst_sets_n_and_z_like_vr)
{
    // VR sound program at 0x14A2: TST.B d8(A5,D?) (0x4A35).
    Rig rig({0x4A35, 0x1002,   // TST.B 2(A5,D1.W)
             0x4A40,           // TST.W D0
             0x4A80});         // TST.L D0
    rig.cpu().set_sr(0x2000 | k_x | k_v | k_c);
    rig.cpu().set_a(5, k_ram);
    rig.cpu().set_d(1, 3);
    rig.board->bus().write_byte(k_ram + 5, 0x80);
    CHECK_EQ(rig.step(), 4u + 10u); // 4 + d8(An,Xn) 10
    CHECK_EQ(flags(rig), k_x | k_n); // V, C cleared, X kept
    rig.cpu().set_d(0, 0x12340000);
    rig.step();
    CHECK_EQ(flags(rig), k_x | k_z);
    rig.step();
    CHECK_EQ(flags(rig), k_x);
}
