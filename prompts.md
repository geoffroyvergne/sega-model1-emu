# Prompt 0: Project Initial Skeleton & Build Pipeline

USER REQUEST: Initialize the Sega Model 1 Emulator project structure.

Please generate the base directory tree, the CMakeLists.txt configuration, and the entry-point source files according to the rules defined in your system guidelines.

### Specifications:
- Language: Modern C++20
- Build System: CMake (minimum version 3.20)
- Libraries: SDL2 (Required for Video, Audio, and Event Poll)
- Platform: Cross-platform (Windows/Linux/macOS)

### Desired Project Tree:
- /cmake
- /src
  - main.cpp
  - /core
    - motherboard.hpp / motherboard.cpp
  - /video
    - video_manager.hpp / video_manager.cpp
- CMakeLists.txt

### Requirements for this step:
1. `CMakeLists.txt`: Must correctly find SDL2 package, set up compiler flags (-Wall, -Wextra, C++20 standard), and define the main executable target.
2. `main.cpp`: Initialize SDL2 Video and Audio subsystems, create a standard window with a resolution of 496x384 (Sega Model 1 resolution), handle a basic SDL event loop (listening for SDL_QUIT to safely close the application), and run at a dummy locked 60 FPS loop.
3. `motherboard` class: A basic placeholder class that will manage the lifecycle of our components later.
4. Provide the exact code for each file and instructions on how to build it using a standard terminal command line.

Do not write any CPU or GPU code yet. Focus purely on a solid, clean compilation target.

# Prompt 1: The Central Memory Bus & ROM Loader

USER REQUEST: Implement the Central Motherboard Bus and basic ROM loading mechanisms.

Now that our SDL2 window is running, we need to map the memory layout of the Sega Model 1 architecture. The V60 CPU uses a 32-bit address space. We will implement a centralized `Bus` class (managed by the `Motherboard`) that routes 8-bit, 16-bit, and 32-bit reads and writes to their respective hardware buffers.

### Memory Map Specifications to Implement:
- Main RAM: 1 MB (0x00000000 to 0x000FFFFF) -> Read/Write
- Main ROM (Game Program): 4 MB max (0x00C00000 to 0x00FFFFFF) -> Read-Only
- VRAM (Video Memory placeholder): 512 KB (0x01000000 to 0x0107FFFF) -> Read/Write

### Requirements for this step:
1. Update `motherboard.hpp/cpp` or create a dedicated `bus.hpp/cpp` component to hold fixed-size allocation buffers (using `std::vector<uint8_t>` or `std::array`) representing Main RAM and Main ROM.
2. Implement 6 core memory access methods in the Bus/Motherboard structure:
   - `uint8_t  read_byte(uint32_t address);`
   - `uint16_t read_word(uint32_t address);`  // Big-Endian or Little-Endian depending on V60 mode, assume Little-Endian for now but wrap it safely.
   - `uint32_t read_long(uint32_t address);`
   - `void     write_byte(uint32_t address, uint8_t value);`
   - `void     write_word(uint32_t address, uint16_t value);`
   - `void     write_long(uint32_t address, uint32_t value);`
3. Implement a `bool load_rom(const std::string& filepath, uint32_t target_address);` method. This method must open a binary file, read its content, and safely copy it into the Main ROM memory buffer starting at the designated offset.
4. Memory Guarding: If a write is attempted in the ROM range, log a warning via `std::cerr` and ignore the write. If an address is completely out of bounds (unmapped), log a critical warning and return `0`.
5. Update `main.cpp` to call `load_rom` with a dummy file path before launching the main loop, printing a success/failure message to the console.

Keep the code highly organized, well-commented, and cross-platform. Do not implement any CPU opcode decoding yet.

# Prompt 2: NEC V60 CPU Core Setup & Instruction Decoder

USER REQUEST: Implement the NEC V60 CPU Core structure and the initial Instruction Fetch/Decode loop.

We need to create the main CPU emulator component for the NEC uPD70616 (V60) processor. The V60 is a 32-bit processor with thirty-two 32-bit general-purpose registers, a Program Counter (PC), and a Program Status Word (PSW). It uses variable-length instructions.

### Hardware Specifications to Implement:
1. Registers Layout:
   - 32 General Purpose Registers (R0 to R31, 32-bit each).
   - Program Counter (PC, 32-bit).
   - Program Status Word (PSW, 32-bit) to track flags (Zero, Sign, Overflow, Carry).
2. CPU State:
   - A pointer or reference to our central `Bus`/`Motherboard` so it can fetch bytes from memory.

### Requirements for this step:
1. Create `/src/core/v60.hpp` and `/src/core/v60.cpp` files. Update `CMakeLists.txt` to include them.
2. Inside `v60.hpp`, declare the `V60` class containing the registers, flags, and a reference to the `Bus`.
3. Implement the main cycle method: `void execute_cycle();`
   - This method must read the byte at the current `PC` (Fetch), increment `PC` by 1, and evaluate it (Decode).
4. Implement a basic switch-case decoder for the first 5 essential operational flows / opcodes:
   - `NOP` (No Operation)
   - `JMP` (Jump to absolute/relative address)
   - `MOV` (Basic register-to-register move)
   - `ADD` / `SUB` (Basic arithmetic affecting Zero and Sign flags)
5. Defensive Logging: If the decoder encounters an unknown or unimplemented opcode byte, it must print a critical log to `std::cerr` including the unhandled byte value and the current `PC` address, then temporarily halt execution or loop safely to avoid crashing.
6. Connect the CPU to the `Motherboard` class so that each tick of the main application loop executes a fixed number of CPU cycles (e.g., call `execute_cycle()` a few times per frame for testing).

Focus entirely on clean structures, clear opcode tracking, and explicit bit manipulation. Do not rush into writing hundreds of instructions yet; we want the infrastructure to be bulletproof.

# Prompt 3: Interrupts, Timers, and VBlank Synchronization

USER REQUEST: Implement the Interrupt handling system and synchronize the CPU execution with the SDL VBlank loop.

To prepare our emulator for real arcade code, we must implement how the NEC V60 handles Interruptions (specifically Maskable Interrupts / IRQs) and tie the emulation speed to the 60Hz video refresh rate.

### Technical Specifications to Implement:
1. Interrupt Vector Table (IVT): The NEC V60 handles interrupts by reading an interrupt vector and jumping to an address calculated from an Interrupt Base Register or a fixed vector table. For now, implement a clean method `void request_interrupt(uint8_t vector_number);`.
2. Program Status Word (PSW) Integration: The CPU must check if interrupts are globally enabled (via an Interrupt Enable Flag inside the PSW) before processing an IRQ.
3. VBlank Trigger: The Sega Model 1 hardware generates a specific IRQ (typically triggered at the end of every video frame render) to tell the game code to update physics and inputs.

### Requirements for this step:
1. Update `v60.hpp` and `v60.cpp` to include:
   - An interrupt status tracker (e.g., a boolean or bitmask for pending IRQs).
   - The `void request_interrupt(uint8_t vector)` method.
   - Logic inside `execute_cycle()` or at the end of the execution block to process the interrupt if the PSW allows it (saving the current PC/PSW to the stack and jumping to the interrupt handler address).
2. Update the main loop inside `main.cpp` and `motherboard.cpp`:
   - Calculate how many CPU cycles must be executed per frame to mimic a 16 MHz processor running at 60 FPS (approx. 266,666 cycles per frame).
   - In the main loop, execute this exact number of cycles, then manually trigger the VBlank interrupt via `cpu.request_interrupt(...)`.
3. Synchronization: Ensure that the SDL window refresh rate is locked at 60Hz (using `SDL_RENDERER_PRESENTVSYNC` or precise `SDL_Delay` calculation) so the hardware loops accurately mirror real-time execution speeds.
4. Add clear logging inside the interrupt handler to print out when a VBlank interrupt is successfully requested and acknowledged by the CPU.

Maintain complete separation between the video timing layer and the CPU execution logic. Keep the code clean and well-structured.

# Prompt 4: Video Manager & VRAM Framebuffer Rendering

USER REQUEST: Connect the Video RAM (VRAM) to the SDL2 Renderer using a raw Framebuffer approach.

Our 60Hz timing is active, but the screen is black. We need to implement the link between the emulator's Video RAM and the actual host window. The Sega Model 1 operates at a resolution of 496x384 pixels. We will implement a direct pixel rendering pipeline where writes to specific VRAM regions are drawn to the screen.

### Technical Specifications to Implement:
1. Framebuffer Allocation: Inside `video_manager.hpp`, maintain a 32-bit RGBA pixel buffer representing the 496x384 screen resolution (`496 * 384 * 4` bytes).
2. SDL Texture Bridge: Use an `SDL_Texture` configured with `SDL_TEXTUREACCESS_STREAMING`. Every frame, we will lock this texture, copy our emulator's pixel buffer into it, and present it via the `SDL_Renderer`.
3. VRAM Mirroring: For testing purposes before the full GPU/TGP tile layers are ready, create a method `void update_framebuffer();` that reads raw color data directly from our VRAM placeholder array (setup in Prompt 1) and translates it into pixels.

### Requirements for this step:
1. Update `video_manager.hpp` and `video_manager.cpp` to handle the `SDL_Texture` lifecycle (Creation, Update/Lock, Rendering, and Destruction).
2. Implement the render loop cycle inside `video_manager.cpp`:
   - Clear the renderer.
   - Lock the `SDL_Texture`, copy the internal 32-bit pixel array data into it, and unlock it.
   - Copy the texture to the renderer using `SDL_RenderCopy` and call `SDL_RenderPresent`.
3. Create a test pattern generator: If the VRAM buffer is entirely empty (zeros), fill the internal pixel array with a temporary visual grid or a basic color gradient (e.g., a Sega logo background or color bars) so we can visually confirm that the pixel rendering pipeline works.
4. Hook this rendering phase inside `main.cpp` immediately after the CPU completes its targeted cycles per frame (during the VBlank period).

Ensure that memory accesses to the pixel array are safe and optimized. Do not add heavy 3D calculations yet; we want a verified 2D pixel pipeline first.

# Prompt 5: Fujitsu TGP (DSP) Geometry Processor Setup

USER REQUEST: Implement the baseline structure for the Fujitsu MB86290 TGP (Digital Signal Processor / Geometry Engine).

Now that we can display pixels, we need to implement the Sega Model 1's 3D coprocessor: the Fujitsu TGP. This component handles vector and matrix operations via dedicated internal data registers and commands. It communicates with the main CPU through shared memory space and specialized hardware I/O registers.

### Technical Specifications to Implement:
1. TGP Internal State:
   - Accumulator and general registers represented as standard floats (`float` or `double`).
   - Space for the Internal Data RAM (typically 2KB - 4KB of fast internal memory used for storing 3D matrices).
2. Host Communication Interface:
   - Define I/O control registers (e.g., Command Register, Status Register).
   - The CPU writes a command ID to the Command Register, which triggers the TGP to process a geometric function.
3. Core Geometric Operations (Baseline Commands):
   - Command: Matrix Multiply (combining translation/rotation matrices).
   - Command: Vector Transformation (transforming a 3D vertex `[X, Y, Z]` by the current active matrix).
   - Command: Perspective Projection / Division (converting transformed 3D coordinates into 2D screen space coordinates).

### Requirements for this step:
1. Create `/src/core/tgp.hpp` and `/src/core/tgp.cpp`. Update `CMakeLists.txt`.
2. Instantiate the `TGP` class within the `Motherboard`, mapping its control registers to the central `Bus` memory map (usually mapped in the `0x00800000` or custom control space depending on structural setup).
3. Implement a `void execute_command(uint32_t command_id);` method. Include a clean switch-case block capturing the 3 critical math commands mentioned above.
4. Add strict logging: If the TGP receives an unsupported command, log it to `std::cerr` along with the parameters passed by the CPU.
5. Create a validation function: In `main.cpp` or inside `motherboard.cpp`, add a startup test routine that loads a dummy 3D vertex `[1.0, 2.0, 3.0]` and a translation matrix into the TGP, executes a transform command, and prints the result via `std::cout` to verify that floating-point math execution is perfectly accurate.

Ensure the TGP implementation remains purely mathematical for now. Do not attempt to draw complex 3D scenes yet; focus exclusively on routing and executing the geometric commands correctly.

# Prompt 6: Input Management & Arcade Port Mapping

USER REQUEST: Map SDL2 Keyboard/Controller inputs into the Sega Model 1 hardware I/O ports.

We have the CPU running, synchronization active, a working video pipeline, and the 3D math engine ready. Now, we must allow the user to interact with the system. The Sega Model 1 architecture exposes player inputs (Buttons, Joysticks, Service/Test switches) via memory-mapped I/O registers. We need to capture host inputs using SDL2 and mirror them inside these specific memory ports.

### Technical Specifications to Implement:
1. Input Ports Definition:
   - Player 1 Input Port (usually a 16-bit or 32-bit register representing Up, Down, Left, Right, Button 1, Button 2, Button 3, Start).
   - Player 2 Input Port (identical structure for a second player).
   - System Port (Coin 1, Coin 2, Test Switch, Service Switch).
2. Bitmask Representation: In arcade hardware, inputs are active-low (0 when pressed, 1 when released) or active-high. We will use a standard active-low bitmask (defaulting to `0xFFFF` or `0xFFFFFFFF` when nothing is pressed).

### Requirements for this step:
1. Create `/src/core/input_manager.hpp` and `/src/core/input_manager.cpp`. Update `CMakeLists.txt`.
2. Inside `input_manager.hpp`, create an `InputManager` class that stores the state masks for Player 1, Player 2, and System buttons.
3. Implement a method `void process_sdl_event(const SDL_Event& event);` to be called inside your main loop. This method must listen to `SDL_KEYDOWN` and `SDL_KEYUP` events and modify the bits accordingly. Map the keys as follows:
   - Arrow Keys / WASD -> Up / Down / Left / Right
   - Keys J, K, L / Z, X, C -> Arcade Buttons 1, 2, 3
   - Key 1 -> Player 1 Start
   - Key 5 -> Insert Coin (System Port)
4. Hook this component into the central `Bus` layout: When the CPU calls `read_word()` or `read_long()` on the designated input hardware addresses (e.g., simulate reading from `0x01C00000` or the chosen I/O block), return the current active bitmask from the `InputManager`.
5. Add testing logs: Whenever the "Insert Coin" or "Start" key is pressed, print a temporary confirmation message to `std::cout` (e.g., "Coin Inserted! Bitmask updated.") to visually audit that the event loop safely translates your real keyboard inputs into hardware registers.

Ensure that the event processing is clean, fast, and does not block the 60Hz loop execution.

# Prompt 7: NEC V60 Instruction Set Extension (Stack and Function Management)

USER REQUEST: Implement stack management, subroutines, and logical instructions for the NEC V60 CPU.

We need to expand our CPU decoder to handle core instructions required for booting games. The NEC V60 heavily relies on stack operations and procedure calls for its main program flow.

### Specifications to Implement:
1. Stack Instructions: `PUSH`, `POP`, `PUSHM` (Push Multiple), `POPM` (Pop Multiple).
2. Control Flow: `CHKN` (Check bounds), `CALL` (Call subroutine), `RET` (Return from subroutine), and conditional jumps (`BZ`, `BNZ`, `BC`, `BNC`, etc. based on PSW flags).
3. Logical Operations: `AND`, `OR`, `XOR`, `NOT`, and bitwise shifts (`SHL`, `SHR`).

### Requirements:
- Update `v60.cpp` with these new opcodes inside the instruction fetch/decode loop.
- Properly manipulate the Stack Pointer (typically R31 or dedicated SP depending on context) and memory addresses.
- Add defensive logging for any misaligned stack operations or unhandled addressing modes.

# Phase 2: CPU instructions and complex decoding


# Prompt 8: Advanced instructions and complex addressing modes

USER REQUEST: Implement advanced arithmetic, bit manipulation, and complex addressing modes for the NEC V60.

Sega Model 1 games use highly optimized 32-bit CISC addressing modes (register indirect, indexed, displacement).

### Specifications to Implement:
1. Advanced Math: `MUL` (Multiply), `DIV` (Divide) for signed and unsigned integers, affecting Overflow and Sign flags.
2. Bit Manipulation: `BCLR` (Bit Clear), `BSET` (Bit Set), `BNOT` (Bit Invert), `TST` (Bit Test).
3. Addressing Mode Decoder: Implement a robust helper function `uint32_t resolve_addressing_mode(...)` to handle complex combinations of base registers, displacements, and scale factors.

### Requirements:
- Ensure all flag updates (PSW) accurately mirror the NEC V60 hardware documentation.
- Compile and verify that the core instruction loop remains clean and auditable.

# Phase 3: Realistic Graphic Rendering & 2D/3D Layers

# Prompt 9: Implementation of the 2D Tile Generator (Scroll Layers)

USER REQUEST: Implement the Sega Model 1 2D Background / Scroll layers in the Video Manager.

Games like Virtua Fighter and Virtua Racing combine 3D polygons with 2D layers for HUDs, text (UI), and backgrounds. We need to parse the Tile VRAM layout.

### Specifications to Implement:
1. Tile Map Structure: Read the 512KB VRAM area dedicated to backgrounds and text maps.
2. Character ROM Parsing: Extract tile graphical patterns (usually 8x8 pixels) from the mapped ROM data.
3. Priority Mixer: Combine up to 4 layers (Scroll 0, Scroll 1, Text, and Polygons) using a priority buffer.

### Requirements:
- Replace the dummy test pattern inside `video_manager.cpp` with a loop that iterates through the parsed background VRAM tiles and draws them onto our 32-bit rendering buffer.


# Phase 3: Realistic Graphic Rendering & 2D/3D Layers

# Prompt 9: Implementation of the 2D Tile Generator (Scroll Layers)

USER REQUEST: Implement the Sega Model 1 2D Background / Scroll layers in the Video Manager.

Games like Virtua Fighter and Virtua Racing combine 3D polygons with 2D layers for HUDs, text (UI), and backgrounds. We need to parse the Tile VRAM layout.

### Specifications to Implement:
1. Tile Map Structure: Read the 512KB VRAM area dedicated to backgrounds and text maps.
2. Character ROM Parsing: Extract tile graphical patterns (usually 8x8 pixels) from the mapped ROM data.
3. Priority Mixer: Combine up to 4 layers (Scroll 0, Scroll 1, Text, and Polygons) using a priority buffer.

### Requirements:
- Replace the dummy test pattern inside `video_manager.cpp` with a loop that iterates through the parsed background VRAM tiles and draws them onto our 32-bit rendering buffer.

# Prompt 10: 3D Polygon Rasterization Pipeline (Flat Shading)

USER REQUEST: Create the 3D Polygon Rasterizer connected to the Fujitsu TGP output.

Now we will link the mathematical output of the TGP (Prompt 5) to the actual visual display to draw the early 3D flat-shaded polygons of Virtua Racing and Virtua Fighter.

### Specifications to Implement:
1. Polygon Wireframe / Flat Filler: Implement a basic scanline triangle filling algorithm (or edge-equation solver) in software.
2. Coordinate Mapping: Take the 2D projected coordinates generated by the TGP and map them to our 496x384 viewport.
3. Face Culling & Depth Sorting: Implement backface culling (ignoring polygons facing away from the camera) and a basic Painter's Algorithm (sorting polygons from back to front by their average Z-depth).

### Requirements:
- The rasterizer must read from the Polygon RAM area, look up color palettes, and draw flat-shaded triangles directly into the Framebuffer.

# Prompt 10: 3D Polygon Rasterization Pipeline (Flat Shading)

USER REQUEST: Create the 3D Polygon Rasterizer connected to the Fujitsu TGP output.

Now we will link the mathematical output of the TGP (Prompt 5) to the actual visual display to draw the early 3D flat-shaded polygons of Virtua Racing and Virtua Fighter.

### Specifications to Implement:
1. Polygon Wireframe / Flat Filler: Implement a basic scanline triangle filling algorithm (or edge-equation solver) in software.
2. Coordinate Mapping: Take the 2D projected coordinates generated by the TGP and map them to our 496x384 viewport.
3. Face Culling & Depth Sorting: Implement backface culling (ignoring polygons facing away from the camera) and a basic Painter's Algorithm (sorting polygons from back to front by their average Z-depth).

### Requirements:
- The rasterizer must read from the Polygon RAM area, look up color palettes, and draw flat-shaded triangles directly into the Framebuffer.

# Phase 4: Audio Subsystem (Z80 & MultiPCM)

# Prompt 11 : Le processeur audio esclave Zilog Z80 et le Bus Son

USER REQUEST: Implement the Zilog Z80 sound CPU skeleton and the Sound Memory Bus.

Sega Model 1 offloads audio processing to a secondary Z80 CPU running at 4 MHz, which communicates with the main CPU through a Command Mailbox.

### Specifications to Implement:
1. Z80 Core Bridge: Integrate a minimal Z80 emulation state machine (Registers: A, B, C, D, E, H, L, PC, SP).
2. Sound Bus: Map the 64KB Z80 address space including Sound RAM and communication ports with the NEC V60.
3. Main Loop Integration: Execute Z80 clock cycles alongside the main V60 CPU inside `motherboard.cpp`.

# Test Prompt: Integration of Unit Testing Infrastructure for Opcodes

USER REQUEST: Implement a unit testing infrastructure using CMake to validate our NEC V60 CPU opcodes.

Before we write more instructions, we need a reliable way to test that our implemented opcodes (NOP, MOV, ADD, SUB) and flag behaviors work perfectly. We want to set up an automated testing target in CMake that validates specific CPU states without launching the full SDL2 window.

### Technical Specifications to Implement:
1. Framework Integration: Update `CMakeLists.txt` to include a standard unit testing framework via CMake's `FetchContent` (such as Catch2 v3) OR create a clean, standalone, macro-based minimal testing runner target named `emulator_tests` if network/FetchContent is not preferred. Let's use a standalone testing target for safety.
2. Isolated CPU Environment: In the testing suite, we need to inject a dummy `Bus` and a mock memory layout where we can manually write specific byte sequences (opcodes), execute exactly 1 cycle via `cpu.execute_cycle()`, and assert the final state of the registers and the PSW flags.

### Requirements for this step:
1. Create a new directory `/tests` at the root of the project.
2. Create `/tests/test_v60_core.cpp` which will contain our first unit test cases.
3. Update `CMakeLists.txt` to add an executable target `emulator_tests` which links against our core files (`src/core/v60.cpp`, `src/core/motherboard.cpp`, etc.) but EXCLUDES `main.cpp` (to avoid duplicate main functions).
4. Implement at least 3 distinct test cases inside `test_v60_core.cpp`:
   - Test `MOV`: Verify that moving a value into a register updates that register correctly.
   - Test `ADD` / `SUB`: Verify arithmetic calculations and ensure the Zero Flag (Z) and Sign Flag (S) inside the PSW are correctly set or cleared based on results (e.g., subtracting R0 from R0 should set the Zero Flag to 1).
   - Test Out-of-Bounds: Verify that fetching an opcode from an unmapped address triggers our defensive logging gracefully.
5. Provide the exact code for the test files and the terminal commands required to compile and run this specific test suite.

Ensure that running the tests outputs a clean, readable checklist in the console showing exactly which tests passed or failed.


# Prompt 7: Stack Management, Functions, and Branching

USER REQUEST: Implement stack operations, subroutine controls, and conditional branches for the NEC V60 CPU.

With our core testing framework validated, we need to implement the next major family of instructions. These are critical for managing the game loops and functional blocks of Virtua Racing and Virtua Fighter.

### Specifications to Implement:
1. Stack Operations:
   - `PUSH rx` / `POP rx`: Decrement/Increment the Stack Pointer (R31) and transfer data.
   - `PUSHM` / `POPM` (Push/Pop Multiple): Handle pushing or popping multiple registers at once using a bitmask.
2. Control Flow & Subroutines:
   - `CALL address`: Push the current PC onto the stack and jump to the target address.
   - `RET`: Pop the top of the stack back into the PC to return from a subroutine.
3. Conditional Branches (using PSW flags):
   - Implement `BZ` (Branch if Zero), `BNZ` (Branch if Not Zero), `BC` (Branch if Carry), `BNC` (Branch if Not Carry), `BV` (Branch if Overflow).
   - These branches modify the PC by a signed displacement fetched from the instruction stream.

### Requirements for this step:
1. Update `v60.cpp` inside the main decode switch-case statement to handle these new opcode bytes.
2. Add corresponding test cases inside `/tests/test_v60_core.cpp` to verify that `PUSH`/`POP` manipulate memory correctly, and that branches only jump when their respective PSW flag condition is met.
3. Ensure defensive code catches a stack underflow or overflow (e.g., if R31 goes out of bounds of the Main RAM space).

Focus on exact pointer/index manipulation for the stack operations. Do not implement complex math yet.

# Prompt 8: Complex addressing modes and advanced mathematics

USER REQUEST: Implement complex CISC addressing modes and advanced arithmetic (Multiplication/Division) for the NEC V60.

Sega Model 1 titles are written in heavily optimized C and Assembly code that utilizes the NEC V60's advanced addressing modes to traverse complex 3D mesh arrays.

### Specifications to Implement:
1. Addressing Mode Decoder Refactoring:
   - Implement a modular helper function `uint32_t resolve_addressing_mode(uint8_t mode_byte)` capable of decoding Register Indirect, Auto-increment/decrement, Displacement, and Indexed addressing.
2. Advanced Mathematics:
   - `MUL` (Multiply signed/unsigned) and `DIV` (Divide signed/unsigned).
   - Ensure `DIV` updates the Sign and Zero flags based on the quotient, sets the Overflow flag if dividing by zero, and logs an error instead of crashing the host process.
3. Logical Bit Testing:
   - `TST` (Bitwise test affecting Zero and Sign flags without modifying registers).

### Requirements for this step:
1. Integrate the addressing mode logic into your existing opcodes (`MOV`, `ADD`, `SUB`) so they can now accept memory operands, not just registers.
2. Add rigorous unit tests for a complex mode (e.g., Register Indirect with Displacement) and a Division-by-Zero safety test.

# Prompt 9: 2D Layer Generator (Scroll & Text Layers)

USER REQUEST: Implement the 2D Background, Scroll layers, and Text Tilemaps inside the Video Manager.

Sega Model 1 games overlay 3D polygons onto complex 2D layers used for user interfaces (HUDs, speedometers, health bars) and 2D background tilemaps. We need to parse the 512 KB VRAM placeholder structure.

### Specifications to Implement:
1. Tile Map Extraction:
   - Model 1 utilizes standard 8x8 pixel tiles. Parse the layout of the mapped VRAM to read tile indices and color palette offsets.
2. Character Data Reading:
   - Simulate reading from a mock Character ROM block containing the actual 8x8 font/tile pixel definitions.
3. Priority Mixer Layout:
   - Create a clean rendering loop inside `video_manager.cpp` that reads Scroll Layer 0, Scroll Layer 1, and the Text Layer, then maps them onto our active 32-bit SDL pixel array.

### Requirements for this step:
1. Replace the temporary test gradient pattern from Prompt 4 with the actual live rendering of these tile layers.
2. Create a small mock function that pre-populates the Text VRAM with a few standard characters (e.g., "SCORE: 000000" or "INSERT COIN") to visually prove at runtime that the 2D tile subsystem renders correctly to the host screen.

# Prompt 8: Complex addressing modes and advanced mathematics

USER REQUEST: Implement complex CISC addressing modes and advanced arithmetic (Multiplication/Division) for the NEC V60.

Sega Model 1 titles utilize the NEC V60's advanced CISC addressing modes to traverse complex 3D structures, mesh vertex arrays, and game object tables. We need to implement a robust addressing decoder and advanced math operations.

### Specifications to Implement:
1. Addressing Mode Decoder Refactoring:
   - Implement a modular helper function `uint32_t resolve_addressing_mode(uint8_t mode_byte)` or a state structure capable of parsing Register Indirect, Auto-increment/decrement, Displacement (8-bit, 16-bit, 32-bit), and Indexed addressing.
   - Integrate this decoder into existing opcodes (`MOV`, `ADD`, `SUB`) so they can now accept memory operands as well as raw registers.
2. Advanced Mathematics:
   - `MUL` (Multiply signed/unsigned): 32-bit multiplication updating the Sign and Zero flags.
   - `DIV` (Divide signed/unsigned): 32-bit division updating the Sign and Zero flags. 
   - Safety Guard: If a Division-by-Zero is attempted, log a critical warning via `std::cerr`, set the Overflow flag in the PSW, and skip the operation safely without crashing the host emulator process.
3. Logical Bit Testing:
   - `TST` (Bitwise test): Performs a logical AND between operands, affecting the Zero and Sign flags without modifying the destination register.

### Requirements for this step:
1. Update `v60.hpp` and `v60.cpp` with the addressing mode helper and the new math switch cases.
2. Add rigorous unit tests inside `/tests/test_v60_core.cpp` to validate:
   - A complex mode (e.g., Register Indirect with Displacement loading a value from Main RAM).
   - Standard 32-bit signed multiplication and division.
   - The Division-by-Zero safety mechanism (verifying that the emulator survives and flags the error correctly).
3. Ensure the project compiles clean with `-Wall -Wextra`.

Focus on structural accuracy so that every instruction behaves identically to a real physical V60 processor.

# Prompt 9: 2D Layer Generator (Scroll & Text Layers)

USER REQUEST: Implement the 2D Background, Scroll layers, and Text Tilemaps inside the Video Manager.

Sega Model 1 games overlay 3D polygons onto complex 2D layers used for user interfaces (HUDs, speedometers, health bars) and 2D background tilemaps. We need to parse the 512 KB VRAM placeholder structure and replace the temporary test pattern with live tile rendering.

### Specifications to Implement:
1. Tile Map Extraction:
   - Model 1 utilizes standard 8x8 pixel tiles. Parse the layout of the mapped VRAM (0x01000000 to 0x0107FFFF) to read tile indices and color palette offsets.
2. Character Data Reading:
   - Simulate reading from a mock Character ROM block containing the actual 8x8 font/tile pixel definitions.
3. Priority Mixer Layout:
   - Create a clean rendering loop inside `video_manager.cpp` that reads Scroll Layer 0, Scroll Layer 1, and the Text Layer, then maps them onto our active 32-bit SDL pixel array.
   - Transparent pixels (usually index 0 or a specific color key) must not overwrite underlying layers.

### Requirements for this step:
1. Replace the temporary test gradient pattern from Prompt 4 with the actual live rendering of these tile layers.
2. Create a small mock function inside `motherboard.cpp` or `video_manager.cpp` that pre-populates the Text VRAM area with a few standard characters (e.g., "SCORE: 000000", "TIME: 99" or "INSERT COIN") to visually prove at runtime that the 2D tile subsystem renders correctly to the host screen.
3. Add a unit test or validation check to verify that out-of-bounds tile indices are handled safely without overflowing the texture buffers.

Focus on a clean and optimized loop structure for mapping pixels. Do not implement complex 3D math here; keep it focused strictly on the 2D pipeline.

# Prompt 10 : Pipeline de Rendu 3D (Rasterizer de Polygones Flat-Shading) & Fix Écran Noir

USER REQUEST: Implement the 3D Polygon Rasterization pipeline and fix the black screen rendering issue by ensuring a solid background color.

The unit tests for the 2D tilemaps are passing, but the screen is currently black. This is likely because the texture is not being cleared with a visible background or the mock tiles are completely transparent. We need to fix this by adding a solid backdrop color and implementing our core 3D flat-shaded polygon rasterizer connected to the Fujitsu TGP (Prompt 5).

### Specifications to Implement:
1. Clear & Background Fix: 
   - At the start of every frame render inside `video_manager.cpp`, explicitly fill the internal 32-bit pixel array with a solid background color (e.g., Arcade Blue: `0x00, 0x00, 0xFF, 0xFF`) before drawing the 2D tilemaps or polygons. This will immediately diagnose and eliminate the black screen issue.
2. Software Polygon Rasterizer:
   - Implement a simple, fast software scanline triangle filling algorithm (or edge function solver) inside `video_manager.cpp`.
   - It must accept three 2D projected screen coordinates `(X, Y)` and a color value.
3. 3D Pipeline Connection:
   - Create a bridge function `void render_tgp_polygons()` that reads vertex data transformed by the TGP, performs basic backface culling, and sends the 2D projected screen coordinates to your triangle filler.

### Requirements for this step:
1. Update `video_manager.hpp/cpp` to include the triangle rasterizer and the explicit background clear color.
2. In `main.cpp` or `motherboard.cpp`, create a 3D demo routine: send a rotating 3D spinning cube's vertices to the TGP, fetch the projected 2D coordinates, and pass them to the new triangle rasterizer every frame.
3. Ensure the project compiles cleanly and runs at 60 FPS.

We want to see the blue background and a basic flat-shaded 3D object rendering on top of our system.

# Prompt 11: Zilog Z80 Sound CPU Infrastructure & Audio Mailbox

USER REQUEST: Implement the Zilog Z80 Sound CPU skeleton, Sound Memory Bus, and Main CPU Mailbox communication interface.

Our 3D visual engine is rendering beautifully. We now need to lay the foundation for the audio subsystem. The Sega Model 1 utilizes a secondary Zilog Z80 processor (running at 4 MHz) to handle background music and sound effects. The main V60 CPU triggers sounds by writing command bytes into a shared communication register (Mailbox).

### Technical Specifications to Implement:
1. Z80 CPU Emulation Skeleton:
   - Create a clean `Z80` class with its primary 8-bit registers (`A`, `B`, `C`, `D`, `E`, `H`, `L`, `F`) and 16-bit registers (`PC`, `SP`).
   - Implement a basic cycle step function: `void execute_z80_cycle();`.
2. Sound Memory Bus Layout:
   - The Z80 operates on a localized 64 KB address space.
   - Map 8 KB of Sound RAM (`0x0000` to `0x1FFF`).
   - Map a Sound Command Mailbox register (`0x2000`) where it reads commands sent by the main CPU.
3. V60 to Z80 Interface (The Mailbox Bridge):
   - In our central `Bus` class, map the Sound Command write register for the V60 (usually mapped at `0x00A00000` or custom control space). When the V60 writes a byte here, it must instantly mirror inside the Z80's mailbox address (`0x2000`) and trigger a Sound IRQ on the Z80.

### Requirements for this step:
1. Create `/src/core/z80.hpp` and `/src/core/z80.cpp`. Update `CMakeLists.txt` to include them.
2. Instantiate the `Z80` core inside the `Motherboard` class. 
3. Update the main loop inside `motherboard.cpp`: For every frame, while the V60 executes its cycles (approx. 266,666 cycles at 16 MHz), execute the relative amount of cycles for the Z80 (approx. 66,666 cycles at 4 MHz) to ensure perfect timing synchronization.
4. Implement a placeholder switch-case in the Z80 execution loop to handle a custom test instruction (e.g., reading the mailbox byte).
5. Add rigorous unit tests in `/tests/test_sound.cpp` or add test cases to `/tests/test_v60_core.cpp` to verify:
   - Writing a sound command from the main V60 CPU properly lands in the Z80 memory space.
   - The Z80 increments its cycles accurately inline with the motherboard ticks.
6. Ensure the application compiles without warnings and maintains a locked 60 FPS.

Do not write raw audio wave generation code or the full Z80 opcode table yet. Focus entirely on the dual-CPU master/slave execution bridge and memory synchronization.

# Prompt 12: Initial Sega MultiPCM Emulation & SDL Audio Output

USER REQUEST: Implement the baseline Sega MultiPCM sound chip structure and configure the SDL2 Audio pipeline for sound generation.

Now that the master/slave CPU synchronization is operational, we must connect our audio subsystem to the physical speakers of the host PC. The Sega MultiPCM is a 28-channel sample-playback (PCM) chip controlled by the Z80. We will implement its core register structure and a direct SDL2 audio callback pipeline.

### Technical Specifications to Implement:
1. MultiPCM Registers Map:
   - Max 28 independent channels. Each channel has registers for: Source Address (ROM pointer), Loop Address, End Address, Volume/Pan, and Sample Rate (Pitch).
   - The Z80 writes to these registers to trigger PCM samples (PCM ROM data).
2. SDL Audio Output Structure:
   - Configure SDL2 Audio Specifications: 44100 Hz sampling rate, 16-bit signed integer format (`AUDIO_S16SYS`), Stereo (2 channels), and a small buffer size (e.g., 512 or 1024 samples) to ensure ultra-low latency.
   - Implement the SDL Audio Callback function (or an explicit streaming queue) to continuously request audio frames from the emulator.

### Requirements for this step:
1. Create `/src/audio/multipcm.hpp` and `/src/audio/multipcm.cpp`. Update `CMakeLists.txt`.
2. Inside `multipcm.hpp`, define the `MultiPCM` class containing an array of 28 channel structures and a reference to a mock PCM ROM data block.
3. Map the MultiPCM registers to the Z80 Sound Bus I/O zone so the Z80 can read/write to them.
4. Implement a lightweight synthesizer / mixing function inside `multipcm.cpp` that iterates through active channels and accumulates their values into a 16-bit stereo stream.
5. Setup a Test Tone: For verification, if no game PCM data is loaded, implement a basic synthesis mode (e.g., a pure 440 Hz Sine wave or a simple square beep) inside the MultiPCM mixer when an audio channel is triggered.
6. Initialize SDL Audio inside `main.cpp` or `video_manager.cpp`, open the audio device, and start playback.
7. Add a unit test or integration test ensuring that opening/closing the emulator initializes and terminates the SDL audio device safely without thread deadlocks.

Keep the mixer logic clean, avoid dynamic memory allocations inside the audio thread callback, and verify that the application maintains its fluid 60 FPS visual rate while streaming audio.

# Prompt 13: ROM Manifest and Arcade ROM Loader

USER REQUEST: Implement an Arcade ROM Set Directory Loader and mapping mechanism for game files.

Now that our core hardware execution, 3D graphics, and SDL Audio are fully synchronized, we need to load real game data. Sega Model 1 ROM sets (like Virtua Fighter or Virtua Racing) consist of unzipped directories containing multiple binary files mapped to different chips (Main CPU, Voice/PCM, Graphics). We will implement a directory parsing mechanism.

### Technical Specifications to Implement:
1. ROM Manifest / Configuration:
   - For clarity, we will define a simple structured mapping (or a small helper struct) inside our loader that identifies arcade files by their precise filename and maps them to their correct physical memory offsets:
     - Main V60 Program ROM -> Map to Bus address 0x00C00000
     - Sound Z80 Program ROM -> Map to Sound RAM / ROM spaces
     - MultiPCM Samples ROM -> Map to the Sound Chip's PCM Data buffer
2. File System Parsing:
   - Use Modern C++ standard `<filesystem>` (`std::filesystem::directory_iterator`) to scan a directory path passed as a command-line argument.

### Requirements for this step:
1. Update `main.cpp` to accept a command-line parameter representing the game folder path (e.g., `./emulator --romdir ./roms/vfighter/`).
2. Create `/src/core/rom_loader.hpp` and `/src/core/rom_loader.cpp`. Update `CMakeLists.txt`.
3. Implement `bool load_game_directory(const std::string& folder_path, Motherboard& motherboard);`
   - This function must look for the files inside the directory, validate their sizes, and use our existing `bus.write_byte()` or dedicated array accessors to fill the Main ROM, Z80 ROM, and PCM ROM buffers.
4. Add strict error checking: If a critical file (like the primary V60 boot ROM) is missing from the folder, print a specific error to `std::cerr` indicating exactly which file is missing, and abort initialization safely.
5. Write a unit test or validation test inside `/tests/` that creates a temporary directory with mock binary files, executes `load_game_directory`, and asserts that the data was copied to the correct target memory buffers.

Keep the codebase modular and leverage standard C++17/C++20 filesystem wrappers to remain cross-platform.

# Prompt 14: Bootstrap Opcode Implementation & Core Execution Advancement

USER REQUEST: Implement the initial bootstrap opcodes for both the NEC V60 and Motorola 68000 to advance the Virtua Racing boot sequence.

Our ROM loader successfully parsed and mapped all 15 files (9600 KB) for Virtua Racing! The CPUs are now hitting their real entry points but halting on unimplemented bootstrap opcodes. We need to implement these specific instructions and expand the decoder to progress past the early hardware initialization.

### Error Analysis & Specifications to Implement:
1. Motorola 68000 Opcode `0x46FC`:
   - This represents `MOVE.W #<data>, SR` (Move immediate to Status Register).
   - Operation: Load the 16-bit immediate value following the opcode into the 68k Status Register (SR), updating system flags and interrupt masks.
2. NEC V60 Opcode `0x13`:
   - This corresponds to a core memory/register transfer or control flow instruction at the boot vector `0x00FE000E` (such as a branch or initial move). 
   - Look up the exact NEC V60 instruction matching `0x13` byte encoding and implement its full execution state and register mapping.
3. Advance Boot Loop Control:
   - Expand both CPU execution switches to gracefully skip or stub basic setup instructions if they are harmless (like clearing registers or setting internal system modes), instead of halting the entire motherboard immediately.

### Requirements for this step:
1. Update your 68000 and V60 core decoder files to properly execute these specific bootstrap opcodes.
2. Add explicit unit tests for both instructions in your test suite:
   - Test that `68000` executing `0x46FC` accurately sets the Status Register flags.
   - Test that `V60` executing `0x13` modifies its target register or program counter exactly as dictated by the V60 technical manuals.
3. Re-run the emulator with the Virtua Racing directory argument. Ensure the logs show that the execution progresses past `PC=0x00000200` (68k) and `PC=0x00FE000E` (V60).

Keep execution logs verbosely printing the next sequence of fetched opcodes so we can map out the next iteration of the boot cycle.

# Prompt 15: Memory-Mapped I/O Registries & Main Opcode Extensions

USER REQUEST: Map the Sega Model 1 System Control registers (0x00E00000 and 0x00C10002) and implement the BTST (68k) and 0x94 (V60) opcodes.

Our Virtua Racing boot sequence has progressed substantially! The execution is now attempting to write initial system configuration states to unmapped memory locations and hitting new unimplemented instructions. We need to stub or map these I/O registers and add support for the blocking opcodes.

### Technical Analysis & Specifications to Implement:
1. System I/O & Comm RAM Mapping (0x00E00000 - 0x00E0001F):
   - On Sega Model 1, this block represents critical System Registers, Timers, and Communication flags. 
   - Map a fixed 4KB buffer for the `0x00E00000` range inside the `Bus` class. Allow read/write access so the game can store its initial parameters instead of logging critical unmapped errors.
2. V60 I/O Space Write (0x00C10002):
   - Stub writes to this I/O-space control port (often used for lamp outputs, coin counters, or basic system drive control) to prevent errors.
3. Motorola 68000 Opcode `0x0807`:
   - This corresponds to `BTST Dn, Dm` or `BTST #<data>, Dm` (Bit Test).
   - Operation: Test a specific bit in a data register or memory location. Update the Zero (Z) flag in the 68k Status Register according to the state of the tested bit.
4. NEC V60 Opcode `0x94`:
   - This byte represents a core control, move, or arithmetic instruction in the V60 instruction set matrix (check the exact mapping for `0x94`, which is commonly an operation like `STC` / store control register or a structural assignment). Implement its execution context.

### Requirements for this step:
1. Update `bus.cpp` to gracefully map and accept reads/writes to `0x00E00000` (System space) and `0x00C10002`.
2. Implement the `BTST` opcode logic inside your 68000 decoder and the `0x94` opcode inside your V60 decoder.
3. Add dedicated unit tests inside your `/tests` directory to verify that your new `BTST` implementation accurately alters the Zero flag based on mock bit states.
4. Run the simulator again and output the subsequent console logs.

Ensure execution registers and memory tracking flow safely without stalling the emulation loop.

# Prompt 16: Motorola 68000 Quick Math Expansion & NEC V60 Opcodes

USER REQUEST: Implement the missing opcodes 0xBA (V60) and 0x5200 (68000) to advance the Sega Model 1 boot phase.

The system registers configuration layer is now operating correctly without critical memory dropouts. Both CPUs have advanced further into their execution sequence but are now halted on new instructions. We must expand the decoding blocks to handle these core routines.

### Technical Analysis & Opcodes to Implement:
1. Motorola 68000 Opcode `0x5200` (`ADDQ` / ADD Quick):
   - This translates to `ADDQ.B #1, D0` (Add a quick immediate value of 1 to the low byte of data register D0).
   - Implementation: Generalize this block to decode the entire `ADDQ` / `SUBQ` instruction family (Opcode pattern `0101xxx0xxxxxxxx`). It allows adding or subtracting an immediate value from 1 to 8 to/from a target register or memory location. Update the standard 68k flags (X, N, Z, V, C) accordingly.
2. NEC V60 Opcode `0xBA`:
   - Identify the exact instruction matching `0xBA` in the NEC V60 instruction set matrix (this is part of the core data movement, compare, or string handling block used inside early loop controls). Implement its execution context, state changes, and register tracking.

### Requirements for this step:
1. Update your 68000 decoder to fully process the `ADDQ` / `SUBQ` opcodes, ensuring bitmask filtering handles various sizes (Byte, Word, Long) and destinations.
2. Update your V60 decoder to support opcode `0xBA`.
3. Expand your test infrastructure (`/tests`) with unit tests verifying:
   - That `68000` executing `ADDQ` appropriately increments the register and sets flags (e.g., test that adding 1 to `0xFF` wraps to `0x00` and triggers the Zero and Carry flags if operating on a byte).
4. Run the compilation target and feed the Sega Model 1 emulator the Virtua Racing ROM directory to log the next sequence.

Ensure thread cycles and timing remain robust. Let's see how much further the bootstrap jumps!

# Prompt 17: Shift/Rotate Opcode Extensions, V60 Opcode 0xDD & Sound I/O Stubbing

USER REQUEST: Implement the missing opcodes 0xDD (V60) and 0xE098 (68000), and stub the Sound I/O registers to advance the Virtua Racing bootstrap.

The core execution loops are running smoothly, but we have reached new hardware initialization roadblocks. The 68k is attempting to perform bitwise shifting, the V60 has moved to a new execution block, and the audio sub-board configuration is throwing unmapped write errors.

### Technical Analysis & Specifications to Implement:
1. Motorola 68000 Opcode `0xE098` (`ASR` / `LSR` Bit Shift):
   - This opcode pattern represents a logical or arithmetic bit shift operation on a Long-word (32-bit) data register (`ASR.L` / `LSR.L`).
   - Implementation: Expand the 68000 decoder to support the Bit Shift and Rotate family (`ASR`, `ASL`, `LSR`, `LSL`). Ensure that shifting updates the standard status register flags, especially the Carry (C), Negative (N), and Zero (Z) flags based on the shifted outcome.
2. NEC V60 Opcode `0xDD`:
   - Identify the exact instruction matching `0xDD` in the NEC V60 opcode matrix (this is part of the extensive bit field, mathematical operations, or advanced system register manipulations). Implement its structural behavior and flag logic.
3. Sound I/O Port Mapping (`0x00C60013` and `0x00C4000B`):
   - On Sega Model 1, these registers interface with the sound communication control layer and the audio reset/interrupt configurations.
   - Update the `Bus` component to gracefully catch and store (stub) writes to the `0x00C6xxxx` and `0x00C4xxxx` regions so the game code can safely progress without hitting critical unmapped errors.

### Requirements for this step:
1. Update the 68000 core to support register-controlled and immediate bit shifts.
2. Implement the V60 opcode `0xDD` block within your primary CPU instruction loop.
3. Stub the sound control register ranges inside `bus.cpp`.
4. Add clear unit tests in `/tests` to verify that your new 68k shift implementation calculates bit displacement and updates flags accurately.
5. Compile the target binary and test it against the Virtua Racing ROM directory to log the next instructions.

Let's see how much deeper into the main menu or initialization sequence the emulation can go!

# Prompt 18: 3D Polygon Display List Parsing and Vertex Data Uploads

USER REQUEST: Implement the Sega Model 1 Polygon Display List parsing and emulate Vertex Data commands (0x05/0x06).

Both the 68000 and V60 CPUs are now executing smoothly without halting! Virtua Racing is actively progressing through its main frame loops and attempting to render 3D scenes. However, our polygon engine is logging unknown display list commands (0xFFFFFFFF) and skipping vertex data uploads (commands 0x05 and 0x06). We need to implement proper parsing of the hardware display lists.

### Technical Analysis & Specifications to Implement:
1. Polygon Display List Parsing:
   - On Sega Model 1, the video hardware processes polygon data using a dual-buffered Display List RAM. The command `0xFFFFFFFF` or specific bit patterns act as end-of-list markers or uninitialized buffers.
   - Refactor the polygon rendering module to accurately fetch words from the active Display List RAM offset, extracting polygon attributes (color bank, transparency bits, face structures).
2. Vertex Object-Path Commands (0x05 and 0x06):
   - Command `0x05` / `0x06`: These hardware commands stream raw 3D coordinate point clouds (Vertices, Normals) directly from the ROM into the TGP/Geometry RAM buffers.
   - Implement the memory transfer behavior for these commands: capture the source pointer, the destination index, and the word count, then copy the data arrays seamlessly so the TGP math module can transform real car/track meshes.

### Requirements for this step:
1. Update `video_manager.cpp` (or your polygon module) to handle the command loop inside the display list. If `0xFFFFFFFF` is encountered at word 0, ensure it cleanly marks an empty or unrendered frame without triggering critical stalling.
2. Implement handling for commands `0x05` and `0x06` to store incoming vertex meshes into a managed geometry array inside your `TGP` or `VideoManager`.
3. Update the software rasterizer (from Prompt 10) to iterate through these newly populated meshes, sending them through the TGP rotation/projection matrix and rendering real triangles into the active SDL screen buffer.
4. Add a unit test or structural assertion validating that vertex stream counts do not cause buffer overflows in the internal polygon structures.

Let's feed the real 3D geometry of Virtua Racing into our rendering pipeline!

# Prompt 19: NEC V60 Interrupt Handling & System Opcode 0x5D Extension

USER REQUEST: Implement the missing NEC V60 opcode 0x5D to allow the Virtua Racing interrupt handler routine to execute and return properly.

Our emulator has successfully crossed into the real V60 interrupt handler subroutine (`0x00FE02BC`) following a VBlank IRQ! However, while executing inside this hardware routine, the V60 has halted at PC=`0x00FFE0E0` on an unimplemented opcode: `0x5D`. We need to implement this instruction to let the game process its frame logic and return safely to the main program thread.

### Technical Analysis & Specifications to Implement:
1. NEC V60 Opcode `0x5D`:
   - Identify the exact mapping of `0x5D` in the NEC V60 instruction matrix. This opcode is typically linked to system state configuration, task registry manipulation, or specialized control transfers (such as saving/restoring specific system state registers or a variation of a return from exception/interrupt if applicable).
   - Implement its precise bit decoding, register state mutation, and execution pipeline.
2. VBlank IRQ Routine Stabilization:
   - Ensure that after the handler code executes, the CPU can successfully clean the stack frames and re-enable maskable interrupts by restoring the Program Status Word (PSW).

### Requirements for this step:
1. Update `v60.cpp` to decode and execute opcode `0x5D` within the primary instruction loop.
2. Add a comprehensive unit test in `/tests` to validate the state of the registers or system flags before and after running this opcode.
3. Compile the emulator target and execute it using the Virtua Racing ROM directory. Verify that the execution stream successfully moves past PC=`0x00FFE0E0`.

Let's finalize this interrupt execution loop to let the game engines talk to each other seamlessly!

# Prompt 20: NEC V60 Opcode 0x47 Extension & Main Loop Advancement

USER REQUEST: Implement the missing NEC V60 opcode 0x47 to advance the Virtua Racing frame execution loop.

The previous core updates successfully cleared the exception barrier, allowing the V60 to run through 500 frames of gameplay logic! However, the V60 has now halted at PC=`0x00FE09BE` on an unimplemented opcode: `0x47`. We need to integrate this instruction to keep the program execution flowing smoothly.

### Technical Analysis & Specifications to Implement:
1. NEC V60 Opcode `0x47`:
   - Decode and locate the exact specification for `0x47` in the NEC V60 instruction matrix (typically maps to comparison operations like `CMP` or data parsing routines used inside loops right after handling interrupts).
   - Implement its precise bitmasks, memory/register data fetch, and corresponding Program Status Word (PSW) flag changes (such as Zero, Sign, or Carry flags).

### Requirements for this step:
1. Update `v60.cpp` to decode and execute opcode `0x47` within the primary execution switch-case structure.
2. Extend the automated testing project (`/tests`) with a unit test demonstrating that opcode `0x47` correctly alters flags based on various inputs (e.g. matching values vs. non-matching values).
3. Recompile the build directory and execute `./build/model1` pointing to the Virtua Racing directory.

Let's see where the execution lands next now that the main loop infrastructure is stabilizing!

# Prompt 21: UART Sound Communication Channel Activation & Inter-CPU Sync

USER REQUEST: Emulate the UART Serial Control registers to prevent dropped audio bytes (0x81) and stabilize the Main Game Loop.

Incredible milestone! The NEC V60 is now running infinitely within its main arcade frame loop, capturing VBlank IRQs, and returning perfectly (`return PC 0x00FE1433`). However, the loop is currently dropping communication bytes destined for the sound hardware: `[UART sound] WARNING: byte 0x81 arrived while the receiver is disabled`. We need to accurately emulate the UART status/control flags to bridge the main engine with the audio subsystem.

### Technical Analysis & Specifications to Implement:
1. UART Status & Control Registers:
   - Identify the memory-mapped register governing the UART control state (typically checking or writing bits that toggle Receiver Enable `RxE` and Transmitter Enable `TxE`).
   - Modify the UART read routine so that when the game checks the UART Status Register, it returns a bitmask indicating that the transmitter is ready (`TxRDY`) and the receiver is enabled/clear.
2. Byte Routing:
   - When a byte (like `0x81`) is written to the UART transmit buffer by the V60, route it immediately into our Sound Board's incoming command queue, triggering the sound Z80/YM3438 modules.

### Requirements for this step:
1. Update your I/O or UART manager component (`bus.cpp` or custom peripheral file) to force the UART Status register to return an "Enabled & Ready" status bitmask.
2. Ensure that incoming bytes are forwarded straight to the audio command queue instead of being dropped.
3. Verify that the project compiles cleanly and run the Virtua Racing directory.

Let's bridge the serial link to unleash the true interactive data flow of the machine!

# Prompt 22: Video Palette RAM Mapping & Real-Time Geometric Poly Rendering

USER REQUEST: Map the hardware Palette RAM (Color Lookup Tables) and bridge the transformed TGP geometry into the active rasterizer.

The main execution and UART communication pipelines are running flawlessly in a continuous loop! To transition from our rotating test cube to the actual graphics of Virtua Racing, we must connect the Sega Model 1 Palette RAM mapping and parse the vertex colors emitted during the game loops.

### Technical Analysis & Specifications to Implement:
1. Palette RAM Mapping (0x01800000 or dedicated video color space):
   - Sega Model 1 utilizes a specific RAM area to store 16-bit or 32-bit color structures (usually BGR565 or ARGB8888 depending on the subsystem layer).
   - Ensure the `Bus` component maps this region correctly, allowing both V60 writes to load color data during bootstrap and the `VideoManager` to read from it during rendering.
2. Direct Polygon Extraction:
   - Instead of discarding the live display list geometry, configure `video_manager.cpp` to look up the exact color index specified by each polygon data block inside the Palette RAM.
   - Pass the real vertex positions transformed by the active TGP matrices straight into our software scanline triangle filler.

### Requirements for this step:
1. Map and implement read/write access for the dedicated Palette RAM block inside `bus.cpp`.
2. Update the rendering sequence inside `video_manager.cpp` to translate 16-bit arcade color values into standard 32-bit RGBA pixels.
3. Replace the rotating test shape completely with the drawing loop that reads the live scene polygons sent by the V60/TGP architecture.
4. Verify that the project compiles cleanly and run the execution command.

Let's switch on the real colors and shapes of the Virtua Racing engine!

# Prompt 23: Motorola 68000 Opcode 0xBDFC (CMP Extension) & Subsystem Synchronization

USER REQUEST: Implement the missing Motorola 68000 opcode 0xBDFC to unlock the co-processor synchronization loop.

We have successfully rendered the high score screen and background graphics of Virtua Racing! The video subsystem is correctly reading the live Palette RAM data. To progress further into the attract mode or gameplay execution, we must resolve a new halt on the secondary Motorola 68000 CPU at PC=`0x000001B6` caused by the unimplemented opcode: `0xBDFC`.

### Technical Analysis & Specifications to Implement:
1. Motorola 68000 Opcode `0xBDFC`:
   - This opcode falls within the `CMP` (Compare) instruction cluster, specifically encoding a comparison involving Long-word (32-bit) boundaries and immediate values or address register displacements (`CMP.L #<data>, An` or variations).
   - Implement its full decoding mask, evaluate the subtraction math internally (without storing the result in the destination), and update the corresponding 68k Status Register flags (Condition Codes: X, N, Z, V, C).
2. CPU Inter-communication Integrity:
   - Ensure that when the 68000 clears this comparison, it updates the shared Comm RAM flags correctly so the V60 and 68000 remain tightly coupled.

### Requirements for this step:
1. Update your 68000 decoder implementation to parse and handle the `0xBDFC` instruction format cleanly.
2. Inject a precise unit test into the `/tests` folder verifying that running this specific 68k comparison modifies the Zero (Z) or Negative (N) flags correctly depending on simulated mismatched or matched memory states.
3. Build the target project directory and launch `./build/model1` pointing to your Virtua Racing ROM directory.

Let's keep both processors advancing side-by-side!
