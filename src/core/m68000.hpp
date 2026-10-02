#pragma once

#include <array>
#include <cstdint>

namespace model1 {

class SoundBus;

// Motorola 68000 - the Model 1 sound board CPU (10 MHz).
//
// This is a skeleton: the programmer's model, reset, exception processing
// for autovectored interrupts, and a small placeholder instruction set. Any
// other opcode halts the CPU with a logged message, like the V60 core.
//
// Programmer's model:
//   D0-D7  32-bit data registers
//   A0-A7  32-bit address registers; A7 is the active stack pointer: the
//          supervisor stack (SSP) when SR.S = 1, else the user stack (USP)
//   PC     program counter (24 bits on the bus)
//   SR     status register: T (bit 15), S (13), interrupt mask (10-8),
//          X N Z V C (4-0)
//
// Reset: SSP = long at 0x000000, PC = long at 0x000004, SR = 0x2700
// (supervisor, interrupts masked).
//
// Interrupts are level-sensitive (set_irq_level). Before each instruction a
// level above the SR mask (or level 7) is taken: SR is copied, the CPU
// enters supervisor mode with the mask raised to the level, the PC (long)
// then the old SR (word) are pushed on the supervisor stack, and execution
// continues at the autovector for that level, the long at (24 + level) * 4.
// The sound board's UART uses level 2 (vector address 0x68).
//
// Implemented instructions (cycle counts from the MC68000 user's manual):
//   NOP                     0x4E71                    4
//   STOP #imm               0x4E72 imm16              4   (privileged)
//   RTE                     0x4E73                    20  (privileged)
//   BRA.B / BRA.W           0x60dd / 0x6000 disp16    10
//   MOVEQ #imm8, Dn         0111 nnn0 iiiiiiii        4
//   SWAP Dn                 0100 1000 0100 0nnn       4
//   TST.B/W/L <ea>          0100 1010 ss ea           4 + ea
//   ADD/SUB <ea>,Dn | Dn,<ea>  1101/1001 rrr ooo ea   B/W 4 + ea, L 6/8 + ea; to memory 8/12 + ea
//   ADDA/SUBA, ADDX/SUBX    same group, opmode 011/111 and register modes  (see execute_add_sub)
//   CMP/CMPA/CMPM/EOR       1011 rrr ooo ea           see execute_compare_eor
//   OR/AND, DIVU/DIVS,      1000 / 1100 rrr ooo ea    see execute_or_and
//   MULU/MULS, SBCD/ABCD, EXG
//   ORI/ANDI/SUBI/ADDI/     0000 ooo0 ss ea + #imm    Dn 8 (B/W), 14-16 (L); mem 12/20 + ea
//   EORI/CMPI               (CMPI mem 8/12 + ea; to CCR/SR 20)
//   ADDQ/SUBQ #1-8, <ea>    0101 ddd s ss ea          Dn 4/8, An 8, mem 8/12 + ea
//   ASx/LSx/ROXx/ROx        1110 ccc d ss i tt rrr    Dn 6/8 + 2n
//                           1110 0tt d 11 ea          8 + ea (memory, word, 1 bit)
//   Bcc.B / Bcc.W           0110 cccc dd / 00 disp16  10 taken, 8 / 12 not taken
//   BSR.B / BSR.W           0110 0001 dd / 00 disp16  18
//   RTS                     0x4E75                    16
//   JSR <ea> / JMP <ea>     0100 1110 10 ea / 11 ea   16-22 / 8-14 by mode
//   LEA <ea>, An            0100 nnn1 11 ea           4-12 by mode
//   DBcc Dn, disp16         0101 cccc 1100 1nnn       12 cc true, 10 loop, 14 expired
//   MOVE.B/W/L <ea>, <ea>   00ss ...                  4 + source ea + dest ea
//   MOVEA.W/L <ea>, An      00ss nnn0 01..            4 + source ea
//   MOVE <ea>, SR           0100 0110 11 ea           12 + ea (privileged)
//   CLR.B/W/L <ea>          0100 0010 ss ea           4/6 (Dn), 8/12 + ea
//   BTST/BCHG/BCLR/BSET     0000 1000 tt ea + #bit    Dn: 10/12/14/12, mem: 8/12 + ea
//                           0000 rrr1 tt ea (bit=Dr)  Dn: 6/8/10/8,    mem: 4/8 + ea
// with all 12 addressing modes: Dn, An, (An), (An)+, -(An), d16(An),
// d8(An,Xn), abs.W, abs.L, d16(PC), d8(PC,Xn), #imm. Effective-address
// times come from the MC68000 user's manual. A word/long access at an odd
// address (address error) halts the CPU.
// Interrupt entry costs 44 cycles.
class M68000 {
public:
    static constexpr uint16_t k_sr_carry = 0x0001;
    static constexpr uint16_t k_sr_overflow = 0x0002;
    static constexpr uint16_t k_sr_zero = 0x0004;
    static constexpr uint16_t k_sr_negative = 0x0008;
    static constexpr uint16_t k_sr_extend = 0x0010;
    static constexpr uint16_t k_sr_mask_shift = 8;
    static constexpr uint16_t k_sr_mask = 0x0700;
    static constexpr uint16_t k_sr_supervisor = 0x2000;
    static constexpr uint16_t k_sr_trace = 0x8000;
    static constexpr uint16_t k_reset_sr = 0x2700;
    static constexpr uint32_t k_autovector_base = 24;
    static constexpr uint32_t k_idle_cycles = 4; // time that passes per step while stopped or halted

    explicit M68000(SoundBus& bus);

    M68000(const M68000&) = delete;
    M68000& operator=(const M68000&) = delete;

    // RESET: loads SSP and PC from the vector table.
    void reset();

    // Executes one step and returns the clock cycles it took: takes a
    // pending interrupt, or executes one instruction, or idles (STOP / halt).
    uint32_t execute_cycle();

    // Level-sensitive interrupt input, 0 (none) to 7 (non-maskable).
    void set_irq_level(int level) { m_irq_level = level & 7; }

    [[nodiscard]] bool is_halted() const { return m_halted; }
    [[nodiscard]] bool is_stopped() const { return m_stopped; }
    [[nodiscard]] uint64_t cycle_count() const { return m_cycles; }
    [[nodiscard]] uint64_t instruction_count() const { return m_instructions; }
    [[nodiscard]] uint64_t interrupt_count() const { return m_interrupts; }

    // Logs the next `count` instructions (address and words) to std::cerr.
    void set_trace(uint64_t count) { m_trace_remaining = count; }

    [[nodiscard]] uint32_t d(int n) const { return m_d[static_cast<std::size_t>(n & 7)]; }
    [[nodiscard]] uint32_t a(int n) const { return m_a[static_cast<std::size_t>(n & 7)]; }
    void set_d(int n, uint32_t value) { m_d[static_cast<std::size_t>(n & 7)] = value; }
    void set_a(int n, uint32_t value) { m_a[static_cast<std::size_t>(n & 7)] = value; }
    [[nodiscard]] uint32_t pc() const { return m_pc; }
    void set_pc(uint32_t value) { m_pc = value; }
    [[nodiscard]] uint16_t sr() const { return m_sr; }
    // Writes SR with hardware semantics (swaps A7 when S changes).
    void set_sr(uint16_t value);

private:
    uint16_t fetch_word();
    uint32_t fetch_long();
    void push_word(uint16_t value);
    void push_long(uint32_t value);
    uint16_t pop_word();
    uint32_t pop_long();

    enum class Size { Byte, Word, Long };
    static constexpr uint32_t size_bytes(Size s) { return s == Size::Byte ? 1u : (s == Size::Word ? 2u : 4u); }
    static constexpr uint32_t size_mask(Size s) { return s == Size::Byte ? 0xFFu : (s == Size::Word ? 0xFFFFu : 0xFFFFFFFFu); }

    // A decoded effective address. Decoding performs its side effects
    // ((An)+ / -(An)) and consumes extension words.
    struct Ea {
        enum class Kind { DataReg, AddrReg, Memory, Immediate, Invalid };
        Kind kind = Kind::Invalid;
        uint32_t reg = 0;
        uint32_t address = 0;
        uint32_t value = 0;
    };

    uint32_t execute_instruction(uint16_t opcode);
    uint32_t execute_bit_operation(uint16_t opcode, bool static_form);
    uint32_t execute_add_sub_quick(uint16_t opcode);
    uint32_t execute_shift_rotate(uint16_t opcode);
    uint32_t execute_immediate_alu(uint16_t opcode);
    uint32_t execute_add_sub(uint16_t opcode);
    uint32_t execute_compare_eor(uint16_t opcode);
    uint32_t execute_or_and(uint16_t opcode);
    uint32_t execute_movem(uint16_t opcode);
    uint32_t add_sub_flags(uint32_t dst, uint32_t src, bool subtract, Size size, bool update_x);
    Ea decode_ea(uint32_t mode, uint32_t reg, Size size);
    uint32_t indexed_address(uint32_t base);
    uint32_t read_ea(const Ea& ea, Size size);
    void write_ea(const Ea& ea, Size size, uint32_t value);
    bool check_alignment(const Ea& ea, Size size);
    void set_nz(uint32_t value, Size size);
    [[nodiscard]] bool condition_true(uint32_t condition) const;

    void take_interrupt(int level);
    [[nodiscard]] bool supervisor() const { return (m_sr & k_sr_supervisor) != 0; }
    void halt(const char* reason, uint16_t opcode, bool show_opcode = true);

    SoundBus& m_bus;
    std::array<uint32_t, 8> m_d{};
    std::array<uint32_t, 8> m_a{}; // a[7] = active stack pointer
    uint32_t m_other_sp = 0;       // the inactive one of USP / SSP
    uint32_t m_pc = 0;
    uint16_t m_sr = k_reset_sr;
    uint32_t m_instruction_pc = 0;

    int  m_irq_level = 0;
    bool m_stopped = false;
    bool m_halted = false;

    uint64_t m_cycles = 0;
    uint64_t m_instructions = 0;
    uint64_t m_interrupts = 0;
    uint64_t m_trace_remaining = 0;
};

} // namespace model1
