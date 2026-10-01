# Sega Model 1 Emulator

An emulator for the Sega Model 1 arcade board, written in C++20 with SDL2. Supported ROM sets: **Virtua Racing** (`vr`) and **Virtua Fighter** (`vf`).

> **Status: Virtua Racing is playable.** It boots through its test-mode screen into attract mode with full 3D (courses, cars, the Bay Bridge, a 3D SEGA logo). A coin and the accelerator start a race, and the car steers and accelerates; the brake, view buttons and shifter are wired to the keyboard too. All the board's processors run their real firmware: the NEC V60 main CPU, the MB86233 TGP geometry DSP, the Z80 I/O board and the 68000 sound CPU. There is no FM music yet, the interrupt controller isn't emulated, and a few 3D details are wrong (see [Known limitations](#known-limitations)). Virtua Fighter's ROM set loads, but its TGP program isn't wired up yet, so it doesn't run.

## Contents

- [Quick start](#quick-start)
- [Purpose](#purpose)
- [Strategy](#strategy)
- [Features](#features)
- [Architecture](#architecture)
- [Building](#building)
- [Running](#running)
- [Testing](#testing)
- [Hardware reference](#hardware-reference)
- [Known limitations](#known-limitations)
- [Roadmap](#roadmap)

## Quick start

```sh
brew install cmake sdl2                                    # macOS; see Building for Linux / Windows
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DMODEL1_TRACE_IRQ=OFF
cmake --build build-release
./build-release/model1 path/to/vr                          # an unzipped MAME "vr" ROM set
```

Press **5** to insert a coin, then **↑** (accelerator) to pick a course and start. Steer with **← →** and brake with **↓**. The [Controls](#controls) section lists every key, and [ROM sets](#rom-sets) lists the files the emulator needs, including the shared `model1io` set.

## Purpose

The Sega Model 1 (1992) was Sega's first 3D arcade board. It combines an NEC V60 main CPU, a Fujitsu MB86233 DSP for geometry (the "TGP"), a polygon renderer, a Z80-based I/O board, and a separate 68000 sound board.

This project aims to emulate that board faithfully, with code that is easy to read and audit. Readability and correctness come before speed: every hardware behaviour is implemented explicitly, and anything that is not real hardware is labelled as such in the code and in this document.

## Strategy

**Build in small, verifiable steps.** Each subsystem starts as a skeleton (state, interfaces, lifecycle), then gets its core behaviour, then a few real operations, then refinement. Every step ends with a clean build and checks of the new behaviour.

**Verify hardware facts before writing them.** Opcode encodings, flag rules, the interrupt sequence, the memory map, TGP function IDs, input bit layouts and colour formats were all checked against MAME's Model 1 and V60 source code (BSD-3-Clause), not written from memory. MAME is used as a reference for facts only; no MAME code is copied.

**Run the real firmware wherever possible.** Three subsystems on the board run their own programs, and each is emulated at the hardware level, with a high-level stand-in for when its ROMs are missing:

| Subsystem | Real hardware | This emulator | Stand-in without the ROMs |
|---|---|---|---|
| TGP | MB86233 DSP running a per-game program | The DSP core runs the program (Virtua Racing) | C++ versions of a few geometry functions (used by Virtua Fighter for now) |
| I/O board | Z80 running EPR-14869, copying inputs into shared memory | The Z80 runs the firmware | `InputManager` writes the shared-memory layout directly |
| Sound board | 68000 running the sound program | The 68000 runs it | none (the sound program is a required file) |

Both stand-ins follow the layouts MAME used before it switched to low-level emulation, and they sit behind the same hardware ports, so the V60 sees the same machine either way.

**Fail loudly, never guess.** Unimplemented opcodes, unsupported addressing modes, unmapped memory accesses, unknown TGP functions and unknown I/O offsets are all logged with the address and value involved. The CPU halts on anything it cannot execute instead of skipping it.

**Keep the emulated machine separate from the host.** Everything under `src/core` is plain C++ with no SDL. Window, rendering and keyboard handling live in host-side code that talks to the core through small interfaces.

## Features

### NEC V60 CPU (`src/core/v60.*`)
- 32 general-purpose registers, PC and PSW, plus the privileged registers needed for interrupts: the interrupt stack pointer, four per-level stack pointers, and the system base register. `LDPR`/`STPR` read and write them by number (SBR moves the interrupt vector table); the other privileged registers (task, MMU, debug and system control) are stored and read back with no effect emulated, except TKCW, whose rounding mode CVTSW uses.
- **24-bit address bus**, little-endian, starting at the real reset address `0xFFFFFFF0`.
- **Instructions:** everything Virtua Racing executes, including:
  - Moves and conversions: MOV, MOVEA, MOVS/MOVZ (sign / zero extend), MOVT (truncate), XCH, SETF, UPDPSW.
  - Arithmetic in byte, half and word sizes: ADD/SUB, ADDC/SUBC, CMP, INC/DEC, NEG, MUL/MULU, DIV/DIVU, REM/REMU. There are also 64-bit forms: MULX/MULUX (32×32→64) and DIVX/DIVUX (64÷32 giving a quotient and a remainder), using a register pair or an 8-byte memory operand.
  - Logic and shifts: AND, OR, XOR, NOT, TEST, SHL (logical shift), SHA (arithmetic shift), ROT/ROTC. Shift counts are signed: positive shifts left, negative shifts right.
  - Single-bit operations on a 32-bit word: TEST1, SET1, CLR1, NOT1. CY receives the previous bit value and Z its inverse.
  - Bit fields: EXTBFS/EXTBFZ/EXTBFL, INSBFR/INSBFL.
  - Strings: MOVCU/MOVCFU/MOVCSU, MOVCD/MOVCFD, SCHCU/SKPCU.
  - Single-precision floating point: MOVF, ADDF, SUBF, MULF, DIVF, CMPF, NEGF, ABSF, SCLF, CVTWS / CVTSW (rounding set by the TKCW register).
  - Stack: PUSH, POP, PUSHM and POPM (register lists, optionally including the PSW).
  - Control flow: BR, the 14 conditional branches (8- and 16-bit displacements), DBcc and TB (decrement and branch), JMP, JSR, CALL/RET, BSR/RSR, RETIS, NOP and HALT.
  - System: LDPR/STPR (privileged registers), IN/OUT (the V60's separate I/O space).
- **Addressing modes:** all of them. That covers register; register indirect; autoincrement/decrement; 8/16/32-bit displacement; displacement-indirect and double displacement; PC-relative, absolute and their indirect forms; immediates; and the indexed variants, where an index register is scaled by the operand size (×1, ×2, ×4, or ×8 for 64-bit operands). Autoincrement and autodecrement step by the operand size, exactly once per instruction. One decoder (`decode_operand`) serves every instruction.
- **Bit addressing** (bit-field group `0x5D`: `EXTBFS`/`EXTBFZ`/`EXTBFL`, `INSBFR`/`INSBFL`): the same decoder yields a base address plus a signed bit offset. Displacements and index registers count bits instead of being added to the address. Fields are 1–32 bits and may cross a 32-bit boundary; only the bytes a field spans are read or written.
- **Flags:** Zero, Sign, Overflow and Carry follow the hardware rules. For example, logic operations clear OV and leave CY unchanged, and byte/half results update only the low bits of a register.
- **Stack checks:** every push and pop, including interrupt entry, is checked. A misaligned SP, or a stack access outside work RAM (an overflow on push, an underflow on pop), is logged as a warning (the first 8 times). Execution continues, because the V60 itself has no stack limits.
- **Maskable interrupts:** pending requests are taken between instructions when PSW.IE is set. The CPU switches to the interrupt stack, saves PSW and PC, and jumps through the vector table. HALT waits for an interrupt; RETIS returns from one.

### Memory map and bus (`src/core/bus.*`)
- The real Model 1 layout: program and boot ROM, two work RAMs, display list, palette and colour RAM.
- ROM is read-only and reads `0xFF` until loaded, like an erased EPROM.
- **Device ports:** devices register 16-bit read/write handlers on address ranges, with mirroring. A 32-bit access to a port becomes two 16-bit accesses, low half first, as on the board's 16-bit data bus.
- Raw binary ROM images can be loaded at any ROM address.

### TGP geometry coprocessor (`src/core/tgp_copro.*`, `src/core/mb86233.*`, `src/core/tgp.*`)
- **Real TGP (Virtua Racing):** with the TGP program (`315-5573.bin`), tables (`opr14742`/`opr14743`) and data ROMs (`mpr-14898`–`14901`) loaded, the board is emulated at the hardware level, as in current MAME:
  - **MB86233 DSP** (`src/core/mb86233.*`), a port of MAME's core: loads, moves between registers, RAM, I/O and program memory, the float ALU (add, subtract, multiply, multiply-accumulate, divide, compare, conversions) and integer operations, repeat, loop counters, a 4-entry call stack. Floating-point mode only, no interrupts (as in MAME). 40 MHz / 3 instructions per second, in lockstep with the V60.
  - **The board** (`src/core/tgp_copro.*`): 16-word FIFOs both ways, an 8K-word copro RAM with four auto-incrementing address registers (stride 4 for vertex lists), the sin/cos, atan, 1/x and 1/√x table units, and the 2 MB data ROM window. On the board the V60 halts on an empty output FIFO or a full input FIFO; here the DSP runs on the spot until it can proceed.
  - **Debugging:** `TgpCopro::set_trace(n)` logs FIFO traffic, `Mb86233::set_trace(n)` logs DSP instructions with registers.
  - **Old firmware dump:** the `315-5573.bin` with CRC32 `0xec913af2` (common in older sets) is a bad dump that MAME replaced in 0.197 (CRC32 `0x3335a19b`): with it the game hangs waiting for TGP results ([MAME Testers 07025](https://mametesters.org/view.php?id=7025)). The loader recognises it and says so.
- **High-level TGP (fallback, Virtua Fighter):** without those ROMs, `src/core/tgp.*` stands in:
  - The real host interface: a command FIFO, a status port, and a shared-RAM port with auto-increment.
  - Geometry functions: matrix write, read, multiply, identity, translate and push/pop on a 32-deep stack, plus transform point. These use the real 4×3 matrix layout and IEEE floats.
  - An emulator-only perspective projection function for debugging.
- **Startup self-test:** drives the TGP through the bus exactly as game code would and checks the results bit-for-bit.

### I/O board and inputs (`src/core/io_board.*`, `src/core/z80.*`, `src/core/eeprom_93c46.*`, `src/core/dual_port_ram.*`, `src/core/input_manager.*`, `src/input/keyboard_input.*`)
- **Shared RAM:** the main CPU reaches the I/O board (837-8950: a Z80 running EPR-14869, a 315-5338A I/O chip, an ADC and a 93C45 EEPROM) only through a 2 KB **MB8421 dual-port RAM** at `0xC00000–0xC00FFF`, as in current MAME. Byte *n* sits on the V60's low byte lane at `0xC00000 + 2n`; the high lane isn't connected.
  - It has no fixed registers: the meaning of each byte is a protocol between the game and the board firmware. Virtua Racing, for example, writes `SEGA` at bytes `0x1A–0x1D`, `0x01` at byte `0x20` (`0xC00040`), then reads and rewrites a 128-byte block at bytes `0x100–0x17F` (`0xC00200–0xC002FE`), the size of the board's EEPROM.
  - Not emulated: the RAM's interrupt mailboxes and access wait states.
- **The I/O board runs its real firmware** (`epr-14869.25` for Virtua Racing, `epr-14869b.25` for Virtua Fighter), as in MAME's `model1io` device:
  - **Z80 at 4 MHz** (`src/core/z80.*`): the full documented instruction set with the CB / ED / DD / FD / DDCB prefixes, IM 0/1/2 and NMI, and T-state timing, clocked in lockstep with the V60 (a quarter cycle per V60 cycle).
  - **Memory map:** 16 KB of the firmware EPROM, 8 KB RAM, the **315-5338A** I/O controller (7 ports, and the address-latch / data registers through which the Z80 reads and writes the shared RAM) and the **MSM6253** ADC (4 analog channels, read one bit at a time).
  - **Ports:** inputs on B–D from `InputManager` (or the three DIP-switch banks, all off, when port A bit 0 selects them), EEPROM lines on A and G, lamps on F, drive board on E (not emulated).
  - **93C45 EEPROM** (`src/core/eeprom_93c46.*`, 64 × 16 bits): the serial protocol of MAME's 93Cxx device, write-protected at power-on, with MAME's programming times (write 1.75 ms, erase 1 ms, write/erase all 8 ms), which also set how long the game's boot waits. It starts from MAME's factory defaults (`93c45.bin` in the `model1io` set) when present, otherwise erased, and isn't saved between runs yet.
  - **Effect on Virtua Racing:** the firmware answers the boot handshake, copies the EEPROM settings into the shared RAM (bytes `0x100–0x17F`) and saves the defaults the game writes back; it publishes the analog channels at bytes 0–7 and the input ports at 8 and up.
- **Control panels** (`InputManager::Profile`, chosen by the ROM loader from the game):
  - **Virtua Fighter:** two joysticks and three buttons each, on ports B–D.
  - **Virtua Racing** (MAME's `vr` ports): the four view buttons VR1–VR3 on the system port (bits 5–7) and VR4 on port C bit 0; the shifter on port C bits 4 (down) and 5 (up). The steering wheel, accelerator and brake are analog, read through the ADC: wheel on channel 0 (centre `0x80`, lower to the left), accelerator on 1 and brake on 2 (released `0x30`, fully pressed `0xFF`), channel 3 unconnected (`0xFF`).
  - On the keyboard the wheel and pedals are driven by held keys: once per frame each moves a step toward its target (full lock or full press) and springs back when released, like MAME's key-driven paddle and pedals.
- **Fallback stand-in:** without the firmware file (it's optional in the ROM loader), `InputManager` plays the board at a high level: it writes the ports into the shared RAM using MAME's former high-level layout (the four analog channels at bytes 0–3 and again at 4–7, system port at 8, players at 9 and 10, lamps at 15) and acknowledges the game's commands in byte `0x20` once per frame without performing them.
- **Keyboard mapping:** keys are matched by physical position, so the layout works on QWERTY and AZERTY.
  - Keys with the same function (W and ↑) can be held together without releasing each other.
  - Auto-repeat is ignored.
  - Every input is released when the window loses focus, so no key gets stuck.

### 2D layers (`src/core/tilemap_renderer.*`)
- The Model 1's 2D hardware, the Sega System 24 tilemap chip, renders each 496×384 frame from tile RAM, character RAM and palette RAM.
- **Tilemaps:** four 512×512-pixel tilemaps of 8×8 tiles, 4 bits per pixel, with 16-colour palettes. Each pair of tilemaps shares the screen through a per-line window mask with 8-pixel columns.
- **Scrolling:** horizontal and vertical scroll per tilemap, with optional per-line horizontal scroll and a per-tilemap disable bit.
- **Layer order:** the board's fixed order. Low-priority tilemaps 3 and 2 form an opaque background, then tilemaps 1 and 0 draw with transparency, then (once implemented) the 3D polygons, then every tilemap's high-priority tiles on top for HUD and text.
- **Colours:** palette entries use the Model 1 format (xBGR 5:5:5 plus an intensity bit) and are decoded once per frame.
- **Speed:** with both demos running, a whole frame (2D layers plus polygons) takes about 8 ms in a Debug build and under 2 ms optimized, well within the 16.7 ms frame budget.

### 3D polygons (`src/core/polygon_renderer.*`)
- **Display lists:** reads the display lists the game writes into display list RAM. There are two 64 KB lists, double-buffered through the list control register, with automatic or manual swapping and a render-enable mask.
- **3D objects** (command `0x01`): models from the 16 MB polygon ROM (or polygon RAM), as MAME's `model1_v.cpp`: transformed by the object matrix (command `0x0B`), projected with the zoom and view translation (`0x09`, `0x0C`), back-face culled (unless double-sided), lit by ambient + diffuse + optional specular light (light direction `0x0A`, parameters `0x06`, specular enable `0x07`) through the colour translation RAM, clipped to the viewport frustum, and depth-sorted with the direct polygons. Command `0x41` (objects above the HUD) is drawn in the same pass for now.
- **Direct polygons:** the V60/TGP send already-projected quad strips (command `0x02`). Each new edge links to the previous one using four link modes.
- **Flat shading:** each quad's colour comes from a colour table (command `0x04`), then palette RAM entry `0x1000 + index`, then the colour translation RAM driven by the quad's luminance.
- **Painter's algorithm:** quads are sorted far-to-near by their z value and drawn whenever the viewport changes and at the end of the list, clipped to the viewport (command `0x03`).
- **Data uploads:** command `0x05` stores 32-bit words into a 16 MB polygon data RAM (object addresses `0x800000` and up), command `0x06` stores lighting parameters (diffuse, ambient, specular, power) in 256 slots, as in MAME. The 3D object renderer reads them. A command whose length runs past the end of the 64 KB list, or that targets an address outside its RAM, is rejected with a warning instead of looping or overflowing. `0xFFFFFFFF` (an erased list) ends the list quietly.
- **Rasterizer:** edge-function fill of each quad as two triangles, a stipple ("moiré") checkerboard mode for translucency, and wireframe lines for quads with only two distinct corners.
- **Layer order:** drawn between the low- and high-priority tilemaps, as on the board.

### Video output (`src/video/video_manager.*`)
- 496×384 window that scales with sharp pixels and keeps its aspect ratio when resized.
- Presentation only: each finished frame is copied into a streaming SDL texture and shown.

### Sound board (`src/core/sound_board.*`, `m68000.*`, `sound_bus.*`, `i8251.*`)
The real Model 1 sound board, from MAME's `segam1audio`: a **Motorola 68000 at 10 MHz** with its own address space. The main board sends it commands over a **serial link**: an i8251 UART on each board, cross-connected, at 31.25 kbaud.
- **i8251 UART:** the mode and command registers, status (TxRDY, RxRDY, TxEMPTY, error flags), transmit holding and shift registers, overrun detection, and the SYNC-character bytes that follow a synchronous mode byte (so the usual `00 00 00 40` software reset is handled silently; actually using synchronous mode is logged as not emulated). Serial timing is real: one 8N1 byte at the ×16 clock takes 160 UART clock ticks, 320 µs.
- **Sound bus:** big-endian and 24-bit, with program ROM at `0x000000` (and its mirror), the UART at `0xC20000` and work RAM at `0xF00000`. Above each MultiPCM's 8 ports, the rest of its 64 KB window (`0xC40008–0xC4FFFF`, `0xC60008–0xC6FFFF`) is a latch: the sound program writes control values there (for example a 24-bit value as bytes at `0xC40007/09/0B`), which are stored, read back and logged on first write. MAME ignores those addresses.
- **68000 skeleton:** D0–D7, A0–A7 with separate supervisor and user stacks, PC and SR. Reset loads the stack pointer and PC from the vector table.
  - **Interrupts:** level-sensitive and autovectored. The UART's "receiver ready" signal drives level 2 (vector address `0x68`).
  - **Instructions:** NOP, STOP, RTE, Bcc/BRA/BSR, RTS, JSR/JMP, MOVEQ, SWAP, TST, ADD/SUB/ADDA/SUBA/ADDX/SUBX, CMP/CMPA/CMPM/EOR, OR/AND, MULU/MULS, DIVU/DIVS (worst-case timing), ABCD/SBCD, EXG, ADDQ/SUBQ, the immediate group (ORI/ANDI/SUBI/ADDI/EORI/CMPI, including ORI/ANDI/EORI to CCR and SR), the shifts and rotates (ASx/LSx/ROXx/ROx, register and memory forms), LEA, DBcc, MOVE/MOVEA, MOVE to SR, CLR, and the bit operations BTST/BCHG/BCLR/BSET (static `#n` and dynamic `Dn` forms). All 12 addressing modes are decoded, and cycle counts come from the MC68000 manual. A word or long access at an odd address, or any other opcode, halts with a logged message.

### Sound output (`src/audio/multipcm.*`, `src/audio/ym3438.*`, `src/audio/audio_output.*`)
- **MultiPCM (×2):** Sega's 28-voice sample playback chip (Yamaha YMW-258-F), following MAME's `multipcm`/`gew` devices. It's part of the SDL-free core.
  - **Registers:** the port interface (slot select, register select, data) and 12-byte sample headers.
  - **Playback:** octave/pitch stepping, 8-bit and packed 12-bit samples with linear interpolation, and loop points.
  - **Volume:** attenuation (0.375 dB per step) and pan, plus the board's sample-ROM bank registers.
  - **Output:** 10 MHz ÷ 224 = 44,642.86 Hz stereo, generated in lockstep with the CPUs, so each register write takes effect at the right sample.
- **YM3438 (timers only):** the FM chip at 8 MHz, clocked from the 68000's cycle count (exactly 180 CPU cycles per FM sample).
  - **Timers:** Timer A overflows every (1024 − A) samples, Timer B every (256 − B) × 16; register `0x27` starts/stops them, enables and clears their status flags. The sound program's main loop waits on Timer A, so this paces the sound driver.
  - **Not emulated yet:** FM synthesis (no FM audio). All register writes are stored for it; the first FM write is logged.
- **Host output:** SDL at 44.1 kHz, signed 16-bit stereo, with a 512-frame device buffer. The chip stream is resampled by linear interpolation and queued with `SDL_QueueAudio`.
  - No emulator data is shared with SDL's audio thread, so there's nothing to lock or deadlock on, and no allocation happens there.
  - Queued audio is capped at 100 ms; anything beyond that is dropped and counted.
  - If no audio device is available, the emulator runs silently.

### Timing (`src/main.cpp`, `src/core/motherboard.*`)
- A 60 Hz frame loop paced by the high-resolution counter; it measured exactly 60.0 FPS.
- **Each frame:**
  - The V60 runs a 266,666-cycle budget. Overshoot is carried into the next frame so the long-run rate is exact.
  - The frame is run in 4,096-cycle slices (256 µs). After each slice, the UARTs advance 1 tick per 32 V60 cycles, the 68000 runs exactly 5/8 of the V60's cycles (about 166,666 per frame), and the sound chips render 5 samples per 1,792 V60 cycles (about 744 per frame). Remainders are carried over.
  - The frame's audio is handed to the host output: 735 frames at 44.1 kHz.
  - The frame is composed: tilemap background, 3D polygons, then tilemap foreground.
  - The VBlank interrupt is raised.
  - The frame is presented.

## Architecture

### Components

```
                         +-------------------------------+
  Host side (SDL)        |  main.cpp                     |
                         |  event loop, 60 Hz pacing     |
                         +---+-------------+---------+---+
                             |             |         |
                   SDL events|   run_frame |         | frame() pixels, audio
                             v             v         v
  +--------------------+  +------------------------+  +---------------------------+
  | KeyboardInput      |  | Motherboard            |  | VideoManager, AudioOutput |
  | src/input          |  | owns every component,  |  | src/video, src/audio      |
  | scancode -> input  |  | wires them, runs them  |  | texture, SDL audio queue  |
  +---------+----------+  | in lockstep            |  +---------------------------+
            | set_input() +-----------+------------+
  ----------|-------------------------|---------------------------------------------------
  Emulated  v                         v
  machine   +-------------------------------------------------------------------------+
  (core)    |                                  Bus                                    |
            |        memory regions (ROM/RAM)  +  16-bit device port handlers         |
            +----+-------------+------------------+--------------+------------+-------+
                 ^             | 0xC00000         | 0xD00000     | 0xC40000   | tile, char,
                 |             v                  v              v            v palette, lists
            +----+----+  +-------------+  +---------------+  +--------+  +-----------------+
            |   V60   |  | MB8421 dual |  | TgpCopro      |  | i8251  |  | TilemapRenderer |
            | main CPU|  | -port RAM   |  |  MB86233 DSP  |  |  UART  |  |  2D layers      |
            +---------+  +------+------+  |  FIFOs, RAM,  |  +---+----+  +-----------------+
                                |         |  tables       |      | serial| PolygonRenderer |
                                v         | (Tgp: HLE     |      v       |  display lists, |
                         +-------------+  |  fallback)    |  +---------+ |  3D objects,    |
                         | IoBoard     |  +---------------+  | Sound   | |  quads          |
                         |  Z80, 5338A,|                     | board   | +-----------------+
                         |  ADC, EEPROM|                     |  68000, |
                         +------+------+                     |  UART,  |
                                ^ ports / analog             |  2 x    |
                         +------+------+                     |  Multi- |
                         | InputManager|                     |  PCM,   |
                         |  profiles,  |                     |  YM3438 |
                         |  wheel/pedal|                     +---------+
                         +-------------+
```

`KeyboardInput` calls `InputManager::set_input()` directly. The I/O board's Z80 then reads those ports and the analog channels through its own bus, and copies them into the dual-port RAM for the V60. When the I/O firmware is missing, `InputManager` writes the RAM itself.

### Design rules

- **The bus is the only path between components.** The CPU reaches memory and devices only through `Bus`. Devices never call the CPU or each other. The `Motherboard` creates every component, registers device ports on the bus, and drives the frame. Each sub-board has its own bus in the same style: `SoundBus` for the 68000, `Z80Bus` (implemented by `IoBoard`) for the Z80, `Mb86233Bus` (implemented by `TgpCopro`) for the DSP.
- **The core has no host dependencies.** `src/core` does not include SDL. The keyboard layer calls `InputManager::set_input()`, and the video layer receives finished frames as a read-only `std::span`. All emulated-hardware decoding, including tiles and colours, happens in the core.
- **Memory is fixed-size, like the hardware.** RAM and ROM are `std::array` buffers sized to the real chips. Large owners (`Bus`, CPU, TGP) are heap-allocated so they never live on the stack.
- **Byte order is explicit.** Multi-byte values are assembled byte by byte in little-endian order, so results don't depend on the host CPU.
- **Logging is built in.** All emulator diagnostics go to stderr with a component tag such as `[V60]`, `[Bus]` or `[TGP]`. Chatty traces can be switched off at compile time.

### Frame lifecycle

```
main loop, once per 1/60 s:
  1. poll SDL events  -> KeyboardInput -> InputManager (port bits, held wheel/pedal keys)
  2. Motherboard::run_frame()
       a. InputManager::update_analog(): wheel and pedals move one step toward the held keys
       b. V60 executes instructions until the frame's cycle budget is spent
          (each instruction is charged 8 cycles; interrupts are taken between instructions),
          in 4,096-cycle slices; after each slice, for the same stretch of time:
            UARTs advance (1 serial clock tick per 32 V60 cycles)
            68000 sound CPU catches up (5 cycles per 8 V60 cycles)
            TGP DSP runs (5 instructions per 6 V60 cycles), when its ROMs are loaded;
              it also runs on the spot whenever the V60 waits on a FIFO
            I/O board Z80 runs (1 T-state per 4 V60 cycles), when its firmware is loaded;
              otherwise the InputManager stand-in answers once per frame
            MultiPCMs render their output (5 samples per 1,792 V60 cycles)
       c. frame composed in the board's layer order:
            TilemapRenderer::render_background  (low-priority tilemaps)
            PolygonRenderer::render             (display list -> 3D objects + quads, depth-sorted)
            TilemapRenderer::render_foreground  (high-priority tilemaps: HUD, text)
       d. display lists swapped (automatic double-buffer mode)
       e. VBlank interrupt requested (Model 1 interrupt level 1)
  3. VideoManager::update_framebuffer(motherboard.frame())  -> pixel buffer
  4. VideoManager::present_frame()                          -> streaming texture -> window
     AudioOutput::submit(sound board audio)                  -> resampled to 44.1 kHz -> SDL queue
  5. sleep until the next frame deadline (resync if far behind)
```

### Project layout

```
CMakeLists.txt              Build configuration: model1_core library, model1 app, emulator_tests
cmake/CompilerWarnings.cmake  Warning flags (-Wall -Wextra -Wpedantic -Wshadow -Wconversion, /W4)
src/main.cpp                SDL setup, event loop, frame pacing, startup self-test call
src/core/motherboard.*      Owns all components, maps device ports, runs one frame
src/core/bus.*              Memory map, device port dispatch, ROM loading
src/core/v60.*              NEC V60 CPU: decode, execute, interrupts
src/core/tgp.*              High-level TGP (fallback): FIFO, shared RAM, a few geometry functions
src/core/tgp_copro.*        Real TGP board: MB86233 + FIFOs, copro RAM, table units, data ROM
src/core/mb86233.*          Fujitsu MB86233 DSP core (TGP)
src/core/sound_board.*      Sound board: 68000 + UART, interrupt wiring
src/core/m68000.*           Motorola 68000 skeleton (sound CPU)
src/core/sound_bus.*        Sound board address space (big-endian)
src/core/i8251.*            i8251 UART with serial timing (sound command link)
src/core/rom_loader.*       ROM set loader: per-game manifests, interleaving, CRC32 checks
src/core/dual_port_ram.hpp  MB8421 dual-port RAM shared with the I/O board (0xC00000)
src/core/input_manager.*    Input ports, per-game control panels (VF joysticks, VR wheel/pedals); I/O board stand-in
src/core/io_board.*         Model 1 I/O board: Z80 + 315-5338A + ADC + EEPROM running EPR-14869
src/core/z80.*              Zilog Z80 core (I/O board CPU)
src/core/eeprom_93c46.*     93C46-style serial EEPROM (64 x 16)
src/audio/multipcm.*        Sega MultiPCM sample playback chip (core, no SDL)
src/audio/ym3438.*          YM3438 FM chip: timers only so far (core, no SDL)
src/audio/audio_output.*    SDL audio output: resampling and queueing (host side)
src/core/tilemap_renderer.* System 24 tilemap chip: 2D layers, window masks, scrolling, palette
src/core/polygon_renderer.* Display-list interpreter, depth sort, flat quad rasterizer
src/core/log.hpp            Hex formatting and compile-time trace switches
src/input/keyboard_input.*  SDL keyboard -> Model 1 inputs (host side)
src/video/video_manager.*   SDL window and streaming texture (presentation only)
tests/test_framework.hpp    Minimal test framework (TEST_CASE, CHECK, CHECK_EQ)
tests/test_main.cpp         Test runner: checklist output, log capture, filtering
tests/v60_test_rig.hpp      Shared CPU fixture (isolated Bus + V60, program/stack areas in RAM B)
tests/test_v60_core.cpp     V60 tests: reset, NOP, MOV, ADD/SUB flags, stack, subroutines, branches, defensive checks
tests/test_v60_addressing_math.cpp  V60 tests: every addressing mode, MUL/DIV, division by zero, TEST
tests/test_v60_boot.cpp     VR boot code: UPDPSW, MOVEA, string moves, IN/OUT, ADDC/SUBC, CMP, INC/DEC, ADD/SUB sizes, LDPR/STPR, JSR, MOVS/MOVZ, DBcc/TB, ROT/ROTC, REM/REMU, bit fields, SETF, NEG, floating point, MOVT, SCHCU/SKPCU, MOVCD/MOVCFD, system registers, I/O stub
tests/test_m68000.cpp       68000 tests: MOVE to SR, CLR, MOVE addressing modes, LEA, DBcc, branches, BTST/BSET/BCLR/BCHG, ADDQ/SUBQ, shifts/rotates, SWAP, immediate ALU group, ADD/SUB family, CMP/CMPA/CMPM/EOR, OR/AND/MUL/DIV/BCD/EXG, TST, sound latch
tests/test_tilemap.cpp      2D layer tests: decoding, scrolling, layering, worst-case safety, demo HUD text
tests/test_polygons.cpp     3D tests: fill, depth sort, clipping, stipple, colours, lists, 0x05/0x06 uploads and bounds, 3D objects (projection, culling, frustum clipping, lighting), cube demo culling
tests/test_sound.cpp        Sound tests: UART timing and errors, 68000 skeleton, V60 -> sound command, lockstep cycles
tests/test_multipcm.cpp     MultiPCM tests: registers, pitch, 8/12-bit samples, loops, pan, banks, 440 Hz tone, audio rate
tests/test_ym3438.cpp       YM3438 tests: Timer A/B periods, flags, stop/restart, VR's Timer A wait (exact cycles)
tests/test_audio_host.cpp   host_tests (SDL): audio device open/close with a deadlock watchdog, resampler
tests/test_rom_loader.cpp   ROM loader tests: mock VF/VR sets in temp folders, every destination, error cases
tests/test_io_board.cpp     I/O board: shared RAM, stand-in, EEPROM protocol and timing, 315-5338A, ADC, a small Z80 firmware, Virtua Racing / Virtua Fighter control panels
tests/test_z80.cpp          Z80 tests: flags, DAA, 16-bit, stack, timing, IX/IY and DDCB, block ops, interrupts, I/O
tests/test_tgp_copro.cpp    TGP tests: DSP moves, ALU, repeat, stalls; FIFOs both ways, copro RAM, tables, data ROM
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

**To play, use an optimized build.** The default `Debug` build is for development: it isn't guaranteed to keep up with 60 FPS once a game runs all four processors. A Release build with interrupt tracing off is the one to play with:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DMODEL1_TRACE_IRQ=OFF
cmake --build build-release
```

For reference, the Release build runs Virtua Racing at about 250 frames per second on an Apple M1 Pro, measured without a window (about 4× the real speed).

**Build directories** used in this project (any names work):

| Directory | Configuration | Use |
|---|---|---|
| `build/` | Debug | Development and unit tests |
| `build-release/` | Release, `MODEL1_TRACE_IRQ=OFF` | Playing |
| `build-sanitize/` | Debug, `MODEL1_SANITIZE=ON` | Memory-safety test runs |

### Build options

| Option | Default | Effect |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Debug` | Use `Release` for an optimized build (single-config generators such as Make and Ninja). |
| `MODEL1_TRACE_IRQ` | `ON` | Logs every CPU interrupt request and acknowledgement. |
| `MODEL1_TRACE_TGP` | `OFF` | Logs every TGP function executed (very verbose). |
| `MODEL1_TRACE_INPUT` | `ON` | Prints a confirmation when a coin or start key is pressed. |
| `MODEL1_BUILD_TESTS` | `ON` | Builds the `emulator_tests` unit test executable. |
| `MODEL1_SANITIZE` | `OFF` | GCC/Clang: builds with AddressSanitizer, UBSan and bounds-checked standard containers. Any out-of-bounds access or undefined behaviour aborts with a report. |

Example:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMODEL1_TRACE_IRQ=OFF
```

## Running

```sh
./build-release/model1 path/to/vr                        # play Virtua Racing (game detected)
./build/model1 path/to/vr                                # same, Debug build
./build/model1 --romdir path/to/vf                       # same, explicit
./build/model1 --romdir path/to/roms --game vr           # force the game
./build/model1 --trace 400 path/to/vr                    # also log each CPU's next 400 instructions
./build/model1 [--tile-demo] [--poly-demo] [--sound-demo] [--no-demo] [--no-audio] [path/to/boot_rom.bin]
build\Debug\model1.exe [same options]   # Windows
```

- `--tile-demo` fills palette, character and tile RAM with a demonstration screen through the bus, the way game code would: scrolled colour bars, a framed box, a high-priority checkerboard band, and HUD text (`SCORE: 000000`, `TIME: 99`, `INSERT COIN`). The text uses an 8×8 font with a drop shadow, uploaded to character RAM, since Model 1 has no character ROM. Use it to check the 2D pipeline without a game.
- `--poly-demo` shows a spinning flat-shaded cube on an arcade-blue backdrop, built the way game code would:
  - Every frame, the cube is rotated and translated *on the TGP*, through its FIFO (matrix functions and transform point).
  - Faces turned away from the camera are culled; the rest are perspective-projected, lit, and written to the display list as direct polygons through the bus.
  - The renderer sorts and fills them.

  Combine it with `--tile-demo` to see the full layer order: background tiles, then the cube, then the high-priority HUD text.
- `--sound-demo` plays a 440 Hz beep, half a second on and half a second off. A 64-point sine wave is placed in MultiPCM 1's sample ROM, and slot 0 is programmed through the sound bus (octave 0, pitch 268) and keyed on and off each half second.
- `--no-audio` doesn't open an audio device.
- `--trace N` logs the next N instructions of each CPU with their address and bytes (`[V60 trace] 0x00FE000E: 0x13 0x80 ...`, `[68000 trace] 0x00000200: 0x46FC 0x2700 ...`), to map out the boot code.
- **With no ROM loaded and no demo option, the 3D demo starts automatically,** so the window isn't just black. `--no-demo` gives a blank machine instead.

- `--romdir <folder>`, or just the folder path, loads an unzipped MAME ROM set (see [ROM sets](#rom-sets)). If a needed file is missing or has the wrong size, the emulator names it and exits without starting.
- A bare *file* argument is a raw binary loaded into the boot ROM at `0xF80000` (up to 512 KB), for test programs. The V60 starts executing at `0xFFFFF0`, which is offset `0x7FFF0` in that file.
- Without an argument, the emulator tries `roms/dummy_program.bin`. If that file is missing, it logs the failure and keeps running.
- Close the window to quit.

### Controls

Keys are matched by physical position: on an AZERTY keyboard, "W A S D" are the keys labelled Z Q S D. The ROM loader picks the control panel from the game, so the same keys drive a joystick in Virtua Fighter and a wheel in Virtua Racing.

**Virtua Racing:**

| Key | Control |
|---|---|
| ← / → (or A / D) | Steer. Holding a key turns the wheel a step per frame toward full lock; releasing it recentres the wheel |
| ↑ (or W) | Accelerator. Ramps up while held, springs back when released |
| ↓ (or S) | Brake, likewise |
| J / K / L (or Z / X / C) | View buttons VR1 (red), VR2 (blue), VR3 (yellow) |
| U (or V) | View button VR4 (green) |
| I / O (or B / N) | Shift down / shift up |
| 5 | Coin |
| 1 | Start (hold it while pressing the accelerator on the course screen for the 7-speed manual gearbox) |
| F2 / 9 | Test / service switches |

To play: insert a coin (5), choose a course with ← →, then press ↑ to start.

**Virtua Fighter and all keys:**

| Key | Input |
|---|---|
| Arrow keys or W A S D | Player 1 joystick (Virtua Racing: ← → steer, ↑ accelerator, ↓ brake) |
| J K L or Z X C | Player 1 buttons 1, 2, 3 (Virtua Racing: view buttons VR1 red, VR2 blue, VR3 yellow) |
| U I O or V B N | Player 1 buttons 4, 5, 6 (Virtua Racing: view button VR4 green, shift down, shift up) |
| 1 / 2 | Start 1 / Start 2 |
| 5 / 6 | Coin 1 / Coin 2 |
| F2 | Test switch |
| 9 | Service switch |

Player 2 has no keys bound yet.

### What you should see

**With the Virtua Racing set,** the log reports the files it found, then the game boots:

```
[ROM] Virtua Racing (vr) in 'roms/vr': 15 of 15 needed files found
[ROM] Loaded 32 files (28360 KB) for Virtua Racing; 7 other files of the set are present but not used
```

The test-mode settings screen appears for a moment, then attract mode: the scrolling *Course Ranking* table and 3D fly-bys of the courses. A coin brings up the course-select screen, and the accelerator starts the race.

**Without a ROM set,** with no ROM loaded, the window shows the spinning cube on a blue backdrop: the automatic 3D demo. With `--no-demo` it is black: video memory is empty, so every pixel is palette entry 0, which is black at power-on, just like the real board. The terminal log looks like this:

```
[V60] Reset, PC=0xFFFFFFF0
[TGP self-test] transform [1, 2, 3] by translate(10, 20, 30) = (11, 22, 33) PASS
[TGP self-test] transform [1, 2, 3] by rotZ(90) * translate = (8, 21, 33) PASS
[TGP self-test] project [11, 22, 33], focal 330 -> screen = (358, -28) PASS
[TGP self-test] all checks passed
[Main] ROM load FAILED: roms/dummy_program.bin (continuing without program)
[Main] No program running: starting the 3D demo (use --no-demo for a blank screen)
[Motherboard] Polygon demo loaded: cube transformed by the TGP, 3 visible faces in display list 0
[V60] CRITICAL: unimplemented opcode 0xFF at PC=0xFFFFFFF0, CPU halted
[68000] CRITICAL: address error: instruction fetch from an odd address (exception not emulated) at PC=0xFFFFFFFF, CPU halted
```

With no ROM, this is expected. Unloaded ROM reads as `0xFF`, which is not an implemented opcode, so the V60 halts and logs it. The sound CPU reads its reset vectors from an empty sound ROM (`0xFFFFFFFF`, an odd address) and halts too. Pressing 5 (coin) or 1 (start) prints a confirmation with the updated port value.

### ROM sets

The loader reads an unzipped ROM set: a folder containing the individual chip dumps, named as in MAME. File names are matched case-insensitively, and the game is detected from the files present (or chosen with `--game`). Supported sets: **Virtua Fighter** (`vf`) and **Virtua Racing** (`vr`).

| Part | Virtua Fighter | Virtua Racing | Loaded into |
|---|---|---|---|
| V60 program (2 chips, byte-interleaved) | `epr-16082.14`, `epr-16083.15` | `epr-14882.14`, `epr-14883.15` | `0x200000–0x2FFFFF` |
| V60 boot ROM (holds the reset address) | `epr-16080.4`, `epr-16081.5` | `epr-14878a.4`, `epr-14879a.5` | `0xFC0000–0xFFFFFF` |
| V60 data ROM (4 interleaved pairs, banked) | `mpr-16084.6` … `mpr-16091.13` | `mpr-14880.6` … `mpr-14889.13` | banks 0–3 of the `0x100000` window |
| 68000 sound program (byte-swapped words) | `epr-16120.7`, `epr-16121.8` | `epr-14870a.7` | sound ROM `0x00000` |
| MultiPCM samples | `mpr-16122.32`, `mpr-16123.33` / `mpr-16124.4`, `mpr-16125.5` | `mpr-14873.32` / `mpr-14876.4` | MultiPCM 1 / 2 sample ROMs |
| TGP program (MB86233) | not wired up yet | `315-5573.bin` | DSP program memory |
| TGP tables (sin/cos, atan, 1/x, 1/√x) | | `opr14742.bin`, `opr14743.bin` (low / high halves) | DSP table units |
| TGP data ROM (4 byte lanes) | | `mpr-14898.39` … `mpr-14901.42` | DSP data ROM window |
| Polygon (model) ROM | not wired up yet | `mpr-14890.26` … `mpr-14897.33` (16-bit halves of 32-bit words) | 16 MB model ROM for 3D objects |
| I/O board firmware (Z80) | `epr-14869b.25` | `epr-14869.25` | I/O board ROM |
| I/O board EEPROM defaults | | `93c45.bin` | 93C45 EEPROM |

**The `model1io` device set.** In current MAME sets the I/O board's firmware (`epr-14869.25`, `epr-14869b.25`) and its EEPROM defaults (`93c45.bin`) aren't in the game's folder: they're in a separate `model1io` set shared by every Model 1 game. Unzip it into a `model1io` folder **next to** the game folder; the loader searches there too:

```
roms/
  vr/          the vr set, unzipped
  model1io/    the model1io set, unzipped
```

If only `model1io.zip` is there, the loader says it needs unzipping.

**Validation:**
- The loader checks that every needed file is present with the exact size before reading anything, and reports each problem by file name.
- Loading is all-or-nothing: an incomplete set leaves memory untouched.
- CRC32 checksums are compared with MAME's; a mismatch (bad dump or other revision) is reported but still loaded.
- **Optional files**, each with a fallback:
  - TGP program, tables and data ROM: without all of them, the high-level TGP stands in (Virtua Racing doesn't run correctly with it).
  - Polygon ROMs: 3D objects are missing. Some older Virtua Racing sets name `mpr-14897.33` as `mpr-14879.33` (same contents); both names are accepted.
  - I/O board firmware: a warning is printed and `InputManager`'s high-level stand-in replaces the board.
  - `93c45.bin`: the EEPROM starts erased, and the game rebuilds its settings on the first boot.
- **Known bad dump:** the `315-5573.bin` with CRC32 `0xec913af2`, common in older sets, makes Virtua Racing hang waiting for TGP results ([MAME Testers 07025](https://mametesters.org/view.php?id=7025)); the loader names it and the corrected dump (CRC32 `0x3335a19b`, MAME 0.197 and later).
- The other files of a set are recognised but not used: Virtua Racing's spare tables and geometrizer firmware (`opr-14744`–`14748`, `315-5571`, `315-5572`; MAME doesn't use them either), and Virtua Fighter's TGP program and polygon ROMs.

After loading, the machine is reset so every CPU fetches its reset vectors from the new ROMs. Wherever a CPU meets an instruction it doesn't implement, it stops and the log shows where.

## Testing

Unit tests live in `tests/` and build into `emulator_tests`, a standalone executable that links only the emulator core (`model1_core`): no SDL and no window. The framework is a small header, `tests/test_framework.hpp`, with no external dependencies.

```sh
cmake -S . -B build
cmake --build build --target emulator_tests
./build/emulator_tests              # run all tests
./build/emulator_tests sub_         # run only tests whose name contains "sub_"
./build/emulator_tests --verbose    # also show each test's emulator log
cd build && ctest --output-on-failure   # through CTest (runs emulator_tests and host_tests)
./build/host_tests                  # SDL audio tests: device lifecycle, resampler
```

`host_tests` links SDL and opens audio devices. It uses SDL's `dummy` audio driver, plus the real device when one is available. A watchdog aborts the run if a test hangs, for example on a deadlock with SDL's audio thread.

The output is a checklist. A failure shows the file and line, the expected and actual values in hex, and the emulator log captured during that test:

```
Running emulator tests

  [PASS] v60_reset_state
  [PASS] v60_nop_advances_pc_only
  [PASS] v60_mov_immediate_to_register
  ...
  [PASS] v60_bz_bnz_follow_zero_flag
  ...
  [PASS] v60_division_by_zero_is_safe
  ...
  [PASS] v60_test_sets_zero_and_sign_without_writing
  [PASS] tilemap_color_decoding
  ...
  [PASS] tilemap_demo_hud_text_renders
  [PASS] polygon_rendering_requires_enable_mask
  ...
  [PASS] polygon_demo_spins_between_frames
  [PASS] uart_reset_state_and_mode_timing
  ...
  [PASS] rom_bad_folders_and_game_names

241 passed, 0 failed
```

The exit code is 0 only if every selected test passed.

**Memory-safety run:** build the tests with sanitizers and bounds-checked containers, in a separate build directory:

```sh
cmake -S . -B build-asan -DMODEL1_SANITIZE=ON
cmake --build build-asan --target emulator_tests
./build-asan/emulator_tests
```

The worst-case tests (maximum tile numbers, palettes and scroll values, every register set to `0xFFFF`) then prove that no lookup leaves its buffer. Without the bounds-checked containers, AddressSanitizer alone would miss an overrun from one memory array into the next one inside the same object.

**Writing a test:** each test builds an isolated machine (a fresh `Bus` and a reset `V60`), writes instruction bytes into RAM, runs `execute_cycle()` and checks registers, PC and PSW:

```cpp
TEST_CASE(v60_sub_r0_from_r0_sets_zero_flag)
{
    CpuRig rig;
    rig.cpu->set_reg(0, 0x1234);
    rig.load({0xAC, 0x60, 0x60}); // SUB.W R0, R0
    rig.step();
    CHECK_EQ(rig.cpu->reg(0), 0u);
    CHECK(rig.flag(k_z));
}
```

Everything the emulator writes to `std::cerr` during a test is captured, and `model1_test::captured_log()` lets a test assert on its own diagnostics. Add new test files to the `emulator_tests` target in `CMakeLists.txt`.

## Hardware reference

### Memory map

24-bit addresses, 16-bit data bus, from the real Model 1 board:

| Address | Contents |
|---|---|
| `0x000000–0x0FFFFF` | Program ROM (read-only) |
| `0x100000–0x1FFFFF` | Data ROM window: one 1 MB bank of the 8 MB data ROM (read-only) |
| `0x200000–0x2FFFFF` | Program ROM: the game's main program (read-only) |
| `0x400000–0x40FFFF` | Work RAM A (battery-backed on hardware) |
| `0x500000–0x53FFFF` | Work RAM B |
| `0x600000–0x61FFFF` | Display list RAM: two 64 KB lists |
| `0x680000–0x680003` | Display list control (list selection, double buffering, render enable) |
| `0x700000–0x70FFFF` | Tile RAM: tilemaps, scroll registers, window masks |
| `0x720000`, `0x740000`, `0x760000`, `0x770000` | Video sync registers (write-only, accepted and ignored) |
| `0x780000–0x7FFFFF` | Character RAM: 8×8 tile graphics |
| `0x900000–0x903FFF` | Palette RAM |
| `0x910000–0x91BFFF` | Colour translation RAM |
| `0xC00000–0xC00FFF` | I/O board shared RAM (MB8421, 2 KB): byte *n* at `0xC00000 + 2n`; inputs at bytes 0–14, lamps at byte 15, the rest is the game/firmware protocol |
| `0xC40000–0xC40003` | Sound command UART (i8251): data at `0xC40000`, control/status at `0xC40002` (byte accesses) |
| `0xD00000–0xDDFFFF` | TGP ports |
| `0xE00000–0xE00FFF` | System registers (interrupt controller, timers): a read/write latch, not yet acted on |
| `0xE00004` | Data ROM bank register: writing `(bank << 4) | 1` selects the bank (takes priority over the latch) |
| `0xF80000–0xFFFFFF` | Boot ROM (read-only, holds the reset address) |

Any other address is unmapped: reads return 0, writes are ignored, and both are logged.

The V60 also has a separate **I/O address space**, reached only by `IN`/`OUT`. The TGP ports are mirrored there at the same addresses, as in MAME. `0xC10000–0xC10003` is a write-only stub for a serial controller.

### V60 interrupts

When an interrupt is pending and PSW.IE (bit 18) is set, the CPU:

1. switches to the interrupt stack and clears IE;
2. pushes the old PSW, then the return PC;
3. jumps to the handler address stored in vector table entry `vector + 0x40`, at `(SBR & ~0xFFF) + entry × 4`.

RETIS undoes all of this. VBlank is requested at the end of every frame as vector 1, Model 1's VBlank interrupt level.

### Inputs (I/O board)

The V60 sees the inputs only as bytes in the shared RAM, written by the I/O board firmware (or the stand-in). Ports are active-low: a bit reads 0 while its input is held, and an idle port reads `0xFF`.

| Address | Contents |
|---|---|
| `0xC00000–0xC0000E` | Analog channels 0–3, then again 0–3 (bytes 0–7) |
| `0xC00010` | System port (byte 8) |
| `0xC00012` | Player 1 port (byte 9) |
| `0xC00014` | Player 2 port (byte 10) |
| `0xC0001E` | Lamps and coin meters (byte 15, written by the game) |

| Bit | System (VF) | Player 1 / 2 (VF) | System (VR) | Player 1 (VR) |
|---|---|---|---|---|
| 0 | Coin 1 | Button 1 | Coin 1 | VR4 (green) |
| 1 | Coin 2 | Button 2 | Coin 2 | |
| 2 | Test | Button 3 | Test | |
| 3 | Service | | Service | |
| 4 | Start 1 | Down | Start | Shift down |
| 5 | Start 2 | Up | VR1 (red) | Shift up |
| 6 | | Right | VR2 (blue) | |
| 7 | | Left | VR3 (yellow) | |

**Virtua Racing analog channels** (MSM6253 ADC): 0 wheel (`0x80` centre, lower to the left), 1 accelerator and 2 brake (`0x30` released, `0xFF` fully pressed), 3 unconnected (`0xFF`). Virtua Fighter has no analog controls (all `0xFF`).

### TGP

The real TGP board's ports are listed in `src/core/tgp_copro.hpp`. The tables below describe the **high-level fallback** (`src/core/tgp.*`), whose function numbers follow the Virtua Fighter firmware.

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

### Sound board

The main board talks to the sound board only through the serial link: the V60 writes bytes to its UART at `0xC40000`, and they arrive at the sound board's UART at `0xC20000`, raising 68000 interrupt level 2.

| Sound address | Contents |
|---|---|
| `0x000000–0x03FFFF` | Sound program ROM (reset vectors at 0 and 4) |
| `0x080000–0x09FFFF` | Mirror of ROM `0x20000–0x3FFFF` |
| `0xC20000–0xC20003` | UART: data at `0xC20001`, control/status at `0xC20003` (8-bit, odd addresses) |
| `0xD00000` | YM3438 FM chip: timers emulated, FM synthesis not yet |
| `0xC40000–0xC40007` | MultiPCM 1: data `0xC40001`, slot select `0xC40003`, register select `0xC40005` |
| `0xC50000–0xC50001` | MultiPCM 1 sample bank (megabyte seen at chip address `0x100000`) |
| `0xC60000–0xC60007`, `0xC70000` | MultiPCM 2 and its bank register |
| `0xF00000–0xF0FFFF` | Work RAM (64 KB as in MAME; the real PCB has 16 KB) |

**MultiPCM slot registers** (select the slot, then the register, then write the data):

| Register | Meaning |
|---|---|
| 0 | Pan, bits 7–4: 0 = centre, 1–7 = left louder, 8 = muted, 9–15 = right louder |
| 1 | Sample number, low 8 bits (bit 8 is register 2 bit 0). Writing it loads the sample header |
| 2, 3 | Pitch: octave = register 3 bits 7–4 (signed); pitch = register 3 bits 3–0 and register 2 bits 7–2. Step = 2^(octave−1) × (1 + pitch/1024) samples per output sample |
| 4 | Bit 7: key on (start from the beginning) or key off |
| 5 | Attenuation, bits 7–1 (0 = loudest, 0.375 dB per step) |
| 6–10 | LFO and envelope parameters (stored; not emulated yet) |

Sample header (12 bytes per sample at the start of sample ROM): start address (3 bytes; bit 22 = 12-bit format), loop start (2 bytes), 0x10000 minus the length (2 bytes), then LFO and envelope settings.

**UART programming** (both sides): write a mode byte to the control register (for example `0x4E`: asynchronous, ×16 clock, 8 data bits, no parity, 1 stop bit), then a command byte (bit 0 transmit enable, bit 2 receive enable, bit 4 error reset, bit 6 back to mode). Then read and write data bytes. Status bits: 0 TxRDY, 1 RxRDY, 2 TxEMPTY, 3 parity error, 4 overrun, 5 framing error.

### 2D layers (System 24 tilemap chip)

**Character RAM** (`0x780000`, 512 KB) holds 16,384 tiles of 8×8 pixels at 4 bits per pixel, 32 bytes per tile. Each 16-bit word holds 4 pixels, leftmost pixel in bits 15–12. Pixel value 0 is transparent.

**Tile RAM** (`0x700000`, 64 KB), as 16-bit word offsets:

| Word offset | Contents |
|---|---|
| `0x0000–0x3FFF` | Tilemaps 0–3, 64×64 entries each. Entry bits 13–0: tile number; 14–7: palette (overlaps the tile number's top bits, so palette *p* selects tiles from block *p* × 128); 15: high priority |
| `0x4000–0x47FF` | Per-line horizontal scroll tables, 512 words per tilemap |
| `0x5000–0x5003` | Horizontal scroll, tilemaps 0–3: 9-bit value; bit 15 enables per-line scroll. A value *v* moves the layer right by *v* pixels |
| `0x5004–0x5007` | Vertical scroll, tilemaps 0–3: 9-bit value; bit 15 disables the tilemap; bits 14–13 select special split modes (not emulated) |
| `0x6000–0x67FF`, `0x6800–0x6FFF` | Window masks for pairs 0/1 and 2/3: 4 words per line, one bit per 8-pixel column. Even tilemaps draw where the bit is 0, odd ones where it is 1 |

**Palette RAM** (`0x900000`) colours, pen = palette × 16 + pixel:

| Bits | Meaning |
|---|---|
| 0–4 | Red |
| 5–9 | Green |
| 10–14 | Blue |
| 15 | Intensity (0 = half brightness) |

For example, `0x801F` is full red and `0xFFFF` is white.

### 3D display lists

Display lists are 16-bit words; a long is two words, low word first. The **list control register** at `0x680000`: word 0 bit 2 = automatic double buffering (lists swap every second frame), bit 3 = list to use in manual mode, bit 6 = list in use; word 1 must have bits 4–0 set for rendering to run.

| Command | Meaning | Status |
|---|---|---|
| `0x00` | No-op | Done |
| `0x01` | Draw a 3D object from polygon ROM or RAM | Done |
| `0x41` | Draw a 3D object above the HUD | Drawn, but below the HUD for now |
| `0x02` | Direct polygons: a strip of pre-projected quads | Done |
| `0x03` | Viewport: centre and clip rectangle | Done |
| `0x04` | Colour table write | Done |
| `0x05` | Polygon RAM upload | Done |
| `0x06` | Lighting parameters upload | Done |
| `0x07` | Mode (bit 0: specular light) | Done |
| `0x08` | Select mode | Accepted, no known effect |
| `0x09` | Zoom | Done |
| `0x0A` | Light direction | Done |
| `0x0B` | Object matrix | Done |
| `0x0C` | View translation | Done |
| `0x0F` | End of list | Done |

**Direct polygon colours:** colour table entry → palette RAM entry `0x1000 + (entry & 0x3FF)` → each 5-bit channel looked up in colour translation RAM (`0x910000`; red, green and blue tables of `0x2000` words) at `(channel << 8) | level`, where level = luminance byte ÷ 2, clamped to 63.

## Known limitations

- **Virtua Racing:** playable, with these gaps:
  - **3D:** objects above the HUD (`0x41`) are drawn below it; a few stray single-pixel points appear in some scenes (degenerate polygons drawn as points). The renderer is a high-level simulation like MAME's, so exact pixel coverage and the colour/luminance path may differ from the real board.
  - **2D:** the tilemap chip's special split modes are drawn as normal mode (a warning is logged); VR uses special mode 1 on tilemaps 2/3.
  - **Sound:** no FM music or effects (YM3438 synthesis missing). On a boot with valid saved settings, the first sound command (`0x81`) can be lost (see *Timing* below).
  - **Drive board** (force feedback, port E) isn't emulated.
- **Virtua Fighter:** the set loads, but its TGP program (`315-5724.bin`) and polygon ROMs aren't wired up, so it doesn't run. It falls back on the high-level TGP, which has only a few functions.
- **CPU:** some V60 instructions are still missing, notably PREPARE/DISPOSE, the downward string searches (SCHCD/SKPCD), string compares, the other bit-string instructions and double-precision floating point (MAME doesn't implement those either). Every instruction is charged a flat 8 cycles.
- **Floating point:** single precision, IEEE results; FPU exceptions (division by zero, overflow) aren't raised. CVTSW rounds to nearest-even by default (MAME rounds halves away from zero).
- **Division by zero** leaves the destination unchanged, sets OV and logs an error; the V60's zero-divide exception is not emulated. Setting OV is our choice: MAME leaves it clear. Exceptions in general (including reserved addressing modes) halt the CPU instead of trapping.
- **Flag rules that differ from MAME:** signed MUL sets OV when the result doesn't fit in the operand size; MAME sets it whenever the upper bits of the product are non-zero, which also flags small negative results. DIVX/DIVUX set OV and leave the destination unchanged when the quotient doesn't fit in 32 bits; MAME doesn't handle that case. Neither has been checked against NEC's documentation.
- **SHA overflow on left shifts** uses the standard definition (the result doesn't fit in the operand size). MAME only checks the bits shifted out, so the two differ when a shift changes the sign. This has not been checked against NEC's documentation.
- **Interrupts are latched, not level-triggered.** The board's interrupt controller is not emulated. Its registers at `0xE00000`–`0xE00FFF` are a 4 KB latch: writes are stored and read back (each byte's first write is logged), but nothing acts on them yet. The data ROM bank register at `0xE00004` still works.
- **V60 I/O-space port `0xC10002`** is a stub. VR's boot writes the i8251 UART reset/mode sequence there (`00 00 00 40 4E`), so it is likely a serial controller. Writes are logged and ignored, and reads stay logged as unmapped. MAME has nothing there either.
- **Frame rate:** 60 Hz. The real hardware runs at about 57.52 Hz (278,144 CPU cycles per frame), and VBlank starts partway through the frame.
- **TGP:** fixed-point mode and DSP interrupts aren't emulated, as in MAME.
- **I/O board:** the EEPROM isn't saved between runs, so the operator settings reset to the defaults each run. The shared RAM's interrupt mailboxes aren't emulated.
- **Inputs:** keyboard only: no game controllers, real analog wheel or player 2 keys.
- **Timing:** the V60 is charged a flat 8 cycles per instruction (as in MAME). On a boot with valid saved settings this lets VR send its first sound command (`0x81`) before the sound CPU has enabled its UART receiver, so the byte is lost, as the real i8251 would lose it; real V60 timings would likely fix this.
- **Sound:** the 68000 core implements only the instructions the VR sound program has needed so far. MultiPCM envelopes (a keyed-on voice plays at full level and stops at key-off), LFOs, gradual volume changes, reverse playback and the effect send are not emulated; the YM3438 only has its timers (Timer A/B, flags, control register 0x27), so there is no FM audio yet; its interrupt output isn't wired to the 68000. Audio is paced by the frame loop, so a slow drift between the host's audio and video clocks could occasionally cause a short gap or a dropped chunk (capped at 100 ms of latency). The main board side of the UART raises no interrupt yet, because the main interrupt controller isn't emulated.

## Roadmap

Planned next steps, roughly in order:

1. **Virtua Racing polish:** draw `0x41` objects above the HUD, remove the stray points, the tilemap chip's special mode 1, and save the EEPROM between runs.
2. **Interrupt controller** at `0xE00000`, with level-triggered VBlank and a real acknowledge.
3. **Sound:** YM3438 FM synthesis, then MultiPCM envelopes and LFOs.
4. **Virtua Fighter:** wire up its TGP program (`315-5724.bin`) and polygon ROMs, then fix whatever its code needs next.
5. **Real video timing:** 57.52 Hz, with VBlank at line 384.
6. **Game controllers:** SDL gamepads and analog sticks for the wheel and pedals; player 2 keys.
7. **More games:** manifests for the other Model 1 sets (Star Wars Arcade, Wing War, NetMerc).
