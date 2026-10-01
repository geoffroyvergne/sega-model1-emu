#pragma once

// Shared fixture for V60 tests: an isolated CPU wired to a real Bus.
//
// Programs are placed in work RAM B (0x500000); the rest of the address space
// follows the Model 1 memory map. Opcode bytes and encodings are V60 ones
// (see the encoding notes in src/core/v60.hpp).

#include "core/bus.hpp"
#include "core/v60.hpp"

#include <cstdint>
#include <initializer_list>
#include <memory>

namespace model1_test {

using model1::Bus;
using model1::V60;

inline constexpr uint32_t k_program = 0x501000;   // program area, work RAM B
inline constexpr uint32_t k_data = 0x510000;      // scratch data area, work RAM B
inline constexpr uint32_t k_stack_top = 0x530000; // initial SP, work RAM B

inline constexpr uint32_t k_z = V60::k_psw_z;
inline constexpr uint32_t k_s = V60::k_psw_s;
inline constexpr uint32_t k_ov = V60::k_psw_ov;
inline constexpr uint32_t k_cy = V60::k_psw_cy;

// A fresh bus (RAM cleared, ROM erased) and a reset V60 with PC pointing at
// the program area.
struct CpuRig {
    std::unique_ptr<Bus> bus = std::make_unique<Bus>();
    std::unique_ptr<V60> cpu = std::make_unique<V60>(*bus);

    CpuRig()
    {
        cpu->reset();
        cpu->set_pc(k_program);
    }

    void load(std::initializer_list<uint8_t> bytes, uint32_t address = k_program)
    {
        for (uint8_t byte : bytes) {
            bus->write_byte(address++, byte);
        }
    }

    uint32_t step() { return cpu->execute_cycle(); }

    bool flag(uint32_t mask) const { return (cpu->psw() & mask) != 0; }

    uint32_t mem(uint32_t address) const { return bus->read_long(address); }
};

// A rig with its stack in work RAM B. PSW keeps IS = 1 (as after reset) so
// changing flags never swaps stack pointers.
struct StackRig : CpuRig {
    StackRig() { cpu->set_reg(V60::k_reg_sp, k_stack_top); }
    void set_flags(uint32_t flags) { cpu->set_psw(V60::k_psw_is | flags); }
    uint32_t sp() const { return cpu->reg(V60::k_reg_sp); }
};

// Byte `index` (0 = least significant) of a value, for little-endian
// immediates and addresses in instruction byte lists.
inline uint8_t byte_of(uint32_t value, int index)
{
    return static_cast<uint8_t>(value >> (8 * index));
}

} // namespace model1_test
