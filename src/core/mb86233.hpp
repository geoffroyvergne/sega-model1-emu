#pragma once

#include <array>
#include <cstdint>

namespace model1 {

// Memory and devices as seen by an MB86233. All accesses are 32-bit words
// at 16-bit word addresses. A read that cannot complete yet (an empty
// FIFO) calls Mb86233::stall(): the instruction is abandoned before any of
// its side effects and retried on the next step.
class Mb86233Bus {
public:
    virtual ~Mb86233Bus() = default;
    virtual uint32_t program_read(uint16_t address) = 0;
    virtual uint32_t data_read(uint16_t address) = 0;
    virtual void     data_write(uint16_t address, uint32_t value) = 0;
    virtual uint32_t io_read(uint16_t address) = 0;
    virtual void     io_write(uint16_t address, uint32_t value) = 0;
    virtual uint32_t rf_read(uint16_t address) = 0;      // register file (0x20-0x2F)
    virtual void     rf_write(uint16_t address, uint32_t value) = 0;
};

// Fujitsu MB86233 DSP - the Model 1 TGP ("geometrizer" / coprocessor),
// running at 40 MHz / 3 instructions per second (one per cycle; floating
// point ALU operations take one extra cycle).
//
// A port of MAME's mb86233 device (Olivier Galibert, from Elsemi's reverse
// engineering), which is the reference here: the chip's documentation is
// minimal. As in MAME:
//  - only floating-point mode is implemented (every Sega program enables it
//    first);
//  - interrupts are not implemented (the copro programs only use one to
//    blink LEDs);
//  - registers: A, B, D (accumulator), P (product), index units B0/B1
//    (base), X0/X1 (index), I0/I1 (increment), counters C0/C1, repeat
//    counter R, shift SFT, VSM (index mask), M (mode), a 4-entry PC stack.
//  - data RAM banks at 0x000-0x0FF and 0x200-0x3FF; some moves add 0x200
//    to one side's address, which reaches other devices (on Model 1, the
//    output FIFO at 0x400).
class Mb86233 {
public:
    // Status flags (as in MAME).
    static constexpr uint32_t k_zrd = 0x00000002; // D zero
    static constexpr uint32_t k_sgd = 0x00000008; // D negative
    static constexpr uint32_t k_zrc = 0x00000001;
    static constexpr uint32_t k_zc0 = 0x40000000; // C0 == 1
    static constexpr uint32_t k_zc1 = 0x80000000; // C1 == 1

    explicit Mb86233(Mb86233Bus& bus);

    Mb86233(const Mb86233&) = delete;
    Mb86233& operator=(const Mb86233&) = delete;

    void reset();

    // Executes one instruction (or retries a stalled one) and returns the
    // cycles it took.
    uint32_t step();

    // Logs the next `count` instructions (PC, opcode, A B D P) to std::cerr.
    void set_trace(uint64_t count) { m_trace_remaining = count; }

    // Called by the bus during a read that cannot complete.
    void stall() { m_stall = true; }

    [[nodiscard]] uint16_t pc() const { return m_pc; }
    [[nodiscard]] uint32_t a() const { return m_a; }
    [[nodiscard]] uint32_t b() const { return m_b; }
    [[nodiscard]] uint32_t d() const { return m_d; }
    [[nodiscard]] uint32_t p() const { return m_p; }
    [[nodiscard]] uint32_t st() const { return m_st; }
    [[nodiscard]] uint64_t instruction_count() const { return m_instructions; }
    [[nodiscard]] uint64_t stall_count() const { return m_stalls; }

private:
    static uint32_t set_exp(uint32_t value, uint32_t exp);
    static uint32_t set_mant(uint32_t value, uint32_t mant);
    static uint32_t get_exp(uint32_t value);
    static uint32_t get_mant(uint32_t value);

    void alu_pre(uint32_t alu);
    void alu_post_int(uint32_t alu);
    uint32_t alu_post_float(uint32_t alu); // returns the extra cycle (0 / 1)
    void alu_update_st() { m_st = (m_st & ~m_alu_stmask) | m_alu_stset; }
    void stset_int(uint32_t value);
    void stset_float(uint32_t value);

    uint16_t ea_pre_0(uint32_t r) const;
    void     ea_post_0(uint32_t r);
    uint16_t ea_pre_1(uint32_t r) const;
    void     ea_post_1(uint32_t r);
    void pcs_push();
    void pcs_pop();

    uint32_t read_reg(uint32_t r);
    void     write_reg(uint32_t r, uint32_t value);
    void     write_internal_1(uint32_t r, uint32_t value, bool bank);
    void     write_io_1(uint32_t r, uint32_t value);

    void log_once(const char* what, uint32_t value);

    Mb86233Bus& m_bus;

    uint32_t m_st = 0, m_a = 0, m_b = 0, m_d = 0, m_p = 0;
    uint32_t m_alu_stmask = 0, m_alu_stset = 0, m_alu_r1 = 0, m_alu_r2 = 0;
    uint16_t m_ppc = 0, m_pc = 0, m_sp = 0;
    uint16_t m_b0 = 0, m_b1 = 0, m_x0 = 0, m_x1 = 0, m_i0 = 0, m_i1 = 0;
    uint16_t m_vsmr = 7, m_mask = 0, m_m = 1;
    std::array<uint16_t, 4> m_pcs{};
    uint8_t m_r = 1, m_rpc = 1, m_c0 = 1, m_c1 = 1, m_sft = 0, m_vsm = 0;
    bool m_stall = false;

    uint64_t m_instructions = 0;
    uint64_t m_stalls = 0;
    uint32_t m_logged = 0; // count of distinct unimplemented-feature messages
    uint64_t m_trace_remaining = 0;
};

} // namespace model1
