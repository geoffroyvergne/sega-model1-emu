#pragma once

#include <cstdint>
#include <iomanip>
#include <ostream>

// Compile-time trace switches. Override from CMake, e.g. -DMODEL1_TRACE_IRQ=OFF.
#ifndef MODEL1_TRACE_IRQ
#define MODEL1_TRACE_IRQ 1
#endif
#ifndef MODEL1_TRACE_TGP
#define MODEL1_TRACE_TGP 0
#endif
#ifndef MODEL1_TRACE_INPUT
#define MODEL1_TRACE_INPUT 1
#endif

namespace model1 {

// Prints a confirmation to stdout when a coin or start input is pressed.
inline constexpr bool k_trace_input = MODEL1_TRACE_INPUT != 0;

// Logs every TGP function executed (very verbose once games run).
inline constexpr bool k_trace_tgp = MODEL1_TRACE_TGP != 0;

// Logs every interrupt request and acknowledgement (two lines per frame once
// a game enables interrupts).
inline constexpr bool k_trace_irq = MODEL1_TRACE_IRQ != 0;

// Stream manipulator that prints a value as zero-padded uppercase hex,
// e.g. `std::cerr << Hex{0xC0, 2}` prints "0xC0".
struct Hex {
    uint32_t value;
    int digits = 8;
};

inline std::ostream& operator<<(std::ostream& os, Hex hex)
{
    const auto flags = os.flags();
    const auto fill = os.fill('0');
    os << "0x" << std::hex << std::uppercase << std::setw(hex.digits) << hex.value;
    os.flags(flags);
    os.fill(fill);
    return os;
}

} // namespace model1
