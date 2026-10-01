#include "core/tgp.hpp"

#include "core/log.hpp"

#include <iostream>

namespace model1 {

namespace {

// Words logged after an unknown function before the log goes quiet.
constexpr std::size_t k_max_logged_dropped_words = 16;

} // namespace

Tgp::Tgp()
{
    reset();
}

Tgp::Matrix Tgp::identity()
{
    return {1.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 1.0f,
            0.0f, 0.0f, 0.0f};
}

void Tgp::reset()
{
    m_matrix = identity();
    m_stack_depth = 0;
    m_have_function = false;
    m_function = 0;
    m_params_needed = 0;
    m_param_count = 0;
    m_desynchronized = false;
    m_dropped_words = 0;
    m_out_read = 0;
    m_out_count = 0;
    m_fifo_write_low = 0;
    m_fifo_read_word = 0;
    m_shared_ram.fill(0);
    m_ram_address = 0;
    m_ram_write_low = 0;
}

std::optional<Tgp::FunctionInfo> Tgp::function_info(uint32_t id)
{
    switch (id) {
    case k_fn_matrix_push:     return FunctionInfo{"matrix_push", 0};
    case k_fn_matrix_pop:      return FunctionInfo{"matrix_pop", 0};
    case k_fn_matrix_write:    return FunctionInfo{"matrix_write", 12};
    case k_fn_clear_stack:     return FunctionInfo{"clear_stack", 0};
    case k_fn_matrix_mul:      return FunctionInfo{"matrix_mul", 12};
    case k_fn_matrix_ident:    return FunctionInfo{"matrix_ident", 0};
    case k_fn_matrix_read:     return FunctionInfo{"matrix_read", 0};
    case k_fn_matrix_trans:    return FunctionInfo{"matrix_trans", 3};
    case k_fn_transform_point: return FunctionInfo{"transform_point", 3};
    case k_fn_debug_project:   return FunctionInfo{"debug_project", 4};
    default:                   return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Host ports
// ---------------------------------------------------------------------------

void Tgp::write_fifo(uint32_t offset, uint16_t value)
{
    if (offset == 0) {
        m_fifo_write_low = value;
    } else {
        // High half completes the word.
        push_input(m_fifo_write_low | (static_cast<uint32_t>(value) << 16));
    }
}

uint16_t Tgp::read_fifo(uint32_t offset)
{
    if (offset == 0) {
        m_fifo_read_word = pop_output();
        return static_cast<uint16_t>(m_fifo_read_word);
    }
    return static_cast<uint16_t>(m_fifo_read_word >> 16);
}

uint16_t Tgp::read_status(uint32_t /*offset*/)
{
    // The HLE executes functions instantly, so the input FIFO is always
    // ready. MAME also returns all ones here.
    return 0xFFFF;
}

uint16_t Tgp::read_ram_address(uint32_t /*offset*/)
{
    return m_ram_address;
}

void Tgp::write_ram_address(uint32_t /*offset*/, uint16_t value)
{
    m_ram_address = value;
}

uint16_t Tgp::read_ram_data(uint32_t offset)
{
    const uint32_t word = m_shared_ram[m_ram_address & (k_shared_ram_words - 1)];
    if (offset == 0) {
        return static_cast<uint16_t>(word);
    }
    // Reading the high half completes the access; bit 15 = auto-increment.
    if ((m_ram_address & 0x8000) != 0) {
        ++m_ram_address;
    }
    return static_cast<uint16_t>(word >> 16);
}

void Tgp::write_ram_data(uint32_t offset, uint16_t value)
{
    if (offset == 0) {
        m_ram_write_low = value;
        return;
    }
    m_shared_ram[m_ram_address & (k_shared_ram_words - 1)] =
        m_ram_write_low | (static_cast<uint32_t>(value) << 16);
    if ((m_ram_address & 0x8000) != 0) {
        ++m_ram_address;
    }
}

// ---------------------------------------------------------------------------
// Command decoding
// ---------------------------------------------------------------------------

void Tgp::push_input(uint32_t word)
{
    if (m_desynchronized) {
        if (m_dropped_words < k_max_logged_dropped_words) {
            std::cerr << "[TGP]   dropped word " << Hex{word} << " (as float: "
                      << word_to_float(word) << ")\n";
        } else if (m_dropped_words == k_max_logged_dropped_words) {
            std::cerr << "[TGP]   further dropped words not logged\n";
        }
        ++m_dropped_words;
        return;
    }

    if (!m_have_function) {
        const uint32_t id = decode_function(word);
        const std::optional<FunctionInfo> info = function_info(id);
        if (!info) {
            std::cerr << "[TGP] CRITICAL: unsupported function " << Hex{id, 3} << " (word "
                      << Hex{word} << "). Its parameter count is unknown, so the TGP is now "
                      << "desynchronized; following words are logged and dropped until reset\n";
            m_desynchronized = true;
            return;
        }
        m_function = id;
        m_params_needed = info->parameter_count;
        m_param_count = 0;
        m_have_function = true;
    } else {
        m_params[m_param_count++] = word;
    }

    if (m_param_count == m_params_needed) {
        m_have_function = false;
        execute_command(m_function);
    }
}

void Tgp::push_output(uint32_t word)
{
    if (m_out_count == k_fifo_depth) {
        std::cerr << "[TGP] WARNING: output FIFO overflow, word " << Hex{word} << " dropped\n";
        return;
    }
    m_out[(m_out_read + m_out_count) % k_fifo_depth] = word;
    ++m_out_count;
}

uint32_t Tgp::pop_output()
{
    if (m_out_count == 0) {
        // Real hardware stalls the V60 until data arrives; with instant HLE
        // execution an empty read means the host asked for too many results.
        std::cerr << "[TGP] WARNING: output FIFO underflow, returning 0\n";
        return 0;
    }
    const uint32_t word = m_out[m_out_read];
    m_out_read = (m_out_read + 1) % k_fifo_depth;
    --m_out_count;
    return word;
}

void Tgp::execute_command(uint32_t command_id)
{
    if constexpr (k_trace_tgp) {
        const std::optional<FunctionInfo> info = function_info(command_id);
        std::cerr << "[TGP] " << (info ? info->name : "?") << " (" << Hex{command_id, 3} << ")\n";
    }

    switch (command_id) {
    case k_fn_matrix_push:     fn_matrix_push();     break;
    case k_fn_matrix_pop:      fn_matrix_pop();      break;
    case k_fn_matrix_write:    fn_matrix_write();    break;
    case k_fn_clear_stack:     m_stack_depth = 0;    break;
    case k_fn_matrix_mul:      fn_matrix_mul();      break;
    case k_fn_matrix_ident:    fn_matrix_ident();    break;
    case k_fn_matrix_read:     fn_matrix_read();     break;
    case k_fn_matrix_trans:    fn_matrix_trans();    break;
    case k_fn_transform_point: fn_transform_point(); break;
    case k_fn_debug_project:   fn_debug_project();   break;
    default:
        log_unsupported(command_id);
        break;
    }
}

void Tgp::log_unsupported(uint32_t command_id) const
{
    std::cerr << "[TGP] CRITICAL: unsupported function " << Hex{command_id, 3} << " with "
              << m_param_count << " parameter(s):";
    for (std::size_t i = 0; i < m_param_count; ++i) {
        std::cerr << ' ' << Hex{m_params[i]} << " (" << word_to_float(m_params[i]) << ')';
    }
    std::cerr << '\n';
}

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

void Tgp::fn_matrix_push()
{
    // A full stack silently ignores the push, as the firmware HLE did.
    if (m_stack_depth < k_matrix_stack_depth) {
        m_stack[m_stack_depth++] = m_matrix;
    } else {
        std::cerr << "[TGP] WARNING: matrix stack overflow, push ignored\n";
    }
}

void Tgp::fn_matrix_pop()
{
    if (m_stack_depth > 0) {
        m_matrix = m_stack[--m_stack_depth];
    } else {
        std::cerr << "[TGP] WARNING: matrix stack underflow, pop ignored\n";
    }
}

void Tgp::fn_matrix_write()
{
    for (std::size_t i = 0; i < k_matrix_size; ++i) {
        m_matrix[i] = param(i);
    }
}

void Tgp::fn_matrix_ident()
{
    m_matrix = identity();
}

void Tgp::fn_matrix_read()
{
    for (float value : m_matrix) {
        push_output_float(value);
    }
}

// current = P * current, where P is the 4x3 parameter matrix (affine, with
// an implicit [0 0 0 1] column). P's transform is applied first.
void Tgp::fn_matrix_mul()
{
    Matrix p{};
    for (std::size_t i = 0; i < k_matrix_size; ++i) {
        p[i] = param(i);
    }
    const Matrix& c = m_matrix;
    Matrix r{};
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t col = 0; col < 3; ++col) {
            r[row * 3 + col] = p[row * 3 + 0] * c[0 + col]
                             + p[row * 3 + 1] * c[3 + col]
                             + p[row * 3 + 2] * c[6 + col];
        }
    }
    // Translation row also picks up the current translation.
    r[9]  += c[9];
    r[10] += c[10];
    r[11] += c[11];
    m_matrix = r;
}

// Pre-translates the current matrix: a translation by (x, y, z) applied
// before the current transform.
void Tgp::fn_matrix_trans()
{
    const float x = param(0);
    const float y = param(1);
    const float z = param(2);
    Matrix& m = m_matrix;
    m[9]  += m[0] * x + m[3] * y + m[6] * z;
    m[10] += m[1] * x + m[4] * y + m[7] * z;
    m[11] += m[2] * x + m[5] * y + m[8] * z;
}

// [x' y' z'] = [x y z 1] * current
void Tgp::fn_transform_point()
{
    const float x = param(0);
    const float y = param(1);
    const float z = param(2);
    const Matrix& m = m_matrix;
    push_output_float(m[0] * x + m[3] * y + m[6] * z + m[9]);
    push_output_float(m[1] * x + m[4] * y + m[7] * z + m[10]);
    push_output_float(m[2] * x + m[5] * y + m[8] * z + m[11]);
}

// Emulator-only: perspective divide onto the 496x384 screen, +y up.
void Tgp::fn_debug_project()
{
    const float x = param(0);
    const float y = param(1);
    const float z = param(2);
    const float focal = param(3);
    if (!(z > 0.0f)) {
        std::cerr << "[TGP] WARNING: debug_project with z=" << z
                  << " (point not in front of the camera), returning (0, 0)\n";
        push_output_float(0.0f);
        push_output_float(0.0f);
        return;
    }
    push_output_float(k_screen_center_x + focal * x / z);
    push_output_float(k_screen_center_y - focal * y / z);
}

} // namespace model1
