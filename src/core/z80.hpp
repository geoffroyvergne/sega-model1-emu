#pragma once

#include <cstdint>

namespace model1 {

// Memory and I/O as seen by a Z80. Implemented by the board that owns the
// CPU (for Model 1: the I/O board), so the core stays independent of it.
class Z80Bus {
public:
    virtual ~Z80Bus() = default;
    virtual uint8_t read(uint16_t address) = 0;
    virtual void write(uint16_t address, uint8_t value) = 0;
    virtual uint8_t in(uint16_t port) = 0;
    virtual void out(uint16_t port, uint8_t value) = 0;
};

// Zilog Z80 - the Model 1 I/O board CPU (4 MHz).
//
// The documented instruction set: the base opcodes and the CB (bit / shift),
// ED (block, 16-bit arithmetic, I/O, interrupt mode) and DD / FD (IX / IY,
// including the DDCB / FDCB bit forms and the IXH / IXL / IYH / IYL byte
// registers) prefixes, with T-state counts from the Zilog user manual.
// Flags include the undocumented bits 5 and 3 (copies of result bits) for
// the common instructions. ED opcodes Zilog does not define behave as an
// 8-T-state NOP, as on the chip (logged once).
//
// Interrupts: maskable INT in modes 0 (the data bus is assumed to hold
// 0xFF, i.e. RST 38h), 1 (RST 38h) and 2 (vector table at I:bus value), and
// NMI (to 0x0066). HALT idles until an interrupt.
class Z80 {
public:
    static constexpr uint8_t k_flag_c = 0x01;
    static constexpr uint8_t k_flag_n = 0x02;
    static constexpr uint8_t k_flag_pv = 0x04;
    static constexpr uint8_t k_flag_x = 0x08; // undocumented copy of bit 3
    static constexpr uint8_t k_flag_h = 0x10;
    static constexpr uint8_t k_flag_y = 0x20; // undocumented copy of bit 5
    static constexpr uint8_t k_flag_z = 0x40;
    static constexpr uint8_t k_flag_s = 0x80;

    explicit Z80(Z80Bus& bus);

    Z80(const Z80&) = delete;
    Z80& operator=(const Z80&) = delete;

    // RESET: PC = 0, interrupts disabled, mode 0, I = R = 0.
    void reset();

    // Executes one instruction (or takes an interrupt, or idles 4 T-states
    // while halted) and returns the T-states it took.
    uint32_t step();

    // Maskable interrupt line (level) and the value the interrupting device
    // puts on the data bus (used in mode 2), and a non-maskable request.
    void set_int_line(bool asserted, uint8_t vector = 0xFF)
    {
        m_int_line = asserted;
        m_int_vector = vector;
    }
    void request_nmi() { m_nmi_pending = true; }

    [[nodiscard]] uint16_t pc() const { return m_pc; }
    void set_pc(uint16_t value) { m_pc = value; }
    [[nodiscard]] uint16_t sp() const { return m_sp; }
    void set_sp(uint16_t value) { m_sp = value; }
    [[nodiscard]] uint8_t a() const { return m_a; }
    void set_a(uint8_t value) { m_a = value; }
    [[nodiscard]] uint8_t f() const { return m_f; }
    void set_f(uint8_t value) { m_f = value; }
    [[nodiscard]] uint16_t bc() const { return static_cast<uint16_t>(m_b << 8 | m_c); }
    [[nodiscard]] uint16_t de() const { return static_cast<uint16_t>(m_d << 8 | m_e); }
    [[nodiscard]] uint16_t hl() const { return static_cast<uint16_t>(m_h << 8 | m_l); }
    void set_bc(uint16_t value) { m_b = static_cast<uint8_t>(value >> 8); m_c = static_cast<uint8_t>(value); }
    void set_de(uint16_t value) { m_d = static_cast<uint8_t>(value >> 8); m_e = static_cast<uint8_t>(value); }
    void set_hl(uint16_t value) { m_h = static_cast<uint8_t>(value >> 8); m_l = static_cast<uint8_t>(value); }
    [[nodiscard]] uint16_t ix() const { return m_ix; }
    [[nodiscard]] uint16_t iy() const { return m_iy; }
    void set_ix(uint16_t value) { m_ix = value; }
    void set_iy(uint16_t value) { m_iy = value; }
    [[nodiscard]] uint8_t i() const { return m_i; }
    [[nodiscard]] uint8_t r() const { return m_r; }
    [[nodiscard]] bool iff1() const { return m_iff1; }
    [[nodiscard]] int interrupt_mode() const { return m_im; }
    [[nodiscard]] bool is_halted() const { return m_halted; }
    [[nodiscard]] uint64_t cycle_count() const { return m_cycles; }
    [[nodiscard]] uint64_t instruction_count() const { return m_instructions; }

private:
    enum class Index { HL, IX, IY };

    uint8_t  fetch_opcode(); // M1 cycle: increments R
    uint8_t  fetch_byte();
    uint16_t fetch_word();
    uint8_t  read(uint16_t address) { return m_bus.read(address); }
    void     write(uint16_t address, uint8_t value) { m_bus.write(address, value); }
    uint16_t read_word(uint16_t address);
    void     write_word(uint16_t address, uint16_t value);
    void     push(uint16_t value);
    uint16_t pop();

    uint32_t execute_main(uint8_t opcode, Index index);
    uint32_t execute_cb();
    uint32_t execute_index_cb(Index index);
    uint32_t execute_ed();
    uint32_t take_interrupt();

    // Register access by the 3-bit field (0 B, 1 C, 2 D, 3 E, 4 H, 5 L, 7 A;
    // 6 is (HL) and is handled by the caller). With an index prefix, H / L
    // mean the high / low byte of IX or IY.
    uint8_t reg8(int r, Index index) const;
    void    set_reg8(int r, uint8_t value, Index index);
    // Register pairs: 0 BC, 1 DE, 2 HL/IX/IY, 3 SP (or AF for PUSH / POP).
    uint16_t rp(int p, Index index) const;
    void     set_rp(int p, uint16_t value, Index index);
    uint16_t index_reg(Index index) const;
    void     set_index_reg(Index index, uint16_t value);
    // Address of (HL), or (IX + d) / (IY + d) (fetches d).
    uint16_t memory_operand(Index index);
    [[nodiscard]] bool condition(int cc) const;

    // ALU
    void alu(int op, uint8_t value); // ADD ADC SUB SBC AND XOR OR CP
    uint8_t inc8(uint8_t value);
    uint8_t dec8(uint8_t value);
    uint8_t rotate_shift(int op, uint8_t value); // RLC RRC RL RR SLA SRA SLL SRL
    void bit_test(int bit, uint8_t value);
    uint16_t add16(uint16_t a, uint16_t b);
    uint16_t adc16(uint16_t a, uint16_t b);
    uint16_t sbc16(uint16_t a, uint16_t b);
    void daa();
    void set_szp(uint8_t value);

    // ED block instructions
    uint32_t block_load(bool increment, bool repeat);
    uint32_t block_compare(bool increment, bool repeat);
    uint32_t block_in(bool increment, bool repeat);
    uint32_t block_out(bool increment, bool repeat);

    Z80Bus& m_bus;

    uint8_t m_a = 0xFF, m_f = 0xFF;
    uint8_t m_b = 0, m_c = 0, m_d = 0, m_e = 0, m_h = 0, m_l = 0;
    uint8_t m_a2 = 0, m_f2 = 0, m_b2 = 0, m_c2 = 0, m_d2 = 0, m_e2 = 0, m_h2 = 0, m_l2 = 0;
    uint16_t m_ix = 0xFFFF, m_iy = 0xFFFF;
    uint16_t m_sp = 0xFFFF, m_pc = 0;
    uint8_t m_i = 0, m_r = 0;
    bool m_iff1 = false, m_iff2 = false;
    int  m_im = 0;
    bool m_halted = false;
    bool m_ei_delay = false; // no interrupt right after EI

    bool m_int_line = false;
    uint8_t m_int_vector = 0xFF;
    bool m_nmi_pending = false;

    uint64_t m_cycles = 0;
    uint64_t m_instructions = 0;
    bool m_logged_undefined_ed = false;
};

} // namespace model1
