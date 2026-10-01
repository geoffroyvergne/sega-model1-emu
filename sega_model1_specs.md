# SEGA MODEL 1 - HARDWARE ARCHITECTURE DATA SHEET

## Main CPU
- CPU Type: NEC uPD70616 (V60) 32-bit RISC-like CISC processor.
- Clock Speed: 16 MHz.
- Target Games Address Mapping: Main ROMs contain specific entry points for Virtua Racing, Virtua Fighter, and Star Wars Arcade.

## Co-Processor / DSP (Digital Signal Processor)
- Type: Fujitsu MB86290 (TGP - Geometry Processor).
- Purpose: Handles 3D matrix operations, vertex transformations, and clipping math.
- Critical behavior: Heavy floating-point calculations used extensively by Virtua Racing.

## Video System
- Display: Raster graphics, 496 x 384 pixels resolution at 60Hz.
- Capabilities: Hardware polygons (Flat shading, Texture mapping added in variations), scroll layers, and priority mixer.
- Framebuffer: Double-buffered VRAM layout.

## Audio Subsystem
- Sound CPU: Zilog Z80 (Clocked at 4 MHz) driving a Sega MultiPCM sound chip (28 channels).
