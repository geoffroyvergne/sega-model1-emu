// Z80 core tests (the Model 1 I/O board CPU): arithmetic flags, DAA, 16-bit
// arithmetic, stack and calls, loop timing, IX / IY forms (including DDCB),
// ED block instructions, interrupts (modes 1 and 2, NMI), HALT and I/O.

#include "test_framework.hpp"

#include "core/z80.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>

namespace {

using model1::Z80;
using model1::Z80Bus;

constexpr uint32_t k_c = Z80::k_flag_c;
constexpr uint32_t k_n = Z80::k_flag_n;
constexpr uint32_t k_pv = Z80::k_flag_pv;
constexpr uint32_t k_h = Z80::k_flag_h;
constexpr uint32_t k_z = Z80::k_flag_z;
constexpr uint32_t k_s = Z80::k_flag_s;
constexpr uint32_t k_main_flags = k_s | k_z | k_h | k_pv | k_n | k_c; // without the undocumented bits

struct RamBus final : Z80Bus {
    std::array<uint8_t, 0x10000> memory{};
    std::array<uint8_t, 0x100> ports{};
    uint16_t last_out_port = 0;
    uint8_t last_out_value = 0;
    uint8_t read(uint16_t address) override { return memory[address]; }
    void write(uint16_t address, uint8_t value) override { memory[address] = value; }
    uint8_t in(uint16_t port) override { return ports[port & 0xFF]; }
    void out(uint16_t port, uint8_t value) override
    {
        last_out_port = port;
        last_out_value = value;
    }
};

struct Rig {
    std::unique_ptr<RamBus> bus = std::make_unique<RamBus>();
    std::unique_ptr<Z80> cpu = std::make_unique<Z80>(*bus);
    explicit Rig(std::initializer_list<uint8_t> program)
    {
        uint16_t address = 0;
        for (uint8_t byte : program) {
            bus->memory[address++] = byte;
        }
        cpu->reset();
        cpu->set_sp(0xF000);
    }
    uint32_t step() { return cpu->step(); }
    uint32_t flags() const { return cpu->f() & k_main_flags; }
};

} // namespace

TEST_CASE(z80_add_and_compare_flags)
{
    Rig rig({0x3E, 0x7F,   // LD A, 0x7F
             0xC6, 0x01,   // ADD A, 1     -> 0x80: S, H, overflow
             0xC6, 0x80,   // ADD A, 0x80  -> 0x00: Z, overflow, carry
             0x3E, 0x10,   // LD A, 0x10
             0xFE, 0x10,   // CP 0x10      -> Z, N; A unchanged
             0xFE, 0x20,   // CP 0x20      -> borrow
             0xD6, 0x11}); // SUB 0x11     -> 0xFF: S, H, N, C
    CHECK_EQ(rig.step(), 7u);
    CHECK_EQ(rig.step(), 7u);
    CHECK_EQ(rig.cpu->a(), 0x80u);
    CHECK_EQ(rig.flags(), k_s | k_h | k_pv);
    rig.step();
    CHECK_EQ(rig.cpu->a(), 0x00u);
    CHECK_EQ(rig.flags(), k_z | k_pv | k_c);
    rig.step();
    rig.step();
    CHECK_EQ(rig.cpu->a(), 0x10u);
    CHECK_EQ(rig.flags(), k_z | k_n);
    rig.step();
    CHECK_EQ(rig.flags(), k_s | k_n | k_c);
    rig.step();
    CHECK_EQ(rig.cpu->a(), 0xFFu);
    CHECK_EQ(rig.flags(), k_s | k_h | k_n | k_c);
}

TEST_CASE(z80_daa_after_add_and_sub)
{
    Rig rig({0x3E, 0x15, 0xC6, 0x27, 0x27,   // 15 + 27, DAA -> 42 (BCD)
             0xD6, 0x15, 0x27,               // 42 - 15, DAA -> 27
             0x3E, 0x99, 0xC6, 0x01, 0x27}); // 99 + 01, DAA -> 00, carry
    for (int i = 0; i < 3; ++i) rig.step();
    CHECK_EQ(rig.cpu->a(), 0x42u);
    rig.step();
    rig.step();
    CHECK_EQ(rig.cpu->a(), 0x27u);
    CHECK(rig.flags() & k_n);
    for (int i = 0; i < 3; ++i) rig.step();
    CHECK_EQ(rig.cpu->a(), 0x00u);
    CHECK_EQ(rig.flags() & (k_z | k_c), k_z | k_c);
}

TEST_CASE(z80_inc_dec_and_logic)
{
    Rig rig({0x06, 0x7F, 0x04,   // LD B, 0x7F ; INC B -> 0x80 overflow
             0x0E, 0x01, 0x0D,   // LD C, 1 ; DEC C -> 0: Z, N
             0x3E, 0xF0, 0xE6, 0x3C, // AND 0x3C -> 0x30: H, parity even
             0xEE, 0x30});       // XOR 0x30 -> 0: Z, parity
    rig.cpu->set_f(k_c);
    rig.step();
    CHECK_EQ(rig.step(), 4u);
    CHECK_EQ(static_cast<uint32_t>(rig.cpu->bc() >> 8), 0x80u);
    CHECK_EQ(rig.flags(), k_s | k_h | k_pv | k_c); // carry kept by INC
    rig.step();
    rig.step();
    CHECK_EQ(static_cast<uint32_t>(rig.cpu->bc() & 0xFF), 0u);
    CHECK_EQ(rig.flags() & (k_z | k_n), k_z | k_n);
    rig.step();
    rig.step();
    CHECK_EQ(rig.cpu->a(), 0x30u);
    CHECK_EQ(rig.flags(), k_h | k_pv);
    rig.step();
    CHECK_EQ(rig.flags(), k_z | k_pv);
}

TEST_CASE(z80_16_bit_arithmetic)
{
    Rig rig({0x21, 0xFF, 0x7F,   // LD HL, 0x7FFF
             0x11, 0x01, 0x00,   // LD DE, 1
             0x19,               // ADD HL, DE -> 0x8000 (H from bit 11)
             0xED, 0x52,         // SBC HL, DE -> 0x7FFF: overflow, N
             0x01, 0x00, 0x80,   // LD BC, 0x8000
             0xED, 0x4A});       // ADC HL, BC -> 0xFFFF: S
    rig.step();
    rig.step();
    CHECK_EQ(rig.step(), 11u);
    CHECK_EQ(rig.cpu->hl(), 0x8000u);
    CHECK(rig.flags() & k_h);
    CHECK_EQ(rig.step(), 15u);
    CHECK_EQ(rig.cpu->hl(), 0x7FFFu);
    CHECK_EQ(rig.flags(), k_pv | k_n | k_h);
    rig.step();
    rig.step();
    CHECK_EQ(rig.cpu->hl(), 0xFFFFu);
    CHECK_EQ(rig.flags() & (k_s | k_z | k_c), k_s);
}

TEST_CASE(z80_calls_stack_and_exchanges)
{
    Rig rig({0xCD, 0x10, 0x00,   // CALL 0x0010
             0x76,               // HALT (return lands here)
             0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
             0xF5,               // 0x10: PUSH AF
             0x3E, 0x00,         // LD A, 0
             0xF1,               // POP AF
             0x08, 0xD9,         // EX AF, AF' ; EXX
             0xEB,               // EX DE, HL
             0xC9});             // RET
    rig.cpu->set_a(0x5A);
    rig.cpu->set_hl(0x1234);
    CHECK_EQ(rig.step(), 17u);
    CHECK_EQ(rig.cpu->pc(), 0x10u);
    CHECK_EQ(rig.bus->memory[0xEFFE], 0x03u); // return address 0x0003, little-endian
    CHECK_EQ(rig.step(), 11u);
    rig.step();
    CHECK_EQ(rig.step(), 10u);
    CHECK_EQ(rig.cpu->a(), 0x5Au);
    rig.step();
    rig.step();
    CHECK_EQ(rig.cpu->hl(), 0u); // alternate set (all zero)
    rig.step();
    CHECK_EQ(rig.cpu->de(), 0u);
    CHECK_EQ(rig.step(), 10u);
    CHECK_EQ(rig.cpu->pc(), 3u);
    CHECK_EQ(rig.cpu->sp(), 0xF000u);
    rig.step();
    CHECK(rig.cpu->is_halted());
}

TEST_CASE(z80_djnz_and_conditional_jumps_timing)
{
    Rig rig({0x06, 0x03,         // LD B, 3
             0x10, 0xFE,         // DJNZ $ (to itself)
             0xAF,               // XOR A -> Z
             0x28, 0x02,         // JR Z, +2
             0x00, 0x00,
             0xC2, 0x00, 0x00,   // JP NZ, 0 (not taken)
             0x20, 0x10});       // JR NZ, +16 (not taken)
    uint32_t cycles = rig.step();
    cycles += rig.step() + rig.step() + rig.step();
    CHECK_EQ(cycles, 7u + 13u + 13u + 8u);
    rig.step();
    CHECK_EQ(rig.step(), 12u);
    CHECK_EQ(rig.cpu->pc(), 9u);
    CHECK_EQ(rig.step(), 10u);
    CHECK_EQ(rig.step(), 7u);
    CHECK_EQ(rig.cpu->pc(), 14u);
}

TEST_CASE(z80_index_registers_and_displacements)
{
    Rig rig({0xDD, 0x21, 0x00, 0x40,   // LD IX, 0x4000
             0xDD, 0x36, 0x05, 0x42,   // LD (IX+5), 0x42
             0xDD, 0x7E, 0x05,         // LD A, (IX+5)
             0xDD, 0x34, 0xFF,         // INC (IX-1)
             0xFD, 0x21, 0x10, 0x40,   // LD IY, 0x4010
             0xFD, 0x66, 0x00,         // LD H, (IY+0): the real H
             0xDD, 0x26, 0x77,         // LD IXH, 0x77 (undocumented)
             0xDD, 0xCB, 0x02, 0xDE,   // SET 3, (IX+2)
             0xDD, 0xCB, 0x02, 0x5E}); // BIT 3, (IX+2)
    rig.bus->memory[0x3FFF] = 0x0F;
    rig.bus->memory[0x4010] = 0xAB;
    CHECK_EQ(rig.step(), 14u);
    CHECK_EQ(rig.step(), 19u);
    CHECK_EQ(rig.bus->memory[0x4005], 0x42u);
    CHECK_EQ(rig.step(), 19u);
    CHECK_EQ(rig.cpu->a(), 0x42u);
    CHECK_EQ(rig.step(), 23u);
    CHECK_EQ(rig.bus->memory[0x3FFF], 0x10u);
    rig.step();
    CHECK_EQ(rig.step(), 19u);
    CHECK_EQ(static_cast<uint32_t>(rig.cpu->hl() >> 8), 0xABu);
    CHECK_EQ(rig.cpu->iy(), 0x4010u);
    CHECK_EQ(rig.step(), 11u);
    CHECK_EQ(rig.cpu->ix(), 0x7700u);
    rig.cpu->set_ix(0x4000);
    CHECK_EQ(rig.step(), 23u);
    CHECK_EQ(rig.bus->memory[0x4002], 0x08u);
    CHECK_EQ(rig.step(), 20u);
    CHECK_EQ(rig.flags() & (k_z), 0u);
}

TEST_CASE(z80_cb_rotates_and_bits)
{
    Rig rig({0xCB, 0x00,         // RLC B
             0xCB, 0x3F,         // SRL A
             0xCB, 0x46,         // BIT 0, (HL)
             0xCB, 0xFE});       // SET 7, (HL)
    rig.cpu->set_bc(0x8100);
    rig.cpu->set_a(0x01);
    rig.cpu->set_hl(0x5000);
    rig.bus->memory[0x5000] = 0x02;
    CHECK_EQ(rig.step(), 8u);
    CHECK_EQ(static_cast<uint32_t>(rig.cpu->bc() >> 8), 0x03u);
    CHECK(rig.flags() & k_c);
    rig.step();
    CHECK_EQ(rig.cpu->a(), 0u);
    CHECK_EQ(rig.flags() & (k_z | k_c), k_z | k_c);
    CHECK_EQ(rig.step(), 12u);
    CHECK(rig.flags() & k_z);
    CHECK_EQ(rig.step(), 15u);
    CHECK_EQ(rig.bus->memory[0x5000], 0x82u);
}

TEST_CASE(z80_block_instructions)
{
    Rig rig({0x21, 0x00, 0x50,   // LD HL, 0x5000
             0x11, 0x00, 0x60,   // LD DE, 0x6000
             0x01, 0x04, 0x00,   // LD BC, 4
             0xED, 0xB0,         // LDIR
             0x21, 0x00, 0x50,   // LD HL, 0x5000
             0x01, 0x10, 0x00,   // LD BC, 16
             0x3E, 0x33,         // LD A, 0x33
             0xED, 0xB1});       // CPIR
    for (uint8_t i = 0; i < 4; ++i) {
        rig.bus->memory[0x5000 + i] = static_cast<uint8_t>(0x11 * (i + 1));
    }
    rig.step();
    rig.step();
    rig.step();
    uint32_t cycles = 0;
    while (rig.cpu->pc() == 9) {
        cycles += rig.step();
    }
    CHECK_EQ(cycles, 21u * 3 + 16u);
    CHECK_EQ(rig.bus->memory[0x6003], 0x44u);
    CHECK_EQ(rig.cpu->bc(), 0u);
    CHECK_EQ(rig.flags() & (k_pv), 0u);
    rig.step();
    rig.step();
    rig.step();
    while (rig.cpu->pc() == 19) {
        rig.step();
    }
    CHECK_EQ(rig.cpu->hl(), 0x5003u); // stopped after the match at 0x5002
    CHECK(rig.flags() & k_z);
    CHECK_EQ(rig.cpu->bc(), 13u);
}

TEST_CASE(z80_interrupts_mode1_mode2_nmi_and_halt)
{
    Rig rig({0xED, 0x56,         // IM 1
             0xFB,               // EI
             0x76});             // HALT
    rig.step();
    rig.step();
    rig.step();
    CHECK(rig.cpu->is_halted());
    CHECK_EQ(rig.step(), 4u); // idles while halted
    rig.cpu->set_int_line(true);
    CHECK_EQ(rig.step(), 13u);
    CHECK_EQ(rig.cpu->pc(), 0x38u);
    CHECK(!rig.cpu->is_halted());
    CHECK(!rig.cpu->iff1());
    CHECK_EQ(rig.bus->memory[0xEFFE], 0x04u); // returns after the HALT

    Rig im2({0x3E, 0x12, 0xED, 0x47, // LD A, 0x12 ; LD I, A
             0xED, 0x5E,             // IM 2
             0xFB, 0x00, 0x00});     // EI ; NOP ; NOP
    im2.bus->memory[0x1240] = 0x00;
    im2.bus->memory[0x1241] = 0x30; // vector 0x40 -> handler 0x3000
    for (int i = 0; i < 4; ++i) im2.step();
    im2.cpu->set_int_line(true, 0x40);
    CHECK_EQ(im2.step(), 4u); // the instruction after EI runs first
    CHECK_EQ(im2.step(), 19u);
    CHECK_EQ(im2.cpu->pc(), 0x3000u);

    Rig nmi({0x00});
    nmi.cpu->request_nmi();
    CHECK_EQ(nmi.step(), 11u);
    CHECK_EQ(nmi.cpu->pc(), 0x66u);
}

TEST_CASE(z80_io_and_neg_and_r_register)
{
    Rig rig({0x3E, 0x12, 0xD3, 0x34,   // LD A, 0x12 ; OUT (0x34), A
             0xDB, 0x56,               // IN A, (0x56)
             0x01, 0x78, 0x00,         // LD BC, 0x0078
             0xED, 0x50,               // IN D, (C)
             0xED, 0x44,               // NEG
             0xED, 0x5F});             // LD A, R
    rig.bus->ports[0x56] = 0x80;
    rig.bus->ports[0x78] = 0x00;
    rig.step();
    CHECK_EQ(rig.step(), 11u);
    CHECK_EQ(rig.bus->last_out_port, 0x1234u); // A on the high address byte
    CHECK_EQ(rig.bus->last_out_value, 0x12u);
    rig.step();
    CHECK_EQ(rig.cpu->a(), 0x80u);
    rig.step();
    CHECK_EQ(rig.step(), 12u);
    CHECK(rig.flags() & k_z);
    rig.step(); // NEG 0x80 -> 0x80, overflow
    CHECK_EQ(rig.cpu->a(), 0x80u);
    CHECK_EQ(rig.flags() & (k_pv | k_c | k_n), k_pv | k_c | k_n);
    rig.step();
    CHECK_EQ(rig.cpu->a(), 10u); // R counts opcode fetches: 6 earlier instructions (8 fetches) + ED 5F itself
}
