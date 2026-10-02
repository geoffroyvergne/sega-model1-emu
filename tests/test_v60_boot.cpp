// V60 instructions used by the Virtua Racing boot code: UPDPSW, MOVEA, the
// string move MOVCU/MOVCFU/MOVCSU, IN/OUT to the V60's I/O space, ADDC/SUBC, CMP, INC/DEC, ADD/SUB sizes, LDPR/STPR, JSR, MOVS/MOVZ, DBcc/TB, ROT/ROTC, REM/REMU, bit fields (0x5D), SETF, NEG,
// plus the boot-time system register latch (0xE00000) and I/O stub (0xC10002).
// Several byte sequences are copied verbatim from the VR boot ROM.

#include "test_framework.hpp"
#include "v60_test_rig.hpp"

#include "core/motherboard.hpp"

#include <bit>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

using namespace model1_test;

namespace {

bool log_contains(const char* text)
{
    return model1_test::captured_log().find(text) != std::string::npos;
}

} // namespace

// ---------------------------------------------------------------------------
// UPDPSW (0x13 .W, 0x4A .H): PSW = (PSW & ~mask) | (value & mask)
// ---------------------------------------------------------------------------

TEST_CASE(v60_updpsw_vr_boot_disables_interrupts)
{
    // VR boot ROM at 0xFE000E: UPDPSW.W #0, #0x40000 - clears PSW.IE only.
    StackRig rig;
    rig.cpu->set_psw(V60::k_psw_is | V60::k_psw_ie | k_z | k_cy);
    rig.load({0x13, 0x80, 0xE0, 0xF4, 0x00, 0x00, 0x04, 0x00});
    rig.step();
    CHECK_EQ(rig.cpu->psw(), V60::k_psw_is | k_z | k_cy);
    CHECK_EQ(rig.cpu->pc(), k_program + 8);
}

TEST_CASE(v60_updpsw_sets_masked_bits_and_cannot_touch_top_byte)
{
    // UPDPSW.W #0xFFFFFFFF, #0xFFFFFFFF: only the low 24 bits may change, so
    // IS (bit 28) and the other top-byte fields are kept as they were.
    StackRig rig;
    rig.cpu->set_psw(V60::k_psw_is);
    rig.load({0x13, 0x80, 0xF4, 0xFF, 0xFF, 0xFF, 0xFF, 0xF4, 0xFF, 0xFF, 0xFF, 0xFF});
    rig.step();
    CHECK_EQ(rig.cpu->psw(), V60::k_psw_is | 0x00FFFFFFu);

    // UPDPSW.H: limited to the low 16 bits (condition codes); IE (bit 18)
    // stays set even though the mask asks to clear it.
    StackRig half;
    half.cpu->set_psw(V60::k_psw_is | V60::k_psw_ie);
    half.load({0x4A, 0x80, 0xE5, 0xF4, 0xFF, 0xFF, 0xFF, 0xFF}); // value #5, mask all ones
    half.step();
    CHECK_EQ(half.cpu->psw(), V60::k_psw_is | V60::k_psw_ie | 0x5u); // Z and OV set, S and CY clear
}

// ---------------------------------------------------------------------------
// MOVEA (0x40 .B, 0x42 .H, 0x44 .W): dst = effective address of src
// ---------------------------------------------------------------------------

TEST_CASE(v60_movea_loads_effective_address)
{
    StackRig rig;
    rig.set_flags(k_z);
    rig.cpu->set_reg(1, k_data);
    rig.cpu->set_reg(4, 3);
    rig.load({0x40, 0x35, 0xF3, 0x00, 0x00, 0x40, 0x00, // VR boot: MOVEA.B /0x400000, R21
              0x44, 0x22, 0x01, 0x10,                   // MOVEA.W 0x10[R1], R2
              0x42, 0x63, 0xC4, 0x61});                 // MOVEA.H [R1](R4), R3 (index scaled by 2)
    rig.step();
    CHECK_EQ(rig.cpu->reg(21), 0x400000u);
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), k_data + 0x10);
    rig.step();
    CHECK_EQ(rig.cpu->reg(3), k_data + 6);
    CHECK(rig.flag(k_z)); // flags untouched
    CHECK_EQ(rig.mem(k_data), 0u); // nothing read or written
}

TEST_CASE(v60_movea_with_register_source_halts)
{
    StackRig rig;
    rig.load({0x44, 0x62, 0x61}); // MOVEA.W R1, R2: a register has no address
    rig.step();
    CHECK(rig.cpu->is_halted());
    CHECK(log_contains("where an address is required"));
}

// ---------------------------------------------------------------------------
// String moves (0x58 bytes / 0x5A halfwords, Format VII-a)
// ---------------------------------------------------------------------------

TEST_CASE(v60_movcfu_vr_boot_memory_fill)
{
    // VR boot ROM at 0xFE0027: MOVCFU.H [R21], #0, [R21], R9 - with a source
    // length of 0, it fills R9 halfwords at [R21] with R26.
    StackRig rig;
    rig.cpu->set_reg(21, k_data);
    rig.cpu->set_reg(9, 4);
    rig.cpu->set_reg(26, 0xABCD);
    for (uint32_t i = 0; i < 6; ++i) {
        rig.bus->write_word(k_data + i * 2, 0x1111);
    }
    rig.load({0x5A, 0x8A, 0x75, 0x00, 0x75, 0x89});
    rig.step();
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK_EQ(rig.bus->read_word(k_data + i * 2), 0xABCDu);
    }
    CHECK_EQ(rig.bus->read_word(k_data + 8), 0x1111u); // stops after R9 elements
    CHECK_EQ(rig.cpu->reg(27), k_data + 8);           // past the last written element
    CHECK_EQ(rig.cpu->reg(28), k_data);               // nothing read from the source
    CHECK_EQ(rig.cpu->pc(), k_program + 6);
}

TEST_CASE(v60_movcu_copies_and_movcsu_stops)
{
    StackRig rig;
    const uint32_t src = k_data;
    const uint32_t dst = k_data + 0x100;
    rig.bus->write_long(src, 0x44332211);
    rig.cpu->set_reg(21, src);
    rig.cpu->set_reg(22, dst);
    rig.load({0x58, 0x08, 0x75, 0x04, 0x76, 0x04}); // MOVCU.B [R21], #4, [R22], #4
    rig.step();
    CHECK_EQ(rig.mem(dst), 0x44332211u);
    CHECK_EQ(rig.cpu->reg(28), src + 4);
    CHECK_EQ(rig.cpu->reg(27), dst + 4);

    StackRig stop;
    stop.bus->write_long(src, 0x44332211);
    stop.cpu->set_reg(21, src);
    stop.cpu->set_reg(22, dst);
    stop.cpu->set_reg(26, 0x22); // stop character
    stop.load({0x58, 0x0C, 0x75, 0x04, 0x76, 0x04}); // MOVCSU.B
    stop.step();
    CHECK_EQ(stop.mem(dst), 0x00002211u); // copied up to and including 0x22
    CHECK_EQ(stop.cpu->reg(28), src + 1);
}

TEST_CASE(v60_unimplemented_string_suboperation_halts)
{
    StackRig rig;
    rig.load({0x58, 0x03, 0x75, 0x04, 0x76, 0x04}); // sub-opcode 0x03: unassigned (MAME too)
    rig.step();
    CHECK(rig.cpu->is_halted());
    CHECK(log_contains("sub-opcode 0x03"));
}

// ---------------------------------------------------------------------------
// IN / OUT: the V60's separate I/O address space
// ---------------------------------------------------------------------------

TEST_CASE(v60_out_in_reach_tgp_ports_in_io_space)
{
    auto board = std::make_unique<model1::Motherboard>();
    board->reset();
    model1::V60& cpu = board->cpu();
    const std::initializer_list<uint8_t> program = {
        0x23, 0x80, 0xF4, 0x10, 0x80, 0xF3, 0x00, 0x00, 0xD0, 0x00, // OUT.H #0x8010, /0xD00000
        0x22, 0x25, 0xF3, 0x00, 0x00, 0xD0, 0x00,                   // IN.H /0xD00000, R5
    };
    uint32_t address = k_program;
    for (uint8_t byte : program) {
        board->bus().write_byte(address++, byte);
    }
    cpu.set_pc(k_program);
    cpu.execute_cycle();
    CHECK_EQ(board->tgp().read_ram_address(0), 0x8010u); // landed in the TGP's I/O-space port
    cpu.execute_cycle();
    CHECK_EQ(cpu.reg(5) & 0xFFFF, 0x8010u);
    // The same address in *memory* space is a different access path: the I/O
    // space write did not go through the memory bus (no unmapped messages).
    CHECK(!log_contains("unmapped"));
}

TEST_CASE(v60_out_to_unmapped_io_space_is_logged_not_fatal)
{
    // VR boot ROM at 0xFE00C4: OUT.B R1, 0x10002[R0] with R0 = 0xE00000 -
    // nothing is wired there (MAME ignores it too).
    auto board = std::make_unique<model1::Motherboard>();
    board->reset();
    model1::V60& cpu = board->cpu();
    cpu.set_reg(0, 0xE00000);
    const std::initializer_list<uint8_t> program = {0x21, 0x01, 0x40, 0x02, 0x00, 0x01, 0x00};
    uint32_t address = k_program;
    for (uint8_t byte : program) {
        board->bus().write_byte(address++, byte);
    }
    cpu.set_pc(k_program);
    cpu.execute_cycle();
    CHECK(!cpu.is_halted());
    CHECK_EQ(cpu.pc(), k_program + 7);
    CHECK(log_contains("unmapped I/O-space write8 at 0x00E10002"));
}

// ---------------------------------------------------------------------------
// ADDC (0x90 .B, 0x92 .H, 0x94 .W) / SUBC (0x98, 0x9A, 0x9C): with carry
// ---------------------------------------------------------------------------

TEST_CASE(v60_addc_rounds_after_right_shift_like_vr_boot)
{
    // VR boot ROM at 0xFE012D: SHL.W #-2, R3 then ADDC.W #0, R3 - a divide by
    // 4 rounded to nearest: the last bit shifted out (CY) is added back.
    for (const auto& [input, rounded] : {std::pair{7u, 2u}, std::pair{5u, 1u}, std::pair{6u, 2u}}) {
        StackRig rig;
        rig.cpu->set_reg(3, input);
        rig.load({0xAD, 0x23, 0xF4, 0xFE,   // SHL.W #-2, R3
                  0x94, 0x23, 0xE0});       // ADDC.W #0, R3
        rig.step();
        rig.step();
        CHECK_EQ(rig.cpu->reg(3), rounded);
        CHECK(!rig.flag(k_cy));
        CHECK_EQ(rig.cpu->pc(), k_program + 7);
    }
}

TEST_CASE(v60_add_then_addc_chains_64_bit_addition)
{
    // R1:R0 += R3:R2 with R0 = 0xFFFFFFFF, R2 = 1: the low carry ripples up.
    StackRig rig;
    rig.cpu->set_reg(0, 0xFFFFFFFF);
    rig.cpu->set_reg(1, 0x00000001);
    rig.cpu->set_reg(2, 0x00000001);
    rig.cpu->set_reg(3, 0x00000002);
    rig.load({0x84, 0x60, 0x62,   // ADD.W  R2, R0
              0x94, 0x61, 0x63}); // ADDC.W R3, R1
    rig.step();
    CHECK_EQ(rig.cpu->reg(0), 0u);
    CHECK(rig.flag(k_cy));
    rig.step();
    CHECK_EQ(rig.cpu->reg(1), 4u); // 1 + 2 + carry
    CHECK(!rig.flag(k_cy));
    CHECK(!rig.flag(k_z));
}

TEST_CASE(v60_addc_byte_flags_and_partial_register_write)
{
    StackRig rig;
    rig.set_flags(k_cy);
    rig.cpu->set_reg(1, 0);
    rig.cpu->set_reg(2, 0x123456FF);
    rig.load({0x90, 0x62, 0x61,   // ADDC.B R1, R2: 0xFF + 0 + 1
              0x90, 0x62, 0x61}); // again, on 0x7F with CY set
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0x12345600u); // only the low byte written
    CHECK(rig.flag(k_cy));
    CHECK(rig.flag(k_z));
    CHECK(!rig.flag(k_ov)); // -1 + 0 + 1 = 0 fits

    rig.cpu->set_reg(2, 0x7F);
    rig.step(); // 0x7F + 0 + 1 = 0x80: signed overflow
    CHECK_EQ(rig.cpu->reg(2), 0x80u);
    CHECK(rig.flag(k_ov));
    CHECK(rig.flag(k_s));
    CHECK(!rig.flag(k_cy));
}

TEST_CASE(v60_subc_borrows_through_carry)
{
    StackRig rig;
    rig.set_flags(k_cy);
    rig.cpu->set_reg(1, 0);
    rig.cpu->set_reg(2, 0);
    rig.load({0x9C, 0x62, 0x61,   // SUBC.W R1, R2: 0 - 0 - 1
              0x9C, 0x62, 0x61,   // SUBC.W R1, R2: 5 - 3 - 0
              0x9A, 0x62, 0x61}); // SUBC.H R1, R2: 0x8000 - 0 - 1
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0xFFFFFFFFu);
    CHECK(rig.flag(k_cy));
    CHECK(rig.flag(k_s));

    rig.set_flags(0);
    rig.cpu->set_reg(1, 3);
    rig.cpu->set_reg(2, 5);
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 2u);
    CHECK(!rig.flag(k_cy));

    rig.set_flags(k_cy);
    rig.cpu->set_reg(1, 0);
    rig.cpu->set_reg(2, 0xABCD8000);
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0xABCD7FFFu); // upper half untouched
    CHECK(rig.flag(k_ov));                  // -32768 - 1 does not fit
    CHECK(!rig.flag(k_cy));
}

// ---------------------------------------------------------------------------
// System registers (0xE00000 latch) and the 0xC10002 I/O-space stub
// ---------------------------------------------------------------------------

TEST_CASE(system_registers_latch_writes_and_keep_bank_register)
{
    auto board = std::make_unique<model1::Motherboard>();
    board->reset();
    model1::Bus& bus = board->bus();

    // VR boot: interrupt controller setup at 0xE00000-0xE0001F. 0xE00000
    // (control, write-only) and 0xE00002 (mask) are the interrupt
    // controller, 0xE00006-0xE0000F the timers; the rest is latched.
    bus.write_byte(0xE00000, 0x10);
    bus.write_byte(0xE00002, 0xFD);
    bus.write_word(0xE00010, 0x0001);
    bus.write_byte(0xE0001F, 0x03);
    CHECK_EQ(bus.read_byte(0xE00000), 0u);
    CHECK_EQ(bus.read_byte(0xE00002), 0xFDu);
    CHECK_EQ(board->interrupts().mask(), 0xFDu);
    CHECK_EQ(bus.read_word(0xE00010), 0x0001u);
    CHECK_EQ(bus.read_long(0xE0001C), 0x03000000u);
    CHECK(log_contains("System register write16 at 0x00E00010"));
    CHECK(!log_contains("unmapped"));

    // 0xE00004 is still the data ROM bank register, not latched.
    bus.write_word(0xE00004, 0x0031);
    CHECK_EQ(bus.data_bank(), 3u);
    CHECK_EQ(bus.system_registers()[4], 0u);

    // A long write straddling latch and bank register: two 16-bit cycles.
    bus.write_long(0xE00002, 0x005100AA);
    CHECK_EQ(bus.data_bank(), 5u);
    CHECK_EQ(bus.read_word(0xE00002), 0x00AAu);

    // Outside the 4 KB block is still unmapped.
    bus.write_byte(0xE01000, 0x01);
    CHECK(log_contains("unmapped write8 at 0x00E01000"));

    // Reset clears the latch and masks every interrupt level again.
    board->reset();
    CHECK_EQ(bus.read_byte(0xE0001F), 0u);
    CHECK_EQ(bus.read_byte(0xE00002), 0xFFu);
}

TEST_CASE(v60_out_to_serial_stub_port_is_accepted)
{
    // VR boot ROM at 0xFE00EB: OUT.B #0x40, 0x10002[R0] with R0 = 0xC00000.
    auto board = std::make_unique<model1::Motherboard>();
    board->reset();
    model1::V60& cpu = board->cpu();
    cpu.set_reg(0, 0xC00000);
    const std::initializer_list<uint8_t> program = {0x21, 0x80, 0xF4, 0x40, 0x40, 0x02, 0x00, 0x01, 0x00};
    uint32_t address = k_program;
    for (uint8_t byte : program) {
        board->bus().write_byte(address++, byte);
    }
    cpu.set_pc(k_program);
    cpu.execute_cycle();
    CHECK(!cpu.is_halted());
    CHECK_EQ(cpu.pc(), k_program + 9);
    CHECK(log_contains("Stub port 0x00C10002 <- 0x40"));
    CHECK(!log_contains("CRITICAL"));
}

// ---------------------------------------------------------------------------
// CMP (0xB8 .B, 0xBA .H, 0xBC .W): flags from second - first operand
// ---------------------------------------------------------------------------

TEST_CASE(v60_cmp_halfword_vr_boot_then_branch_lower)
{
    // VR boot ROM at 0xFE0134: CMP.H #0x100, R3 then BL (branch if CY).
    struct Case { uint32_t r3; bool z; bool cy; bool s; };
    for (const Case c : {Case{0x00000100, true, false, false},
                         Case{0x000000FF, false, true, true},
                         Case{0x00000200, false, false, false},
                         Case{0xABCD0100, true, false, false}}) { // upper half ignored
        StackRig rig;
        rig.cpu->set_reg(3, c.r3);
        rig.load({0xBA, 0x23, 0xF4, 0x00, 0x01,  // CMP.H #0x100, R3
                  0x62, 0x07});                  // BL +7
        rig.step();
        CHECK_EQ(rig.cpu->reg(3), c.r3); // nothing written
        CHECK_EQ(rig.flag(k_z), c.z);
        CHECK_EQ(rig.flag(k_cy), c.cy);
        CHECK_EQ(rig.flag(k_s), c.s);
        CHECK(!rig.flag(k_ov));
        CHECK_EQ(rig.cpu->pc(), k_program + 5);
        rig.step();
        CHECK_EQ(rig.cpu->pc(), c.cy ? k_program + 5 + 7 : k_program + 7);
    }
}

TEST_CASE(v60_cmp_word_overflow_and_byte_immediates)
{
    StackRig rig;
    rig.cpu->set_reg(1, 1);
    rig.cpu->set_reg(2, 0x80000000);
    rig.load({0xBC, 0x62, 0x61,         // CMP.W R1, R2: most negative - 1
              0xB8, 0x80, 0xE5, 0xE5}); // CMP.B #5, #5: both operands only read
    rig.step();
    CHECK(rig.flag(k_ov));
    CHECK(!rig.flag(k_s));
    CHECK(!rig.flag(k_cy));
    CHECK_EQ(rig.cpu->reg(2), 0x80000000u);
    rig.step();
    CHECK(rig.flag(k_z));
    CHECK(!rig.flag(k_ov));
    CHECK(!rig.cpu->is_halted());
}

// ---------------------------------------------------------------------------
// INC (0xD8-0xDD) / DEC (0xD0-0xD5): one operand, low opcode bit = mode m
// ---------------------------------------------------------------------------

TEST_CASE(v60_inc_dec_registers_and_flags)
{
    StackRig rig;
    rig.cpu->set_reg(2, 0xFFFFFFFF);
    rig.load({0xDD, 0x62,   // INC.W R2 (VR boot ROM at 0xFE0143)
              0xD9, 0x63,   // INC.B R3
              0xD3, 0x64}); // DEC.H R4
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0u);
    CHECK(rig.flag(k_z));
    CHECK(rig.flag(k_cy));
    CHECK(!rig.flag(k_ov));
    CHECK_EQ(rig.cpu->pc(), k_program + 2);

    rig.cpu->set_reg(3, 0xAABBCC7F);
    rig.step();
    CHECK_EQ(rig.cpu->reg(3), 0xAABBCC80u); // only the low byte written
    CHECK(rig.flag(k_ov));
    CHECK(rig.flag(k_s));
    CHECK(!rig.flag(k_cy));

    rig.cpu->set_reg(4, 0x12340000);
    rig.step();
    CHECK_EQ(rig.cpu->reg(4), 0x1234FFFFu);
    CHECK(rig.flag(k_cy)); // borrow
    CHECK(rig.flag(k_s));
    CHECK(!rig.flag(k_z));
}

TEST_CASE(v60_inc_word_in_memory)
{
    StackRig rig;
    rig.cpu->set_reg(1, k_data);
    rig.bus->write_long(k_data + 0x10, 0x7FFFFFFF);
    rig.load({0xDC, 0x01, 0x10,   // INC.W 0x10[R1]
              0xD4, 0x01, 0x10}); // DEC.W 0x10[R1]
    rig.step();
    CHECK_EQ(rig.mem(k_data + 0x10), 0x80000000u);
    CHECK(rig.flag(k_ov));
    rig.step();
    CHECK_EQ(rig.mem(k_data + 0x10), 0x7FFFFFFFu);
    CHECK(rig.flag(k_ov)); // most negative - 1
    CHECK_EQ(rig.cpu->pc(), k_program + 6);
}

// ---------------------------------------------------------------------------
// ADD (0x80 .B, 0x82 .H, 0x84 .W) / SUB (0xA8, 0xAA, 0xAC): all sizes
// ---------------------------------------------------------------------------

TEST_CASE(v60_add_halfword_vr_boot)
{
    // VR boot ROM at 0xFE015B: ADD.H R2, R26 (format I, register source).
    StackRig rig;
    rig.cpu->set_reg(2, 0x00000001);
    rig.cpu->set_reg(26, 0xAAAAFFFF);
    rig.load({0x82, 0x42, 0x7A});
    rig.step();
    CHECK_EQ(rig.cpu->reg(26), 0xAAAA0000u); // only the low half written
    CHECK(rig.flag(k_z));
    CHECK(rig.flag(k_cy));
    CHECK(!rig.flag(k_ov));
    CHECK_EQ(rig.cpu->pc(), k_program + 3);
}

TEST_CASE(v60_add_sub_byte_and_halfword_flags)
{
    StackRig rig;
    rig.cpu->set_reg(1, 1);
    rig.cpu->set_reg(2, 0x1122337F);
    rig.load({0x80, 0x62, 0x61,   // ADD.B R1, R2: 0x7F + 1
              0xA8, 0x62, 0x61,   // SUB.B R1, R2: 0x00 - 1
              0xAA, 0x62, 0x61}); // SUB.H R1, R2: 0x8000 - 1
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0x11223380u);
    CHECK(rig.flag(k_ov));
    CHECK(rig.flag(k_s));
    CHECK(!rig.flag(k_cy));

    rig.cpu->set_reg(2, 0x11223300);
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0x112233FFu);
    CHECK(rig.flag(k_cy)); // borrow
    CHECK(rig.flag(k_s));
    CHECK(!rig.flag(k_ov));

    rig.cpu->set_reg(2, 0xFFFF8000);
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0xFFFF7FFFu);
    CHECK(rig.flag(k_ov)); // -32768 - 1 does not fit
    CHECK(!rig.flag(k_s));
}

// ---------------------------------------------------------------------------
// LDPR (0x12) / STPR (0x02): privileged registers
// ---------------------------------------------------------------------------

TEST_CASE(v60_ldpr_sets_sbr_like_vr_boot_and_moves_vector_table)
{
    // VR boot ROM at 0xFE01CE: MOVEA.W /0xFFF000, R1 then LDPR R1, #5 (SBR).
    StackRig rig;
    rig.load({0x44, 0x21, 0xF3, 0x00, 0xF0, 0xFF, 0x00,
              0x12, 0x01, 0xE5,   // LDPR R1, #5
              0x02, 0x22, 0xE5}); // STPR #5, R2
    rig.step();
    rig.step();
    CHECK_EQ(rig.cpu->sbr(), 0x00FFF000u);
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0x00FFF000u);
    CHECK(!rig.cpu->is_halted());

    // Interrupts now vector through the new table: entry vector + 0x40.
    StackRig irq;
    irq.cpu->set_reg(1, k_data);
    irq.bus->write_long(k_data + (3 + V60::k_irq_vector_base) * 4, k_program + 0x80);
    irq.load({0x12, 0x01, 0xE5, 0xCD}); // LDPR R1, #5; NOP
    irq.step();
    irq.cpu->set_psw(irq.cpu->psw() | V60::k_psw_ie);
    irq.cpu->request_interrupt(3);
    irq.step();
    CHECK_EQ(irq.cpu->pc(), k_program + 0x80);
}

TEST_CASE(v60_ldpr_stpr_stack_pointers_follow_active_stack)
{
    StackRig rig; // PSW.IS = 1: the interrupt stack (ISP) is in R31
    rig.cpu->set_reg(3, 0x00520000);
    rig.load({0x12, 0x03, 0xE0,   // LDPR R3, #0: ISP is active, so SP changes
              0x12, 0x03, 0xE1,   // LDPR R3, #1: L0SP, inactive
              0x02, 0x24, 0xE0,   // STPR #0, R4
              0x12, 0x03, 0xE7,   // LDPR R3, #7 (SYCW): stored only
              0x02, 0x25, 0xE7}); // STPR #7, R5
    rig.step();
    CHECK_EQ(rig.sp(), 0x00520000u);
    rig.cpu->set_reg(3, 0x00510000);
    rig.step();
    CHECK_EQ(rig.cpu->level_sp(0), 0x00510000u);
    CHECK_EQ(rig.sp(), 0x00520000u);
    rig.step();
    CHECK_EQ(rig.cpu->reg(4), 0x00520000u);
    rig.step();
    rig.step();
    CHECK_EQ(rig.cpu->reg(5), 0x00510000u);
    CHECK(log_contains("privileged register 7 = 0x00510000 (stored, no effect emulated)"));
}

TEST_CASE(v60_ldpr_reserved_register_and_privilege_halt)
{
    StackRig reserved;
    reserved.load({0x12, 0x01, 0xEC}); // LDPR R1, #12: reserved
    reserved.step();
    CHECK(reserved.cpu->is_halted());
    CHECK(log_contains("reserved privileged register 12"));

    StackRig user;
    user.cpu->set_psw(1u << V60::k_psw_el_shift); // execution level 1
    user.load({0x12, 0x01, 0xE5});
    user.step();
    CHECK(user.cpu->is_halted());
    CHECK(log_contains("privileged instruction 0x12 outside execution level 0"));
}

// ---------------------------------------------------------------------------
// JSR (0xE8 / 0xE9): push the return address, jump; RSR returns
// ---------------------------------------------------------------------------

TEST_CASE(v60_jsr_pc_relative_like_vr_boot_and_rsr_returns)
{
    // VR boot ROM at 0xFE01E7: JSR disp32[PC] (6 bytes).
    StackRig rig;
    const uint32_t sp = rig.sp();
    rig.load({0xE8, 0xF2, 0x4E, 0x00, 0x00, 0x00});
    rig.load({0xCA}, k_program + 0x4E); // RSR
    rig.step();
    CHECK_EQ(rig.cpu->pc(), k_program + 0x4E);
    CHECK_EQ(rig.sp(), sp - 4);
    CHECK_EQ(rig.mem(sp - 4), k_program + 6); // return past the displacement
    rig.step();
    CHECK_EQ(rig.cpu->pc(), k_program + 6);
    CHECK_EQ(rig.sp(), sp);
}

TEST_CASE(v60_jsr_register_relative_and_register_operand_halts)
{
    StackRig rig;
    rig.cpu->set_reg(1, k_program + 0x100);
    rig.load({0xE8, 0x01, 0x10}); // JSR 0x10[R1]
    rig.step();
    CHECK_EQ(rig.cpu->pc(), k_program + 0x110);
    CHECK_EQ(rig.mem(rig.sp()), k_program + 3);

    StackRig bad;
    const uint32_t sp = bad.sp();
    bad.load({0xE9, 0x61}); // JSR R1: a register has no address
    bad.step();
    CHECK(bad.cpu->is_halted());
    CHECK_EQ(bad.sp(), sp); // nothing pushed
}

// ---------------------------------------------------------------------------
// MOVS / MOVZ (0x0A-0x0D, 0x1C-0x1D): sign- / zero-extending moves
// ---------------------------------------------------------------------------

TEST_CASE(v60_movz_halfword_immediate_like_vr_boot)
{
    // VR boot ROM at 0xFE0264: MOVZ.HW #0x5008, R1.
    StackRig rig;
    rig.set_flags(k_z | k_cy);
    rig.cpu->set_reg(1, 0xFFFFFFFF);
    rig.load({0x1D, 0x21, 0xF4, 0x08, 0x50});
    rig.step();
    CHECK_EQ(rig.cpu->reg(1), 0x00005008u); // all 32 bits written
    CHECK(rig.flag(k_z));                   // flags untouched
    CHECK(rig.flag(k_cy));
    CHECK_EQ(rig.cpu->pc(), k_program + 5);
}

TEST_CASE(v60_movs_movz_extend_by_size)
{
    StackRig rig;
    rig.load({0x1C, 0x63, 0x62,   // MOVS.HW R2, R3
              0x0A, 0x64, 0x62,   // MOVS.BH R2, R4: writes the low half only
              0x0D, 0x65, 0x62,   // MOVZ.BW R2, R5
              0x0B, 0x66, 0x62,   // MOVZ.BH R2, R6
              0x0C, 0x67, 0x62}); // MOVS.BW R2, R7
    rig.cpu->set_reg(2, 0x12348090);
    rig.cpu->set_reg(4, 0xAAAAAAAA);
    rig.cpu->set_reg(6, 0xBBBBBBBB);
    for (int i = 0; i < 5; ++i) {
        rig.step();
    }
    CHECK_EQ(rig.cpu->reg(3), 0xFFFF8090u);
    CHECK_EQ(rig.cpu->reg(4), 0xAAAAFF90u);
    CHECK_EQ(rig.cpu->reg(5), 0x00000090u);
    CHECK_EQ(rig.cpu->reg(6), 0xBBBB0090u);
    CHECK_EQ(rig.cpu->reg(7), 0xFFFFFF90u);
    CHECK(!rig.cpu->is_halted());
}

// ---------------------------------------------------------------------------
// DBcc (0xC6 / 0xC7): decrement, branch while non-zero and condition holds
// ---------------------------------------------------------------------------

TEST_CASE(v60_dbr_fill_loop_like_vr_boot)
{
    // VR boot ROM at 0xFE1469: MOV.W R0, [R1+] ; DBR R2, -3.
    StackRig rig;
    rig.cpu->set_reg(0, 0xCAFEF00D);
    rig.cpu->set_reg(1, k_data);
    rig.cpu->set_reg(2, 4);
    rig.load({0x2D, 0x40, 0x81,
              0xC6, 0xA2, 0xFD, 0xFF});
    for (int i = 0; i < 4 * 2; ++i) {
        rig.step();
    }
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK_EQ(rig.mem(k_data + i * 4), 0xCAFEF00Du);
    }
    CHECK_EQ(rig.mem(k_data + 16), 0u); // exactly R2 iterations
    CHECK_EQ(rig.cpu->reg(2), 0u);
    CHECK_EQ(rig.cpu->pc(), k_program + 7);
}

TEST_CASE(v60_dbcc_condition_stops_the_loop)
{
    StackRig rig;
    rig.cpu->set_reg(3, 10);
    rig.load({0xC7, 0x43, 0x10, 0x00}); // DBNE R3, +0x10
    rig.set_flags(0);                   // Z clear: NE holds -> branch
    rig.step();
    CHECK_EQ(rig.cpu->reg(3), 9u);
    CHECK_EQ(rig.cpu->pc(), k_program + 0x10);

    rig.cpu->set_pc(k_program);
    rig.set_flags(k_z); // Z set: falls through though R3 is still non-zero
    rig.step();
    CHECK_EQ(rig.cpu->reg(3), 8u);
    CHECK_EQ(rig.cpu->pc(), k_program + 4);
}

TEST_CASE(v60_tb_ends_table_loop_on_zero_entry_like_vr_boot)
{
    // VR boot ROM at 0xFFE58C: MOV.W [R20+], R21 ; TB R21, exit ; ... ; BR loop.
    // Here the loop body just counts entries in R6.
    StackRig rig;
    rig.bus->write_long(k_data, 7);
    rig.bus->write_long(k_data + 4, 3);
    rig.bus->write_long(k_data + 8, 0); // terminator
    rig.cpu->set_reg(20, k_data);
    rig.load({0x2D, 0x75, 0x94,         // 0: MOV.W [R20+], R21 (VR bytes)
              0xC7, 0xB5, 0x0A, 0x00,   // 3: TB R21, +10 -> 13
              0xD9, 0x66,               // 7: INC.B R6
              0xD9, 0x66,               // 9: INC.B R6
              0x6A, 0xF5,               // 11: BR -11 -> 0
              0xCD});                   // 13: NOP
    for (int i = 0; i < 20 && rig.cpu->pc() != k_program + 13; ++i) {
        rig.step();
    }
    CHECK_EQ(rig.cpu->pc(), k_program + 13);
    CHECK_EQ(rig.cpu->reg(6), 4u);   // two entries, two increments each
    CHECK_EQ(rig.cpu->reg(21), 0u);  // TB does not decrement
    CHECK_EQ(rig.cpu->reg(20), k_data + 12);
}

// ---------------------------------------------------------------------------
// ROT (0x89 / 0x8B / 0x8D) and ROTC (0x99 / 0x9B / 0x9D)
// ---------------------------------------------------------------------------

TEST_CASE(v60_rot_halfword_byte_swap_like_vr_boot)
{
    // VR boot ROM at 0xFE0E7E: ROT.H #8, R0.
    StackRig rig;
    rig.set_flags(k_cy | k_ov);
    rig.cpu->set_reg(0, 0xAAAA1234);
    rig.load({0x8B, 0x20, 0xE8});
    rig.step();
    CHECK_EQ(rig.cpu->reg(0), 0xAAAA3412u); // upper half untouched
    CHECK(!rig.flag(k_cy)); // last bit out: bit 8 of 0x1234 = 0
    CHECK(!rig.flag(k_ov));
    CHECK(!rig.flag(k_z));
    CHECK_EQ(rig.cpu->pc(), k_program + 3);
}

TEST_CASE(v60_rot_directions_and_carry)
{
    StackRig rig;
    rig.cpu->set_reg(1, 0x01);
    rig.cpu->set_reg(2, 0x80000000);
    rig.load({0x89, 0x21, 0xF4, 0xFF,   // ROT.B #-1, R1
              0x8D, 0x22, 0xE1,         // ROT.W #1, R2
              0x8D, 0x22, 0xE0});       // ROT.W #0, R2: CY cleared
    rig.step();
    CHECK_EQ(rig.cpu->reg(1), 0x80u);
    CHECK(rig.flag(k_cy));
    CHECK(rig.flag(k_s));
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 1u);
    CHECK(rig.flag(k_cy));
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 1u);
    CHECK(!rig.flag(k_cy));
}

TEST_CASE(v60_rotc_rotates_through_carry)
{
    StackRig rig;
    rig.set_flags(0);
    rig.cpu->set_reg(3, 0x80);
    rig.cpu->set_reg(4, 0);
    rig.load({0x99, 0x23, 0xE1,         // ROTC.B #1, R3: bit 7 -> CY, CY (0) -> bit 0
              0x99, 0x23, 0xE1,         // again: CY (1) comes back in
              0x9D, 0x24, 0xF4, 0xFF,   // ROTC.W #-1, R4 with CY set
              0x9D, 0x24, 0xE0});       // ROTC.W #0: CY kept
    rig.step();
    CHECK_EQ(rig.cpu->reg(3), 0u);
    CHECK(rig.flag(k_cy));
    CHECK(rig.flag(k_z));
    rig.step();
    CHECK_EQ(rig.cpu->reg(3), 1u);
    CHECK(!rig.flag(k_cy));
    rig.set_flags(k_cy);
    rig.step();
    CHECK_EQ(rig.cpu->reg(4), 0x80000000u);
    CHECK(!rig.flag(k_cy));
    rig.set_flags(k_cy);
    rig.step();
    CHECK(rig.flag(k_cy));
}

// ---------------------------------------------------------------------------
// REM / REMU (0x50-0x55): remainder
// ---------------------------------------------------------------------------

TEST_CASE(v60_remu_rounds_up_to_multiple_like_vr_boot)
{
    // VR boot ROM at 0xFC3BA5: round R0 up to a multiple of 0x780 (1920).
    struct Case { uint32_t in; uint32_t out; };
    for (const Case c : {Case{3841, 5760}, Case{3840, 3840}, Case{1, 1920}, Case{0, 0}}) {
        StackRig rig;
        rig.cpu->set_reg(0, c.in);
        rig.load({0x2D, 0x40, 0x61,                         //  0: MOV.W R0, R1
                  0x2D, 0x22, 0xF4, 0x80, 0x07, 0x00, 0x00, //  3: MOV.W #0x780, R2
                  0xB5, 0x42, 0x60,                         // 10: DIVU.W R2, R0
                  0x55, 0x42, 0x61,                         // 13: REMU.W R2, R1
                  0x64, 0x04,                               // 16: BE +4 -> 20
                  0xDD, 0x60,                               // 18: INC.W R0
                  0x95, 0x42, 0x60});                       // 20: MULU.W R2, R0
        for (int i = 0; i < 8 && rig.cpu->pc() != k_program + 23; ++i) {
            rig.step();
        }
        CHECK_EQ(rig.cpu->pc(), k_program + 23);
        CHECK_EQ(rig.cpu->reg(0), c.out);
        CHECK_EQ(rig.cpu->reg(1), c.in % 1920);
    }
}

TEST_CASE(v60_rem_signed_and_sizes)
{
    StackRig rig;
    rig.set_flags(k_cy | k_ov);
    rig.cpu->set_reg(1, 2);
    rig.cpu->set_reg(2, static_cast<uint32_t>(-7));
    rig.load({0x54, 0x62, 0x61,   // REM.W R1, R2: -7 % 2 = -1 (dividend's sign)
              0x50, 0x62, 0x61,   // REM.B R1, R2
              0x53, 0x62, 0x61,   // REMU.H R1, R2
              0x54, 0x62, 0x61}); // REM.W R1, R2: most negative % -1 = 0
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0xFFFFFFFFu);
    CHECK(rig.flag(k_s));
    CHECK(!rig.flag(k_ov));
    CHECK(rig.flag(k_cy)); // unchanged

    rig.cpu->set_reg(2, 0x123456F9); // low byte -7
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0x123456FFu); // upper bytes untouched

    rig.cpu->set_reg(1, 3);
    rig.cpu->set_reg(2, 0xABCD8000); // 32768 % 3 = 2 (unsigned)
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0xABCD0002u);
    CHECK(!rig.flag(k_s));

    rig.cpu->set_reg(1, static_cast<uint32_t>(-1));
    rig.cpu->set_reg(2, 0x80000000);
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0u);
    CHECK(rig.flag(k_z));
}

TEST_CASE(v60_rem_by_zero_leaves_destination_and_sets_ov)
{
    StackRig rig;
    rig.cpu->set_reg(1, 0);
    rig.cpu->set_reg(2, 1234);
    rig.load({0x55, 0x62, 0x61}); // REMU.W R1, R2
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 1234u);
    CHECK(rig.flag(k_ov));
    CHECK(!rig.cpu->is_halted());
    CHECK(log_contains("remainder by zero"));
}

// ---------------------------------------------------------------------------
// Bit fields (0x5D group): EXTBFS / EXTBFZ / EXTBFL, INSBFR / INSBFL
// ---------------------------------------------------------------------------

TEST_CASE(v60_extbfl_left_justifies_field_like_vr)
{
    // VR at 0xFFE0E0: EXTBFL /0x501480, #11, R1.
    StackRig rig;
    rig.set_flags(k_z | k_cy);
    rig.bus->write_long(0x501480, 0x12345ABC); // low 11 bits: 0x2BC
    rig.load({0x5D, 0xAA, 0xF3, 0x80, 0x14, 0x50, 0x00, 0x0B, 0x61});
    rig.step();
    CHECK_EQ(rig.cpu->reg(1), 0x2BCu << 21);
    CHECK(rig.flag(k_z)); // flags untouched
    CHECK(rig.flag(k_cy));
    CHECK_EQ(rig.cpu->pc(), k_program + 9);
    CHECK(!rig.cpu->is_halted());
}

TEST_CASE(v60_extbfs_extbfz_with_bit_displacement)
{
    // disp8[R1] in bit addressing: R1 is the base, the displacement counts bits.
    StackRig rig;
    rig.cpu->set_reg(1, k_data);
    rig.bus->write_long(k_data, 0x000AB000); // bits 12-19 = 0xAB
    rig.load({0x5D, 0x28, 0x01, 0x0C, 0x08, 0x62,   // EXTBFS 12[R1], #8, R2
              0x5D, 0x29, 0x01, 0x0C, 0x08, 0x63}); // EXTBFZ 12[R1], #8, R3
    rig.step();
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0xFFFFFFABu);
    CHECK_EQ(rig.cpu->reg(3), 0x000000ABu);
}

TEST_CASE(v60_extbfz_indexed_offset_crossing_32_bits_and_negative_offset)
{
    StackRig rig;
    rig.cpu->set_reg(1, k_data);
    rig.cpu->set_reg(3, 28); // bit offset from the index register (not scaled)
    rig.cpu->set_reg(4, 12); // field length from a register
    rig.bus->write_long(k_data, 0xF0000000);
    rig.bus->write_byte(k_data + 4, 0xFF);   // bits 28-39 are all ones
    rig.load({0x5D, 0x69, 0xC3, 0x61, 0x84, 0x62,   // EXTBFZ [R1](R3), R4, R2
              0x5D, 0x29, 0x05, 0xFC, 0x04, 0x65}); // EXTBFZ -4[R5], #4, R5
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0xFFFu); // 12 bits across the long boundary

    rig.bus->write_byte(k_data + 8, 0x5A);
    rig.cpu->set_reg(5, k_data + 9); // 4 bits back: byte k_data+8, bits 4-7
    rig.step();
    CHECK_EQ(rig.cpu->reg(5), 0x5u);
}

TEST_CASE(v60_insbfr_insbfl_write_only_the_field)
{
    StackRig rig;
    rig.cpu->set_reg(1, k_data);
    rig.cpu->set_reg(5, 0x123456CD);
    rig.cpu->set_reg(6, 0xAB000000);
    rig.bus->write_long(k_data, 0x00EEFFFF);
    rig.load({0x5D, 0x58, 0x65, 0x01, 0x04, 0x08,   // INSBFR R5, 4[R1], #8: low byte 0xCD at bits 4-11
              0x5D, 0x59, 0x66, 0x01, 0x10, 0x08}); // INSBFL R6, 16[R1], #8: top byte 0xAB at bits 16-23
    rig.step();
    CHECK_EQ(rig.mem(k_data), 0x00EEFCDFu);
    rig.step();
    CHECK_EQ(rig.mem(k_data), 0x00ABFCDFu); // byte 3 untouched
    CHECK(!rig.cpu->is_halted());
}

TEST_CASE(v60_bit_field_invalid_forms_halt)
{
    StackRig zero_length;
    zero_length.cpu->set_reg(1, k_data);
    zero_length.load({0x5D, 0x29, 0x01, 0x00, 0x00, 0x62}); // length 0
    zero_length.step();
    CHECK(zero_length.cpu->is_halted());
    CHECK(log_contains("bit field length 0 outside 1-32"));

    StackRig register_field;
    register_field.load({0x5D, 0x69, 0x61, 0x08, 0x62}); // bit field "in" R1: no address
    register_field.step();
    CHECK(register_field.cpu->is_halted());
    CHECK(log_contains("where an address is required"));

    StackRig unknown;
    unknown.load({0x5D, 0x00});
    unknown.step();
    CHECK(unknown.cpu->is_halted());
    CHECK(log_contains("bit-field instruction 0x5D, sub-opcode 0x00"));
}

// ---------------------------------------------------------------------------
// SETF (0x47): store a condition as a byte
// ---------------------------------------------------------------------------

TEST_CASE(v60_setf_stores_comparison_result_like_vr)
{
    // VR at 0xFE09B9: CMP.B #1, 0x14[R10] ; SETF #E, /0x40DD20.
    for (const auto& [value, expected] : {std::pair{1u, 1u}, std::pair{2u, 0u}}) {
        StackRig rig;
        rig.cpu->set_reg(10, k_data);
        rig.bus->write_byte(k_data + 0x14, static_cast<uint8_t>(value));
        rig.bus->write_byte(0x40DD20, 0xEE);
        rig.bus->write_byte(0x40DD21, 0xEE);
        rig.load({0xB8, 0x80, 0xE1, 0x0A, 0x14,
                  0x47, 0x80, 0xE4, 0xF3, 0x20, 0xDD, 0x40, 0x00});
        rig.step();
        const uint32_t psw_after_cmp = rig.cpu->psw();
        rig.step();
        CHECK_EQ(rig.bus->read_byte(0x40DD20), expected);
        CHECK_EQ(rig.bus->read_byte(0x40DD21), 0xEEu); // a byte store only
        CHECK_EQ(rig.cpu->psw(), psw_after_cmp);       // flags not affected
        CHECK_EQ(rig.cpu->pc(), k_program + 13);
    }
}

TEST_CASE(v60_setf_all_sixteen_conditions)
{
    // Flags S = 1, CY = 1, Z = 0, OV = 0.
    const uint32_t expected[16] = {
        0, 1,  // V, NV
        1, 0,  // L, NL
        0, 1,  // E, NE
        1, 0,  // NH (CY or Z), H
        1, 0,  // N, P
        1, 0,  // always, never
        1, 0,  // LT (S != OV), GE
        1, 0,  // LE, GT
    };
    for (uint8_t code = 0; code < 16; ++code) {
        StackRig rig;
        rig.set_flags(k_s | k_cy);
        rig.cpu->set_reg(2, 0xAAAAAAAA);
        rig.load({0x47, 0x22, static_cast<uint8_t>(0xE0 | code)}); // SETF #code, R2
        rig.step();
        CHECK_EQ(rig.cpu->reg(2), 0xAAAAAA00u | expected[code]); // low byte only
    }
}

TEST_CASE(v60_setf_condition_from_register_low_nibble)
{
    StackRig rig;
    rig.set_flags(k_z);
    rig.cpu->set_reg(3, 0xF4); // only bits 3-0 count: 4 = E
    rig.load({0x47, 0x62, 0x63, // SETF R3, R2
              0x47, 0x62, 0x63});
    rig.step();
    CHECK_EQ(rig.cpu->reg(2) & 0xFF, 1u);
    rig.set_flags(0);
    rig.step();
    CHECK_EQ(rig.cpu->reg(2) & 0xFF, 0u);
}

// ---------------------------------------------------------------------------
// NEG (0x39 / 0x3B / 0x3D): dst = 0 - src
// ---------------------------------------------------------------------------

TEST_CASE(v60_neg_absolute_value_like_vr)
{
    // VR at 0xFE6791: BGE +5 ; NEG.W R0, R0 (skipped when R0 >= 0).
    for (const auto& [in, out] : {std::pair{static_cast<uint32_t>(-1234), 1234u}, std::pair{1234u, 1234u}}) {
        StackRig rig;
        rig.cpu->set_reg(0, in);
        rig.load({0xF5, 0x60,         // TEST.W R0 (sets S from R0)
                  0x6D, 0x05,         // BGE +5
                  0x3D, 0x40, 0x60,   // NEG.W R0, R0
                  0xCD});             // NOP
        for (int i = 0; i < 3 && rig.cpu->pc() != k_program + 7; ++i) {
            rig.step();
        }
        CHECK_EQ(rig.cpu->pc(), k_program + 7);
        CHECK_EQ(rig.cpu->reg(0), out);
    }
}

TEST_CASE(v60_neg_flags_and_sizes)
{
    StackRig rig;
    rig.cpu->set_reg(1, 5);
    rig.cpu->set_reg(2, 0xAAAAAAAA);
    rig.load({0x3D, 0x41, 0x62,   // NEG.W R1, R2: -5
              0x3D, 0x41, 0x62,   // NEG.W R1, R2 with R1 = 0
              0x39, 0x41, 0x62,   // NEG.B R1, R2 with R1 = 0x80: overflow
              0x3B, 0x41, 0x62}); // NEG.H R1, R2
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), static_cast<uint32_t>(-5));
    CHECK(rig.flag(k_s));
    CHECK(rig.flag(k_cy)); // non-zero result: borrow
    CHECK(!rig.flag(k_ov));

    rig.cpu->set_reg(1, 0);
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0u);
    CHECK(rig.flag(k_z));
    CHECK(!rig.flag(k_cy));

    rig.cpu->set_reg(1, 0x80);
    rig.cpu->set_reg(2, 0x12345678);
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0x12345680u); // -(-128) wraps to -128; low byte only
    CHECK(rig.flag(k_ov));

    rig.cpu->set_reg(1, 0x0001);
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0x1234FFFFu);
}

// ---------------------------------------------------------------------------
// Single-precision floating point (0x5C / 0x5F)
// ---------------------------------------------------------------------------

namespace {

uint32_t fbits(float f) { return std::bit_cast<uint32_t>(f); }
float as_float(uint32_t bits) { return std::bit_cast<float>(bits); }

} // namespace

TEST_CASE(v60_cvtsw_rounding_like_vr)
{
    // VR at 0xFED52B: CVTSW R5, R5 (5F E1 65 65). Default rounding: nearest,
    // ties to even.
    struct Case { float in; uint32_t out; };
    for (const Case c : {Case{2.5f, 2u}, Case{3.5f, 4u}, Case{-2.5f, static_cast<uint32_t>(-2)},
                         Case{2.7f, 3u}, Case{-0.4f, 0u}}) {
        StackRig rig;
        rig.cpu->set_reg(5, fbits(c.in));
        rig.load({0x5F, 0xE1, 0x65, 0x65});
        rig.step();
        CHECK_EQ(rig.cpu->reg(5), c.out);
        CHECK_EQ(rig.cpu->pc(), k_program + 4);
    }

    // TKCW (privileged register 8) = 3: round toward zero.
    StackRig trunc;
    trunc.cpu->set_reg(1, 3);
    trunc.cpu->set_reg(5, fbits(-2.7f));
    trunc.load({0x12, 0x01, 0xE8,         // LDPR R1, #8
                0x5F, 0xE1, 0x65, 0x65}); // CVTSW R5, R5
    trunc.step();
    trunc.step();
    CHECK_EQ(trunc.cpu->reg(5), static_cast<uint32_t>(-2));
    CHECK(trunc.flag(k_s));

    StackRig overflow;
    overflow.cpu->set_reg(5, fbits(3.0e9f));
    overflow.load({0x5F, 0xE1, 0x65, 0x65});
    overflow.step();
    CHECK_EQ(overflow.cpu->reg(5), 0x80000000u);
    CHECK(overflow.flag(k_ov));
}

TEST_CASE(v60_cvtws_integer_to_float)
{
    StackRig rig;
    rig.cpu->set_reg(1, static_cast<uint32_t>(-7));
    rig.load({0x5F, 0x60, 0x61, 0x62}); // CVTWS R1, R2
    rig.step();
    CHECK_EQ(as_float(rig.cpu->reg(2)), -7.0f);
    CHECK(rig.flag(k_s));
    CHECK(rig.flag(k_cy)); // as in MAME: set for negative results
}

TEST_CASE(v60_float_arithmetic)
{
    StackRig rig;
    rig.cpu->set_reg(1, fbits(1.5f));
    rig.cpu->set_reg(2, fbits(2.25f));
    rig.cpu->set_reg(3, k_data);
    rig.bus->write_long(k_data, fbits(0.25f));
    rig.load({0x5C, 0x78, 0x61, 0x62,   // ADDF.S R1, R2   -> 3.75
              0x5C, 0x79, 0x61, 0x62,   // SUBF.S R1, R2   -> 2.25
              0x5C, 0x7A, 0x61, 0x62,   // MULF.S R1, R2   -> 3.375
              0x5C, 0x38, 0x63, 0x62,   // ADDF.S [R3], R2 -> 3.625 (memory source)
              0x5C, 0x7B, 0x64, 0x62}); // DIVF.S R4, R2   -> divide by +0: +inf
    rig.step();
    CHECK_EQ(as_float(rig.cpu->reg(2)), 3.75f);
    rig.step();
    CHECK_EQ(as_float(rig.cpu->reg(2)), 2.25f);
    rig.step();
    CHECK_EQ(as_float(rig.cpu->reg(2)), 3.375f);
    rig.step();
    CHECK_EQ(as_float(rig.cpu->reg(2)), 3.625f);
    CHECK(!rig.flag(k_z));
    CHECK(!rig.flag(k_s));
    rig.cpu->set_reg(4, fbits(0.0f));
    rig.step();
    CHECK(std::isinf(as_float(rig.cpu->reg(2))));
    CHECK(!rig.cpu->is_halted());
}

TEST_CASE(v60_float_compare_move_negate_abs_scale)
{
    StackRig rig;
    rig.cpu->set_reg(1, fbits(2.0f));
    rig.cpu->set_reg(2, fbits(1.0f));
    rig.set_flags(k_cy);
    rig.load({0x5C, 0x60, 0x61, 0x62,   // CMPF R1, R2: 1 < 2 -> S
              0x5C, 0x68, 0x61, 0x63,   // MOVF.S R1, R3: flags unchanged
              0x5C, 0x69, 0x63, 0x63,   // NEGF.S R3, R3 -> -2
              0x5C, 0x6A, 0x63, 0x64,   // ABSF.S R3, R4 -> 2
              0x5C, 0x70, 0x65, 0x64,   // SCLF.S R5 (= -2), R4 -> 2 * 2^-2 = 0.5
              0x5C, 0x30, 0xE3, 0x64}); // SCLF.S #3, R4 -> 4 (m1 = 0: immediate, m2 = 1: register)
    rig.step();
    CHECK(rig.flag(k_s));
    CHECK(!rig.flag(k_z));
    CHECK(!rig.flag(k_cy));
    rig.set_flags(k_z | k_cy);
    rig.step();
    CHECK_EQ(as_float(rig.cpu->reg(3)), 2.0f);
    CHECK(rig.flag(k_z)); // MOVF leaves the flags alone
    rig.step();
    CHECK_EQ(as_float(rig.cpu->reg(3)), -2.0f);
    CHECK(rig.flag(k_s));
    rig.step();
    CHECK_EQ(as_float(rig.cpu->reg(4)), 2.0f);
    rig.cpu->set_reg(5, 0xFFFE);
    rig.step();
    CHECK_EQ(as_float(rig.cpu->reg(4)), 0.5f);
    rig.step();
    CHECK_EQ(as_float(rig.cpu->reg(4)), 4.0f);

    StackRig equal;
    equal.cpu->set_reg(1, fbits(-0.0f));
    equal.cpu->set_reg(2, fbits(0.0f));
    equal.load({0x5C, 0x60, 0x61, 0x62}); // CMPF: -0 == +0
    equal.step();
    CHECK(equal.flag(k_z));

    StackRig unknown;
    unknown.load({0x5C, 0x01});
    unknown.step();
    CHECK(unknown.cpu->is_halted());
    CHECK(log_contains("floating-point instruction 0x5C, sub-opcode 0x01"));
}

// ---------------------------------------------------------------------------
// MOVT (0x19 .HB, 0x29 .WB, 0x2B .WH): move truncated
// ---------------------------------------------------------------------------

TEST_CASE(v60_movt_truncates_and_flags_overflow_like_vr)
{
    // VR at 0xFFA831: MOVT.HB [R1+], 4[R19] (19 C0 81 13 04).
    StackRig rig;
    rig.set_flags(k_z | k_cy);
    rig.cpu->set_reg(1, k_data);
    rig.cpu->set_reg(19, k_data + 0x100);
    rig.bus->write_word(k_data, 0xFF80);     // -128: fits a byte
    rig.bus->write_word(k_data + 2, 0x0180); // 384: does not
    rig.bus->write_byte(k_data + 0x105, 0xEE);
    rig.load({0x19, 0xC0, 0x81, 0x13, 0x04,
              0x19, 0xC0, 0x81, 0x13, 0x04});
    rig.step();
    CHECK_EQ(rig.bus->read_byte(k_data + 0x104), 0x80u);
    CHECK_EQ(rig.bus->read_byte(k_data + 0x105), 0xEEu); // a byte store
    CHECK_EQ(rig.cpu->reg(1), k_data + 2);               // autoincrement by a halfword
    CHECK(!rig.flag(k_ov));
    CHECK(rig.flag(k_z)); // other flags unchanged
    CHECK(rig.flag(k_cy));
    rig.step();
    CHECK_EQ(rig.bus->read_byte(k_data + 0x104), 0x80u);
    CHECK(rig.flag(k_ov));
    CHECK_EQ(rig.cpu->pc(), k_program + 10);
}

TEST_CASE(v60_movt_word_to_byte_and_half)
{
    StackRig rig;
    rig.cpu->set_reg(1, 0x00007FFF);
    rig.cpu->set_reg(2, 0xAAAAAAAA);
    rig.cpu->set_reg(3, 0xBBBBBBBB);
    rig.load({0x2B, 0x41, 0x62,   // MOVT.WH R1, R2: 0x7FFF fits
              0x29, 0x41, 0x63}); // MOVT.WB R1, R3: 0x7FFF does not fit a byte
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0xAAAA7FFFu);
    CHECK(!rig.flag(k_ov));
    rig.step();
    CHECK_EQ(rig.cpu->reg(3), 0xBBBBBBFFu);
    CHECK(rig.flag(k_ov));
}

// ---------------------------------------------------------------------------
// SCHCU / SKPCU (string group sub-ops 0x18 / 0x1A): search upward
// ---------------------------------------------------------------------------

TEST_CASE(v60_schcu_finds_halfword_like_vr)
{
    // VR at 0xFEDD9B: SCHCU.H [R1], R9, R0 (5A B8 61 89 60).
    const uint16_t table[] = {0x0010, 0x0020, 0x0030, 0x0040};
    for (const auto& [wanted, index] : {std::pair{0x0030u, 2u}, std::pair{0x0099u, 4u}}) {
        StackRig rig;
        for (uint32_t i = 0; i < 4; ++i) {
            rig.bus->write_word(k_data + i * 2, table[i]);
        }
        rig.cpu->set_reg(1, k_data);
        rig.cpu->set_reg(9, 4);
        rig.cpu->set_reg(0, wanted);
        rig.load({0x5A, 0xB8, 0x61, 0x89, 0x60});
        rig.step();
        CHECK_EQ(rig.cpu->reg(27), index);
        CHECK_EQ(rig.cpu->reg(28), k_data + index * 2);
        CHECK_EQ(rig.flag(k_z), index == 4); // set when not found (as in MAME)
        CHECK_EQ(rig.cpu->pc(), k_program + 5);
    }
}

TEST_CASE(v60_skpcu_skips_matching_bytes)
{
    StackRig rig;
    rig.bus->write_long(k_data, 0x41202020); // "   A"
    rig.cpu->set_reg(1, k_data);
    rig.load({0x58, 0x1A, 0x61, 0x08, 0xF4, 0x20}); // SKPCU.B [R1], #8, #' ' (m1 = m2 = 0)
    rig.step();
    CHECK_EQ(rig.cpu->reg(27), 3u); // first non-space
    CHECK_EQ(rig.cpu->reg(28), k_data + 3);
    CHECK(!rig.flag(k_z));
}

// ---------------------------------------------------------------------------
// MOVCD / MOVCFD (string sub-ops 0x09 / 0x0B): copy downward
// ---------------------------------------------------------------------------

TEST_CASE(v60_movcd_overlapping_copy_moves_up_safely)
{
    // Shift 4 halfwords up by one element in place (src and dst overlap),
    // as a scrolling table does: an upward copy would smear element 0.
    StackRig rig;
    for (uint32_t i = 0; i < 5; ++i) {
        rig.bus->write_word(k_data + i * 2, static_cast<uint16_t>(0x1000 + i));
    }
    rig.cpu->set_reg(21, k_data);
    rig.cpu->set_reg(22, k_data + 2);
    rig.load({0x5A, 0x09, 0x75, 0x04, 0x76, 0x04}); // MOVCD.H [R21], #4, [R22], #4
    rig.step();
    CHECK_EQ(rig.bus->read_word(k_data + 2), 0x1000u);
    CHECK_EQ(rig.bus->read_word(k_data + 4), 0x1001u);
    CHECK_EQ(rig.bus->read_word(k_data + 8), 0x1003u);
    CHECK_EQ(rig.bus->read_word(k_data), 0x1000u); // source start untouched
    CHECK_EQ(rig.cpu->reg(28), k_data - 2);        // src + (4 - 4 - 1) * 2, as in MAME
    CHECK_EQ(rig.cpu->pc(), k_program + 6);
}

TEST_CASE(v60_movcfd_fills_upper_part)
{
    StackRig rig;
    rig.bus->write_long(k_data, 0x22221111); // source: 2 halfwords
    rig.cpu->set_reg(21, k_data);
    rig.cpu->set_reg(22, k_data + 0x100);
    rig.cpu->set_reg(26, 0xFFFF);
    rig.load({0x5A, 0x0B, 0x75, 0x02, 0x76, 0x04}); // MOVCFD.H [R21], #2, [R22], #4
    rig.step();
    CHECK_EQ(rig.bus->read_word(k_data + 0x100), 0x1111u);
    CHECK_EQ(rig.bus->read_word(k_data + 0x102), 0x2222u);
    CHECK_EQ(rig.bus->read_word(k_data + 0x104), 0xFFFFu);
    CHECK_EQ(rig.bus->read_word(k_data + 0x106), 0xFFFFu);
}

// ---------------------------------------------------------------------------
// XCH (0x41 / 0x43 / 0x45): exchange
// ---------------------------------------------------------------------------

TEST_CASE(v60_xch_swaps_registers_like_vr)
{
    // VR at 0xFFA6E9 uses XCH.W between registers.
    StackRig rig;
    rig.set_flags(k_z | k_cy);
    rig.cpu->set_reg(0, 0x11111111);
    rig.cpu->set_reg(26, 0x22222222);
    rig.load({0x45, 0x40, 0x7A}); // XCH.W R0, R26 (format I, m = 1: register)
    rig.step();
    CHECK_EQ(rig.cpu->reg(0), 0x22222222u);
    CHECK_EQ(rig.cpu->reg(26), 0x11111111u);
    CHECK(rig.flag(k_z)); // flags untouched
    CHECK(rig.flag(k_cy));
    CHECK_EQ(rig.cpu->pc(), k_program + 3);
}

TEST_CASE(v60_xch_halfword_with_memory)
{
    StackRig rig;
    rig.cpu->set_reg(1, k_data);
    rig.cpu->set_reg(2, 0xAAAA1234);
    rig.bus->write_long(k_data, 0xBBBB5678);
    rig.load({0x43, 0x02, 0x61}); // XCH.H R2, [R1] (format I, m = 0: register indirect)
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0xAAAA5678u);       // low half only
    CHECK_EQ(rig.bus->read_long(k_data), 0xBBBB1234u);
}

// ---------------------------------------------------------------------------
// Bit strings (0x5B): SCH0BSU / SCH1BSU, MOVBSU / MOVBSD
// ---------------------------------------------------------------------------

TEST_CASE(v60_bit_string_search_like_vf)
{
    // VF at 0xFFAFA5 uses SCH1BSU. SCH1BSU 0[R1], #24, R2: first 1 bit.
    StackRig rig;
    rig.cpu->set_reg(1, k_data);
    rig.bus->write_byte(k_data, 0x00);
    rig.bus->write_byte(k_data + 1, 0x00);
    rig.bus->write_byte(k_data + 2, 0x10); // bit 20
    rig.load({0x5B, 0x22, 0x01, 0x00, 0x18, 0x62,   // SCH1BSU 0[R1], #24, R2
              0x5B, 0x22, 0x01, 0x00, 0x10, 0x63,   // SCH1BSU 0[R1], #16, R3: none
              0x5B, 0x20, 0x01, 0x00, 0x10, 0x64}); // SCH0BSU 0[R1], #16, R4
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 20u);
    CHECK(!rig.flag(k_z));
    CHECK_EQ(rig.cpu->reg(28), k_data + 2); // the byte holding the bit
    rig.step();
    CHECK_EQ(rig.cpu->reg(3), 16u); // not found: the length, Z set
    CHECK(rig.flag(k_z));
    rig.bus->write_byte(k_data, 0xFF);
    rig.bus->write_byte(k_data + 1, 0x0F);
    rig.step();
    CHECK_EQ(rig.cpu->reg(4), 12u); // first 0 bit
    CHECK(!rig.flag(k_z));
    CHECK(!rig.cpu->is_halted());
}

TEST_CASE(v60_bit_string_moves_up_and_down)
{
    // MOVBSU / MOVBSD 0[R1], #12, 4[R3]: bits 0-11 of the source to bits
    // 4-15 of the destination; the other destination bits are kept.
    for (uint8_t sub : {uint8_t{0x08}, uint8_t{0x09}}) {
        StackRig rig;
        rig.cpu->set_reg(1, k_data);
        rig.cpu->set_reg(3, k_data + 0x10);
        rig.bus->write_byte(k_data, 0xBC);
        rig.bus->write_byte(k_data + 1, 0xFA); // only the low nibble (0xA) is in range
        rig.bus->write_byte(k_data + 0x10, 0x0F);
        rig.bus->write_byte(k_data + 0x11, 0x00);
        rig.bus->write_byte(k_data + 0x12, 0xFF);
        rig.load({0x5B, sub, 0x01, 0x00, 0x0C, 0x03, 0x04});
        rig.step();
        CHECK_EQ(rig.bus->read_byte(k_data + 0x10), 0xCFu);
        CHECK_EQ(rig.bus->read_byte(k_data + 0x11), 0xABu);
        CHECK_EQ(rig.bus->read_byte(k_data + 0x12), 0xFFu);
        CHECK(!rig.cpu->is_halted());
    }
}

TEST_CASE(v60_level_triggered_interrupt_line)
{
    // The Model 1 interrupt controller holds the line while a level is
    // pending; the CPU asks it for the vector when it takes the interrupt.
    StackRig irq;
    irq.cpu->set_reg(1, k_data);
    irq.bus->write_long(k_data + (3 + V60::k_irq_vector_base) * 4, k_program + 0x80);
    irq.load({0x12, 0x01, 0xE5, 0xCD, 0xCD}); // LDPR R1, #5 (SBR); NOP; NOP
    irq.step();
    int acknowledged = 0;
    irq.cpu->set_irq_acknowledge([&acknowledged] { ++acknowledged; return uint8_t{3}; });
    irq.cpu->set_irq_line(true);
    irq.step(); // IE clear: not taken
    CHECK_EQ(acknowledged, 0);
    irq.cpu->set_psw(irq.cpu->psw() | V60::k_psw_ie);
    irq.step();
    CHECK_EQ(acknowledged, 1);
    CHECK_EQ(irq.cpu->pc(), k_program + 0x80);
}

// ---------------------------------------------------------------------------
// MOVD (0x3F): 64-bit move
// ---------------------------------------------------------------------------

TEST_CASE(v60_movd_moves_64_bits_like_vf)
{
    // VF at 0xFE4B37: MOVD /abs, R1 (register pair R1 / R2). Then back to
    // memory: MOVD R1, /abs + 8. Flags unchanged.
    StackRig rig;
    rig.bus->write_long(k_data, 0x11223344);
    rig.bus->write_long(k_data + 4, 0x55667788);
    rig.set_flags(V60::k_psw_z);
    rig.load({0x3F, 0x21, 0xF3, 0x00, 0x00, 0x51, 0x00,   // MOVD /0x510000, R1
              0x3F, 0x01, 0xF3, 0x08, 0x00, 0x51, 0x00}); // MOVD R1, /0x510008
    rig.step();
    CHECK_EQ(rig.cpu->reg(1), 0x11223344u);
    CHECK_EQ(rig.cpu->reg(2), 0x55667788u);
    rig.step();
    CHECK_EQ(rig.mem(k_data + 8), 0x11223344u);
    CHECK_EQ(rig.mem(k_data + 12), 0x55667788u);
    CHECK(rig.flag(k_z));
    CHECK(!rig.cpu->is_halted());
}

// ---------------------------------------------------------------------------
// RVBIT / RVBYT, PREPARE / DISPOSE, TASI, GETPSW, RETIU
// ---------------------------------------------------------------------------

TEST_CASE(v60_reverse_bits_and_bytes)
{
    StackRig rig;
    rig.cpu->set_reg(2, 0xAABBCC00);
    rig.load({0x08, 0x22, 0xF4, 0x01,                          // RVBIT #0x01, R2
              0x2C, 0x23, 0xF4, 0x78, 0x56, 0x34, 0x12});      // RVBYT #0x12345678, R3
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0xAABBCC80u); // only the low byte is written
    rig.step();
    CHECK_EQ(rig.cpu->reg(3), 0x78563412u);
}

TEST_CASE(v60_prepare_and_dispose_stack_frame)
{
    StackRig rig;
    rig.cpu->set_reg(V60::k_reg_fp, 0x00ABCDEF);
    const uint32_t sp = rig.sp();
    rig.load({0xDE, 0xF4, 0x10, 0x00, 0x00, 0x00, // PREPARE #16
              0xCC});                             // DISPOSE
    rig.step();
    CHECK_EQ(rig.cpu->reg(V60::k_reg_fp), sp - 4);
    CHECK_EQ(rig.sp(), sp - 4 - 16);
    CHECK_EQ(rig.mem(sp - 4), 0x00ABCDEFu); // the old FP
    rig.step();
    CHECK_EQ(rig.sp(), sp);
    CHECK_EQ(rig.cpu->reg(V60::k_reg_fp), 0x00ABCDEFu);
}

TEST_CASE(v60_tasi_and_getpsw)
{
    StackRig rig;
    rig.cpu->set_reg(4, 0x12345600);
    rig.cpu->set_reg(5, 0x000000FF);
    rig.load({0xE1, 0x64,   // TASI R4: was 0, now 0xFF; Z clear
              0xE1, 0x65,   // TASI R5: was 0xFF; Z set
              0xF7, 0x63}); // GETPSW R3
    rig.step();
    CHECK_EQ(rig.cpu->reg(4), 0x123456FFu);
    CHECK(!rig.flag(k_z));
    rig.step();
    CHECK(rig.flag(k_z));
    rig.step();
    CHECK_EQ(rig.cpu->reg(3), rig.cpu->psw());
    CHECK(!rig.cpu->is_halted());
}

// ---------------------------------------------------------------------------
// String compare (CMPC / CMPCS), downward search (SCHCD), decimal group (0x59)
// ---------------------------------------------------------------------------

namespace {
void put_string(StackRig& rig, uint32_t address, const char* text)
{
    for (uint32_t i = 0; text[i] != 0; ++i) {
        rig.bus->write_byte(address + i, static_cast<uint8_t>(text[i]));
    }
}
} // namespace

TEST_CASE(v60_string_compare)
{
    // CMPC.B [R1], #4, [R2], #4: S = first string greater, Z = equal.
    for (const auto& [a, b, z, sign] : {std::tuple{"ABCD", "ABCD", true, false},
                                        std::tuple{"ABCD", "ABCE", false, false},
                                        std::tuple{"ABDA", "ABCZ", false, true}}) {
        StackRig rig;
        rig.cpu->set_reg(1, k_data);
        rig.cpu->set_reg(2, k_data + 0x10);
        put_string(rig, k_data, a);
        put_string(rig, k_data + 0x10, b);
        rig.load({0x58, 0x00, 0x61, 0x04, 0x62, 0x04});
        rig.step();
        CHECK_EQ(rig.flag(k_z), z);
        CHECK_EQ(rig.flag(k_s), sign);
        CHECK(!rig.cpu->is_halted());
    }
    // CMPCS stops at the R26 character (CY cleared): "AB.x" vs "AB.y" equal up to '.'.
    StackRig stop;
    stop.cpu->set_reg(1, k_data);
    stop.cpu->set_reg(2, k_data + 0x10);
    stop.cpu->set_reg(26, '.');
    put_string(stop, k_data, "AB.x");
    put_string(stop, k_data + 0x10, "AB.y");
    stop.load({0x58, 0x02, 0x61, 0x04, 0x62, 0x04});
    stop.step();
    CHECK(!stop.flag(k_cy));
    CHECK(!stop.flag(k_s));
    CHECK_EQ(stop.cpu->reg(28), 4u + 2); // length 1 + index of the stop character
}

TEST_CASE(v60_string_search_down)
{
    // SCHCD.B [R1], #4, #'B': from index 4 down (MAME), first 'B' at 3.
    StackRig rig;
    rig.cpu->set_reg(1, k_data);
    put_string(rig, k_data, "ABCB");
    rig.load({0x58, 0x19, 0x61, 0x04, 0xF4, 0x42});
    rig.step();
    CHECK_EQ(rig.cpu->reg(27), 3u);
    CHECK_EQ(rig.cpu->reg(28), k_data + 3);
    CHECK(!rig.flag(k_z));
    CHECK(!rig.cpu->is_halted());
}

TEST_CASE(v60_decimal_arithmetic_and_conversion)
{
    StackRig rig;
    rig.cpu->set_reg(1, k_data);
    rig.cpu->set_reg(2, k_data + 0x10);
    rig.cpu->set_reg(3, k_data + 0x20);
    rig.bus->write_byte(k_data, 0x38);
    rig.load({0x59, 0x00, 0xF4, 0x45, 0x61, 0x00,         // ADDDC #0x45, [R1]: 38 + 45 = 83
              0x59, 0x00, 0xF4, 0x70, 0x61, 0x00,         // ADDDC #0x70, [R1]: 83 + 70 = 153: 53, CY
              0x59, 0x01, 0xF4, 0x05, 0x61, 0x00,         // SUBDC #0x05, [R1]: 53 - 5 - 1 = 47
              0x59, 0x10, 0xF4, 0x47, 0x62, 0x30,         // CVTDPZ #0x47, [R2], #'0': "47"
              0x59, 0x18, 0xF4, 0x34, 0x37, 0x63, 0x30}); // CVTDZP #"47", [R3]
    rig.step();
    CHECK_EQ(rig.bus->read_byte(k_data), 0x83u);
    CHECK(!rig.flag(k_cy));
    rig.step();
    CHECK_EQ(rig.bus->read_byte(k_data), 0x53u);
    CHECK(rig.flag(k_cy));
    rig.step();
    CHECK_EQ(rig.bus->read_byte(k_data), 0x47u);
    CHECK(!rig.flag(k_cy));
    rig.step();
    CHECK_EQ(rig.bus->read_byte(k_data + 0x10), static_cast<uint32_t>('4'));
    CHECK_EQ(rig.bus->read_byte(k_data + 0x11), static_cast<uint32_t>('7'));
    rig.step();
    CHECK_EQ(rig.bus->read_byte(k_data + 0x20), 0x47u);
    CHECK(!rig.cpu->is_halted());
}
