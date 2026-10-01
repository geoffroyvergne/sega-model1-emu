#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace model1 {

// Fujitsu MB86233 "TGP" geometry coprocessor - high-level emulation (HLE).
//
// On real hardware the TGP is a programmable DSP running a per-game firmware
// ROM; the geometry "commands" are routines in that firmware. This class does
// not run the firmware. Instead it reimplements the firmware's functions in
// C++, following the Virtua Fighter function table documented by MAME's
// former HLE TGP core (MAME 0.190, ftab_vf). A cycle-accurate MB86233 core
// can replace this later behind the same host interface.
//
// Host (V60) interface - real Model 1 ports, 16-bit data bus:
//   0xD00000  shared RAM address  (R/W, 16-bit; bit 15 = auto-increment)
//   0xD20000  shared RAM data     (+0 low half, +2 high half of a 32-bit word)
//   0xD80000  FIFO                write: +0 latches low half, +2 pushes the
//                                        32-bit word into the input FIFO
//                                 read:  +0 pops the output FIFO, returns the
//                                        low half; +2 returns the high half
//   0xDC0000  FIFO status         (read-only)
//
// Command protocol (Virtua Fighter firmware):
//   1. Write the function word: function ID in bits 31-23 (encode_function()).
//   2. Write the function's parameters, one 32-bit IEEE-754 float per word.
//   3. Once the last parameter arrives the function executes; results are
//      read back from the output FIFO, one 32-bit word each.
//
// Matrices are 4x3, stored row-major as 12 floats: rows 0-2 hold the
// rotation/scale basis, row 3 the translation. Points are row vectors:
//   [x' y' z'] = [x y z 1] * M
class Tgp {
public:
    // Function IDs from the Virtua Fighter TGP firmware table.
    enum Function : uint32_t {
        k_fn_matrix_push      = 0x05, // push current matrix
        k_fn_matrix_pop       = 0x06, // pop into current matrix
        k_fn_matrix_write     = 0x07, // 12 floats -> current matrix
        k_fn_clear_stack      = 0x08, // empty the matrix stack
        k_fn_matrix_mul       = 0x09, // current = param * current (12 floats)
        k_fn_matrix_ident     = 0x10, // current = identity
        k_fn_matrix_read      = 0x11, // current matrix -> 12 floats out
        k_fn_matrix_trans     = 0x12, // pre-translate current by (x, y, z)
        k_fn_transform_point  = 0x1B, // (x, y, z) -> 3 floats out

        // Emulator-only debug function, NOT part of any Model 1 firmware
        // (projection is done by the polygon renderer on real hardware).
        // ID chosen beyond the firmware table so it cannot collide.
        // (x, y, z, focal) -> (screen_x, screen_y)
        k_fn_debug_project    = 0x1FF,
    };

    static constexpr std::size_t k_fifo_depth = 16;          // real FIFO depth
    static constexpr std::size_t k_matrix_stack_depth = 32;
    static constexpr std::size_t k_shared_ram_words = 0x2000; // 32 KB
    static constexpr std::size_t k_max_parameters = 12;
    static constexpr std::size_t k_matrix_size = 12;

    // Screen centre used by the debug projection (496x384 display).
    static constexpr float k_screen_center_x = 248.0f;
    static constexpr float k_screen_center_y = 192.0f;

    using Matrix = std::array<float, k_matrix_size>;

    Tgp();

    Tgp(const Tgp&) = delete;
    Tgp& operator=(const Tgp&) = delete;
    Tgp(Tgp&&) = delete;
    Tgp& operator=(Tgp&&) = delete;

    void reset();

    // --- Host port handlers (registered on the Bus by the Motherboard) ------
    uint16_t read_fifo(uint32_t offset);
    void     write_fifo(uint32_t offset, uint16_t value);
    uint16_t read_status(uint32_t offset);
    uint16_t read_ram_address(uint32_t offset);
    void     write_ram_address(uint32_t offset, uint16_t value);
    uint16_t read_ram_data(uint32_t offset);
    void     write_ram_data(uint32_t offset, uint16_t value);

    // --- Word-level FIFO access ---------------------------------------------
    // Feeds one 32-bit word to the TGP (function word or parameter).
    void push_input(uint32_t word);
    // Pops one result word; logs and returns 0 if the output FIFO is empty.
    uint32_t pop_output();
    [[nodiscard]] std::size_t output_count() const { return m_out_count; }

    // Runs a function whose parameters are already in m_params.
    void execute_command(uint32_t command_id);

    // --- Helpers ----------------------------------------------------------------
    static constexpr uint32_t encode_function(uint32_t id) { return id << 23; }
    static constexpr uint32_t decode_function(uint32_t word) { return word >> 23; }
    static uint32_t float_to_word(float value) { return std::bit_cast<uint32_t>(value); }
    static float word_to_float(uint32_t word) { return std::bit_cast<float>(word); }

    [[nodiscard]] const Matrix& current_matrix() const { return m_matrix; }
    [[nodiscard]] std::size_t stack_depth() const { return m_stack_depth; }
    [[nodiscard]] bool is_desynchronized() const { return m_desynchronized; }

private:
    struct FunctionInfo {
        const char* name;
        std::size_t parameter_count;
    };

    // Parameter count and name of an implemented function, if any.
    static std::optional<FunctionInfo> function_info(uint32_t id);

    void push_output(uint32_t word);
    void push_output_float(float value) { push_output(float_to_word(value)); }
    [[nodiscard]] float param(std::size_t index) const { return word_to_float(m_params[index]); }

    void fn_matrix_push();
    void fn_matrix_pop();
    void fn_matrix_write();
    void fn_matrix_mul();
    void fn_matrix_ident();
    void fn_matrix_read();
    void fn_matrix_trans();
    void fn_transform_point();
    void fn_debug_project();

    void log_unsupported(uint32_t command_id) const;

    static Matrix identity();

    // Geometry state (lives in the DSP's internal RAM on real hardware).
    Matrix m_matrix = identity();
    std::array<Matrix, k_matrix_stack_depth> m_stack{};
    std::size_t m_stack_depth = 0;

    // Command decoding.
    bool        m_have_function = false;
    uint32_t    m_function = 0;
    std::size_t m_params_needed = 0;
    std::size_t m_param_count = 0;
    std::array<uint32_t, k_max_parameters> m_params{};

    // After an unknown function the parameter count is unknown, so the word
    // stream can no longer be parsed. Further words are logged and dropped
    // until reset (the same recovery strategy MAME's HLE used).
    bool        m_desynchronized = false;
    std::size_t m_dropped_words = 0;

    // Output FIFO (ring buffer).
    std::array<uint32_t, k_fifo_depth> m_out{};
    std::size_t m_out_read = 0;
    std::size_t m_out_count = 0;

    // Host port latches: 32-bit words travel as two 16-bit halves.
    uint16_t m_fifo_write_low = 0;
    uint32_t m_fifo_read_word = 0;

    // Shared RAM, reachable by the V60 through the address/data ports.
    std::array<uint32_t, k_shared_ram_words> m_shared_ram{};
    uint16_t m_ram_address = 0;
    uint16_t m_ram_write_low = 0;
};

} // namespace model1
