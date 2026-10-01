// NEC V60 core tests: run hand-assembled instructions on a CPU wired to a
// real Bus, one execute_cycle() at a time, and check registers, PC and PSW.

#include "test_framework.hpp"
#include "v60_test_rig.hpp"

#include <string>

using namespace model1_test;

// ---------------------------------------------------------------------------
// Reset and NOP
// ---------------------------------------------------------------------------

TEST_CASE(v60_reset_state)
{
    CpuRig rig;
    rig.cpu->reset();
    CHECK_EQ(rig.cpu->pc(), 0xFFFFFFF0u);
    CHECK_EQ(rig.cpu->psw(), 0x10000000u); // interrupt-stack mode, flags clear
    CHECK(!rig.cpu->is_halted());
    CHECK_EQ(rig.cpu->instruction_count(), 0u);
}

TEST_CASE(v60_nop_advances_pc_only)
{
    CpuRig rig;
    rig.load({0xCD}); // NOP
    const uint32_t psw = rig.cpu->psw();
    const uint32_t cycles = rig.step();
    CHECK_EQ(rig.cpu->pc(), k_program + 1);
    CHECK_EQ(rig.cpu->psw(), psw);
    CHECK_EQ(cycles, V60::k_average_cycles_per_instruction);
    CHECK_EQ(rig.cpu->instruction_count(), 1u);
}

// ---------------------------------------------------------------------------
// MOV
// ---------------------------------------------------------------------------

TEST_CASE(v60_mov_immediate_to_register)
{
    CpuRig rig;
    rig.load({0x2D, 0x21, 0xE5,                         // MOV.W #5, R1         (quick immediate)
              0x2D, 0x22, 0xF4, 0x78, 0x56, 0x34, 0x12}); // MOV.W #0x12345678, R2
    rig.step();
    CHECK_EQ(rig.cpu->reg(1), 5u);
    CHECK_EQ(rig.cpu->pc(), k_program + 3);
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0x12345678u);
    CHECK_EQ(rig.cpu->pc(), k_program + 10);
}

TEST_CASE(v60_mov_register_to_register_keeps_flags)
{
    CpuRig rig;
    rig.cpu->set_reg(3, 0xCAFEBABE);
    rig.cpu->set_psw(V60::k_psw_is | k_z | k_cy);
    rig.load({0x2D, 0x64, 0x63}); // MOV.W R3, R4  (Format I: flags name R4 as destination)
    rig.step();
    CHECK_EQ(rig.cpu->reg(4), 0xCAFEBABEu);
    CHECK_EQ(rig.cpu->reg(3), 0xCAFEBABEu);
    CHECK(rig.flag(k_z));  // MOV does not touch flags
    CHECK(rig.flag(k_cy));
}

TEST_CASE(v60_mov_memory_round_trip)
{
    CpuRig rig;
    rig.cpu->set_reg(1, 0x0BADF00D);
    rig.cpu->set_reg(6, 0x510000);
    rig.load({0x2D, 0x01, 0x66,   // MOV.W R1, [R6]
              0x2D, 0x22, 0x66}); // MOV.W [R6], R2
    rig.step();
    CHECK_EQ(rig.bus->read_long(0x510000), 0x0BADF00Du);
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0x0BADF00Du);
}

// ---------------------------------------------------------------------------
// ADD / SUB and the Z, S, OV, CY flags
// ---------------------------------------------------------------------------

TEST_CASE(v60_add_positive_result_clears_flags)
{
    CpuRig rig;
    rig.cpu->set_reg(1, 5);
    rig.cpu->set_reg(2, 7);
    rig.load({0x84, 0x62, 0x61}); // ADD.W R1, R2  (R2 = R2 + R1)
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 12u);
    CHECK(!rig.flag(k_z));
    CHECK(!rig.flag(k_s));
    CHECK(!rig.flag(k_ov));
    CHECK(!rig.flag(k_cy));
}

TEST_CASE(v60_sub_r0_from_r0_sets_zero_flag)
{
    CpuRig rig;
    rig.cpu->set_reg(0, 0x1234);
    rig.cpu->set_psw(V60::k_psw_is | k_s); // S set beforehand, must be cleared
    rig.load({0xAC, 0x60, 0x60});         // SUB.W R0, R0
    rig.step();
    CHECK_EQ(rig.cpu->reg(0), 0u);
    CHECK(rig.flag(k_z));
    CHECK(!rig.flag(k_s));
    CHECK(!rig.flag(k_cy));
    CHECK(!rig.flag(k_ov));
}

TEST_CASE(v60_sub_negative_result_sets_sign_and_borrow)
{
    CpuRig rig;
    rig.cpu->set_reg(1, 1);
    rig.cpu->set_reg(2, 2);
    rig.load({0xAC, 0x42, 0x61}); // SUB.W R2, R1  (R1 = 1 - 2)
    rig.step();
    CHECK_EQ(rig.cpu->reg(1), 0xFFFFFFFFu);
    CHECK(rig.flag(k_s));
    CHECK(rig.flag(k_cy)); // borrow
    CHECK(!rig.flag(k_z));
    CHECK(!rig.flag(k_ov));
}

TEST_CASE(v60_add_signed_overflow_and_carry)
{
    CpuRig rig;
    rig.cpu->set_reg(1, 0x7FFFFFFF);
    rig.load({0x84, 0x21, 0xE1}); // ADD.W #1, R1: 0x7FFFFFFF + 1
    rig.step();
    CHECK_EQ(rig.cpu->reg(1), 0x80000000u);
    CHECK(rig.flag(k_ov));
    CHECK(rig.flag(k_s));
    CHECK(!rig.flag(k_cy));

    rig.cpu->set_reg(1, 0xFFFFFFFF);
    rig.load({0x84, 0x21, 0xE1}, k_program + 3); // ADD.W #1, R1: 0xFFFFFFFF + 1
    rig.step();
    CHECK_EQ(rig.cpu->reg(1), 0u);
    CHECK(rig.flag(k_cy));
    CHECK(rig.flag(k_z));
    CHECK(!rig.flag(k_ov));
}

// ---------------------------------------------------------------------------
// Defensive behaviour
// ---------------------------------------------------------------------------

TEST_CASE(v60_fetch_from_unmapped_address_is_logged)
{
    // 0x300000 is a hole in the Model 1 memory map. The fetch must be
    // reported by the bus, read as 0 (the HALT opcode), and leave the CPU
    // stopped in a well-defined state instead of crashing.
    CpuRig rig;
    rig.cpu->set_pc(0x300000);
    rig.step();

    const std::string log = model1_test::captured_log();
    CHECK(log.find("[Bus] CRITICAL: unmapped read8 at 0x00300000") != std::string::npos);
    CHECK(log.find("[V60] HALT at PC=0x00300000") != std::string::npos);
    CHECK(rig.cpu->is_halted());
    CHECK_EQ(rig.cpu->pc(), 0x300001u);

    // Further cycles are idle: no more fetches, no new log lines.
    const std::size_t log_size = model1_test::captured_log().size();
    rig.step();
    CHECK_EQ(model1_test::captured_log().size(), log_size);
}

TEST_CASE(v60_unimplemented_opcode_halts_and_logs)
{
    CpuRig rig;
    rig.load({0xCD, 0x01}); // NOP, then LDTASK (not implemented)
    rig.step();
    rig.step();
    const std::string log = model1_test::captured_log();
    CHECK(log.find("[V60] CRITICAL: unimplemented opcode 0x01 at PC=0x00501001") != std::string::npos);
    CHECK(rig.cpu->is_halted());
    CHECK_EQ(rig.cpu->pc(), k_program + 1); // rewound to the faulting instruction
    CHECK_EQ(rig.cpu->instruction_count(), 1u);
}

// ---------------------------------------------------------------------------
// Stack: PUSH / POP / PUSHM / POPM
// SP is R31; it points at the last pushed item and the stack grows down.
// ---------------------------------------------------------------------------


TEST_CASE(v60_push_register_decrements_sp_then_stores)
{
    StackRig rig;
    rig.cpu->set_reg(1, 0x12345678);
    rig.load({0xEF, 0x61}); // PUSH R1
    rig.step();
    CHECK_EQ(rig.sp(), k_stack_top - 4);
    CHECK_EQ(rig.mem(k_stack_top - 4), 0x12345678u);
    CHECK_EQ(rig.cpu->pc(), k_program + 2);
}

TEST_CASE(v60_pop_register_loads_then_increments_sp)
{
    StackRig rig;
    rig.cpu->set_reg(V60::k_reg_sp, k_stack_top - 4);
    rig.bus->write_long(k_stack_top - 4, 0xCAFEF00D);
    rig.load({0xE7, 0x62}); // POP R2
    rig.step();
    CHECK_EQ(rig.cpu->reg(2), 0xCAFEF00Du);
    CHECK_EQ(rig.sp(), k_stack_top);
}

TEST_CASE(v60_push_pop_sequence_is_last_in_first_out)
{
    StackRig rig;
    rig.cpu->set_reg(3, k_data);
    rig.bus->write_long(k_data, 0x0BADF00D);
    rig.load({0xEE, 0xE5,   // PUSH #5
              0xEE, 0x63,   // PUSH [R3]   (memory operand)
              0xE7, 0x64,   // POP R4      -> 0x0BADF00D
              0xE7, 0x65}); // POP R5      -> 5
    rig.step();
    rig.step();
    CHECK_EQ(rig.sp(), k_stack_top - 8);
    CHECK_EQ(rig.mem(k_stack_top - 4), 5u);
    CHECK_EQ(rig.mem(k_stack_top - 8), 0x0BADF00Du);
    rig.step();
    rig.step();
    CHECK_EQ(rig.cpu->reg(4), 0x0BADF00Du);
    CHECK_EQ(rig.cpu->reg(5), 5u);
    CHECK_EQ(rig.sp(), k_stack_top);
}

TEST_CASE(v60_pop_to_memory_uses_updated_sp)
{
    // POP pops first, then decodes its destination, so an SP-relative
    // destination sees the incremented SP.
    StackRig rig;
    rig.cpu->set_reg(V60::k_reg_sp, k_stack_top - 4);
    rig.bus->write_long(k_stack_top - 4, 77);
    rig.load({0xE6, 0x1F, 0x08}); // POP 8[R31]
    rig.step();
    CHECK_EQ(rig.sp(), k_stack_top);
    CHECK_EQ(rig.mem(k_stack_top + 8), 77u);
}

TEST_CASE(v60_pushm_popm_register_list_with_psw)
{
    // List bit 31 = PSW, bits 30-0 = R30-R0. PUSHM pushes the PSW first, then
    // R30 down to R0, so the lowest register ends at the lowest address.
    StackRig rig;
    rig.cpu->set_reg(1, 0x101);
    rig.cpu->set_reg(2, 0x202);
    rig.cpu->set_reg(30, 0x3030);
    rig.set_flags(k_z | k_cy);
    const uint32_t psw_before = rig.cpu->psw();
    const uint32_t list = 0xC0000006; // PSW, R30, R2, R1
    rig.load({0xEC, 0xF4, byte_of(list, 0), byte_of(list, 1), byte_of(list, 2), byte_of(list, 3),
              0xE4, 0xF4, byte_of(list, 0), byte_of(list, 1), byte_of(list, 2), byte_of(list, 3)});
    rig.step(); // PUSHM
    CHECK_EQ(rig.sp(), k_stack_top - 16);
    CHECK_EQ(rig.mem(k_stack_top - 4), psw_before);
    CHECK_EQ(rig.mem(k_stack_top - 8), 0x3030u);
    CHECK_EQ(rig.mem(k_stack_top - 12), 0x202u);
    CHECK_EQ(rig.mem(k_stack_top - 16), 0x101u);

    rig.cpu->set_reg(1, 0);
    rig.cpu->set_reg(2, 0);
    rig.cpu->set_reg(30, 0);
    rig.set_flags(k_s);
    rig.step(); // POPM
    CHECK_EQ(rig.cpu->reg(1), 0x101u);
    CHECK_EQ(rig.cpu->reg(2), 0x202u);
    CHECK_EQ(rig.cpu->reg(30), 0x3030u);
    CHECK_EQ(rig.cpu->psw(), psw_before); // low 16 PSW bits restored
    CHECK_EQ(rig.sp(), k_stack_top);
}

// ---------------------------------------------------------------------------
// Subroutines: CALL / RET, BSR / RSR
// ---------------------------------------------------------------------------

TEST_CASE(v60_call_ret_round_trip)
{
    // CALL target, args: push AP, AP = address of args, push return PC, jump.
    // RET #n: pop PC, pop AP, then drop n bytes of arguments.
    StackRig rig;
    rig.cpu->set_reg(V60::k_reg_ap, 0xA0A0);
    const uint32_t target = k_program + 0x200;
    const uint32_t args = k_data + 0x50;
    rig.load({0x49, 0x80,
              0xF3, byte_of(target, 0), byte_of(target, 1), byte_of(target, 2), byte_of(target, 3),
              0xF3, byte_of(args, 0), byte_of(args, 1), byte_of(args, 2), byte_of(args, 3)}); // CALL /target, /args
    rig.load({0xE2, 0xE8}, target); // RET #8
    rig.cpu->set_reg(V60::k_reg_sp, k_stack_top - 8); // as if 8 bytes of arguments were pushed

    rig.step();
    CHECK_EQ(rig.cpu->pc(), target);
    CHECK_EQ(rig.cpu->reg(V60::k_reg_ap), args);
    CHECK_EQ(rig.mem(k_stack_top - 12), 0xA0A0u);         // saved AP
    CHECK_EQ(rig.mem(k_stack_top - 16), k_program + 12); // return address
    CHECK_EQ(rig.sp(), k_stack_top - 16);

    rig.step();
    CHECK_EQ(rig.cpu->pc(), k_program + 12);
    CHECK_EQ(rig.cpu->reg(V60::k_reg_ap), 0xA0A0u);
    CHECK_EQ(rig.sp(), k_stack_top); // return frame and arguments released
}

TEST_CASE(v60_bsr_rsr_round_trip)
{
    StackRig rig;
    rig.load({0x48, 0x00, 0x01});  // BSR +0x100 (relative to the opcode)
    rig.load({0xCA}, k_program + 0x100); // RSR
    rig.step();
    CHECK_EQ(rig.cpu->pc(), k_program + 0x100);
    CHECK_EQ(rig.mem(k_stack_top - 4), k_program + 3);
    rig.step();
    CHECK_EQ(rig.cpu->pc(), k_program + 3);
    CHECK_EQ(rig.sp(), k_stack_top);
}

TEST_CASE(v60_call_with_register_target_halts)
{
    // A register has no address: an addressing-mode exception on hardware.
    StackRig rig;
    rig.load({0x49, 0x41, 0x62}); // CALL R1, ...
    rig.step();
    CHECK(rig.cpu->is_halted());
    CHECK(model1_test::captured_log().find("where an address is required") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Conditional branches. V60 names: BE = branch if zero (BZ), BNE = not zero
// (BNZ), BL = carry (BC), BNL = no carry (BNC), BV = overflow.
// Displacements are signed and relative to the opcode address.
// ---------------------------------------------------------------------------

namespace {

// Runs one 8-bit-displacement branch (opcode, disp +0x10) with the given
// flags and returns true if it was taken.
bool branch_taken(uint8_t opcode, uint32_t flags)
{
    StackRig rig;
    rig.set_flags(flags);
    rig.load({opcode, 0x10});
    rig.step();
    return rig.cpu->pc() == k_program + 0x10;
}

} // namespace

TEST_CASE(v60_bz_bnz_follow_zero_flag)
{
    CHECK(branch_taken(0x64, k_z));   // BE (BZ), Z = 1
    CHECK(!branch_taken(0x64, 0));    // BE (BZ), Z = 0
    CHECK(branch_taken(0x65, 0));     // BNE (BNZ), Z = 0
    CHECK(!branch_taken(0x65, k_z));  // BNE (BNZ), Z = 1
}

TEST_CASE(v60_bc_bnc_follow_carry_flag)
{
    CHECK(branch_taken(0x62, k_cy));  // BL (BC), CY = 1
    CHECK(!branch_taken(0x62, 0));
    CHECK(branch_taken(0x63, 0));     // BNL (BNC), CY = 0
    CHECK(!branch_taken(0x63, k_cy));
}

TEST_CASE(v60_bv_bnv_follow_overflow_flag)
{
    CHECK(branch_taken(0x60, k_ov));  // BV, OV = 1
    CHECK(!branch_taken(0x60, 0));
    CHECK(branch_taken(0x61, 0));     // BNV, OV = 0
    CHECK(!branch_taken(0x61, k_ov));
}

TEST_CASE(v60_branch_ignores_unrelated_flags)
{
    // Every other flag set: BE still not taken without Z, BL without CY.
    CHECK(!branch_taken(0x64, k_s | k_ov | k_cy));
    CHECK(!branch_taken(0x62, k_s | k_ov | k_z));
    CHECK(!branch_taken(0x60, k_s | k_z | k_cy));
}

TEST_CASE(v60_branch_displacements_are_signed_and_opcode_relative)
{
    StackRig rig;
    rig.set_flags(k_z);
    rig.load({0x64, 0xF0}); // BE -0x10
    rig.step();
    CHECK_EQ(rig.cpu->pc(), k_program - 0x10);

    StackRig wide;
    wide.set_flags(k_z);
    wide.load({0x74, 0x00, 0xFF}); // BE (16-bit) -0x100
    wide.step();
    CHECK_EQ(wide.cpu->pc(), k_program - 0x100);

    StackRig not_taken;
    not_taken.load({0x74, 0x00, 0xFF}); // Z clear: skip the 3-byte instruction
    not_taken.step();
    CHECK_EQ(not_taken.cpu->pc(), k_program + 3);
}

TEST_CASE(v60_all_conditions_against_all_flag_combinations)
{
    auto expected = [](int condition, bool z, bool s, bool ov, bool cy) {
        switch (condition) {
        case 0x0: return ov;          // BV
        case 0x1: return !ov;         // BNV
        case 0x2: return cy;          // BL
        case 0x3: return !cy;         // BNL
        case 0x4: return z;           // BE
        case 0x5: return !z;          // BNE
        case 0x6: return cy || z;     // BNH
        case 0x7: return !(cy || z);  // BH
        case 0x8: return s;           // BN
        case 0x9: return !s;          // BP
        case 0xA: return true;        // BR
        case 0xC: return s != ov;     // BLT
        case 0xD: return s == ov;     // BGE
        case 0xE: return (s != ov) || z;     // BLE
        default:  return !((s != ov) || z);  // BGT
        }
    };
    for (int condition = 0; condition < 16; ++condition) {
        if (condition == 0xB) {
            continue; // 0x6B / 0x7B are not branch opcodes
        }
        for (uint32_t flags = 0; flags < 16; ++flags) {
            const bool want = expected(condition, (flags & k_z) != 0, (flags & k_s) != 0,
                                       (flags & k_ov) != 0, (flags & k_cy) != 0);
            CHECK_EQ(branch_taken(static_cast<uint8_t>(0x60 | condition), flags), want);
        }
    }
}

TEST_CASE(v60_countdown_loop_with_sub_and_bnz)
{
    // loop: SUB.W #1, R1 ; BNE loop   - runs until R1 reaches zero.
    StackRig rig;
    rig.cpu->set_reg(1, 5);
    rig.load({0xAC, 0x21, 0xE1,   // SUB.W #1, R1
              0x65, 0xFD});       // BNE -3 (back to SUB)
    int steps = 0;
    while (rig.cpu->pc() != k_program + 5 && steps < 100) {
        rig.step();
        ++steps;
    }
    CHECK_EQ(rig.cpu->reg(1), 0u);
    CHECK_EQ(steps, 10); // 5 iterations x 2 instructions
    CHECK(rig.flag(k_z));
}

// ---------------------------------------------------------------------------
// Stack bounds checks (defensive logging; execution continues)
// ---------------------------------------------------------------------------

TEST_CASE(v60_stack_overflow_below_work_ram_is_logged)
{
    // SP at the very bottom of work RAM B: the push lands below it, in an
    // unmapped hole.
    StackRig rig;
    rig.cpu->set_reg(V60::k_reg_sp, Bus::k_ram_b_base);
    rig.load({0xEE, 0xE1}); // PUSH #1
    rig.step();
    const std::string log = model1_test::captured_log();
    CHECK(log.find("[V60] WARNING: stack push overflow: access outside work RAM, SP=0x004FFFFC") != std::string::npos);
    CHECK(log.find("[Bus] CRITICAL: unmapped write32 at 0x004FFFFC") != std::string::npos);
    CHECK(!rig.cpu->is_halted());
    CHECK_EQ(rig.sp(), Bus::k_ram_b_base - 4);
}

TEST_CASE(v60_stack_underflow_past_work_ram_is_logged)
{
    // SP at the end of work RAM B: popping reads past it.
    StackRig rig;
    rig.cpu->set_reg(V60::k_reg_sp, Bus::k_ram_b_base + Bus::k_ram_b_size);
    rig.load({0xE7, 0x61}); // POP R1
    rig.step();
    CHECK(model1_test::captured_log().find("[V60] WARNING: stack pop underflow: access outside work RAM, SP=0x00540000")
          != std::string::npos);
    CHECK(!rig.cpu->is_halted());
}

TEST_CASE(v60_stack_inside_work_ram_is_silent)
{
    StackRig rig;
    rig.cpu->set_reg(V60::k_reg_sp, Bus::k_ram_a_base + 0x100); // work RAM A also counts
    rig.load({0xEE, 0xE1, 0xE7, 0x61}); // PUSH #1 ; POP R1
    rig.step();
    rig.step();
    CHECK(model1_test::captured_log().find("WARNING: stack") == std::string::npos);
    CHECK_EQ(rig.cpu->reg(1), 1u);
}

TEST_CASE(v60_misaligned_stack_is_logged)
{
    StackRig rig;
    rig.cpu->set_reg(V60::k_reg_sp, k_stack_top + 2);
    rig.load({0xEE, 0xE1}); // PUSH #1
    rig.step();
    CHECK(model1_test::captured_log().find("[V60] WARNING: stack push: misaligned, SP=0x0052FFFE") != std::string::npos);
    CHECK_EQ(rig.mem(k_stack_top - 2), 1u); // the access still happens (V60 allows it)
}
