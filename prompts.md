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

