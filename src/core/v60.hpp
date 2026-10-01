#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <optional>

namespace model1 {

class Bus;

// NEC uPD70616 (V60) main CPU.
//
// Programmer-visible state:
//   R0-R31  32-bit general-purpose registers
//           (R29 = AP argument pointer, R30 = FP frame pointer, R31 = SP)
//   PC      program counter
//   PSW     program status word (see k_psw_* below)
// Privileged state used by interrupts:
//   ISP     interrupt stack pointer (SP while PSW.IS = 1)
//   L0SP-L3SP  per-execution-level stack pointers (SP while PSW.IS = 0)
//   SBR     system base register; bits 31-12 locate the vector table
// LDPR / STPR reach them by number: 0 ISP, 1-4 L0SP-L3SP, 5 SBR, then
// 6 TR, 7 SYCW, 8 TKCW, 9 PIR, 15 PSW2, 16-23 ATBR0-3/ATLR0-3, 24 TRMOD,
// 25-26 ADTR0-1, 27-28 ADTMR0-1. Those after SBR (task, MMU, debug and
// system control) are stored and read back, with no effect emulated, except
// TKCW's rounding control (bits 2-0), used by CVTSW.
//
// The V60 drives a 24-bit external address bus: every address the CPU puts
// on the bus is masked with k_address_mask. After reset the CPU fetches from
// 0xFFFFFFF0, which appears on the bus as 0xFFFFF0. Data is little-endian and
// may be unaligned.
//
// Instruction encoding:
//   Byte 0 is the opcode. Two-operand instructions ("Format I/II") follow
//   with a flags byte:
//     Format I  : 0 m d rrrrr  - one operand is register Rr; the other is a
//                                general operand field with mode bit m.
//                                d=1: Rr is the destination; d=0: the source.
//     Format II : 1 m1 m2 xxxxx - both operands are general operand fields.
//   One-operand instructions take the mode bit m from bit 0 of the opcode.
//
// General operand field: a mode byte selected by the mode bit m, then any
// displacement / address / immediate bytes. "disp" values are signed.
//   m=1  000 rrrrr  disp8[disp8[Rn]]    double displacement: [ [Rn+d1] ] + d2
//        001 rrrrr  disp16[disp16[Rn]]
//        010 rrrrr  disp32[disp32[Rn]]
//        011 rrrrr  Rn                  register
//        100 rrrrr  [Rn+]               autoincrement (by operand size, after)
//        101 rrrrr  [-Rn]               autodecrement (by operand size, before)
//        110 xxxxx  indexed modes: xxxxx = index register Rx, scaled by the
//                   operand size (x1, x2, x4, x8). A second mode byte follows:
//                   000-010 bbbbb  disp8/16/32[Rb](Rx)
//                   011     bbbbb  [Rb](Rx)
//                   100-110 bbbbb  [disp8/16/32[Rb]](Rx)
//                   111 1 0000-0010  disp8/16/32[PC](Rx)
//                   111 1 0011       /abs32(Rx)
//                   111 1 1000-1010  [disp8/16/32[PC]](Rx)
//                   111 1 1011       [/abs32](Rx)
//   m=0  000 rrrrr  disp8[Rn]           displacement
//        001 rrrrr  disp16[Rn]
//        010 rrrrr  disp32[Rn]
//        011 rrrrr  [Rn]                register indirect
//        100 rrrrr  [disp8[Rn]]         displacement indirect: [Rn+d] holds the address
//        101 rrrrr  [disp16[Rn]]
//        110 rrrrr  [disp32[Rn]]
//        111 0iiii  #i                  immediate quick (0-15)
//        111 10000-10010  disp8/16/32[PC]
//        111 10011  /abs32              direct address
//        111 10100  #imm                immediate (operation size)
//        111 11000-11010  [disp8/16/32[PC]]
//        111 11011  [/abs32]            direct address deferred
//        111 11100-11110  disp[disp[PC]]  PC double displacement
//   PC-relative modes and branches are relative to the address of the
//   opcode byte, not the byte after it.
//
//   String instructions (Format VII-a: 0x58 bytes, 0x5A halfwords) follow
//   with a sub-opcode byte (bits 4-0 = operation, bit 6 = mode bit m of
//   operand 1, bit 5 = mode bit of operand 2), then: operand 1, its length
//   byte, operand 2, its length byte. A length byte with bit 7 set means
//   "the length is in register Rn" (bits 4-0), else it is the 7-bit value.
//
// Maskable interrupts:
//   An external device requests an interrupt with a vector number. Before
//   each instruction, if a request is pending and PSW.IE = 1, the CPU:
//     1. switches to the interrupt stack (PSW.IS = 1, SP = ISP),
//     2. clears PSW.IE (and the trace/exception bits), sets EL = 0,
//     3. pushes the old PSW, then the return PC,
//     4. jumps through vector table entry (vector + 0x40) at SBR & ~0xFFF.
//   RETIS pops PC and PSW, which restores the previous stack and IE state.
class V60 {
public:
    static constexpr uint32_t k_reset_pc = 0xFFFFFFF0;
    static constexpr uint32_t k_reset_psw = 0x10000000; // IS=1: interrupt stack
    static constexpr uint32_t k_address_mask = 0x00FFFFFF; // 24-bit bus

    // PSW bits.
    static constexpr uint32_t k_psw_z  = 1u << 0;  // zero
    static constexpr uint32_t k_psw_s  = 1u << 1;  // sign
    static constexpr uint32_t k_psw_ov = 1u << 2;  // overflow
    static constexpr uint32_t k_psw_cy = 1u << 3;  // carry / borrow
    static constexpr uint32_t k_psw_te = 1u << 16; // trace enable
    static constexpr uint32_t k_psw_ae = 1u << 17; // address trap enable
    static constexpr uint32_t k_psw_ie = 1u << 18; // maskable interrupt enable
    static constexpr uint32_t k_psw_el_shift = 24;  // execution level, 2 bits
    static constexpr uint32_t k_psw_el = 3u << k_psw_el_shift;
    static constexpr uint32_t k_psw_tp = 1u << 27; // trace pending
    static constexpr uint32_t k_psw_is = 1u << 28; // running on interrupt stack
    static constexpr uint32_t k_psw_em = 1u << 29; // emulation mode
    static constexpr uint32_t k_psw_asa = 1u << 31; // address space architecture

    // Maskable interrupt vector N uses vector table entry N + this offset.
    static constexpr uint32_t k_irq_vector_base = 0x40;

    // Placeholder timing: real per-instruction cycle counts are not modelled
    // yet, so every instruction (and interrupt entry) is charged this
    // average. MAME's V60 core uses the same approximation.
    static constexpr uint32_t k_average_cycles_per_instruction = 8;

    static constexpr int k_register_count = 32;
    static constexpr int k_reg_ap = 29;
    static constexpr int k_reg_fp = 30;
    static constexpr int k_reg_sp = 31;

    explicit V60(Bus& bus);

    V60(const V60&) = delete;
    V60& operator=(const V60&) = delete;
    V60(V60&&) = delete;
    V60& operator=(V60&&) = delete;

    // Power-on / RESET pin state.
    void reset();

    // Executes one step and returns the number of clock cycles it consumed:
    // either takes a pending interrupt, or fetches the opcode byte at PC,
    // advances PC, decodes and executes one instruction.
    // A halted CPU consumes cycles without executing anything; a CPU halted by
    // the HALT instruction wakes up when it accepts an interrupt.
    uint32_t execute_cycle();

    // Latches a maskable interrupt request with the vector number the
    // requesting device would supply during the acknowledge cycle. The request
    // stays pending until the CPU accepts it (PSW.IE = 1). When several are
    // pending, the lowest vector number is taken first.
    void request_interrupt(uint8_t vector);

    [[nodiscard]] bool is_halted() const { return m_halt_reason != HaltReason::None; }
    [[nodiscard]] bool has_pending_interrupt() const { return m_pending_irqs.any(); }
    [[nodiscard]] uint64_t instruction_count() const { return m_instruction_count; }
    [[nodiscard]] uint64_t interrupt_count() const { return m_interrupt_count; }

    // Logs the next `count` instructions (address and bytes) to std::cerr.
    void set_trace(uint64_t count) { m_trace_remaining = count; }

    // State access for the motherboard, debuggers and tests.
    [[nodiscard]] uint32_t reg(int index) const { return m_reg[static_cast<std::size_t>(index & 0x1F)]; }
    void set_reg(int index, uint32_t value) { m_reg[static_cast<std::size_t>(index & 0x1F)] = value; }
    [[nodiscard]] uint32_t pc() const { return m_pc; }
    void set_pc(uint32_t value) { m_pc = value; }
    [[nodiscard]] uint32_t psw() const { return m_psw; }
    // Writes the PSW with hardware semantics (swaps SP on IS / EL changes).
    void set_psw(uint32_t value) { write_psw(value); }
    [[nodiscard]] uint32_t isp() const { return m_isp; }
    void set_isp(uint32_t value) { m_isp = value; }
    [[nodiscard]] uint32_t level_sp(int level) const { return m_level_sp[static_cast<std::size_t>(level & 3)]; }
    void set_level_sp(int level, uint32_t value) { m_level_sp[static_cast<std::size_t>(level & 3)] = value; }
    [[nodiscard]] uint32_t sbr() const { return m_sbr; }
    void set_sbr(uint32_t value) { m_sbr = value; }

private:
    // Opcode bytes. Values are from the V60 instruction set.
    enum Opcode : uint8_t {
        k_op_halt     = 0x00,
        k_op_mov_b    = 0x09,
        k_op_movs_bh  = 0x0A, // move sign-extended (byte -> half)
        k_op_movz_bh  = 0x0B, // move zero-extended (byte -> half)
        k_op_movs_bw  = 0x0C,
        k_op_movz_bw  = 0x0D,
        k_op_movs_hw  = 0x1C,
        k_op_movz_hw  = 0x1D,
        k_op_movt_hb  = 0x19, // move truncated (keeps the low bits)
        k_op_movt_wb  = 0x29,
        k_op_movt_wh  = 0x2B,
        k_op_stpr     = 0x02, // store privileged register
        k_op_ldpr     = 0x12, // load privileged register
        k_op_updpsw_w = 0x13, // update PSW fields under a mask (24 bits)
        k_op_in_b     = 0x20, // I/O space: IN reads, OUT writes
        k_op_out_b    = 0x21,
        k_op_in_h     = 0x22,
        k_op_out_h    = 0x23,
        k_op_in_w     = 0x24,
        k_op_out_w    = 0x25,
        k_op_mov_h    = 0x1B,
        k_op_movea_b  = 0x40, // move effective address (byte / half / word
        k_op_xch_b    = 0x41, // exchange two operands
        k_op_xch_h    = 0x43,
        k_op_xch_w    = 0x45,
        k_op_movea_h  = 0x42, // operand size: sets index scale and
        k_op_movea_w  = 0x44,
        k_op_setf     = 0x47, // store a condition as a byte (1 / 0) // autoincrement step)
        k_op_updpsw_h = 0x4A, // update PSW fields under a mask (16 bits)
        k_op_rem_b    = 0x50, // remainder (signed / unsigned)
        k_op_remu_b   = 0x51,
        k_op_rem_h    = 0x52,
        k_op_remu_h   = 0x53,
        k_op_rem_w    = 0x54,
        k_op_remu_w   = 0x55,
        k_op_bit_field = 0x5D, // bit-field group: EXTBFS/Z/L, INSBFR/L (sub-opcode follows)
        k_op_float     = 0x5C, // single-precision float group (sub-opcode follows)
        k_op_float_convert = 0x5F, // CVTWS / CVTSW (sub-opcode follows)
        k_op_string_b = 0x58, // string instructions on bytes (sub-opcode follows)
        k_op_string_h = 0x5A, // string instructions on halfwords
        k_op_mov_w    = 0x2D,
        k_op_not_b    = 0x38,
        k_op_neg_b    = 0x39, // dst = 0 - src
        k_op_not_h    = 0x3A,
        k_op_neg_h    = 0x3B,
        k_op_not_w    = 0x3C,
        k_op_neg_w    = 0x3D,
        k_op_bsr      = 0x48, // branch to subroutine, 16-bit displacement
        k_op_call     = 0x49,
        k_op_bcc8_first  = 0x60, // conditional branches, 8-bit displacement
        k_op_br8      = 0x6A,
        k_op_bcc8_last   = 0x6F,
        k_op_bcc16_first = 0x70, // conditional branches, 16-bit displacement
        k_op_br16     = 0x7A,
        k_op_bcc16_last  = 0x7F,
        k_op_add_b    = 0x80,
        k_op_mul_b    = 0x81,
        k_op_add_h    = 0x82,
        k_op_mul_h    = 0x83,
        k_op_add_w    = 0x84,
        k_op_mul_w    = 0x85,
        k_op_mulx     = 0x86, // signed 32 x 32 -> 64
        k_op_test1    = 0x87,
        k_op_or_b     = 0x88,
        k_op_rot_b    = 0x89, // rotate (signed count: + left, - right)
        k_op_or_h     = 0x8A,
        k_op_rot_h    = 0x8B,
        k_op_or_w     = 0x8C,
        k_op_rot_w    = 0x8D,
        k_op_addc_b   = 0x90, // add with carry
        k_op_mulu_b   = 0x91,
        k_op_addc_h   = 0x92,
        k_op_mulu_h   = 0x93,
        k_op_addc_w   = 0x94,
        k_op_mulu_w   = 0x95,
        k_op_mulux    = 0x96, // unsigned 32 x 32 -> 64
        k_op_set1     = 0x97,
        k_op_subc_b   = 0x98, // subtract with borrow (CY)
        k_op_rotc_b   = 0x99, // rotate through CY
        k_op_subc_h   = 0x9A,
        k_op_rotc_h   = 0x9B,
        k_op_subc_w   = 0x9C,
        k_op_rotc_w   = 0x9D,
        k_op_and_b    = 0xA0,
        k_op_div_b    = 0xA1,
        k_op_and_h    = 0xA2,
        k_op_div_h    = 0xA3,
        k_op_and_w    = 0xA4,
        k_op_div_w    = 0xA5,
        k_op_divx     = 0xA6, // signed 64 / 32 -> 32 quotient, 32 remainder
        k_op_clr1     = 0xA7,
        k_op_sub_b    = 0xA8,
        k_op_shl_b    = 0xA9,
        k_op_sub_h    = 0xAA,
        k_op_shl_h    = 0xAB,
        k_op_sub_w    = 0xAC,
        k_op_shl_w    = 0xAD,
        k_op_xor_b    = 0xB0,
        k_op_divu_b   = 0xB1,
        k_op_xor_h    = 0xB2,
        k_op_divu_h   = 0xB3,
        k_op_xor_w    = 0xB4,
        k_op_divu_w   = 0xB5,
        k_op_divux    = 0xB6, // unsigned 64 / 32 -> 32 quotient, 32 remainder
        k_op_not1     = 0xB7,
        k_op_cmp_b    = 0xB8, // compare: flags from second - first operand
        k_op_sha_b    = 0xB9,
        k_op_cmp_h    = 0xBA,
        k_op_sha_h    = 0xBB,
        k_op_cmp_w    = 0xBC,
        k_op_sha_w    = 0xBD,
        k_op_dbcc     = 0xC6, // decrement and branch: V L E NH N R LT LE
        k_op_dbcc_not = 0xC7, // NV NL NE H P TB GE GT (TB: branch if register == 0)
        k_op_rsr      = 0xCA, // return from BSR
        k_op_nop      = 0xCD,
        k_op_dec_b_m0 = 0xD0, // one-operand instructions: low bit = mode bit m
        k_op_dec_b_m1 = 0xD1,
        k_op_dec_h_m0 = 0xD2,
        k_op_dec_h_m1 = 0xD3,
        k_op_dec_w_m0 = 0xD4,
        k_op_dec_w_m1 = 0xD5,
        k_op_jmp_m0   = 0xD6,
        k_op_jmp_m1   = 0xD7,
        k_op_inc_b_m0 = 0xD8,
        k_op_inc_b_m1 = 0xD9,
        k_op_inc_h_m0 = 0xDA,
        k_op_inc_h_m1 = 0xDB,
        k_op_inc_w_m0 = 0xDC,
        k_op_inc_w_m1 = 0xDD,
        k_op_ret_m0   = 0xE2,
        k_op_ret_m1   = 0xE3,
        k_op_popm_m0  = 0xE4,
        k_op_popm_m1  = 0xE5,
        k_op_pop_m0   = 0xE6,
        k_op_pop_m1   = 0xE7,
        k_op_jsr_m0   = 0xE8, // jump to subroutine (return with RSR)
        k_op_jsr_m1   = 0xE9,
        k_op_pushm_m0 = 0xEC,
        k_op_pushm_m1 = 0xED,
        k_op_push_m0  = 0xEE,
        k_op_push_m1  = 0xEF,
        k_op_test_b_m0 = 0xF0, // TEST: set Z/S from one operand
        k_op_test_b_m1 = 0xF1,
        k_op_test_h_m0 = 0xF2,
        k_op_test_h_m1 = 0xF3,
        k_op_test_w_m0 = 0xF4,
        k_op_test_w_m1 = 0xF5,
        k_op_retis_m0 = 0xFA,
        k_op_retis_m1 = 0xFB,
    };

    // Branch condition, selected by the low nibble of a Bcc opcode.
    enum class Condition : uint8_t {
        V = 0x0, NV = 0x1,   // overflow / no overflow
        L = 0x2, NL = 0x3,   // lower (CY) / not lower: unsigned <, >=
        E = 0x4, NE = 0x5,   // equal (Z) / not equal
        NH = 0x6, H = 0x7,   // not higher / higher: unsigned <=, >
        N = 0x8, P = 0x9,    // negative (S) / positive
        R = 0xA,             // always (BR)
        LT = 0xC, GE = 0xD,  // signed <, >=
        LE = 0xE, GT = 0xF,  // signed <=, >
    };

    enum class HaltReason {
        None,
        HaltInstruction,      // HALT executed; waits for an interrupt
        UnimplementedOpcode,
        UnimplementedOperand, // addressing mode not supported yet / reserved
        ImmediateDestination, // immediate used as a write target
        AddressRequired,      // operand has no address (register / immediate)
        InvalidRegisterPair,  // 64-bit operand in R31 (no R32 to pair with)
        PrivilegedInstruction, // LDPR / STPR outside execution level 0
        ReservedPrivilegedRegister, // LDPR / STPR register number 10-14 or > 28
    };

    // Quad (8 bytes) is only used to address the 64-bit operands of MULX /
    // DIVX: it sets the autoincrement step and index scale.
    enum class OperandSize : uint8_t { Byte = 1, Half = 2, Word = 4, Quad = 8 };
    static constexpr uint32_t bytes_of(OperandSize size) { return static_cast<uint32_t>(size); }

    enum class ShiftKind { Logical, Arithmetic };
    enum class LogicOp { And, Or, Xor };
    enum class Signedness { Signed, Unsigned };
    enum class BitOp { Test, Set, Clear, Invert };

    // A decoded general operand. Side effects of decoding (autoincrement /
    // autodecrement, indirect address reads) have already happened.
    struct Operand {
        enum class Kind { Register, Memory, Immediate, Invalid };
        Kind     kind    = Kind::Invalid;
        uint8_t  reg     = 0; // Register
        uint32_t address = 0; // Memory: effective address
        uint32_t value   = 0; // Immediate
        int32_t  bit_offset = 0; // bit addressing (decode_bit_operand): bits from `address`
    };

    struct OperandPair {
        Operand source;
        Operand destination;
    };

    // --- Instruction stream (advance PC) ------------------------------------
    uint8_t  fetch_byte();
    uint16_t fetch_word();
    uint32_t fetch_long();
    uint32_t fetch_displacement(int bytes); // sign-extended

    // --- Data memory ----------------------------------------------------------
    uint32_t read_data(uint32_t address, OperandSize size);
    void     write_data(uint32_t address, OperandSize size, uint32_t value);
    uint32_t read_data_long(uint32_t address) { return read_data(address, OperandSize::Word); }
    void     write_data_long(uint32_t address, uint32_t value) { write_data(address, OperandSize::Word, value); }
    void     push_long(uint32_t value);
    uint32_t pop_long();
    // Logs (capped) a stack access that is misaligned or falls outside work
    // RAM - a stack overflow (push) or underflow (pop). Execution continues:
    // the V60 itself has no stack limits.
    void     check_stack_access(const char* operation, uint32_t address);

    // --- Operand decoding ---------------------------------------------------
    Operand     decode_operand(bool mode_m, OperandSize size);
    Operand     decode_group7(uint8_t mode, OperandSize size);
    Operand     decode_indexed(uint8_t index_reg, OperandSize size);
    OperandPair decode_format12(OperandSize source_size, OperandSize destination_size);
    uint32_t    read_operand(const Operand& operand, OperandSize size);
    bool        write_operand(const Operand& operand, OperandSize size, uint32_t value);
    std::optional<uint32_t> operand_address(const Operand& operand);
    bool        require_valid(const OperandPair& operands) const;

    // --- Opcode handlers ----------------------------------------------------
    void op_halt();
    void op_nop();
    void op_branch(Condition condition, bool long_displacement);
    void op_jmp(bool mode_m);
    void op_mov(OperandSize size);
    void op_add_sub(bool subtract, OperandSize size);
    void op_add_sub_carry(bool subtract, OperandSize size);
    void op_cmp(OperandSize size);
    void op_jsr(bool mode_m);
    void op_exchange(OperandSize size);
    void op_neg(OperandSize size);
    void op_setf();
    void op_bit_field();
    void op_float(bool convert_group);
    // Bit addressing (bit-field instructions): a general operand decoded as
    // an address plus a signed bit offset (see decode_operand). Registers
    // and immediates have no bit address: they halt.
    Operand decode_bit_operand(bool mode_m);
    // Length byte of a bit-field instruction: bit 7 set = register Rn
    // (bits 4-0) holds the length, else the byte itself. 1-32, else halts.
    bool fetch_field_length(uint32_t& length);
    uint32_t read_field(const Operand& at, uint32_t length);
    void write_field(const Operand& at, uint32_t length, uint32_t field);
    void op_dbcc(bool negated_group);
    void op_move_extend(OperandSize source_size, OperandSize destination_size, bool sign_extend_value);
    void op_move_truncate(OperandSize source_size, OperandSize destination_size);
    void op_ldpr();
    void op_stpr();
    // Privileged register `number` (see above); false for a reserved number.
    bool read_privileged(uint32_t number, uint32_t& value);
    bool write_privileged(uint32_t number, uint32_t value);
    void op_inc_dec(bool decrement, OperandSize size, bool mode_m);
    uint32_t add_sub_flags(uint32_t dst, uint32_t src, bool carry_in, bool subtract, OperandSize size);
    void op_logic(LogicOp op, OperandSize size);
    void op_not(OperandSize size);
    void op_shift(ShiftKind kind, OperandSize size);
    void op_rotate(bool through_carry, OperandSize size);
    void op_rem(Signedness signedness, OperandSize size);
    void op_mul(Signedness signedness, OperandSize size);
    void op_div(Signedness signedness, OperandSize size);
    void op_mul_extended(Signedness signedness);
    void op_div_extended(Signedness signedness);
    void op_bit(BitOp op);
    void op_test(OperandSize size, bool mode_m);
    void op_updpsw(uint32_t field_limit);
    void op_movea(OperandSize size);
    void op_in(OperandSize size);
    void op_out(OperandSize size);
    void op_string(OperandSize size);
    void op_move_string_up(OperandSize size, bool fill, bool stop_at_r26);
    void op_search_string_up(OperandSize size, bool search_equal);
    void op_move_string_down(OperandSize size, bool fill);
    void op_push(bool mode_m);
    void op_pop(bool mode_m);
    void op_pushm(bool mode_m);
    void op_popm(bool mode_m);
    void op_call();
    void op_ret(bool mode_m);
    void op_bsr();
    void op_rsr();
    void op_retis(bool mode_m);

    // --- Interrupts -----------------------------------------------------------
    [[nodiscard]] bool can_accept_interrupt() const;
    void accept_interrupt();

    // --- PSW ------------------------------------------------------------------
    // Writes the PSW. If PSW.IS changes, or PSW.EL changes while IS = 0, the
    // current SP is saved to the old stack's register and the new one loaded.
    void write_psw(uint32_t value);
    uint32_t& stack_pointer_slot();
    void set_flag(uint32_t mask, bool state);
    [[nodiscard]] bool flag(uint32_t mask) const { return (m_psw & mask) != 0; }
    void set_zs_flags(uint32_t result, OperandSize size);
    void set_zs_flags_long(uint32_t result) { set_zs_flags(result, OperandSize::Word); }
    [[nodiscard]] bool condition_holds(Condition condition) const;

    // Stops execution and logs why, including the faulting instruction address.
    void halt(HaltReason reason, uint8_t detail);

    Bus& m_bus;

    std::array<uint32_t, k_register_count> m_reg{};
    uint32_t m_pc  = k_reset_pc;
    uint32_t m_psw = k_reset_psw;

    // Privileged registers.
    uint32_t m_isp = 0;
    std::array<uint32_t, 4> m_level_sp{};
    uint32_t m_sbr = 0;
    bool m_bit_addressing = false; // decode_operand in bit-addressing form
    static constexpr uint32_t k_privileged_count = 29; // numbers 0-28
    std::array<uint32_t, k_privileged_count> m_privileged_other{}; // TR, SYCW, ... (no behaviour)

    // Address of the opcode byte of the instruction currently executing.
    uint32_t m_instruction_pc = k_reset_pc;
    uint8_t  m_opcode = 0;

    // One bit per interrupt vector number awaiting acknowledgement.
    std::bitset<256> m_pending_irqs;

    HaltReason m_halt_reason = HaltReason::None;
    uint64_t   m_instruction_count = 0;
    uint64_t   m_interrupt_count = 0;
    uint64_t   m_trace_remaining = 0;

    // Stack warnings logged so far (capped to avoid log floods).
    uint32_t   m_stack_warning_count = 0;
};

} // namespace model1
