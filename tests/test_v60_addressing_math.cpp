// NEC V60 tests: addressing modes on MOV / ADD / SUB, multiply / divide
// (including division by zero), and TEST.
//
// Encoding reminders (see src/core/v60.hpp):
//   Format I flags byte  0 m d rrrrr : register Rr is the destination (d=1)
//                                      or the source (d=0); the other operand
//                                      is a general field with mode bit m.
//   Format II flags byte 1 m1 m2 ... : two general fields.

#include "test_framework.hpp"
#include "v60_test_rig.hpp"

#include <string>

using namespace model1_test;

namespace {

// Fills 16 words at k_data + 0x100 with 0xA0000000 + i (i = 0..15).
void fill_table(CpuRig& rig)
{
    for (uint32_t i = 0; i < 16; ++i) {
        rig.bus->write_long(k_data + 0x100 + 4 * i, 0xA0000000u + i);
    }
}

bool log_contains(const char* text)
{
    return model1_test::captured_log().find(text) != std::string::npos;
}

} // namespace

// ---------------------------------------------------------------------------
// Register indirect with displacement: disp[Rn]
// ---------------------------------------------------------------------------

TEST_CASE(v60_displacement_modes_read_base_plus_signed_offset)
{
    CpuRig rig;
    fill_table(rig);
    rig.cpu->set_reg(5, k_data + 0x100);
    rig.load({0x2D, 0x21, 0x05, 0x08,                          // MOV.W 8[R5], R1
              0x2D, 0x22, 0x05, 0xFC,                          // MOV.W -4[R5], R2
              0x2D, 0x23, 0x25, 0x3C, 0x00,                    // MOV.W 0x3C[R5], R3   (16-bit disp)
              0x2D, 0x24, 0x45, 0x00, 0xFF, 0xFF, 0xFF});      // MOV.W -0x100[R5], R4 (32-bit disp)
    rig.bus->write_long(k_data + 0xFC, 0x5EC0DE00);
    rig.bus->write_long(k_data, 0x0000BEEF);
    rig.step();
    CHECK_EQ(rig.cpu->reg(1), 0xA0000002u);
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0x5EC0DE00u);
    rig.step();
    CHECK_EQ(rig.cpu->reg(3), 0xA000000Fu);
    rig.step();
    CHECK_EQ(rig.cpu->reg(4), 0x0000BEEFu);
    CHECK_EQ(rig.cpu->reg(5), k_data + 0x100u); // base register untouched
    CHECK_EQ(rig.cpu->pc(), k_program + 4 + 4 + 5 + 7);
}

TEST_CASE(v60_displacement_mode_as_destination)
{
    CpuRig rig;
    rig.cpu->set_reg(1, 0xCAFEBABE);
    rig.cpu->set_reg(6, k_data);
    rig.load({0x2D, 0x01, 0x06, 0x08}); // MOV.W R1, 8[R6]
    rig.step();
    CHECK_EQ(rig.mem(k_data + 8), 0xCAFEBABEu);
    CHECK_EQ(rig.mem(k_data), 0u);
}

TEST_CASE(v60_add_sub_with_memory_operands)
{
    CpuRig rig;
    rig.cpu->set_reg(5, k_data);
    rig.bus->write_long(k_data + 4, 30);
    rig.bus->write_long(k_data + 8, 100);
    rig.bus->write_long(k_data + 0x10, 7);
    rig.cpu->set_reg(1, 7);
    rig.cpu->set_reg(2, 12);
    rig.load({0x84, 0x22, 0x05, 0x04,         // ADD.W 4[R5], R2        R2 = 12 + 30
              0xAC, 0x01, 0x05, 0x10,         // SUB.W R1, 0x10[R5]     [R5+16] = 7 - 7
              0xAC, 0x80, 0x05, 0x04, 0x05, 0x08}); // SUB.W 4[R5], 8[R5]  [R5+8] = 100 - 30
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 42u);
    rig.step();
    CHECK_EQ(rig.mem(k_data + 0x10), 0u);
    CHECK(rig.flag(k_z)); // flags come from the memory result
    rig.step();
    CHECK_EQ(rig.mem(k_data + 8), 70u);
    CHECK(!rig.flag(k_z));
}

// ---------------------------------------------------------------------------
// Other non-indexed modes
// ---------------------------------------------------------------------------

TEST_CASE(v60_register_indirect_and_indirect_modes)
{
    CpuRig rig;
    fill_table(rig);
    rig.bus->write_long(k_data + 0x40, k_data + 0x100 + 12); // pointer
    rig.cpu->set_reg(5, k_data);
    const uint32_t ptr = k_data + 0x40;
    rig.load({0x2D, 0x21, 0x65,                   // MOV.W [R5], R1             (reads k_data: 0)
              0x2D, 0x22, 0x85, 0x40,             // MOV.W [0x40[R5]], R2       (through the pointer)
              0x2D, 0x63, 0x05, 0x40, 0x04,       // MOV.W 4[0x40[R5]], R3      (double displacement)
              0x2D, 0x24, 0xFB, byte_of(ptr, 0), byte_of(ptr, 1), byte_of(ptr, 2), byte_of(ptr, 3)}); // MOV.W [/ptr], R4
    rig.step();
    CHECK_EQ(rig.cpu->reg(1), 0u);
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0xA0000003u);
    rig.step();
    CHECK_EQ(rig.cpu->reg(3), 0xA0000004u);
    rig.step();
    CHECK_EQ(rig.cpu->reg(4), 0xA0000003u);
}

TEST_CASE(v60_autoincrement_and_autodecrement_step_by_operand_size)
{
    CpuRig rig;
    fill_table(rig);
    rig.cpu->set_reg(5, k_data + 0x100);
    rig.load({0x2D, 0x61, 0x85,   // MOV.W [R5+], R1
              0x2D, 0x62, 0x85,   // MOV.W [R5+], R2
              0x2D, 0x63, 0xA5}); // MOV.W [-R5], R3
    rig.step();
    CHECK_EQ(rig.cpu->reg(1), 0xA0000000u);
    CHECK_EQ(rig.cpu->reg(5), k_data + 0x104u);
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0xA0000001u);
    CHECK_EQ(rig.cpu->reg(5), k_data + 0x108u);
    rig.step();
    CHECK_EQ(rig.cpu->reg(3), 0xA0000001u);
    CHECK_EQ(rig.cpu->reg(5), k_data + 0x104u);
}

TEST_CASE(v60_read_modify_write_applies_autoincrement_once)
{
    CpuRig rig;
    rig.cpu->set_reg(5, k_data);
    rig.bus->write_long(k_data, 41);
    rig.load({0x84, 0xA0, 0xE1, 0x85}); // ADD.W #1, [R5+]
    rig.step();
    CHECK_EQ(rig.mem(k_data), 42u);
    CHECK_EQ(rig.cpu->reg(5), k_data + 4u);
}

TEST_CASE(v60_pc_relative_and_absolute_modes)
{
    CpuRig rig;
    rig.bus->write_long(k_program + 0x40, 0x1111AAAA);
    rig.bus->write_long(k_data, 0x2222BBBB);
    rig.load({0x2D, 0x21, 0xF0, 0x40,                                                  // MOV.W 0x40[PC], R1
              0x2D, 0x22, 0xF3, byte_of(k_data, 0), byte_of(k_data, 1), byte_of(k_data, 2), byte_of(k_data, 3)}); // MOV.W /k_data, R2
    rig.step();
    CHECK_EQ(rig.cpu->reg(1), 0x1111AAAAu); // relative to the opcode address
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0x2222BBBBu);
}

// ---------------------------------------------------------------------------
// Indexed modes: base + Rx * operand size
// ---------------------------------------------------------------------------

TEST_CASE(v60_indexed_modes_scale_index_by_operand_size)
{
    CpuRig rig;
    fill_table(rig);
    rig.bus->write_long(k_data + 0x40, k_data + 0x100); // pointer for the indirect form
    rig.cpu->set_reg(4, 3);           // index Rx = R4
    rig.cpu->set_reg(6, k_data);      // base  Rb = R6
    const uint32_t table = k_data + 0x100;
    // MOV.W <indexed>, R1: Format I flags 0x61 (m=1), mode 0xC4 = indexed by R4.
    rig.load({0x2D, 0x61, 0xC4, 0x26, 0x00, 0x01,                // MOV.W 0x100[R6](R4), R1
              0x2D, 0x62, 0xC4, 0x86, 0x40,                      // MOV.W [0x40[R6]](R4), R2
              0x2D, 0x63, 0xC4, 0xF3, byte_of(table, 0), byte_of(table, 1), byte_of(table, 2), byte_of(table, 3)}); // MOV.W /table(R4), R3
    rig.step();
    CHECK_EQ(rig.cpu->reg(1), 0xA0000003u); // table + 3 * 4
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0xA0000003u);
    rig.step();
    CHECK_EQ(rig.cpu->reg(3), 0xA0000003u);
    CHECK_EQ(rig.cpu->reg(4), 3u); // index register untouched

    // Byte and halfword operations scale the same index by 1 and 2.
    rig.bus->write_long(k_data + 0x200, 0);
    rig.bus->write_long(k_data + 0x204, 0);
    rig.cpu->set_reg(1, 0);
    rig.cpu->set_reg(6, k_data + 0x200);
    const uint32_t next = rig.cpu->pc();
    rig.load({0x38, 0xE0, 0x61, 0xC4, 0x66,   // NOT.B R1, [R6](R4): byte at +3
              0x3A, 0xE0, 0x61, 0xC4, 0x66},  // NOT.H R1, [R6](R4): half at +6
             next);
    rig.step();
    CHECK_EQ(rig.mem(k_data + 0x200), 0xFF000000u);
    rig.step();
    CHECK_EQ(rig.mem(k_data + 0x204), 0xFFFF0000u);
}

TEST_CASE(v60_indexed_mode_as_destination)
{
    CpuRig rig;
    rig.cpu->set_reg(1, 0x600DF00D);
    rig.cpu->set_reg(4, 5);
    rig.cpu->set_reg(6, k_data);
    // Format I, register R1 as source (d=0); the destination field is indexed,
    // which needs mode bit m=1: flags 0 1 0 00001 = 0x41.
    rig.load({0x2D, 0x41, 0xC4, 0x06, 0x08}); // MOV.W R1, 8[R6](R4)
    rig.step();
    CHECK_EQ(rig.mem(k_data + 8 + 5 * 4), 0x600DF00Du);
}

TEST_CASE(v60_reserved_addressing_mode_halts_and_logs)
{
    CpuRig rig;
    rig.load({0x2D, 0x61, 0xE0}); // m=1, group 111: reserved
    rig.step();
    CHECK(rig.cpu->is_halted());
    CHECK_EQ(rig.cpu->pc(), k_program);
    CHECK(log_contains("unhandled addressing mode (mode byte 0xE0)"));
}

// ---------------------------------------------------------------------------
// MUL / MULU
// ---------------------------------------------------------------------------

TEST_CASE(v60_mul_signed_results_and_overflow)
{
    {
        CpuRig rig;
        rig.cpu->set_reg(1, 6);
        rig.cpu->set_reg(2, 7);
        rig.load({0x85, 0x62, 0x61}); // MUL.W R1, R2
        rig.step();
        CHECK_EQ(rig.cpu->reg(2), 42u);
        CHECK(!rig.flag(k_ov));
    }
    {
        CpuRig rig;
        rig.cpu->set_reg(1, 0xFFFFFFFF); // -1
        rig.cpu->set_reg(2, 5);
        rig.load({0x85, 0x62, 0x61});
        rig.step();
        CHECK_EQ(rig.cpu->reg(2), 0xFFFFFFFBu); // -5 fits: no overflow
        CHECK(rig.flag(k_s));
        CHECK(!rig.flag(k_ov));
    }
    {
        CpuRig rig;
        rig.cpu->set_reg(1, 0x10000);
        rig.cpu->set_reg(2, 0x10000);
        rig.load({0x85, 0x62, 0x61});
        rig.step();
        CHECK_EQ(rig.cpu->reg(2), 0u); // truncated
        CHECK(rig.flag(k_ov));
        CHECK(rig.flag(k_z));
    }
    {
        CpuRig rig; // MUL.B: 16 * 8 = 128 does not fit in a signed byte
        rig.cpu->set_reg(1, 0x10);
        rig.cpu->set_reg(2, 0xAABBCC08);
        rig.load({0x81, 0x62, 0x61});
        rig.step();
        CHECK_EQ(rig.cpu->reg(2), 0xAABBCC80u); // only the low byte changes
        CHECK(rig.flag(k_ov));
    }
}

TEST_CASE(v60_mulu_unsigned_overflow)
{
    CpuRig rig;
    rig.cpu->set_reg(1, 0xFFFFFFFF);
    rig.cpu->set_reg(2, 2);
    rig.load({0x95, 0x62, 0x61}); // MULU.W R1, R2
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0xFFFFFFFEu);
    CHECK(rig.flag(k_ov));
}

TEST_CASE(v60_mul_with_memory_operand)
{
    CpuRig rig;
    rig.cpu->set_reg(5, k_data);
    rig.bus->write_long(k_data + 4, 9);
    rig.cpu->set_reg(2, 11);
    rig.load({0x85, 0x22, 0x05, 0x04}); // MUL.W 4[R5], R2
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 99u);
}

// ---------------------------------------------------------------------------
// DIV / DIVU
// ---------------------------------------------------------------------------

TEST_CASE(v60_div_quotient_sets_sign_and_zero)
{
    {
        CpuRig rig;
        rig.cpu->set_reg(1, 7);
        rig.cpu->set_reg(2, 0xFFFFFFD6); // -42
        rig.load({0xA5, 0x62, 0x61});    // DIV.W R1, R2
        rig.step();
        CHECK_EQ(rig.cpu->reg(2), 0xFFFFFFFAu); // -6
        CHECK(rig.flag(k_s));
        CHECK(!rig.flag(k_z));
        CHECK(!rig.flag(k_ov));
    }
    {
        CpuRig rig;
        rig.cpu->set_reg(1, 4);
        rig.cpu->set_reg(2, 0xFFFFFFF9); // -7 / 4 = -1 (rounds toward zero)
        rig.load({0xA5, 0x62, 0x61});
        rig.step();
        CHECK_EQ(rig.cpu->reg(2), 0xFFFFFFFFu);
    }
    {
        CpuRig rig;
        rig.cpu->set_reg(1, 9);
        rig.cpu->set_reg(2, 3);
        rig.load({0xB5, 0x62, 0x61}); // DIVU.W: 3 / 9 = 0
        rig.step();
        CHECK_EQ(rig.cpu->reg(2), 0u);
        CHECK(rig.flag(k_z));
    }
}

TEST_CASE(v60_div_most_negative_by_minus_one_overflows)
{
    CpuRig rig;
    rig.cpu->set_reg(1, 0xFFFFFFFF);
    rig.cpu->set_reg(2, 0x80000000);
    rig.load({0xA5, 0x62, 0x61}); // DIV.W R1, R2
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0x80000000u); // unchanged
    CHECK(rig.flag(k_ov));
}

TEST_CASE(v60_division_by_zero_is_safe)
{
    // Every divide form: logs an error, sets OV, leaves the destination
    // unchanged, keeps running (no host crash, no CPU halt).
    struct Case {
        const char* name;
        uint8_t opcode;
    };
    for (const Case& c : {Case{"DIV.W", 0xA5}, Case{"DIVU.W", 0xB5}, Case{"DIV.B", 0xA1}, Case{"DIVU.H", 0xB3}}) {
        CpuRig rig;
        rig.cpu->set_reg(1, 0);
        rig.cpu->set_reg(2, 1234);
        rig.load({c.opcode, 0x62, 0x61, 0xCD}); // <div> R1, R2 ; NOP
        rig.step();
        CHECK_EQ(rig.cpu->reg(2), 1234u);
        CHECK(rig.flag(k_ov));
        CHECK(!rig.cpu->is_halted());
        CHECK_EQ(rig.cpu->pc(), k_program + 3);
        rig.step(); // the next instruction still runs
        CHECK_EQ(rig.cpu->pc(), k_program + 4);
        CHECK(log_contains("[V60] ERROR: division by zero at PC=0x00501000"));
    }
    {
        // 64-bit DIVX: dividend in R2 (low) / R3 (high), divisor in R1 = 0.
        CpuRig rig;
        rig.cpu->set_reg(2, 5);
        rig.cpu->set_reg(3, 1);
        rig.load({0xA6, 0x62, 0x61});
        rig.step();
        CHECK_EQ(rig.cpu->reg(2), 5u);
        CHECK_EQ(rig.cpu->reg(3), 1u);
        CHECK(rig.flag(k_ov));
        CHECK(!rig.cpu->is_halted());
    }
    {
        // Memory dividend, divisor zero.
        CpuRig rig;
        rig.cpu->set_reg(5, k_data);
        rig.bus->write_long(k_data, 777);
        rig.load({0xA5, 0x80, 0xE0, 0x65}); // DIV.W #0, [R5]
        rig.step();
        CHECK_EQ(rig.mem(k_data), 777u);
        CHECK(rig.flag(k_ov));
    }
}

TEST_CASE(v60_successful_division_clears_overflow)
{
    StackRig rig;
    rig.set_flags(k_ov);
    rig.cpu->set_reg(1, 2);
    rig.cpu->set_reg(2, 10);
    rig.load({0xA5, 0x62, 0x61}); // DIV.W R1, R2
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 5u);
    CHECK(!rig.flag(k_ov));
}

TEST_CASE(v60_divx_quotient_and_remainder)
{
    CpuRig rig;
    rig.cpu->set_reg(1, 7);
    rig.cpu->set_reg(2, 0xFFFFFFD3); // -45 as a 64-bit dividend in R3:R2
    rig.cpu->set_reg(3, 0xFFFFFFFF);
    rig.load({0xA6, 0x62, 0x61}); // DIVX R1, R2
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0xFFFFFFFAu); // quotient -6
    CHECK_EQ(rig.cpu->reg(3), 0xFFFFFFFDu); // remainder -3
    CHECK(rig.flag(k_s));
}

// ---------------------------------------------------------------------------
// TEST
// ---------------------------------------------------------------------------

TEST_CASE(v60_test_sets_zero_and_sign_without_writing)
{
    StackRig rig;
    rig.cpu->set_reg(1, 0x80000000);
    rig.cpu->set_reg(5, k_data);
    rig.bus->write_long(k_data, 0x00000080);
    rig.set_flags(k_cy | k_ov | k_z);
    rig.load({0xF5, 0x61,   // TEST.W R1     -> negative
              0xF0, 0x65,   // TEST.B [R5]   -> 0x80: negative byte
              0xF2, 0x65,   // TEST.H [R5]   -> 0x0080: positive halfword
              0xF4, 0xE0}); // TEST.W #0     -> zero
    rig.step();
    CHECK(rig.flag(k_s));
    CHECK(!rig.flag(k_z));
    CHECK(!rig.flag(k_cy)); // CY and OV are cleared
    CHECK(!rig.flag(k_ov));
    CHECK_EQ(rig.cpu->reg(1), 0x80000000u); // nothing written
    rig.step();
    CHECK(rig.flag(k_s));
    rig.step();
    CHECK(!rig.flag(k_s));
    CHECK(!rig.flag(k_z));
    rig.step();
    CHECK(rig.flag(k_z));
    CHECK(!rig.flag(k_s));
    CHECK_EQ(rig.mem(k_data), 0x00000080u);
    CHECK_EQ(rig.cpu->pc(), k_program + 8);
}
