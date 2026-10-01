# Sega Model 1 Emulator

An emulator for the Sega Model 1 arcade board (Virtua Racing, Virtua Fighter, Star Wars Arcade), written in C++20 with SDL2.

> **Status: early development. It does not run games yet.** The machine skeleton is in place: a partial NEC V60 CPU with interrupts, the real Model 1 memory map, the TGP geometry coprocessor's command interface, player inputs, and a 60 FPS video pipeline. Most CPU instructions and the real graphics hardware are still to come.

## Contents

- [Purpose](#purpose)
- [Strategy](#strategy)
- [Features](#features)
- [Architecture](#architecture)
- [Building](#building)
- [Running](#running)
- [Hardware reference](#hardware-reference)
- [Known limitations](#known-limitations)
- [Roadmap](#roadmap)

## Purpose

The Sega Model 1 (1992) was Sega's first 3D arcade board. It combines an NEC V60 main CPU, a Fujitsu MB86233 DSP for geometry (the "TGP"), a polygon renderer, and a separate sound board.

This project aims to emulate that board faithfully, with code that is easy to read and audit. Readability and correctness come before speed: every hardware behaviour is implemented explicitly, and anything that is not real hardware is labelled as such in the code and in this document.

## Strategy

**Build in small, verifiable steps.** Each subsystem starts as a skeleton (state, interfaces, lifecycle), then gets its core behaviour, then a few real operations, then refinement. Every step ends with a clean build and checks of the new behaviour.

**Verify hardware facts before writing them.** Opcode encodings, flag rules, the interrupt sequence, the memory map, TGP function IDs, input bit layouts and colour formats were all checked against MAME's Model 1 and V60 source code (BSD-3-Clause), not written from memory. MAME is used as a reference for facts only; no MAME code is copied.

**High-level emulation where low-level isn't practical yet.** Two subsystems on the real board run their own firmware:

| Subsystem | Real hardware | This emulator |
|---|---|---|
| TGP | MB86233 DSP running a per-game firmware ROM | Reimplements the firmware's functions in C++, numbered like the Virtua Fighter firmware |
| I/O board | Z80 running its own ROM, copying inputs into shared memory | Provides the shared-memory layout directly |

Both follow the layouts MAME used before it switched to low-level emulation, and both sit behind the real hardware ports. A cycle-accurate version can later replace either one without changing the V60's view of the machine.

**Fail loudly, never guess.** Unimplemented opcodes, unsupported addressing modes, unmapped memory accesses, unknown TGP functions and unknown I/O offsets are all logged with the address and value involved. The CPU halts on anything it cannot execute instead of skipping it.

**Keep the emulated machine separate from the host.** Everything under `src/core` is plain C++ with no SDL. Window, rendering and keyboard handling live in host-side code that talks to the core through small interfaces.

## Features

### NEC V60 CPU (`src/core/v60.*`)
- 32 general-purpose registers, PC and PSW, plus the privileged registers needed for interrupts: the interrupt stack pointer, four per-level stack pointers, and the system base register.
- **24-bit address bus**, little-endian, starting at the real reset address `0xFFFFFFF0`.
- **Instructions:** NOP, HALT, BR (8- and 16-bit displacement), JMP, MOV.W, ADD.W, SUB.W and RETIS.
- **Operands:** register, immediate and short immediate, in both of the V60's two-operand encodings. JMP also supports register-indirect, PC-relative and absolute targets.
- **Flags:** Zero, Sign, Overflow and Carry are computed exactly as the hardware does.
- **Maskable interrupts:** pending requests are taken between instructions when PSW.IE is set. The CPU switches to the interrupt stack, saves PSW and PC, and jumps through the vector table. HALT waits for an interrupt; RETIS returns from one.

### Memory map and bus (`src/core/bus.*`)
- The real Model 1 layout: program and boot ROM, two work RAMs, display list, palette and colour RAM.
- ROM is read-only and reads `0xFF` until loaded, like an erased EPROM.
- **Device ports:** devices register 16-bit read/write handlers on address ranges, with mirroring. A 32-bit access to a port becomes two 16-bit accesses, low half first, as on the board's 16-bit data bus.
- Raw binary ROM images can be loaded at any ROM address.

### TGP geometry coprocessor (`src/core/tgp.*`)
- The real host interface: a command FIFO, a status port, and a shared-RAM port with auto-increment.
- Geometry functions: matrix write, read, multiply, identity, translate and push/pop on a 32-deep stack, plus transform point. These use the real 4×3 matrix layout and IEEE floats.
- An emulator-only perspective projection function for debugging.
- **Startup self-test:** drives the TGP through the bus exactly as game code would and checks the results bit-for-bit.

### Inputs (`src/core/input_manager.*`, `src/input/keyboard_input.*`)
- System, Player 1 and Player 2 ports as 16-bit active-low bitmasks, using Virtua Fighter's bit layout. Lamp and coin-meter output can be written and read back.
- **Keyboard mapping:** keys are matched by physical position, so the layout works on QWERTY and AZERTY.
  - Keys with the same function (W and ↑) can be held together without releasing each other.
  - Auto-repeat is ignored.
  - Every input is released when the window loses focus, so no key gets stuck.

### Video (`src/video/video_manager.*`)
- 496×384 window that scales with sharp pixels and keeps its aspect ratio when resized.
- **Frame pipeline:** emulator memory is converted to 32-bit pixels, copied into a streaming SDL texture, then presented.
- **Debug framebuffer:** a temporary, emulator-only region that the screen mirrors, using the real Model 1 colour format.
- **Test pattern:** shown while the framebuffer is empty. It has colour bars with a moving marker, grey and RGB ramps, and a border.

### Timing (`src/main.cpp`, `src/core/motherboard.*`)
- A 60 Hz frame loop paced by the high-resolution counter; it measured exactly 60.0 FPS.
- **Each frame:**
  - The V60 runs a 266,666-cycle budget. Overshoot is carried into the next frame so the long-run rate is exact.
  - The VBlank interrupt is raised.
  - The frame is drawn.

## Architecture

### Components

```
                         +-------------------------------+
  Host side (SDL)        |  main.cpp                     |
                         |  event loop, 60 Hz pacing     |
                         +---+-------------+---------+---+
                             |             |         |
                   SDL events|   run_frame |         | vram() view
                             v             v         v
  +--------------------+  +------------------------+  +----------------------+
  | KeyboardInput      |  | Motherboard            |  | VideoManager         |
  | src/input          |  | owns every component,  |  | src/video            |
  | scancode -> input  |  | wires them to the bus  |  | pixels -> texture    |
  +---------+----------+  +-----------+------------+  +----------------------+
            | set_input()             |
  ----------|-------------------------|------------------------------------------
  Emulated  v                         v
  machine   +-------------------------------------------------------------+
  (core)    |                            Bus                              |
            |  memory regions (ROM/RAM)  +  16-bit device port handlers   |
            +-----+---------------------+--------------------+------------+
                  ^                     |                    |
                  | fetch / read /      | 0xC00000           | 0xD00000-0xDDFFFF
                  | write               v                    v
            +-----+------+     +------------------+   +--------------+
            |    V60     |     |  InputManager    |   |     TGP      |
            |  main CPU  |     |  I/O board ports |   |  geometry    |
            +------------+     +------------------+   +--------------+
```

### Design rules

- **The bus is the only path between components.** The CPU reaches memory and devices only through `Bus`. Devices never call the CPU or each other. The `Motherboard` creates every component, registers device ports on the bus, and drives the frame.
- **The core has no host dependencies.** `src/core` does not include SDL. The keyboard layer calls `InputManager::set_input()`, and the video layer receives framebuffer memory as a read-only `std::span`.
- **Memory is fixed-size, like the hardware.** RAM and ROM are `std::array` buffers sized to the real chips. Large owners (`Bus`, CPU, TGP) are heap-allocated so they never live on the stack.
- **Byte order is explicit.** Multi-byte values are assembled byte by byte in little-endian order, so results don't depend on the host CPU.
- **Logging is built in.** All emulator diagnostics go to stderr with a component tag such as `[V60]`, `[Bus]` or `[TGP]`. Chatty traces can be switched off at compile time.

### Frame lifecycle

```
main loop, once per 1/60 s:
  1. poll SDL events  -> KeyboardInput -> InputManager port bits
  2. Motherboard::run_frame()
       a. V60 executes instructions until the frame's cycle budget is spent
          (each instruction is charged 8 cycles; interrupts are taken between instructions)
       b. VBlank interrupt requested (Model 1 interrupt level 1)
  3. VideoManager::update_framebuffer(bus.vram())  -> 32-bit pixel buffer
  4. VideoManager::present_frame()                 -> streaming texture -> window
  5. sleep until the next frame deadline (resync if far behind)
```

### Project layout

```
CMakeLists.txt              Build configuration and trace options
cmake/CompilerWarnings.cmake  Warning flags (-Wall -Wextra -Wpedantic -Wshadow -Wconversion, /W4)
src/main.cpp                SDL setup, event loop, frame pacing, startup self-test call
src/core/motherboard.*      Owns all components, maps device ports, runs one frame
src/core/bus.*              Memory map, device port dispatch, ROM loading
src/core/v60.*              NEC V60 CPU: decode, execute, interrupts
src/core/tgp.*              TGP geometry coprocessor: FIFO, shared RAM, geometry functions
src/core/input_manager.*    I/O board input ports and lamps
src/core/log.hpp            Hex formatting and compile-time trace switches
src/input/keyboard_input.*  SDL keyboard -> Model 1 inputs (host side)
src/video/video_manager.*   SDL window, streaming texture, colour decoding, test pattern
```

## Building

### Requirements

- A C++20 compiler: Clang 14+, GCC 11+ or MSVC 2022
- CMake 3.20 or newer
- SDL2 development files

### macOS

```sh
brew install cmake sdl2
cmake -S . -B build
cmake --build build
```

### Linux (Debian / Ubuntu)

```sh
sudo apt install build-essential cmake libsdl2-dev
cmake -S . -B build
cmake --build build
```

### Windows (Visual Studio + vcpkg)

```sh
vcpkg install sdl2:x64-windows
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
```

The build copies `SDL2.dll` next to the executable, so it runs straight from the build folder.

Only the macOS build has been tested so far. The Linux and Windows steps are standard but have not been run.

### Build options

| Option | Default | Effect |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Debug` | Use `Release` for an optimized build (single-config generators such as Make and Ninja). |
| `MODEL1_TRACE_IRQ` | `ON` | Logs every CPU interrupt request and acknowledgement. |
| `MODEL1_TRACE_TGP` | `OFF` | Logs every TGP function executed (very verbose). |
| `MODEL1_TRACE_INPUT` | `ON` | Prints a confirmation when a coin or start key is pressed. |

Example:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMODEL1_TRACE_IRQ=OFF
```

## Running

```sh
./build/model1 [path/to/program_rom.bin]         # macOS / Linux
build\Debug\model1.exe [path\to\program_rom.bin]  # Windows
```

- The optional argument is a raw binary file, loaded into the boot ROM at `0xF80000` (up to 512 KB). The V60 starts executing at `0xFFFFF0`, which is offset `0x7FFF0` in that file.
- Without an argument, the emulator tries `roms/dummy_program.bin`. If that file is missing, it logs the failure and keeps running.
- Close the window to quit.

### Controls

Keys are matched by physical position: on an AZERTY keyboard, "W A S D" are the keys labelled Z Q S D.

| Key | Input |
|---|---|
| Arrow keys or W A S D | Player 1 joystick |
| J K L or Z X C | Player 1 buttons 1, 2, 3 |
| 1 / 2 | Start 1 / Start 2 |
| 5 / 6 | Coin 1 / Coin 2 |
| F2 | Test switch |
| 9 | Service switch |

Player 2 has no keys bound yet.

### What you should see

The window shows the test pattern. With no ROM loaded, the terminal log looks like this:

```
[V60] Reset, PC=0xFFFFFFF0
[TGP self-test] transform [1, 2, 3] by translate(10, 20, 30) = (11, 22, 33) PASS
[TGP self-test] transform [1, 2, 3] by rotZ(90) * translate = (8, 21, 33) PASS
[TGP self-test] project [11, 22, 33], focal 330 -> screen = (358, -28) PASS
[TGP self-test] all checks passed
[Main] ROM load FAILED: roms/dummy_program.bin (continuing without program)
[V60] CRITICAL: unimplemented opcode 0xFF at PC=0xFFFFFFF0, CPU halted
[Video] VRAM is empty: showing test pattern
```

This is expected. Unloaded ROM reads as `0xFF`, which is not an implemented opcode, so the CPU halts and logs it. Pressing 5 (coin) or 1 (start) prints a confirmation with the updated port value.

## Hardware reference

### Memory map

24-bit addresses, 16-bit data bus, from the real Model 1 board:

| Address | Contents |
|---|---|
| `0x000000–0x2FFFFF` | Program ROM (read-only) |
| `0x400000–0x40FFFF` | Work RAM A (battery-backed on hardware) |
| `0x500000–0x53FFFF` | Work RAM B |
| `0x600000–0x61FFFF` | Display list RAM |
| `0x800000–0x87FFFF` | Debug framebuffer (**emulator-only**, in a range the real board leaves unused) |
| `0x900000–0x903FFF` | Palette RAM |
| `0x910000–0x91BFFF` | Colour translation RAM |
| `0xC00000–0xC0003F` | I/O board: inputs and lamps |
| `0xD00000–0xDDFFFF` | TGP ports |
| `0xF80000–0xFFFFFF` | Boot ROM (read-only, holds the reset address) |

Any other address is unmapped: reads return 0, writes are ignored, and both are logged.

### V60 interrupts

When an interrupt is pending and PSW.IE (bit 18) is set, the CPU:

1. switches to the interrupt stack and clears IE;
2. pushes the old PSW, then the return PC;
3. jumps to the handler address stored in vector table entry `vector + 0x40`, at `(SBR & ~0xFFF) + entry × 4`.

RETIS undoes all of this. VBlank is requested at the end of every frame as vector 1, Model 1's VBlank interrupt level.

### Inputs (I/O board)

Ports are 16 bits and active-low: a bit reads 0 while its input is held, and an idle port reads `0xFFFF`.

| Address | Contents |
|---|---|
| `0xC00000–0xC0000F` | Analog channels 0–7 (no analog controls yet: read 0) |
| `0xC00010` | System: bit 0 coin 1, 1 coin 2, 2 test, 3 service, 4 start 1, 5 start 2 |
| `0xC00012` | Player 1: bits 0–2 buttons 1–3, 4 down, 5 up, 6 right, 7 left |
| `0xC00014` | Player 2: same layout as player 1 |
| `0xC0001E` | Lamps and coin meters (written by the game) |

### TGP

| Port | Use |
|---|---|
| `0xD80000` | FIFO: write a function word, then its float parameters; read results back |
| `0xDC0000` | FIFO status (read-only) |
| `0xD00000` | Shared RAM address (bit 15 = auto-increment) |
| `0xD20000` | Shared RAM data |

A function word carries the function ID in bits 31–23. Parameters and results are 32-bit IEEE floats. Matrices are 4×3: three basis rows and a translation row.

| ID | Function | Parameters → results |
|---|---|---|
| `0x05` / `0x06` | Matrix push / pop | none |
| `0x07` | Matrix write | 12 floats |
| `0x08` | Clear matrix stack | none |
| `0x09` | Matrix multiply | 12 floats |
| `0x10` | Identity matrix | none |
| `0x11` | Matrix read | none → 12 floats |
| `0x12` | Translate | x, y, z |
| `0x1B` | Transform point | x, y, z → x′, y′, z′ |
| `0x1FF` | Perspective projection (**emulator-only debug function**) | x, y, z, focal → screen x, y |

An unknown function ID is logged. Because its parameter count is unknown, every following word is logged and dropped until the TGP is reset.

### Debug framebuffer pixel format

496×384 pixels, 2 bytes each, little-endian, row by row from `0x800000`. Pixels use the real Model 1 colour format:

| Bits | Meaning |
|---|---|
| 0–4 | Red |
| 5–9 | Green |
| 10–14 | Blue |
| 15 | Intensity (0 = half brightness) |

For example, `0x801F` is full red and `0xFFFF` is white.

## Known limitations

- **No games run yet.** Real ROM sets are split across interleaved chips, and data-ROM bank switching, the tilemap layer and the interrupt controller are not emulated.
- **CPU:** only a handful of instructions and no memory-operand addressing modes. Every instruction is charged a flat 8 cycles.
- **Interrupts are latched, not level-triggered.** The board's interrupt controller (acknowledge and mask registers at `0xE00000`) is not emulated.
- **Frame rate:** 60 Hz. The real hardware runs at about 57.52 Hz (278,144 CPU cycles per frame), and VBlank starts partway through the frame.
- **TGP:** only a few firmware functions, numbered as in Virtua Fighter only. Functions complete instantly instead of taking DSP time.
- **Inputs:** Virtua Fighter bit layout only. No game controllers, analog controls or player 2 keys.
- **Video:** the framebuffer is a debug stand-in. The real board draws through tile layers and polygons.
- **No sound emulation.**
- **No automated test suite in the repository.** Subsystems were checked with temporary test programs during development; the TGP self-test is the only check built into the emulator.

## Roadmap

Planned next steps, roughly in order:

1. **Interrupt controller** at `0xE00000`, with level-triggered VBlank and a real acknowledge.
2. **More V60 instructions:** `UPDPSW` and `LDPR` (boot code needs them to enable interrupts), then memory-operand addressing modes and the rest of the instruction set.
3. **Real video timing:** 57.52 Hz, with VBlank at line 384.
4. **ROM set loading:** interleaved chip pairs and data-ROM banking.
5. **Graphics:** palette, tilemap layers, then the polygon renderer driven by the display lists.
6. **Repository test suite** (CTest) covering the CPU, bus, TGP and inputs.
7. **Sound board** (68000 + MultiPCM).
