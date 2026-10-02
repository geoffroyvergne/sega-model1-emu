# Sega Model 1 Emulator

An emulator for the Sega Model 1 arcade board, written in C++20 with SDL2. Supported ROM sets: **Virtua Racing** (`vr`), **Virtua Fighter** (`vf`) and **Star Wars Arcade** (`swa`).

> **Status: Virtua Racing, Virtua Fighter and Star Wars Arcade are playable.**
>
> **Star Wars Arcade:** it boots into attract mode and plays its missions in 3D: pilot or pilot-and-gunner mode, mission select, the Admiral Ackbar briefing, the hyperspace jump, and space battles with TIE fighters, Star Destroyers and asteroids, with the radar blips drawn above the cockpit HUD. Sound effects and music both play: the music comes from the separate Digital Sound Board (a Z80 driving an MPEG audio decoder), emulated with its own MPEG-1 Layer II decoder. The game takes **two coins per credit** by default.
>
> **Virtua Fighter:** it runs its attract mode, player select and full fights (two players), with sound. MAME marks it not working because its only known TGP program dump (`315-5724.bin`) is flagged as bad, but it runs here without visible problems so far.
>
> **Virtua Racing:** It boots through its test-mode screen into attract mode with full 3D (courses, cars, the Bay Bridge, a 3D SEGA logo). A coin and the accelerator start a race, and the car steers and accelerates; the brake, view buttons and shifter are wired to the keyboard too. All the board's processors run their real firmware: the NEC V60 main CPU, the MB86233 TGP geometry DSP, the Z80 I/O board and the 68000 sound CPU. The sound board is complete (two MultiPCMs and the YM3438 FM chip), as is the main board's interrupt controller. A few 3D details are wrong (see [Known limitations](#known-limitations)).

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
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./build-release/model1 path/to/vr                          # an unzipped MAME "vr" ROM set
```

Press **5** to insert a coin. On the course screen, hold **→** until your course is highlighted, then press **↑** (accelerator) to start. Steer with **← →** and brake with **↓**. A game controller works too: plug it in, Back is the coin, the left stick steers and the triggers are the pedals. The [Controls](#controls) section lists every key, and [ROM sets](#rom-sets) lists the files the emulator needs, including the shared `model1io` set.

## Purpose

The Sega Model 1 (1992) was Sega's first 3D arcade board. It combines an NEC V60 main CPU, a Fujitsu MB86233 DSP for geometry (the "TGP"), a polygon renderer, a Z80-based I/O board, and a separate 68000 sound board.

This project aims to emulate that board faithfully, with code that is easy to read and audit. Readability and correctness come before speed: every hardware behaviour is implemented explicitly, and anything that is not real hardware is labelled as such in the code and in this document.

## Strategy

**Build in small, verifiable steps.** Each subsystem starts as a skeleton (state, interfaces, lifecycle), then gets its core behaviour, then a few real operations, then refinement. Every step ends with a clean build and checks of the new behaviour.

**Verify hardware facts before writing them.** Opcode encodings, flag rules, the interrupt sequence, the memory map, TGP function IDs, input bit layouts and colour formats were all checked against MAME's Model 1 and V60 source code (BSD-3-Clause), not written from memory. MAME is used as a reference for facts only; no MAME code is copied.

**Run the real firmware wherever possible.** Three subsystems on the board run their own programs, and each is emulated at the hardware level, with a high-level stand-in for when its ROMs are missing:

| Subsystem | Real hardware | This emulator | Stand-in without the ROMs |
|---|---|---|---|
| TGP | MB86233 DSP running a per-game program | The DSP core runs the program (all three games) | C++ versions of a few geometry functions |
| I/O board | Z80 running EPR-14869, copying inputs into shared memory | The Z80 runs the firmware | `InputManager` writes the shared-memory layout directly |
| Sound board | 68000 running the sound program | The 68000 runs it | none (the sound program is a required file) |
| Digital Sound Board (SWA music) | Z80 running EPR-16471, driving an MPEG audio decoder | The Z80 runs the program; the MPEG decoder is emulated | none: no music without its ROMs |

Both stand-ins follow the layouts MAME used before it switched to low-level emulation, and they sit behind the same hardware ports, so the V60 sees the same machine either way.

**Fail loudly, never guess.** Unimplemented opcodes, unsupported addressing modes, unmapped memory accesses, unknown TGP functions and unknown I/O offsets are all logged with the address and value involved. The CPU halts on anything it cannot execute instead of skipping it.

**Keep the emulated machine separate from the host.** Everything under `src/core` is plain C++ with no SDL. Window, rendering and keyboard handling live in host-side code that talks to the core through small interfaces.

## Features

### NEC V60 CPU (`src/core/v60.*`)
- 32 general-purpose registers, PC and PSW, plus the privileged registers needed for interrupts: the interrupt stack pointer, four per-level stack pointers, and the system base register. `LDPR`/`STPR` read and write them by number (SBR moves the interrupt vector table); the other privileged registers (task, MMU, debug and system control) are stored and read back with no effect emulated, except TKCW, whose rounding mode CVTSW uses.
- **24-bit address bus**, little-endian, starting at the real reset address `0xFFFFFFF0`.
- **Instructions:** everything Virtua Racing executes, including:
  - Moves and conversions: MOV, MOVD (64-bit), MOVEA, MOVS/MOVZ (sign / zero extend), MOVT (truncate), XCH, SETF, RVBIT/RVBYT (reverse bits / bytes), UPDPSW, GETPSW, TASI (test and set).
  - Arithmetic in byte, half and word sizes: ADD/SUB, ADDC/SUBC, CMP, INC/DEC, NEG, MUL/MULU, DIV/DIVU, REM/REMU. There are also 64-bit forms: MULX/MULUX (32×32→64) and DIVX/DIVUX (64÷32 giving a quotient and a remainder), using a register pair or an 8-byte memory operand.
  - Logic and shifts: AND, OR, XOR, NOT, TEST, SHL (logical shift), SHA (arithmetic shift), ROT/ROTC. Shift counts are signed: positive shifts left, negative shifts right.
  - Single-bit operations on a 32-bit word: TEST1, SET1, CLR1, NOT1. CY receives the previous bit value and Z its inverse.
  - Bit fields: EXTBFS/EXTBFZ/EXTBFL, INSBFR/INSBFL.
  - Bit strings: SCH0BSU/SCH1BSU (find the first 0 / 1 bit), MOVBSU/MOVBSD (copy a bit string upward / downward).
  - Strings: MOVCU/MOVCFU/MOVCSU, MOVCD/MOVCFD, SCHCU/SKPCU, SCHCD/SKPCD, CMPC/CMPCF/CMPCS.
  - Decimal (BCD): ADDDC, SUBDC, SUBRDC, CVTDPZ / CVTDZP (packed ↔ unpacked).
  - Single-precision floating point: MOVF, ADDF, SUBF, MULF, DIVF, CMPF, NEGF, ABSF, SCLF, CVTWS / CVTSW (rounding set by the TKCW register).
  - Stack: PUSH, POP, PUSHM and POPM (register lists, optionally including the PSW).
  - Control flow: BR, the 14 conditional branches (8- and 16-bit displacements), DBcc and TB (decrement and branch), JMP, JSR, CALL/RET, BSR/RSR, PREPARE/DISPOSE (stack frames), RETIS/RETIU, NOP and HALT.
  - System: LDPR/STPR (privileged registers), IN/OUT (the V60's separate I/O space).
- **Addressing modes:** all of them. That covers register; register indirect; autoincrement/decrement; 8/16/32-bit displacement; displacement-indirect and double displacement; PC-relative, absolute and their indirect forms; immediates; and the indexed variants, where an index register is scaled by the operand size (×1, ×2, ×4, or ×8 for 64-bit operands). Autoincrement and autodecrement step by the operand size, exactly once per instruction. One decoder (`decode_operand`) serves every instruction.
- **Bit addressing** (bit-field group `0x5D`: `EXTBFS`/`EXTBFZ`/`EXTBFL`, `INSBFR`/`INSBFL`): the same decoder yields a base address plus a signed bit offset. Displacements and index registers count bits instead of being added to the address. Fields are 1–32 bits and may cross a 32-bit boundary; only the bytes a field spans are read or written.
- **Flags:** Zero, Sign, Overflow and Carry follow the hardware rules. For example, logic operations clear OV and leave CY unchanged, and byte/half results update only the low bits of a register.
- **Stack checks:** every push and pop, including interrupt entry, is checked. A misaligned SP, or a stack access outside work RAM (an overflow on push, an underflow on pop), is logged as a warning (the first 8 times). Execution continues, because the V60 itself has no stack limits.
- **Maskable interrupts:** pending requests are taken between instructions when PSW.IE is set. The CPU switches to the interrupt stack, saves PSW and PC, and jumps through the vector table. HALT waits for an interrupt; RETIS returns from one.
- **Verified against MAME's V60:** a differential test (not in the repository) compiled MAME's V60 core standalone and ran it in lockstep with this one on Virtua Racing (attract mode, a coin, course select, a race and an off-track excursion: 80 million instructions). Every instruction was compared: registers, PC, PSW and the bytes written. The differences are the deliberate ones listed under [Known limitations](#known-limitations):
  - CVTSW ties: −194.5 converts to −194 here and to −195 in MAME.
  - CVTSW of infinity.
  - The overflow flag of MUL.W and SHA.H, which VR never branches on.

  Switching CVTSW to MAME's behaviour left the game's run unchanged.

### Memory map and bus (`src/core/bus.*`)
- The real Model 1 layout: program and boot ROM, two work RAMs, display list, palette and colour RAM.
- ROM is read-only and reads `0xFF` until loaded, like an erased EPROM.
- **Device ports:** devices register 16-bit read/write handlers on address ranges, with mirroring. A 32-bit access to a port becomes two 16-bit accesses, low half first, as on the board's 16-bit data bus.
- Raw binary ROM images can be loaded at any ROM address.
- **Interrupt controller and timers** (`src/core/interrupt_controller.*`, the board's GLUE chip at `0xE00000`, as in MAME): a pending bit per level, a mask, acknowledge cycles; VBlank (level 1), the main UART (level 3) and two programmable timers (level 0, every 2,048 × *period* CPU cycles, with readable counts) as sources. Star Wars Arcade relies on the timers. See [V60 interrupts](#v60-interrupts).
- **Unmapped accesses** are logged once per address (memory and I/O space separately), so a game polling an empty address doesn't flood the log.

### TGP geometry coprocessor (`src/core/tgp_copro.*`, `src/core/mb86233.*`, `src/core/tgp.*`)
- **Real TGP (all three games):** with the TGP program (`315-5573.bin` for VR, `315-5724.bin` for VF, `315-5711.bin` for SWA) and tables (`opr14742`/`opr14743`) loaded, plus the data ROMs (VR `mpr-14898`–`14901`, SWA `mpr-16472`–`16475`; VF has none), the board is emulated at the hardware level, as in current MAME:
  - **MB86233 DSP** (`src/core/mb86233.*`), a port of MAME's core: loads, moves between registers, RAM, I/O and program memory, the float ALU (add, subtract, multiply, multiply-accumulate, divide, compare, conversions) and integer operations, repeat, loop counters, a 4-entry call stack. Floating-point mode only, no interrupts (as in MAME). 40 MHz / 3 instructions per second, in lockstep with the V60.
  - **The board** (`src/core/tgp_copro.*`): 16-word FIFOs both ways, an 8K-word copro RAM with four auto-incrementing address registers (stride 4 for vertex lists), the sin/cos, atan, 1/x and 1/√x table units, and the 2 MB data ROM window. On the board the V60 halts on an empty output FIFO or a full input FIFO; here the DSP runs on the spot until it can proceed.
  - **Runs alongside the V60:** the DSP advances after every V60 instruction (5 DSP instructions per 6 V60 cycles), as it runs concurrently on the board. The two share the copro RAM. A DSP running in batches, once per 4,096-cycle slice, lagged up to about 500 V60 instructions behind, and then read RAM the V60 had already rewritten for its next request. In Virtua Racing that corrupted the collision and ground queries: the car could leave the track into a void where the road disappeared, with track pieces floating in the sky. A regression test sends a TGP request from V60 code and checks that the answer is ready two instructions later.
  - **Debugging:** `TgpCopro::set_trace(n)` logs FIFO traffic, `Mb86233::set_trace(n)` logs DSP instructions with registers.
  - **Old firmware dump:** the `315-5573.bin` with CRC32 `0xec913af2` (common in older sets) is a bad dump that MAME replaced in 0.197 (CRC32 `0x3335a19b`): with it the game hangs waiting for TGP results ([MAME Testers 07025](https://mametesters.org/view.php?id=7025)). The loader recognises it and says so.
- **High-level TGP (fallback):** without a TGP program and tables, `src/core/tgp.*` stands in:
  - The real host interface: a command FIFO, a status port, and a shared-RAM port with auto-increment.
  - Geometry functions: matrix write, read, multiply, identity, translate and push/pop on a 32-deep stack, plus transform point. These use the real 4×3 matrix layout and IEEE floats.
  - An emulator-only perspective projection function for debugging.
- **Startup self-test:** drives the TGP through the bus exactly as game code would and checks the results bit-for-bit.

### I/O board and inputs (`src/core/io_board.*`, `src/core/z80.*`, `src/core/eeprom_93c46.*`, `src/core/dual_port_ram.*`, `src/core/input_manager.*`, `src/input/keyboard_input.*`, `src/input/gamepad_input.*`)
- **Shared RAM:** the main CPU reaches the I/O board (837-8950: a Z80 running EPR-14869, a 315-5338A I/O chip, an ADC and a 93C45 EEPROM) only through a 2 KB **MB8421 dual-port RAM** at `0xC00000–0xC00FFF`, as in current MAME. Byte *n* sits on the V60's low byte lane at `0xC00000 + 2n`; the high lane isn't connected.
  - It has no fixed registers: the meaning of each byte is a protocol between the game and the board firmware. Virtua Racing, for example, writes `SEGA` at bytes `0x1A–0x1D`, `0x01` at byte `0x20` (`0xC00040`), then reads and rewrites a 128-byte block at bytes `0x100–0x17F` (`0xC00200–0xC002FE`), the size of the board's EEPROM.
  - Not emulated: the RAM's interrupt mailboxes and access wait states.
- **The I/O board runs its real firmware** (`epr-14869.25` for Virtua Racing, `epr-14869b.25` for Virtua Fighter and Star Wars Arcade), as in MAME's `model1io` device:
  - **Z80 at 4 MHz** (`src/core/z80.*`): the full documented instruction set with the CB / ED / DD / FD / DDCB prefixes, IM 0/1/2 and NMI, and T-state timing, clocked in lockstep with the V60 (a quarter cycle per V60 cycle).
  - **Memory map:** 16 KB of the firmware EPROM, 8 KB RAM, the **315-5338A** I/O controller (7 ports, and the address-latch / data registers through which the Z80 reads and writes the shared RAM) and the **MSM6253** ADC (4 inputs read one bit at a time; port A bit 0, the same line that selects the DIP switches, switches them from channels 0–3 to channels 4–7 through two analog switches, as in MAME).
  - **Ports:** inputs on B–D from `InputManager` (or the three DIP-switch banks, all off, when port A bit 0 selects them), EEPROM lines on A and G, lamps on F, drive board on E (not emulated).
  - **93C45 EEPROM** (`src/core/eeprom_93c46.*`, 64 × 16 bits): the serial protocol of MAME's 93Cxx device, write-protected at power-on, with MAME's programming times (write 1.75 ms, erase 1 ms, write/erase all 8 ms), which also set how long the game's boot waits. It starts from MAME's factory defaults (`93c45.bin` in the `model1io` set) when present, otherwise erased, and isn't saved between runs yet.
  - **Effect on Virtua Racing:** the firmware answers the boot handshake, copies the EEPROM settings into the shared RAM (bytes `0x100–0x17F`) and saves the defaults the game writes back; it publishes the analog channels at bytes 0–7 and the input ports at 8 and up.
- **Control panels** (`InputManager::Profile`, chosen by the ROM loader from the game):
  - **Virtua Fighter:** two joysticks and three buttons each, on ports B–D.
  - **Star Wars Arcade** (MAME's `swa` ports): the pilot's flight stick on channels 0 (X) and 1 (Y), the throttle on channel 2, the gunner's stick on channels 4 and 5. Sticks rest at `0x7F` and range from 27 to 227 (X: 227 = left; Y: 27 = up, i.e. stick pushed forward). The throttle runs from 200 (slow) to 28 (fast). Buttons on port C: bit 0 pilot trigger, bit 1 pilot torpedo, bit 2 gunner trigger, bit 3 gunner torpedo, bit 4 the pilot's VR (view) button; start 1 / start 2 on the system port. On the keyboard the sticks spring back to centre, and the throttle is a lever: it moves while its key is held and stays where it's left.
  - **Virtua Racing** (MAME's `vr` ports): the four view buttons VR1–VR3 on the system port (bits 5–7) and VR4 on port C bit 0; the shifter on port C bits 4 (down) and 5 (up). The steering wheel, accelerator and brake are analog, read through the ADC: wheel on channel 0 (centre `0x80`, lower to the left), accelerator on 1 and brake on 2 (released `0x30`, fully pressed `0xFF`), channel 3 unconnected (`0xFF`).
  - On the keyboard the wheel and pedals are driven by held keys, moving a step per frame toward their target (full lock or full press). The pedals spring back when released.
  - **The wheel acts like the cabinet's.** While a pedal is held (racing), it turns quickly and springs back to centre on release. With no pedal held (menus), it turns slowly (about half a second from centre to full lock) and stays where it's left.
    - Why: Virtua Racing picks the course from the wheel's absolute position: centred is course 1, about `0xB0` course 2, `0xD0` and up course 3. A keyboard wheel that always re-centred, like MAME's key-driven paddle, would always fall back to course 1.
    - The spring starts ⅓ second after a pedal goes down, so the wheel holds still while the game reads the confirmation.
- **Fallback stand-in:** without the firmware file (it's optional in the ROM loader), `InputManager` plays the board at a high level: it writes the ports into the shared RAM using MAME's former high-level layout (the analog channels at bytes 0–7: for Star Wars Arcade all eight, for the other games channels 0–3 twice; system port at 8, players at 9 and 10, lamps at 15) acknowledges the game's commands in byte `0x20` once per frame without performing them, and reports the board ready (`0x40` in byte `0x21`, which Virtua Fighter waits for at boot, as MAME's former high-level driver did). Virtua Racing and Virtua Fighter run on it (Star Wars Arcade hasn't been tried without the firmware), but the real firmware is preferred: unzip MAME's `model1io` set next to the game folder.
- **Keyboard mapping:** keys are matched by physical position, so the layout works on QWERTY and AZERTY.
  - Keys with the same function (W and ↑) can be held together without releasing each other.
  - Auto-repeat is ignored.
  - Every input is released when the window loses focus, so no key gets stuck.
- **Game controllers** (`src/input/gamepad_input.*`, SDL's GameController API): any pad SDL recognises (Xbox, PlayStation, Switch Pro and many others), up to two (the first connected plays player 1, the second player 2), plugged in or out at any time. The keyboard and the pads share each input through `SharedPresses`, so an input stays pressed until every key and button holding it is released.
  - **Analog controls** go to `InputManager::set_axis` (dead zone 15 % on sticks, 5 % on triggers, rescaled so full deflection still reaches the end):
    - **Virtua Racing:** the wheel follows the left stick in proportion while racing (a pedal held, as for the keys). In menus, a stick pushed past halfway acts like the arrow keys, so the wheel stays on the chosen course when you let go. The triggers are proportional pedals.
    - **Star Wars Arcade:** the left sticks are the pilot's and gunner's flight sticks, proportional over MAME's range. The triggers move the throttle lever, faster the harder they're pressed.
    - **Virtua Fighter:** the left stick acts as the joystick (pressed past 50 %, released below 35 %, so it doesn't chatter).
  - **Extra mappings:** a `gamecontrollerdb.txt` file (SDL's community controller database format) in the working directory or next to the executable is loaded at startup, for pads SDL doesn't know.

### 2D layers (`src/core/tilemap_renderer.*`)
- The Model 1's 2D hardware, the Sega System 24 tilemap chip, renders each 496×384 frame from tile RAM, character RAM and palette RAM.
- **Tilemaps:** four 512×512-pixel tilemaps of 8×8 tiles, 4 bits per pixel, with 16-colour palettes. Each pair of tilemaps shares the screen through a per-line window mask with 8-pixel columns.
- **Scrolling:** horizontal and vertical scroll per tilemap, with optional per-line horizontal scroll and a per-tilemap disable bit.
- **Split modes:** a tilemap pair can be drawn as one layer split at a line (mode 1) or a column (modes 2/3), as MAME's `segaic24`. Virtua Racing draws its sky and landscape this way (see [2D layers](#2d-layers-system-24-tilemap-chip)).
- **Layer order:** the board's fixed order. Low-priority tilemaps 3 and 2 form an opaque background, then tilemaps 1 and 0 draw with transparency, then (once implemented) the 3D polygons, then every tilemap's high-priority tiles on top for HUD and text.
- **Colours:** palette entries use the Model 1 format (xBGR 5:5:5 plus an intensity bit) and are decoded once per frame.
- **Speed:** with both demos running, a whole frame (2D layers plus polygons) takes about 8 ms in a Debug build and under 2 ms optimized, well within the 16.7 ms frame budget.

### 3D polygons (`src/core/polygon_renderer.*`)
- **Display lists:** reads the display lists the game writes into display list RAM. There are two 64 KB lists, double-buffered through the list control register, with automatic or manual swapping and a render-enable mask.
- **3D objects** (command `0x01`): models from the 16 MB polygon ROM (or polygon RAM), as MAME's `model1_v.cpp`: transformed by the object matrix (command `0x0B`), projected with the zoom and view translation (`0x09`, `0x0C`), back-face culled (unless double-sided), lit by ambient + diffuse + optional specular light (light direction `0x0A`, parameters `0x06`, specular enable `0x07`) through the colour translation RAM, clipped to the viewport frustum, and depth-sorted with the direct polygons. Command `0x41` draws the same way in the above-HUD pass (see *Layer order*).
- **Direct polygons:** the V60/TGP send already-projected quad strips (command `0x02`). Each new edge links to the previous one using four link modes.
- **Flat shading:** each quad's colour comes from a colour table (command `0x04`), then palette RAM entry `0x1000 + index`, then the colour translation RAM driven by the quad's luminance.
- **Painter's algorithm:** quads are sorted far-to-near by their z value and drawn whenever the viewport changes and at the end of the list, clipped to the viewport (command `0x03`).
- **Data uploads:** command `0x05` stores 32-bit words into a 16 MB polygon data RAM (object addresses `0x800000` and up), command `0x06` stores lighting parameters (diffuse, ambient, specular, power) in 256 slots, as in MAME. The 3D object renderer reads them. A command whose length runs past the end of the 64 KB list, or that targets an address outside its RAM, is rejected with a warning instead of looping or overflowing. `0xFFFFFFFF` (an erased list) ends the list quietly.
- **Rasterizer:** edge-function fill of each quad as two triangles, a stipple ("moiré") checkerboard mode for translucency, and wireframe lines for quads with only two distinct corners.
- **Layer order:** two passes, as on the board (and MAME). Objects (`0x01`) and direct polygons go between the low- and high-priority tilemaps. Objects above the HUD (`0x41`, e.g. Star Wars Arcade's radar blips) go after the high-priority (HUD) layers, but only through HUD pixels that are near-black (every channel at most 8), MAME's stencil: they show inside the radar screen but under its grid lines and the cockpit frame.

### Video output (`src/video/video_manager.*`)
- 496×384 window that scales with sharp pixels and keeps its aspect ratio when resized.
- Presentation only: each finished frame is copied into a streaming SDL texture and shown.

### Sound board (`src/core/sound_board.*`, `m68000.*`, `sound_bus.*`, `i8251.*`)
The real Model 1 sound board, from MAME's `segam1audio`: a **Motorola 68000 at 10 MHz** with its own address space. The main board sends it commands over a **serial link**: an i8251 UART on each board, cross-connected, at 31.25 kbaud.
- **i8251 UART:** the mode and command registers, status (TxRDY, RxRDY, TxEMPTY, error flags), transmit holding and shift registers, overrun detection, and the SYNC-character bytes that follow a synchronous mode byte (so the usual `00 00 00 40` software reset is handled silently; actually using synchronous mode is logged as not emulated). Serial timing is real: one 8N1 byte at the ×16 clock takes 160 UART clock ticks, 320 µs.
- **Sound bus:** big-endian and 24-bit, with program ROM at `0x000000` (and its mirror), the UART at `0xC20000` and work RAM at `0xF00000`. Above each MultiPCM's 8 ports, the rest of its 64 KB window (`0xC40008–0xC4FFFF`, `0xC60008–0xC6FFFF`) is a latch: the sound program writes control values there (for example a 24-bit value as bytes at `0xC40007/09/0B`), which are stored, read back and logged on first write. MAME ignores those addresses.
- **68000 skeleton:** D0–D7, A0–A7 with separate supervisor and user stacks, PC and SR. Reset loads the stack pointer and PC from the vector table.
  - **Interrupts:** level-sensitive and autovectored. The UART's "receiver ready" signal drives level 2 (vector address `0x68`).
  - **Instructions:** NOP, STOP, RTE, Bcc/BRA/BSR, RTS, JSR/JMP, MOVEQ, SWAP, TST, ADD/SUB/ADDA/SUBA/ADDX/SUBX, CMP/CMPA/CMPM/EOR, OR/AND, MULU/MULS, DIVU/DIVS (worst-case timing), ABCD/SBCD, EXG, MOVEM (all forms), NEG/NEGX/NOT, EXT, ADDQ/SUBQ, the immediate group (ORI/ANDI/SUBI/ADDI/EORI/CMPI, including ORI/ANDI/EORI to CCR and SR), the shifts and rotates (ASx/LSx/ROXx/ROx, register and memory forms), LEA, DBcc, MOVE/MOVEA, MOVE to SR, CLR, and the bit operations BTST/BCHG/BCLR/BSET (static `#n` and dynamic `Dn` forms). All 12 addressing modes are decoded, and cycle counts come from the MC68000 manual. A word or long access at an odd address, or any other opcode, halts with a logged message.
  - **Verified against Musashi:** a differential test (not in the repository) ran Virtua Racing's sound program through this core and through Musashi, a mature 68000 emulator, instruction by instruction, over 40 seconds of play (attract mode, a coin, course select, the start of a race). Registers, flags and memory writes matched on all 41.7 million instructions; the only differences were a few ADDA/SUBA cycle counts (±2 cycles).

### Sound output (`src/audio/multipcm.*`, `src/audio/ym3438.*`, `src/audio/audio_output.*`)
- **MultiPCM (×2):** Sega's 28-voice sample playback chip (Yamaha YMW-258-F), following MAME's `multipcm`/`gew` devices. It's part of the SDL-free core.
  - **Registers:** the port interface (slot select, register select, data) and 12-byte sample headers.
  - **Playback:** octave/pitch stepping, 8-bit and packed 12-bit samples with linear interpolation, and loop points.
  - **Volume:** attenuation (0.375 dB per step) and pan, plus the board's sample-ROM bank registers. Attenuation changes glide when the game asks for it (register 5 bit 0 clear: 128 steps per 78 ms when getting louder, half that speed when getting quieter). Virtua Racing's sound program asks for a glide on about 90% of its volume changes.
  - **Envelopes:** attack, decay 1 down to the decay level, decay 2, and release at key-off, on a 10-bit level spanning 96 dB. Rates come from the YMF278B ("OPL4") tables, scaled by octave and pitch (key rate scaling). Rate 0 holds, and release rate 15 stops the note at once. The sample header loads the settings, and registers 7–9 can change them.
  - **LFOs:** vibrato (pitch, up to ±79 cents) and tremolo (level, up to −24 dB), at 8 speeds from 0.17 to 7 Hz.
  - **Output:** 10 MHz ÷ 224 = 44,642.86 Hz stereo, generated in lockstep with the CPUs, so each register write takes effect at the right sample. Levels match MAME: a full-scale sample at full volume leaves the chip at about −0.1 dB, and the two chips are mixed at half level each.
- **YM3438 (OPN2C) FM synthesis:** the FM chip at 8 MHz, clocked from the 68000's cycle count (exactly 180 CPU cycles per FM sample, 55,555.6 Hz), following MAME's `ymfm` (verified against Nuked-OPN2's die analysis). It's part of the SDL-free core.
  - **Synthesis:** 6 channels of 4 operators, the 8 algorithms, operator 1 self-feedback, detune and multiple, and the chip's log-sin and power tables. The tables are generated from their formulas; all 512 entries match the values read from the die.
  - **Envelopes:** attack (exponential), decay, sustain and release, with rates 0–63 after key scaling, clocked every third sample as on the chip. SSG-EG (repeat, alternate and hold) is included.
  - **LFO:** tremolo (AM, up to 11.8 dB) and vibrato (PM), at the 8 hardware rates (3.98–72.2 Hz).
  - **Other features:** channel 3's per-operator frequencies and CSM key-on (Timer A overflows), the channel 6 DAC (`0x2A`/`0x2B`/`0x2C`), the latched frequency high byte, and per-channel left/right output.
  - **Output:** each channel is clipped to 9 bits and the six are summed, scaled as in MAME (one channel at full volume ≈ ±5,460).
  - **Mix:** the sound board resamples the FM stream to the MultiPCM rate (exactly 56 FM samples per 45 output samples, linear interpolation) and mixes with MAME's gains: 0.5 per MultiPCM, 0.3 for the YM3438.
  - **Timers:** Timer A overflows every (1024 − A) samples, Timer B every (256 − B) × 16; register `0x27` starts/stops them, enables and clears their status flags. The sound program's main loop waits on Timer A, so this paces the sound driver.
  - **In Virtua Racing:** a 30-second run (attract mode, a coin, the start of a race) uses FM channels 5 and 6 for the coin chime: a stereo pair, slightly detuned, algorithm 4 with full feedback. The music in that run is all MultiPCM.
- **Host output:** SDL at 44.1 kHz, signed 16-bit stereo, with a 512-frame device buffer. The chip stream is resampled by linear interpolation and queued with `SDL_QueueAudio`.
  - **A 50 ms safety margin:** the emulator delivers one frame's audio (735 output frames) every 1/60 s, while the device reads 512 frames on its own clock. At startup, or if the queue ever runs nearly dry, it is topped up with silence to about 50 ms, so the device never waits between deliveries.
  - **Rate control:** the resampling ratio is nudged by at most ±0.5% (inaudible) to hold the queue near that target. This absorbs the drift between the 60 Hz frame loop and the sound card's clock without gaps or dropped audio. On CoreAudio the queue stays between about 1,800 and 2,500 frames.
  - Queued audio is capped at 100 ms; anything beyond that is dropped and counted.
  - At shutdown the log reports frames played and dropped, underruns, and the silence inserted.
  - No emulator data is shared with SDL's audio thread, so there's nothing to lock or deadlock on, and no allocation happens there.
  - If no audio device is available, the emulator runs silently.
  - **Speed warning:** if the emulator can't keep up with real time (for example a Debug build at `-O0`: about 43 FPS on Virtua Racing), the game runs slow and the sound must stutter. The main loop measures this every 2 seconds and logs a warning once.
- **Digital Sound Board** (`src/core/digital_sound_board.*`, `src/audio/mp2_decoder.*`, as MAME's `dsbz80`): Star Wars Arcade's music board.
  - **Z80 at 4 MHz** (the same core as the I/O board), in lockstep with the V60: 32 KB of program ROM (`epr-16471.2`), 32 KB RAM, an i8251 UART whose RxRDY drives the Z80's interrupt, and the MPEG control ports (start / end addresses, play once or loop, volume, stereo / left / right, playing position).
  - **Commands:** the board's UART shares the line the 68000 sound board transmits on, so the 68000 forwards the music commands, as on the cabinet.
  - **MPEG-1 Layer II decoder** written from the standard (ISO/IEC 11172-3): bit allocation tables B.2a–d, scale factors, grouped and plain samples, joint stereo, and the polyphase synthesis filter bank with the standard's window. It decodes straight from the 4 MB music ROM (`mpr-16514`/`16515`) at bit positions, since Star Wars Arcade's frames start 3 bits into a byte: it scans for the next sync word after each frame's data, so sync patterns inside audio data aren't mistaken for frames. Verified against mpg123: every one of the ROM's 7,280 complete frames decodes to within 1 LSB (rounding) of mpg123's output.
  - **Playback:** as MAME: start and end written while music plays are latched as loop points; a looping piece restarts from the latched start when the stream ends. The 32 kHz output is scaled by the volume, resampled to the sound board's rate by linear interpolation and added to its mix at full level (MAME's separate "mpeg" speaker at gain 1.0).

### Timing (`src/main.cpp`, `src/core/motherboard.*`)
- A 60 Hz frame loop paced by the high-resolution counter; it measured exactly 60.0 FPS.
- **Each frame:**
  - The V60 runs a 266,666-cycle budget. Overshoot is carried into the next frame so the long-run rate is exact.
  - The TGP DSP advances after every V60 instruction (5 DSP instructions per 6 V60 cycles).
  - The frame is run in 4,096-cycle slices (256 µs). After each slice, the UARTs advance 1 tick per 32 V60 cycles, the GLUE timers count down, the 68000 runs exactly 5/8 of the V60's cycles (about 166,666 per frame), and the sound chips render 5 samples per 1,792 V60 cycles (about 744 per frame). Remainders are carried over.
  - The frame's audio is handed to the host output: 735 frames at 44.1 kHz.
  - The frame is composed: tilemap background, 3D polygons below the HUD, tilemap foreground (HUD), then the 3D objects above the HUD.
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
  | KeyboardInput,     |  | Motherboard            |  | VideoManager, AudioOutput |
  | GamepadInput       |  | owns every component,  |  | src/video, src/audio      |
  | -> SharedPresses   |  | wires them, runs them  |  | texture, SDL audio queue  |
  +---------+----------+  | in lockstep            |  +---------------------------+
            | set_input() +-----------+------------+
            | set_axis()              |
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

`KeyboardInput` and `GamepadInput` report presses through `SharedPresses`, which calls `InputManager::set_input()` when the first device presses an input and when the last one releases it. Controller sticks and triggers go straight to `InputManager::set_axis()`. The I/O board's Z80 then reads those ports and the analog channels through its own bus, and copies them into the dual-port RAM for the V60. When the I/O firmware is missing, `InputManager` writes the RAM itself.

### Design rules

- **The bus is the only path between components.** The CPU reaches memory and devices only through `Bus`. Devices never call the CPU or each other. The `Motherboard` creates every component, registers device ports on the bus, and drives the frame. Each sub-board has its own bus in the same style: `SoundBus` for the 68000, `Z80Bus` (implemented by `IoBoard`) for the Z80, `Mb86233Bus` (implemented by `TgpCopro`) for the DSP.
- **The core has no host dependencies.** `src/core` does not include SDL. The keyboard and controller layers call `InputManager::set_input()` / `set_axis()`, and the video layer receives finished frames as a read-only `std::span`. All emulated-hardware decoding, including tiles and colours, happens in the core.
- **Memory is fixed-size, like the hardware.** RAM and ROM are `std::array` buffers sized to the real chips. Large owners (`Bus`, CPU, TGP) are heap-allocated so they never live on the stack.
- **Byte order is explicit.** Multi-byte values are assembled byte by byte in little-endian order, so results don't depend on the host CPU.
- **Logging is built in.** All emulator diagnostics go to stderr with a component tag such as `[V60]`, `[Bus]` or `[TGP]`. Chatty traces can be switched off at compile time.

### Frame lifecycle

```
main loop, once per 1/60 s:
  1. poll SDL events  -> KeyboardInput / GamepadInput -> SharedPresses -> InputManager
                          (port bits, held keys, controller sticks and triggers)
  2. Motherboard::run_frame()
       a. InputManager::update_analog(): wheel, pedals, flight sticks and throttle follow
          the held keys (a step per frame) or the controller's sticks and triggers
       b. V60 executes instructions until the frame's cycle budget is spent
          (each instruction is charged 8 cycles; interrupts are taken between instructions);
          after each instruction the TGP DSP catches up (5 instructions per 6 V60 cycles),
          when its ROMs are loaded; it also runs on the spot whenever the V60 waits on a FIFO.
          In 4,096-cycle slices; after each slice, for the same stretch of time:
            UARTs advance (1 serial clock tick per 32 V60 cycles)
            GLUE timers count down; an expiry raises interrupt level 0
            68000 sound CPU catches up (5 cycles per 8 V60 cycles)
            I/O board Z80 runs (1 T-state per 4 V60 cycles), when its firmware is loaded;
            Digital Sound Board Z80 likewise, when loaded (Star Wars Arcade)
              otherwise the InputManager stand-in answers once per frame
            MultiPCMs render their output (5 samples per 1,792 V60 cycles), mixed with the FM
              chip and, for Star Wars Arcade, the Digital Sound Board's decoded music
       c. frame composed in the board's layer order:
            TilemapRenderer::render_background  (low-priority tilemaps)
            PolygonRenderer::render (BelowHud)  (0x01 objects + 0x02 direct quads, depth-sorted)
            TilemapRenderer::render_foreground  (high-priority tilemaps: HUD, text)
            PolygonRenderer::render (AboveHud)  (0x41 objects, through near-black HUD pixels)
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
src/core/input_manager.*    Input ports, per-game control panels (VF joysticks, VR wheel/pedals, SWA flight sticks/throttle), analog axes; I/O board stand-in
src/core/interrupt_controller.*  Main board interrupt controller (pending levels, mask, acknowledge, 0xE00000) and GLUE timers (0xE00006)
src/core/io_board.*         Model 1 I/O board: Z80 + 315-5338A + ADC + EEPROM running EPR-14869
src/core/digital_sound_board.*  Digital Sound Board (SWA music): Z80 + i8251 + MPEG playback control
src/core/z80.*              Zilog Z80 core (I/O board CPU)
src/core/eeprom_93c46.*     93C46-style serial EEPROM (64 x 16)
src/audio/multipcm.*        Sega MultiPCM sample playback chip (core, no SDL)
src/audio/ym3438.*          YM3438 FM chip: synthesis, envelopes, LFO, DAC, timers (core, no SDL)
src/audio/mp2_decoder.*     MPEG-1 Layer II decoder (ISO 11172-3), bit-position based (core, no SDL)
src/audio/audio_output.*    SDL audio output: resampling and queueing (host side)
src/core/tilemap_renderer.* System 24 tilemap chip: 2D layers, window masks, scrolling, palette
src/core/polygon_renderer.* Display-list interpreter, depth sort, flat quad rasterizer
src/core/log.hpp            Hex formatting and compile-time trace switches
src/input/keyboard_input.*  SDL keyboard -> Model 1 inputs (host side)
src/input/gamepad_input.*   SDL game controllers -> Model 1 inputs, analog sticks and triggers (host side)
src/input/shared_presses.hpp  keyboard and controllers holding the same input
src/video/video_manager.*   SDL window and streaming texture (presentation only)
tests/test_framework.hpp    Minimal test framework (TEST_CASE, CHECK, CHECK_EQ)
tests/test_main.cpp         Test runner: checklist output, log capture, filtering
tests/v60_test_rig.hpp      Shared CPU fixture (isolated Bus + V60, program/stack areas in RAM B)
tests/test_v60_core.cpp     V60 tests: reset, NOP, MOV, ADD/SUB flags, stack, subroutines, branches, defensive checks
tests/test_v60_addressing_math.cpp  V60 tests: every addressing mode, MUL/DIV, division by zero, TEST
tests/test_v60_boot.cpp     VR boot code: UPDPSW, MOVEA, string moves, IN/OUT, ADDC/SUBC, CMP, INC/DEC, ADD/SUB sizes, LDPR/STPR, JSR, MOVS/MOVZ, DBcc/TB, ROT/ROTC, REM/REMU, bit fields, SETF, NEG, floating point, MOVT, SCHCU/SKPCU, MOVCD/MOVCFD, system registers, I/O stub
tests/test_m68000.cpp       68000 tests: MOVE to SR, CLR, MOVE addressing modes, LEA, DBcc, branches, BTST/BSET/BCLR/BCHG, ADDQ/SUBQ, shifts/rotates, SWAP, immediate ALU group, ADD/SUB family, CMP/CMPA/CMPM/EOR, OR/AND/MUL/DIV/BCD/EXG, TST, sound latch
tests/test_tilemap.cpp      2D layer tests: decoding, scrolling, layering, split modes, worst-case safety, demo HUD text
tests/test_polygons.cpp     3D tests: fill, depth sort, clipping, stipple, colours, lists, 0x05/0x06 uploads and bounds, 3D objects (projection, culling, frustum clipping, lighting), cube demo culling
tests/test_sound.cpp        Sound tests: UART timing and errors, 68000 skeleton, V60 -> sound command, lockstep cycles
tests/test_multipcm.cpp     MultiPCM tests: registers, pitch, 8/12-bit samples, loops, pan, banks, envelopes, attenuation glides, vibrato/tremolo, 440 Hz tone, audio rate
tests/test_ym3438.cpp       YM3438 tests: timers, VR's Timer A wait; FM pitch, level, pan, envelopes, algorithms, feedback, latch, DAC, LFO, lockstep mix
tests/test_audio_host.cpp   host_tests (SDL): audio device open/close with a deadlock watchdog, resampler, rate control, no underruns at 60 Hz deliveries
tests/test_input_host.cpp   host_tests (SDL): controller buttons and sticks per game, analog wheel / pedals / flight sticks / throttle, keyboard and pad sharing inputs
tests/test_rom_loader.cpp   ROM loader tests: mock VF/VR sets in temp folders, every destination, error cases
tests/test_io_board.cpp     I/O board: shared RAM, stand-in, EEPROM protocol and timing, 315-5338A, ADC, a small Z80 firmware, Virtua Racing / Virtua Fighter / Star Wars Arcade control panels, ADC bank switch
tests/test_z80.cpp          Z80 tests: flags, DAA, 16-bit, stack, timing, IX/IY and DDCB, block ops, interrupts, I/O
tests/test_tgp_copro.cpp    TGP tests: DSP moves, ALU, repeat, stalls; FIFOs both ways, copro RAM, tables, data ROM; DSP alongside the V60
tests/test_dsb.cpp          Digital Sound Board: MP2 decoder against mpg123 values for synthetic frames, unaligned frames, limits; play once, loop points, volume, pan, UART interrupt
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

**Both build types are fast enough to play.** Measured on an Apple M1 Pro without a window, Virtua Racing runs at:

| Build | Speed |
|---|---|
| Release | about 234 frames per second (3.9× real time) |
| Debug (default; `-O1` with debug info, see `MODEL1_DEBUG_OPTIMIZE`) | about 196 frames per second (3.3× real time) |
| Debug at `-O0` (`MODEL1_DEBUG_OPTIMIZE=OFF`) | about 43 frames per second (72% of real time): the game runs slow and the sound stutters, and the emulator logs a warning |

A Release build:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

Interrupt tracing (`MODEL1_TRACE_IRQ`) is off by default. Turned on, it prints two lines per frame, and a terminal that can't keep up stalls the emulator, and the sound with it. Build directories created before this default changed keep their old setting; reconfigure them with `-DMODEL1_TRACE_IRQ=OFF`.

**Build directories** used in this project (any names work):

| Directory | Configuration | Use |
|---|---|---|
| `build/` | Debug | Development and unit tests |
| `build-release/` | Release | Playing |
| `build-sanitize/` | Debug, `MODEL1_SANITIZE=ON` | Memory-safety test runs |

### Build options

| Option | Default | Effect |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Debug` | Use `Release` for a fully optimized build (single-config generators such as Make and Ninja). |
| `MODEL1_DEBUG_OPTIMIZE` | `ON` | GCC/Clang: compiles Debug builds at `-O1` (debug info kept), fast enough to play. Turn off for `-O0` when stepping through code in a debugger. |
| `MODEL1_TRACE_IRQ` | `OFF` | Logs every CPU interrupt request and acknowledgement (two lines per frame). |
| `MODEL1_TRACE_TGP` | `OFF` | Logs every TGP function executed (very verbose). |
| `MODEL1_TRACE_INPUT` | `ON` | Prints a confirmation when a coin or start key is pressed. |
| `MODEL1_BUILD_TESTS` | `ON` | Builds the `emulator_tests` unit test executable. |
| `MODEL1_SANITIZE` | `OFF` | GCC/Clang: builds with AddressSanitizer, UBSan and bounds-checked standard containers. Any out-of-bounds access or undefined behaviour aborts with a report. |

Example:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMODEL1_TRACE_INPUT=OFF
```

## Running

```sh
./build-release/model1 path/to/vr                        # play Virtua Racing (game detected)
./build-release/model1 path/to/vf                        # play Virtua Fighter
./build-release/model1 path/to/swa                       # play Star Wars Arcade
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
- `--sound-demo` plays a 440 Hz beep, half a second on and half a second off. A 64-point sine wave is placed in MultiPCM 1's sample ROM, and slot 0 is programmed through the sound bus (octave 0, pitch 268) and keyed on and off each half second, with a 3 ms attack and a 170 ms release so the beep doesn't click.
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
| ← / → (or A / D) | Steer. While a pedal is held, the wheel turns quickly and re-centres on release; with no pedal held (course selection) it turns slowly and stays where you leave it |
| ↑ (or W) | Accelerator. Ramps up while held, springs back when released |
| ↓ (or S) | Brake, likewise |
| J / K / L (or Z / X / C) | View buttons VR1 (red), VR2 (blue), VR3 (yellow) |
| U (or V) | View button VR4 (green) |
| I / O (or B / N) | Shift down / shift up |
| 5 | Coin |
| 1 | Start (hold it while pressing the accelerator on the course screen for the 7-speed manual gearbox) |
| F2 / 9 | Test / service switches |

To play: insert a coin (5). On the course screen, hold → until your course is highlighted (Big Forest is the default; about ¼ second gives Bay Bridge, ½ second Acropolis; ← goes back), release, then press ↑ to start.

**Virtua Fighter and all keys:**

| Key | Input |
|---|---|
| Arrow keys or W A S D | Player 1 joystick (Virtua Racing: ← → steer, ↑ accelerator, ↓ brake) |
| J K L or Z X C | Player 1 buttons 1, 2, 3 (Virtua Racing: view buttons VR1 red, VR2 blue, VR3 yellow) |
| U I O or V B N | Player 1 buttons 4, 5, 6 (Virtua Racing: view button VR4 green, shift down, shift up) |
| Keypad 8 / 5 / 4 / 6 | Player 2 joystick (up / down / left / right) |
| Keypad 1 / 2 / 3 | Player 2 buttons 1, 2, 3 |
| 1 / 2 | Start 1 / Start 2 |
| 5 / 6 | Coin 1 / Coin 2 |
| F2 | Test switch |
| 9 | Service switch |

**Star Wars Arcade:**

| Key | Control |
|---|---|
| Arrow keys or W A S D | Pilot's flight stick (↑ pushes it forward: nose down, as in MAME). Springs back to centre on release |
| I / O (or B / N) | Throttle faster / slower; it stays where you leave it |
| J / K (or Z / X) | Pilot's laser trigger / torpedo button |
| L (or C) | Pilot's VR (view) button |
| Keypad 8 / 5 / 4 / 6 | Gunner's stick |
| Keypad 1 / 2 | Gunner's laser trigger / torpedo button |
| 5 | Coin (two per credit by default) |
| 1 / 2 | Start (pilot / gunner) |
| F2 / 9 | Test / service: in the test menu, 9 moves the cursor and F2 selects |

To play: press 5 twice, then 1. Choose *Pilot* (one player) or *Pilot & Gunner* with the stick and press 1, then pick a mission the same way.

In Virtua Fighter, buttons 1, 2 and 3 are Guard, Punch and Kick. On a keyboard without a numeric keypad, player 2 needs an external keypad or a second game controller (or play against the computer).

**Game controllers:** plug in one or two (the first connected is player 1, the second player 2), before starting or while playing; the log names each one (`[Gamepad] 'Xbox Wireless Controller' connected as player 1`). Buttons are named here by their Xbox labels; on a PlayStation pad, A B X Y are ✕ ○ □ △.

| Control | Virtua Fighter | Virtua Racing | Star Wars Arcade |
|---|---|---|---|
| Left stick | Joystick | Steering wheel (proportional while racing; in menus it nudges the wheel like the arrow keys, and the wheel stays) | Flight stick, proportional (pilot on pad 1, gunner on pad 2) |
| D-pad | Joystick | ← → steer like the keys, ↑ ↓ accelerator / brake | Flight stick, like the keys |
| A / B / X | Guard / Punch / Kick | VR1 (red) / VR2 (blue) / VR3 (yellow) | Laser trigger / torpedo / VR view button |
| Y | | VR4 (green) | |
| LB / RB | | Shift down / up | Throttle faster / slower |
| RT / LT (pad 1) | | Accelerator / brake, proportional | Throttle faster / slower, in proportion to the press; the lever stays |
| Start | Start (pad 2: start 2) | Start | Start (pad 2: start 2, gunner) |
| Back / View / Select | Coin (pad 2: coin 2) | Coin | Coin |

Player 2's pad has no Y, LB, RB or trigger functions (the cabinets have none for player 2). The test and service switches stay on the keyboard (F2, 9).

### What you should see

**With the Virtua Racing set,** the log reports the files it found, then the game boots:

```
[ROM] Virtua Racing (vr) in 'roms/vr': 15 of 15 needed files found
[ROM] Loaded 32 files (28360 KB) for Virtua Racing; 7 other files of the set are present but not used
```

The test-mode settings screen appears for a moment, then attract mode: the scrolling *Course Ranking* table and 3D fly-bys of the courses. A coin brings up the course-select screen, and the accelerator starts the race.

**With the Star Wars Arcade set,** the EPA "Recycle it" screen and the Lucasfilm copyright appear, then the attract mode (Star Destroyer fly-bys). The music starts with the attract mode. Two coins give a credit; start leads to the mode and mission selection, the briefing and the hyperspace jump. Expect two warnings: the TGP program's checksum differs from MAME's (if your dump is the other known one), and `93c45.bin` is missing when your `model1io` set doesn't have it. Both are harmless.

**With a game controller** plugged in, the log names it: `[Gamepad] 'Xbox Wireless Controller' connected as player 1`.

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

The loader reads an unzipped ROM set: a folder containing the individual chip dumps, named as in MAME. File names are matched case-insensitively, and the game is detected from the files present (or chosen with `--game`). Supported sets: **Virtua Fighter** (`vf`), **Virtua Racing** (`vr`) and **Star Wars Arcade** (`swa`); the Star Wars Arcade files are listed after the table.

| Part | Virtua Fighter | Virtua Racing | Loaded into |
|---|---|---|---|
| V60 program (2 chips, byte-interleaved) | `epr-16082.14`, `epr-16083.15` | `epr-14882.14`, `epr-14883.15` | `0x200000–0x2FFFFF` |
| V60 boot ROM (holds the reset address) | `epr-16080.4`, `epr-16081.5` | `epr-14878a.4`, `epr-14879a.5` | `0xFC0000–0xFFFFFF` |
| V60 data ROM (4 interleaved pairs, banked) | `mpr-16084.6` … `mpr-16091.13` | `mpr-14880.6` … `mpr-14889.13` | banks 0–3 of the `0x100000` window |
| 68000 sound program (byte-swapped words) | `epr-16120.7`, `epr-16121.8` | `epr-14870a.7` | sound ROM `0x00000` |
| MultiPCM samples | `mpr-16122.32`, `mpr-16123.33` / `mpr-16124.4`, `mpr-16125.5` | `mpr-14873.32` / `mpr-14876.4` | MultiPCM 1 / 2 sample ROMs |
| TGP program (MB86233) | `315-5724.bin` | `315-5573.bin` | DSP program memory |
| TGP tables (sin/cos, atan, 1/x, 1/√x) | `opr14742.bin`, `opr14743.bin` | `opr14742.bin`, `opr14743.bin` (low / high halves) | DSP table units |
| TGP data ROM (4 byte lanes) | | `mpr-14898.39` … `mpr-14901.42` | DSP data ROM window |
| Polygon (model) ROM | `mpr-16096.26` … `mpr-16103.33` | `mpr-14890.26` … `mpr-14897.33` (16-bit halves of 32-bit words) | 16 MB model ROM for 3D objects |
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
**Star Wars Arcade** (`swa`):
- V60 program `epr-16669.14`, `epr-16670.15`; boot ROM `epr-16668.5` (MAME loads the one 512 KB chip at `0xF80000` and mirrors it at `0x000000` and `0x080000`, so the folder holds it once).
- 68000 sound program `epr-16470.7`; MultiPCM samples `mpr-16486.32`, `mpr-16487.33` / `mpr-16484.4`, `mpr-16485.5`.
- TGP program `315-5711.bin` with `opr14742.bin` / `opr14743.bin`, TGP data ROM `mpr-16472.39` … `mpr-16475.42`; polygon ROM `mpr-16476.26` … `mpr-16481.31`.
- I/O board `epr-14869b.25` and `93c45.bin` (both from `model1io`).
- Digital Sound Board: Z80 program `epr-16471.2` and MPEG music `mpr-16514.57`, `mpr-16515.58` (optional: without them the game runs without music).
- MAME flags `315-5711.bin` as a bad dump (CRC32 `0x6A21F304`). Other dumps exist (e.g. CRC32 `0xC5DDB8FC`); the loader reports the mismatch and loads them anyway, and the game runs with it.

- The other files of a set are recognised but not used: the sets' spare tables and geometrizer firmware (`opr-14744`–`14748`, `315-5571`, `315-5572`; MAME doesn't use them either). Virtua Fighter has no TGP data ROM; its TGP runs from the program and tables alone.

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
./build/host_tests                  # SDL tests: audio device lifecycle, resampler, queue margin; controller mapping
```

`host_tests` (12 tests) links SDL and opens audio devices; its controller tests feed simulated controller events, so they need no pad plugged in. It uses SDL's `dummy` audio driver, plus the real device when one is available. A watchdog aborts the run if a test hangs, for example on a deadlock with SDL's audio thread.

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

279 passed, 0 failed
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
| `0xE00000` | Interrupt controller: control (write 0x10 clear all pending, 0x20 acknowledge the level last taken) |
| `0xE00002` | Interrupt controller: mask (bit n set = level n masked; 0xFF at power-on) |
| `0xE00004` | Data ROM bank register: writing `(bank << 4) | 1` selects the bank (takes priority over the latch) |
| `0xE00006` | Timer mode (write-only, stored, no known effect) |
| `0xE00008`, `0xE0000A` | Timer 0 / 1 period *p* (write): a non-zero *p* (re)starts the timer, which raises interrupt level 0 every 2,048 × *p* CPU cycles; 0 stops it |
| `0xE0000C`, `0xE0000E` | Timer 0 / 1 count (read): time left, in units of 2,048 cycles; writes are ignored |
| `0xE00010–0xE00FFF` | Other system registers: a read/write latch, not acted on |
| `0xF80000–0xFFFFFF` | Boot ROM (read-only, holds the reset address) |

Any other address is unmapped: reads return 0, writes are ignored, and both are logged.

The V60 also has a separate **I/O address space**, reached only by `IN`/`OUT`. The TGP ports are mirrored there at the same addresses, as in MAME. `0xC10000–0xC10003` is a write-only stub for a serial controller.

### V60 interrupts

When an interrupt is pending and PSW.IE (bit 18) is set, the CPU:

1. switches to the interrupt stack and clears IE;
2. pushes the old PSW, then the return PC;
3. jumps to the handler address stored in vector table entry `vector + 0x40`, at `(SBR & ~0xFFF) + entry × 4`.

RETIS undoes all of this.

The **interrupt controller** (`src/core/interrupt_controller.*`, as MAME's model1.cpp) holds a pending bit per level and drives the V60's interrupt line while any is pending. The CPU's acknowledge returns the lowest pending level as the vector, and the level stays pending until the program writes 0x20 to `0xE00000`. Sources:
- **VBlank:** level 1, at the end of every frame.
- **Timers:** level 0, each time either GLUE timer expires (`src/core/interrupt_controller.*`, `GlueTimers`). Star Wars Arcade unmasks it (mask `0x3C`); VR and VF never do.
- **Main UART:** level 3, when it becomes ready to send or receives a byte. Virtua Fighter unmasks level 3 while its sound queue holds data; the handler sends the next byte, so without this VF would have no sound.

Masked levels are ignored when they fire. VR and VF write `0xFF`, then `0xFD` (VBlank only), to the mask at boot.

### Inputs (I/O board)

The V60 sees the inputs only as bytes in the shared RAM, written by the I/O board firmware (or the stand-in). Ports are active-low: a bit reads 0 while its input is held, and an idle port reads `0xFF`.

| Address | Contents |
|---|---|
| `0xC00000–0xC0000E` | Analog channels (bytes 0–7, as the firmware publishes them) |
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

**Virtua Racing analog channels** (MSM6253 ADC): 0 wheel (`0x80` centre, lower to the left), 1 accelerator and 2 brake (`0x30` released, `0xFF` fully pressed), 3 unconnected (`0xFF`). Virtua Fighter has no analog controls (all `0xFF`). **Star Wars Arcade:** see *Control panels* under *Features → I/O board and inputs*; its buttons are all on the player 1 port.

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
| `0xD00000–0xD00007` | YM3438 FM chip: address / data for part 1 at `0xD00001` / `0xD00003`, part 2 at `0xD00005` / `0xD00007`; status on reads |
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
| 4 | Bit 7: key on (start from the beginning, with the attack) or key off (start the release; release rate 15 stops at once) |
| 5 | Attenuation, bits 7–1 (0 = loudest, 0.375 dB per step); bit 0 = 1 sets it at once, 0 glides to it |
| 6 | LFO speed (bits 5–3), vibrato depth (bits 2–0) |
| 7 | Attack rate (bits 7–4), decay 1 rate (bits 3–0) |
| 8 | Decay level (bits 7–4, 6 dB per step), decay 2 rate (bits 3–0) |
| 9 | Key rate scaling (bits 7–4; 15 = off), release rate (bits 3–0) |
| 10 | Tremolo depth (bits 2–0) |

Sample header (12 bytes per sample at the start of sample ROM): start address (3 bytes; bit 22 = 12-bit format), loop start (2 bytes), 0x10000 minus the length (2 bytes), then byte 7 → register 6, bytes 8–10 → registers 7–9, byte 11 → register 10 (copied when the sample is selected).

**UART programming** (both sides): write a mode byte to the control register (for example `0x4E`: asynchronous, ×16 clock, 8 data bits, no parity, 1 stop bit), then a command byte (bit 0 transmit enable, bit 2 receive enable, bit 4 error reset, bit 6 back to mode). Then read and write data bytes. Status bits: 0 TxRDY, 1 RxRDY, 2 TxEMPTY, 3 parity error, 4 overrun, 5 framing error.

### 2D layers (System 24 tilemap chip)

**Character RAM** (`0x780000`, 512 KB) holds 16,384 tiles of 8×8 pixels at 4 bits per pixel, 32 bytes per tile. Each 16-bit word holds 4 pixels, leftmost pixel in bits 15–12. Pixel value 0 is transparent.

**Tile RAM** (`0x700000`, 64 KB), as 16-bit word offsets:

| Word offset | Contents |
|---|---|
| `0x0000–0x3FFF` | Tilemaps 0–3, 64×64 entries each. Entry bits 13–0: tile number; 14–7: palette (overlaps the tile number's top bits, so palette *p* selects tiles from block *p* × 128); 15: high priority |
| `0x4000–0x47FF` | Per-line horizontal scroll tables, 512 words per tilemap |
| `0x5000–0x5003` | Horizontal scroll, tilemaps 0–3: 9-bit value; bit 15 enables per-line scroll. A value *v* moves the layer right by *v* pixels |
| `0x5004–0x5007` | Vertical scroll, tilemaps 0–3: 9-bit value; bit 15 disables the tilemap; bits 14–13 (of tilemaps 0 and 2) select the pair's special split mode (below) |
| `0x6000–0x67FF`, `0x6800–0x6FFF` | Window masks for pairs 0/1 and 2/3: 4 words per line, one bit per 8-pixel column. Even tilemaps draw where the bit is 0, odd ones where it is 1 |

**Special split modes** (bits 14–13 of `0x5004` for pair 0/1, `0x5006` for pair 2/3): the two tilemaps of the pair are drawn as one layer. Both scroll with the even tilemap's values, the window masks are ignored, and the screen is split in two:

| Mode | Split | Which tilemap where |
|---|---|---|
| 1 | At line (−vscroll) & `0x1FF` | Even tilemap above, odd below; swapped when bit 9 of −vscroll is 0 |
| 2, 3 | At column hscroll & `0x1FF` | Even tilemap left, odd right; swapped when hscroll bit 9 is 0 |

With per-line horizontal scroll, each line uses its own value (in modes 2/3 that moves the split column too). Only tiles of the pass's priority are drawn, opaque pass included. Virtua Racing uses mode 1 on tilemaps 2/3 for its sky and landscape. Drawn as normal tilemaps, the landscape wrapped around into the top of the screen in place of the sky, leaving a band of a different colour as the horizon moved down.

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
| `0x41` | Draw a 3D object above the HUD | Done (second pass, through dark HUD pixels) |
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
  - **3D:** a few stray single-pixel points appear in some scenes (degenerate polygons drawn as points). The player car's stippled shadow reaches further down the screen than its rear tyres. It's a separate object (the car's silhouette flattened onto the ground, extending under the rear wing), and our renderer draws it exactly as MAME's would; whether the real board shows the same is unverified. The renderer is a high-level simulation like MAME's, so exact pixel coverage and the colour/luminance path may differ from the real board.
  - **Sound:** on a boot with valid saved settings, the first sound command (`0x81`) can be lost (see *Timing* below).
  - **Drive board** (force feedback, port E) isn't emulated.
- **Virtua Fighter:** MAME marks the game not working, and its TGP program dump `315-5724.bin` as bad. Here it runs its attract mode, player select and fights without visible problems so far, but a geometry error from that dump could still show up somewhere.
- **CPU:** every V60 instruction MAME implements is implemented, except the exception, trap and task instructions (TRAP, TRAPFL, BRK, BRKV, CHKA*, CHLVL, LDTASK/STTASK, CLRTLB), which halt with a message; no Model 1 game seen so far uses them. Double-precision floating point isn't implemented (MAME doesn't either). The downward string search follows MAME, which its source notes differs from NEC's manual. Every instruction is charged a flat 8 cycles.
- **Floating point:** single precision, IEEE results; FPU exceptions (division by zero, overflow) aren't raised. CVTSW rounds to nearest-even by default (MAME rounds halves away from zero), and converts out-of-range values and infinity to `0x80000000` (MAME's result depends on the host: 0 on x86, `0xFFFFFFFF` on ARM). Virtua Racing converts an infinity from a TGP division every game frame during a race; switching to MAME's behaviour changes nothing visible.
- **Division by zero** leaves the destination unchanged, sets OV and logs an error; the V60's zero-divide exception is not emulated. Setting OV is our choice: MAME leaves it clear. Exceptions in general (including reserved addressing modes) halt the CPU instead of trapping.
- **Flag rules that differ from MAME:** signed MUL sets OV when the result doesn't fit in the operand size; MAME sets it whenever the upper bits of the product are non-zero, which also flags small negative results. DIVX/DIVUX set OV and leave the destination unchanged when the quotient doesn't fit in 32 bits; MAME doesn't handle that case. Neither has been checked against NEC's documentation.
- **SHA overflow on left shifts** uses the standard definition (the result doesn't fit in the operand size). MAME only checks the bits shifted out, so the two differ when a shift changes the sign. This has not been checked against NEC's documentation.
- **Star Wars Arcade:** the Digital Sound Board's volume register bit 7 and ports `0xEA`/`0xEB` (written at boot) have unknown purposes and are ignored, as in MAME. MAME flags its TGP program `315-5711.bin` as a bad dump; it runs here without visible problems so far. The test menu's *Input test* (page 2) shows the sticks and throttle, and its *adjust* option (start button) calibrates them.
- **Timers:** the count is updated per 4,096-cycle slice of the main CPU, so an expiry can be up to that late (0.26 ms).
- **V60 I/O-space port `0xC10002`** is a stub. VR's boot writes the i8251 UART reset/mode sequence there (`00 00 00 40 4E`), so it is likely a serial controller. Writes are logged and ignored, and reads stay logged as unmapped. MAME has nothing there either.
- **Frame rate:** 60 Hz. The real hardware runs at about 57.52 Hz (278,144 CPU cycles per frame), and VBlank starts partway through the frame.
- **TGP:** fixed-point mode and DSP interrupts aren't emulated, as in MAME.
- **I/O board:** the EEPROM isn't saved between runs, so the operator settings reset to the defaults each run. The shared RAM's interrupt mailboxes aren't emulated.
- **Inputs:** keyboard and up to two game controllers. Racing wheels and flight sticks that SDL exposes as plain joysticks rather than game controllers aren't supported (a `gamecontrollerdb.txt` mapping can turn some into controllers). Bindings are fixed. Force feedback (the drive board) isn't emulated.
- **Timing:** the V60 is charged a flat 8 cycles per instruction (as in MAME). On a boot with valid saved settings this lets VR send its first sound command (`0x81`) before the sound CPU has enabled its UART receiver, so the byte is lost, as the real i8251 would lose it; real V60 timings would likely fix this.
- **Sound:** the YM3438's interrupt output isn't wired to the 68000 (the sound program polls Timer A instead), and its busy flag is never set (writes take effect at once). The 68000 core implements the instructions the VR, VF and SWA sound programs use. The MultiPCM's effect send isn't emulated (it feeds an external effects DSP the Model 1 board doesn't have), and octave −8 plays at 2^−9 (MAME wraps it to 2^7). Smooth sound needs the emulator to run at full speed: use a Release build.

## Roadmap

Planned next steps, roughly in order:

1. **Virtua Racing polish:** remove the stray points, and save the EEPROM between runs.
2. **Real video timing:** 57.52 Hz, with VBlank at line 384.
3. **More games:** manifests for the other Model 1 sets (Wing War, NetMerc).
