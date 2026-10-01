#include "core/motherboard.hpp"

#include <array>
#include <iomanip>
#include <iostream>

namespace model1 {

Motherboard::Motherboard()
    : m_bus(std::make_unique<Bus>())
    , m_tgp(std::make_unique<Tgp>())
    , m_inputs(std::make_unique<InputManager>())
    , m_cpu(std::make_unique<V60>(*m_bus))
{
    // TGP host ports. Each register block is mirrored across its 128 KB
    // window, so only the low address bits select the register half.
    Tgp& tgp = *m_tgp;
    m_bus->map_io("TGP shared RAM address", k_tgp_ram_address_port, k_tgp_port_span, 0x1,
        [&tgp](uint32_t offset) { return tgp.read_ram_address(offset); },
        [&tgp](uint32_t offset, uint16_t value) { tgp.write_ram_address(offset, value); });
    m_bus->map_io("TGP shared RAM data", k_tgp_ram_data_port, k_tgp_port_span, 0x3,
        [&tgp](uint32_t offset) { return tgp.read_ram_data(offset); },
        [&tgp](uint32_t offset, uint16_t value) { tgp.write_ram_data(offset, value); });
    m_bus->map_io("TGP FIFO", k_tgp_fifo_port, k_tgp_port_span, 0x3,
        [&tgp](uint32_t offset) { return tgp.read_fifo(offset); },
        [&tgp](uint32_t offset, uint16_t value) { tgp.write_fifo(offset, value); });
    InputManager& inputs = *m_inputs;
    m_bus->map_io("I/O board", k_io_board_base, InputManager::k_io_block_size,
        InputManager::k_io_block_size - 1,
        [&inputs](uint32_t offset) { return inputs.read_io(offset); },
        [&inputs](uint32_t offset, uint16_t value) { inputs.write_io(offset, value); });
    m_bus->map_io("TGP FIFO status", k_tgp_status_port, k_tgp_port_span, 0x3,
        [&tgp](uint32_t offset) { return tgp.read_status(offset); },
        nullptr);

    std::cerr << "[Motherboard] Created\n";
}

Motherboard::~Motherboard()
{
    std::cerr << "[Motherboard] Destroyed after " << m_frame_count << " frames\n";
}

void Motherboard::reset()
{
    m_frame_count = 0;
    m_cycle_balance = 0;
    m_bus->reset();
    m_tgp->reset();
    m_inputs->reset();
    m_cpu->reset();
    std::cerr << "[Motherboard] Reset\n";
}

void Motherboard::run_frame()
{
    // Active display: run the main CPU for one frame's worth of cycles.
    // (The TGP and sound CPU will be interleaved here later.)
    m_cycle_balance += k_main_cycles_per_frame;
    while (m_cycle_balance > 0) {
        m_cycle_balance -= m_cpu->execute_cycle();
    }

    // End of frame: the board's interrupt controller raises VBlank. The CPU
    // takes it before its next instruction if PSW.IE allows.
    m_cpu->request_interrupt(k_vblank_irq_level);
    ++m_frame_count;
}

bool Motherboard::run_tgp_self_test()
{
    // Host-side helpers: 32-bit FIFO transfers over the bus, as the V60 does.
    auto send_word = [this](uint32_t word) { m_bus->write_long(k_tgp_fifo_port, word); };
    auto send_float = [&send_word](float value) { send_word(Tgp::float_to_word(value)); };
    auto send_function = [&send_word](uint32_t id) { send_word(Tgp::encode_function(id)); };
    auto receive_float = [this]() { return Tgp::word_to_float(m_bus->read_long(k_tgp_fifo_port)); };

    bool all_passed = true;
    auto check = [&all_passed](const char* label, const std::array<float, 3>& got,
                               const std::array<float, 3>& expected, std::size_t count) {
        bool passed = true;
        std::cout << "[TGP self-test] " << label << " = (";
        for (std::size_t i = 0; i < count; ++i) {
            // Bit-exact comparison: these inputs have exact float results.
            passed = passed && Tgp::float_to_word(got[i]) == Tgp::float_to_word(expected[i]);
            std::cout << (i ? ", " : "") << std::setprecision(9) << got[i];
        }
        std::cout << ") " << (passed ? "PASS" : "FAIL") << '\n';
        all_passed = all_passed && passed;
    };

    m_tgp->reset();

    // 1. Load a translation matrix T = translate(10, 20, 30).
    const std::array<float, Tgp::k_matrix_size> translation = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f,
        10.0f, 20.0f, 30.0f,
    };
    send_function(Tgp::k_fn_matrix_write);
    for (float value : translation) {
        send_float(value);
    }

    // 2. Transform vertex [1, 2, 3] by T: expect [11, 22, 33].
    send_function(Tgp::k_fn_transform_point);
    send_float(1.0f);
    send_float(2.0f);
    send_float(3.0f);
    const std::array<float, 3> moved = {receive_float(), receive_float(), receive_float()};
    check("transform [1, 2, 3] by translate(10, 20, 30)", moved, {11.0f, 22.0f, 33.0f}, 3);

    // 3. Multiply in a 90-degree rotation about Z (x -> y, y -> -x), applied
    //    before T: [1, 2, 3] -> [-2, 1, 3] -> [8, 21, 33].
    const std::array<float, Tgp::k_matrix_size> rotate_z90 = {
         0.0f, 1.0f, 0.0f,
        -1.0f, 0.0f, 0.0f,
         0.0f, 0.0f, 1.0f,
         0.0f, 0.0f, 0.0f,
    };
    send_function(Tgp::k_fn_matrix_mul);
    for (float value : rotate_z90) {
        send_float(value);
    }
    send_function(Tgp::k_fn_transform_point);
    send_float(1.0f);
    send_float(2.0f);
    send_float(3.0f);
    const std::array<float, 3> rotated = {receive_float(), receive_float(), receive_float()};
    check("transform [1, 2, 3] by rotZ(90) * translate", rotated, {8.0f, 21.0f, 33.0f}, 3);

    // 4. Debug projection of [11, 22, 33] with focal length 330:
    //    x = 248 + 330 * 11 / 33 = 358, y = 192 - 330 * 22 / 33 = -28.
    send_function(Tgp::k_fn_debug_project);
    send_float(11.0f);
    send_float(22.0f);
    send_float(33.0f);
    send_float(330.0f);
    const std::array<float, 3> projected = {receive_float(), receive_float(), 0.0f};
    check("project [11, 22, 33], focal 330 -> screen", projected, {358.0f, -28.0f, 0.0f}, 2);

    if (m_tgp->output_count() != 0 || m_tgp->is_desynchronized()) {
        std::cout << "[TGP self-test] FAIL: unexpected TGP state after test\n";
        all_passed = false;
    }
    std::cout << "[TGP self-test] " << (all_passed ? "all checks passed" : "FAILED") << '\n';

    m_tgp->reset();
    return all_passed;
}

} // namespace model1
