#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace model1 {

class Motherboard;

// Loads an unzipped Model 1 ROM set (a directory of chip dumps, named as in
// MAME) into the emulated machine.
//
// Each supported game has a manifest - file name, size, CRC32, and where
// the file goes - taken from MAME's model1 ROM definitions:
//
//   V60 region offsets (little-endian):
//     0x200000-0x2FFFFF  program ROM: two 512 KB chips, byte-interleaved
//                        (even file -> even addresses, odd file -> odd)
//     0xFC0000-0xFFFFFF  boot ROM: two 128 KB chips (holds the reset address)
//     0x1000000+         banked data ROM: interleaved chip pairs, 1 MB per
//                        bank, seen through the 0x100000 window
//   68000 sound program: 128 KB chips stored as byte-swapped 16-bit words
//   MultiPCM 1 / 2 sample ROMs: 2 MB chips
//   I/O board Z80 firmware (64 KB): optional - if missing or wrong-sized, a
//   warning is printed and a high-level stand-in replaces the board. As in
//   MAME, it may sit in its own device set: a 'model1io' folder next to the
//   game folder is searched too. 93c45.bin, the board EEPROM's factory
//   contents, is loaded into the EEPROM when present.
//   TGP program (8 KB), tables (two 128 KB chips -> 64K 32-bit words) and
//   data ROMs (four 512 KB chips -> 2 MB of 32-bit words), Virtua Racing:
//   optional; with all of them loaded the real TGP DSP runs
//
// Other files of the set are recognised but not used: Virtua Racing's
// spare TGP tables and geometrizer firmware (unused by MAME too), and
// Virtua Fighter's TGP program and polygon ROMs (not wired up yet).
//
// Loading is all-or-nothing: every file the emulator needs is found
// (case-insensitively), size-checked and read first. Any missing or
// wrong-sized file is reported by name and nothing is written. A CRC32
// mismatch (bad dump or different revision) is reported but still loaded,
// as MAME does.
//
// After a successful load, reset the machine so the CPUs fetch their reset
// vectors from the new ROMs.

enum class RomRole {
    MainCpu,      // offset is a V60 region offset (see above)
    SoundCpu,     // offset into the 68000 program ROM
    MultiPcm1,    // offset into MultiPCM 1's sample ROM
    MultiPcm2,    // offset into MultiPCM 2's sample ROM
    IoBoardCpu,   // I/O board Z80 firmware: optional; without it a
                  // high-level stand-in replaces the board
    IoBoardEeprom, // I/O board EEPROM factory contents (93c45.bin): optional
    TgpProgram,   // TGP (MB86233) program, tables and data ROM: optional as
    TgpTables,    // a group; with all of them the real TGP runs, otherwise
    TgpData,      // the high-level Tgp stands in
    PolygonRom,   // 3D model ROM (16 MB of 32-bit words): optional
    DsbProgram,   // Digital Sound Board Z80 program and MPEG data: optional
    DsbMpeg,      // as a group; without them there is no music
    NotUsed,      // part of the set, not needed by this emulator yet
};

enum class RomLayout {
    Plain,      // file bytes in order
    EvenBytes,  // file byte i -> region offset + 2i
    OddBytes,   // file byte i -> region offset + 2i (offset is odd)
    WordSwap,   // bytes swapped within each 16-bit word
    Word32Low,  // file 16-bit word i -> bytes 4i, 4i+1 (low half of 32-bit word i)
    Word32High, // file 16-bit word i -> bytes 4i+2, 4i+3 (offset ends in 2)
    Byte32Lane, // file byte i -> byte 4i + (offset & 3)
};

struct RomFileSpec {
    std::string_view name;
    std::string_view description;
    RomRole role;
    uint32_t offset;
    uint32_t size;      // bytes; 0 for NotUsed entries (not checked)
    RomLayout layout;
    uint32_t crc32;     // 0 for NotUsed entries
};

struct GameSpec {
    std::string_view id;     // MAME short name, e.g. "vf"
    std::string_view title;
    std::span<const RomFileSpec> files;
};

// All games the loader knows.
[[nodiscard]] std::span<const GameSpec> known_games();

// CRC-32 (IEEE 802.3), as used by MAME and zip files.
[[nodiscard]] uint32_t crc32(std::span<const uint8_t> data);

// Loads the ROM set in `folder_path`. `game_id` selects the game; if empty,
// the game is detected from the files present. Returns false (with errors
// on std::cerr) if the folder or a needed file is missing or the wrong
// size; in that case no emulated memory is modified.
bool load_game_directory(const std::string& folder_path, Motherboard& motherboard, std::string_view game_id = {});

} // namespace model1
