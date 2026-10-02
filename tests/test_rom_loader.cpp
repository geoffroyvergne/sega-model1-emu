// ROM set loader tests: mock ROM sets (real file names and sizes, patterned
// contents) are written to temporary directories and loaded; the tests then
// check every destination: interleaved program ROM, boot ROM, banked data
// ROM, byte-swapped sound program, MultiPCM sample ROMs - and that errors
// leave the machine untouched.

#include "test_framework.hpp"

#include "core/motherboard.hpp"
#include "core/rom_loader.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using model1::GameSpec;
using model1::Motherboard;
using model1::RomFileSpec;
using model1::RomRole;

// A fresh temporary directory, removed afterwards.
class TempDir {
public:
    TempDir()
    {
        static std::atomic<int> counter{0};
        m_path = fs::temp_directory_path() / ("model1_rom_test_" + std::to_string(counter++) + "_" +
                                              std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(m_path);
        fs::create_directories(m_path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const fs::path& path() const { return m_path; }
    [[nodiscard]] std::string str() const { return m_path.string(); }

private:
    fs::path m_path;
};

// Deterministic content for byte `i` of the file called `name`.
uint8_t pattern(std::string_view name, std::size_t i)
{
    uint32_t seed = 0;
    for (char c : name) {
        seed = seed * 31 + static_cast<uint8_t>(c);
    }
    return static_cast<uint8_t>(seed + i * 13 + (i >> 9));
}

void write_file(const fs::path& path, std::size_t size, std::string_view pattern_name)
{
    std::vector<char> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<char>(pattern(pattern_name, i));
    }
    std::ofstream(path, std::ios::binary).write(data.data(), static_cast<std::streamsize>(size));
}

const GameSpec& game(std::string_view id)
{
    for (const GameSpec& g : model1::known_games()) {
        if (g.id == id) {
            return g;
        }
    }
    return model1::known_games().front();
}

// Writes every file the emulator needs for `id` (optionally upper-cased,
// optionally skipping one file).
void write_set(const fs::path& folder, std::string_view id, bool upper_case = false, std::string_view skip = {})
{
    fs::create_directories(folder);
    for (const RomFileSpec& spec : game(id).files) {
        if (spec.role == RomRole::NotUsed || spec.name == skip) {
            continue;
        }
        std::string name(spec.name);
        if (upper_case) {
            for (char& c : name) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
        }
        write_file(folder / name, spec.size, spec.name);
    }
}

void write_set(const TempDir& dir, std::string_view id, bool upper_case = false, std::string_view skip = {})
{
    write_set(dir.path(), id, upper_case, skip);
}

bool log_contains(std::string_view text)
{
    return model1_test::captured_log().find(text) != std::string::npos;
}

} // namespace

TEST_CASE(rom_crc32_reference_value)
{
    const std::string_view check = "123456789";
    const std::vector<uint8_t> bytes(check.begin(), check.end());
    CHECK_EQ(model1::crc32(bytes), 0xCBF43926u);
    CHECK_EQ(model1::crc32({}), 0u);
}

TEST_CASE(rom_virtua_fighter_set_lands_in_the_right_places)
{
    TempDir dir;
    write_set(dir, "vf");
    write_file(dir.path() / "315-5571.bin", 16, "unused"); // a geometrizer program: recognised, not used
    auto board = std::make_unique<Motherboard>();
    board->reset();
    CHECK(model1::load_game_directory(dir.str(), *board)); // game detected from the files
    CHECK(log_contains("Virtua Fighter (vf)"));
    CHECK(log_contains("18 of 18 needed files found"));
    // 18 needed + the optional I/O board firmware, TGP program, 2 table ROMs
    // and 8 polygon ROMs.
    CHECK(log_contains("Loaded 30 files"));
    CHECK(board->io_board().has_firmware());
    CHECK(log_contains("present but not used"));
    CHECK(log_contains("CRC32")); // mock contents: checksum warnings, still loaded
    CHECK(board->tgp_copro().is_active()); // program + tables (VF has no TGP data ROM)

    model1::Bus& bus = board->bus();
    // Program ROM: two 512 KB chips byte-interleaved at 0x200000.
    CHECK_EQ(bus.read_byte(0x200000), pattern("epr-16082.14", 0));
    CHECK_EQ(bus.read_byte(0x200001), pattern("epr-16083.15", 0));
    CHECK_EQ(bus.read_byte(0x200002), pattern("epr-16082.14", 1));
    CHECK_EQ(bus.read_byte(0x2FFFFE), pattern("epr-16082.14", 0x7FFFF));
    CHECK_EQ(bus.read_byte(0x2FFFFF), pattern("epr-16083.15", 0x7FFFF));
    // Boot ROM: two 128 KB chips; the reset vector area is the last one.
    CHECK_EQ(bus.read_byte(0xFC0000), pattern("epr-16080.4", 0));
    CHECK_EQ(bus.read_byte(0xFE0000), pattern("epr-16081.5", 0));
    CHECK_EQ(bus.read_byte(0xFFFFF0), pattern("epr-16081.5", 0x1FFF0));
    // Banked data ROM: bank 0 after reset; the bank register switches it.
    CHECK_EQ(bus.read_byte(0x100000), pattern("mpr-16084.6", 0));
    CHECK_EQ(bus.read_byte(0x100001), pattern("mpr-16085.7", 0));
    bus.write_word(Motherboard::k_bank_register_port, 0x11); // (bank 1 << 4) | 1
    CHECK_EQ(bus.read_byte(0x100000), pattern("mpr-16086.8", 0));
    bus.write_byte(Motherboard::k_bank_register_port, 0x31); // byte write: bank 3
    CHECK_EQ(bus.read_byte(0x1FFFFF), pattern("mpr-16091.13", 0x7FFFF));
    bus.write_word(Motherboard::k_bank_register_port, 0x71); // bank 7: not populated
    CHECK_EQ(bus.read_byte(0x100000), 0xFFu);
    bus.write_word(Motherboard::k_bank_register_port, 0x02); // low nibble 2: other bank, no effect
    CHECK_EQ(bus.data_bank(), 7u);

    // Sound program: byte-swapped words; ROM 0x20000+ mirrored at 0x080000.
    model1::SoundBus& sound = board->sound().bus();
    CHECK_EQ(sound.read_byte(0x00000), pattern("epr-16120.7", 1));
    CHECK_EQ(sound.read_byte(0x00001), pattern("epr-16120.7", 0));
    CHECK_EQ(sound.read_byte(0x20000), pattern("epr-16121.8", 1));
    CHECK_EQ(sound.read_byte(0x80000), sound.read_byte(0x20000));
    // MultiPCM sample ROMs: two 2 MB chips each.
    CHECK_EQ(board->sound().pcm1().sample_rom_byte(0), pattern("mpr-16122.32", 0));
    CHECK_EQ(board->sound().pcm1().sample_rom_byte(0x200000), pattern("mpr-16123.33", 0));
    CHECK_EQ(board->sound().pcm2().sample_rom_byte(0x3FFFFF), pattern("mpr-16125.5", 0x1FFFFF));

    // After reset the 68000 takes its stack pointer and PC from the loaded
    // (byte-swapped) vectors.
    board->reset();
    const uint32_t ssp = (static_cast<uint32_t>(pattern("epr-16120.7", 1)) << 24) |
                         (static_cast<uint32_t>(pattern("epr-16120.7", 0)) << 16) |
                         (static_cast<uint32_t>(pattern("epr-16120.7", 3)) << 8) | pattern("epr-16120.7", 2);
    CHECK_EQ(board->sound().cpu().a(7), ssp);
    CHECK_EQ(bus.data_bank(), 0u); // reset selects bank 0
}

TEST_CASE(rom_virtua_racing_detected_and_case_insensitive)
{
    TempDir dir;
    write_set(dir, "vr", /*upper_case=*/true);
    auto board = std::make_unique<Motherboard>();
    board->reset();
    CHECK(model1::load_game_directory(dir.str(), *board));
    CHECK(log_contains("Virtua Racing (vr)"));
    CHECK_EQ(board->bus().read_byte(0x200001), pattern("epr-14883.15", 0));
    CHECK_EQ(board->sound().pcm2().sample_rom_byte(5), pattern("mpr-14876.4", 5));
}

TEST_CASE(rom_missing_critical_file_is_reported_and_nothing_is_loaded)
{
    TempDir dir;
    write_set(dir, "vf", false, "epr-16081.5"); // the boot ROM holding the reset vector
    auto board = std::make_unique<Motherboard>();
    board->reset();
    CHECK(!model1::load_game_directory(dir.str(), *board));
    CHECK(log_contains("[ROM] ERROR: missing file 'epr-16081.5' (V60 boot ROM (high, reset vector), 128 KB)"));
    CHECK(log_contains("nothing loaded"));
    // All-or-nothing: even the files that were present were not written.
    CHECK_EQ(board->bus().read_byte(0x200000), 0xFFu);
    CHECK_EQ(board->bus().read_byte(0xFC0000), 0xFFu);
    CHECK_EQ(board->sound().pcm1().sample_rom_byte(0), 0u);
}

TEST_CASE(rom_wrong_size_is_reported)
{
    TempDir dir;
    write_set(dir, "vr");
    write_file(dir.path() / "epr-14882.14", 1000, "epr-14882.14"); // truncated dump
    auto board = std::make_unique<Motherboard>();
    board->reset();
    CHECK(!model1::load_game_directory(dir.str(), *board));
    CHECK(log_contains("'epr-14882.14' is 1000 bytes, expected 524288"));
    CHECK_EQ(board->bus().read_byte(0x200000), 0xFFu);
}

TEST_CASE(rom_bad_folders_and_game_names)
{
    auto board = std::make_unique<Motherboard>();
    board->reset();
    CHECK(!model1::load_game_directory("/definitely/not/a/folder", *board));
    CHECK(log_contains("is not a directory"));

    TempDir empty;
    write_file(empty.path() / "vf.zip", 10, "zip");
    CHECK(!model1::load_game_directory(empty.str(), *board));
    CHECK(log_contains("unzip the ROM set"));

    TempDir vr;
    write_set(vr, "vr");
    CHECK(!model1::load_game_directory(vr.str(), *board, "vf")); // forced game, wrong files
    CHECK(log_contains("missing file 'epr-16082.14'"));
    CHECK(!model1::load_game_directory(vr.str(), *board, "outrun"));
    CHECK(log_contains("unknown game 'outrun'"));
}

TEST_CASE(rom_io_board_firmware_is_optional)
{
    TempDir dir;
    write_set(dir, "vr");
    std::filesystem::remove(dir.path() / "epr-14869.25");
    auto board = std::make_unique<Motherboard>();
    board->reset();
    CHECK(model1::load_game_directory(dir.str(), *board));
    CHECK(log_contains("'epr-14869.25' (I/O board firmware) missing or wrong size: using the high-level I/O board stand-in"));
    CHECK(!board->io_board().has_firmware());
    board->reset();
    CHECK_EQ(board->bus().read_byte(0xC00010), 0xFFu); // stand-in publishes the ports
}

TEST_CASE(rom_io_board_firmware_found_in_model1io_device_folder)
{
    // MAME layout: the I/O board firmware lives in its own set, "model1io",
    // next to the game's folder.
    TempDir dir;
    write_set(dir.path() / "vr", "vr", false, "epr-14869.25");
    fs::create_directories(dir.path() / "model1io");
    write_file(dir.path() / "model1io" / "epr-14869.25", 0x10000, "epr-14869.25");
    auto board = std::make_unique<Motherboard>();
    board->reset();
    CHECK(model1::load_game_directory((dir.path() / "vr").string(), *board));
    CHECK(board->io_board().has_firmware());
    CHECK(!log_contains("I/O board firmware) missing"));
}

TEST_CASE(rom_io_board_zip_only_gives_unzip_hint)
{
    TempDir dir;
    write_set(dir.path() / "vr", "vr", false, "epr-14869.25");
    write_file(dir.path() / "model1io.zip", 16, "zip");
    auto board = std::make_unique<Motherboard>();
    board->reset();
    CHECK(model1::load_game_directory((dir.path() / "vr").string(), *board));
    CHECK(!board->io_board().has_firmware());
    CHECK(log_contains("unzip it into a 'model1io' folder next to the game folder"));
}

TEST_CASE(rom_eeprom_defaults_loaded_from_93c45)
{
    TempDir dir;
    write_set(dir, "vr");
    std::vector<char> eeprom(0x80, 0);
    eeprom[0] = 0x45;          // word 0 = 0x5345 ("SE"), little-endian
    eeprom[1] = 0x53;
    eeprom[0x7E] = 0x34;       // last word = 0x1234
    eeprom[0x7F] = 0x12;
    std::ofstream(dir.path() / "93c45.bin", std::ios::binary).write(eeprom.data(), 0x80);
    auto board = std::make_unique<Motherboard>();
    board->reset();
    CHECK(model1::load_game_directory(dir.str(), *board));
    CHECK_EQ(board->io_board().eeprom().word(0), 0x5345u);
    CHECK_EQ(board->io_board().eeprom().word(63), 0x1234u);
    board->reset(); // the EEPROM keeps its contents across resets
    CHECK_EQ(board->io_board().eeprom().word(0), 0x5345u);
}

