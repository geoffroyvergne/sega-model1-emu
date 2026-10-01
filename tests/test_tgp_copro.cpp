// TGP coprocessor tests: the MB86233 DSP core (moves, ALU, branches, repeat,
// stalls) and the copro board around it (FIFOs both ways, copro RAM with
// auto-increment, table units, data ROM window), using small hand-assembled
// DSP programs in the encodings of Virtua Racing's TGP firmware.

#include "test_framework.hpp"

#include "core/tgp_copro.hpp"

#include <bit>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace {

using model1::TgpCopro;

// --- Instruction encodings (see Mb86233) ------------------------------------
constexpr uint32_t k_reg_x1 = 0x03;
constexpr uint32_t k_reg_a = 0x10;
constexpr uint32_t k_reg_b = 0x13;
constexpr uint32_t k_reg_d = 0x19;
constexpr uint32_t k_reg_p = 0x1C;

uint32_t ldi(uint32_t reg, uint32_t value) { return ((0x40 | reg) << 24) | (value & 0xFFFFFF); }
// mov [X1] -> reg (r1 = 0x1A0: address = X1)
uint32_t mov_x1_to_reg(uint32_t reg) { return 0x1C000000 | (7 << 18) | (((3u << 6) | reg) << 9) | 0x1A0; }
// mov reg -> [X1]
uint32_t mov_reg_to_x1(uint32_t reg) { return 0x1C000000 | (7 << 18) | (reg << 9) | 0x1A0; }
// mov reg -> reg, with an ALU operation
uint32_t mov_reg_reg(uint32_t from, uint32_t to, uint32_t alu = 0)
{
    return 0x1C000000 | (alu << 21) | (7 << 18) | (((6u << 6) | to) << 9) | from;
}
// I/O space: mov io[address] -> reg, mov reg -> io[address] (address < 0x80)
uint32_t mov_io_to_reg(uint32_t address, uint32_t reg) { return 0x1C000000 | (7 << 18) | (((4u << 6) | reg) << 9) | address; }
uint32_t mov_reg_to_io(uint32_t reg, uint32_t address) { return 0x1C000000 | (7 << 18) | (((1u << 6) | reg) << 9) | address; }
uint32_t branch_always(uint32_t address) { return 0xBF600000 | address; }
uint32_t rep(uint32_t count) { return 0x3C000000 | (2 << 17) | count; }
constexpr uint32_t k_alu_fml = 0x08;  // P = A * B
constexpr uint32_t k_alu_fmsd = 0x09; // D = D + P, P = A * B
constexpr uint32_t k_alu_fadd = 0x06; // D = D + A

uint32_t f(float value) { return std::bit_cast<uint32_t>(value); }
float as_float(uint32_t bits) { return std::bit_cast<float>(bits); }

std::unique_ptr<TgpCopro> make_copro(std::initializer_list<uint32_t> program)
{
    auto copro = std::make_unique<TgpCopro>();
    std::vector<uint8_t> bytes(TgpCopro::k_program_words * 4, 0);
    std::size_t i = 0;
    for (uint32_t word : program) {
        for (int b = 0; b < 4; ++b) {
            bytes[i * 4 + static_cast<std::size_t>(b)] = static_cast<uint8_t>(word >> (8 * b));
        }
        ++i;
    }
    copro->load_program(bytes);
    copro->reset();
    return copro;
}

void push(TgpCopro& copro, uint32_t word)
{
    copro.write_fifo(0, static_cast<uint16_t>(word));
    copro.write_fifo(2, static_cast<uint16_t>(word >> 16));
}

uint32_t pop(TgpCopro& copro)
{
    const uint32_t low = copro.read_fifo(0);
    return low | (static_cast<uint32_t>(copro.read_fifo(2)) << 16);
}

// The DSP side of "multiply two floats": read A and B from the input FIFO,
// P = A * B, write P to the output FIFO, loop.
std::unique_ptr<TgpCopro> make_multiplier()
{
    return make_copro({
        ldi(k_reg_x1, 0x100),           // 0: X1 = input FIFO
        mov_x1_to_reg(k_reg_a),         // 1
        mov_x1_to_reg(k_reg_b),         // 2
        mov_reg_reg(k_reg_a, k_reg_a, k_alu_fml), // 3: P = A * B
        ldi(k_reg_x1, 0x400),           // 4: X1 = output FIFO
        mov_reg_to_x1(k_reg_p),         // 5
        branch_always(0),               // 6
    });
}

} // namespace

TEST_CASE(tgp_dsp_fifo_round_trip_and_stall)
{
    auto copro = make_multiplier();
    copro->run(600); // nothing sent yet: the DSP stalls on the empty FIFO
    CHECK_EQ(copro->dsp().pc(), 1u);
    CHECK(copro->dsp().stall_count() > 0);
    CHECK_EQ(copro->output_fifo_size(), 0u);

    push(*copro, f(2.0f));
    push(*copro, f(3.5f));
    copro->run(600);
    CHECK_EQ(copro->output_fifo_size(), 1u);
    CHECK_EQ(as_float(pop(*copro)), 7.0f);
}

TEST_CASE(tgp_v60_read_waits_for_the_dsp)
{
    // The V60 reads before the DSP has run: on hardware the V60 is halted
    // until the result arrives; here the DSP runs on the spot.
    auto copro = make_multiplier();
    push(*copro, f(-1.5f));
    push(*copro, f(4.0f));
    CHECK_EQ(as_float(pop(*copro)), -6.0f);

    // Reading with nothing to compute: the DSP waits for input forever.
    CHECK_EQ(pop(*copro), 0u);
    CHECK(model1_test::captured_log().find("deadlock") != std::string::npos);
}

TEST_CASE(tgp_dsp_multiply_accumulate_and_repeat)
{
    // D = 0; repeat 3 times: D += P, P = A * B (A = 2, B = 3): the first
    // accumulate adds the initial P (0), the next two add 6 each.
    auto copro = make_copro({
        ldi(k_reg_x1, 0x100),
        mov_x1_to_reg(k_reg_a),
        mov_x1_to_reg(k_reg_b),
        ldi(k_reg_d, 0),
        rep(3),
        mov_reg_reg(k_reg_a, k_reg_a, k_alu_fmsd),
        mov_reg_reg(k_reg_a, k_reg_a, k_alu_fadd), // D = D + A = 12 + 2
        ldi(k_reg_x1, 0x400),
        mov_reg_to_x1(k_reg_d),
        branch_always(9),
    });
    push(*copro, f(2.0f));
    push(*copro, f(3.0f));
    CHECK_EQ(as_float(pop(*copro)), 14.0f);
}

TEST_CASE(tgp_copro_ram_auto_increment_both_sides)
{
    auto copro = make_copro({
        ldi(k_reg_a, 0x10),
        mov_reg_to_io(k_reg_a, 0x00),   // RAM address register 0 = 0x10
        mov_io_to_reg(0x01, k_reg_b),   // B = RAM[0x10], address -> 0x11
        mov_io_to_reg(0x01, k_reg_d),   // D = RAM[0x11]
        ldi(k_reg_x1, 0x400),
        mov_reg_to_x1(k_reg_b),
        mov_reg_to_x1(k_reg_d),
        branch_always(7),
    });
    // The V60 fills RAM[0x10..0x11] with auto-increment (address bit 15).
    copro->write_ram_address(0, 0x8010);
    for (uint32_t value : {0x11112222u, 0x33334444u}) {
        copro->write_ram_data(0, static_cast<uint16_t>(value));
        copro->write_ram_data(2, static_cast<uint16_t>(value >> 16));
    }
    CHECK_EQ(copro->read_ram_address(0), 0x8012u);
    CHECK_EQ(copro->ram_word(0x10), 0x11112222u);
    CHECK_EQ(pop(*copro), 0x11112222u);
    CHECK_EQ(pop(*copro), 0x33334444u);
}

TEST_CASE(tgp_copro_sincos_table_and_data_rom_window)
{
    auto copro = make_copro({
        ldi(k_reg_a, 0x4000 + 0x100),   // angle in the second quarter
        mov_reg_to_io(k_reg_a, 0x20),
        mov_io_to_reg(0x20, k_reg_b),   // table[0x4000 - 0x100] (mirrored)
        mov_io_to_reg(0x21, k_reg_d),   // angle + 0x4000: third quarter, sign flipped
        ldi(k_reg_x1, 0x400),
        mov_reg_to_x1(k_reg_b),
        mov_reg_to_x1(k_reg_d),
        ldi(k_reg_a, 0x8000),
        mov_reg_to_io(k_reg_a, 0x2E),   // data ROM page: words 0x8000-0xFFFF
        ldi(k_reg_x1, 0x8005),          // I/O 0x8005: data ROM word 0x8005
        0x1C000000 | (7 << 18) | (((4u << 6) | k_reg_b) << 9) | 0x1A0, // mov io[X1] -> B
        ldi(k_reg_x1, 0x400),
        mov_reg_to_x1(k_reg_b),
        branch_always(13),
    });
    std::vector<uint8_t> tables(TgpCopro::k_table_words * 4, 0);
    auto put = [&tables](std::size_t index, uint32_t value) {
        for (int b = 0; b < 4; ++b) {
            tables[index * 4 + static_cast<std::size_t>(b)] = static_cast<uint8_t>(value >> (8 * b));
        }
    };
    put(0x4000 - 0x100, f(0.75f));
    put(0x0100, f(0.25f));
    copro->load_tables(tables);
    std::vector<uint8_t> data(TgpCopro::k_data_rom_words * 4, 0);
    data[0x8005 * 4] = 0x5A;
    copro->load_data_rom(data);
    CHECK(copro->is_active());
    CHECK_EQ(as_float(pop(*copro)), 0.75f);  // 0x4100: second quarter, mirrored index 0x3F00
    CHECK_EQ(as_float(pop(*copro)), -0.25f); // 0x8100: index 0x100, sign flipped
    CHECK_EQ(pop(*copro), 0x5Au);            // data ROM word 0x8005
}
