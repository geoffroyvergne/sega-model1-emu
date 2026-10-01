#include "core/rom_loader.hpp"

#include "core/log.hpp"
#include "core/motherboard.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <system_error>
#include <vector>

namespace model1 {

namespace {

namespace fs = std::filesystem;

constexpr uint32_t k_data_rom_region = 0x1000000; // V60 region offset of the banked data ROM

// Files on the Model 1 CPU board, shared by every game (TGP tables and
// geometrizer firmware), not used by this emulator. The copro table ROMs
// opr14742 / opr14743 are listed per game: Virtua Racing's real TGP uses
// them.
#define MODEL1_CPU_BOARD_FILES                                                                             \
    {"opr-14744.58", "TGP 1/x table", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},                       \
    {"opr-14745.59", "TGP 1/x table", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},                       \
    {"opr-14746.62", "TGP tables", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},                          \
    {"opr-14747.63", "TGP tables", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},                          \
    {"opr14748.bin", "TGP tables", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},                          \
    {"315-5571.bin", "geometrizer firmware", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},                \
    {"315-5572.bin", "geometrizer firmware", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0}

// Virtua Fighter (MAME "vf").
constexpr RomFileSpec k_vf_files[] = {
    {"epr-16082.14", "V60 program ROM (even bytes)", RomRole::MainCpu, 0x200000, 0x80000, RomLayout::EvenBytes, 0xb23f22ee},
    {"epr-16083.15", "V60 program ROM (odd bytes)", RomRole::MainCpu, 0x200001, 0x80000, RomLayout::OddBytes, 0xd12c77f8},
    {"epr-16080.4", "V60 boot ROM (low)", RomRole::MainCpu, 0xfc0000, 0x20000, RomLayout::Plain, 0x3662e1a5},
    {"epr-16081.5", "V60 boot ROM (high, reset vector)", RomRole::MainCpu, 0xfe0000, 0x20000, RomLayout::Plain, 0x6dec06ce},
    {"mpr-16084.6", "V60 data ROM bank 0 (even)", RomRole::MainCpu, 0x1000000, 0x80000, RomLayout::EvenBytes, 0x483f453b},
    {"mpr-16085.7", "V60 data ROM bank 0 (odd)", RomRole::MainCpu, 0x1000001, 0x80000, RomLayout::OddBytes, 0x5fa01277},
    {"mpr-16086.8", "V60 data ROM bank 1 (even)", RomRole::MainCpu, 0x1100000, 0x80000, RomLayout::EvenBytes, 0xdeac47a1},
    {"mpr-16087.9", "V60 data ROM bank 1 (odd)", RomRole::MainCpu, 0x1100001, 0x80000, RomLayout::OddBytes, 0x7a64daac},
    {"mpr-16088.10", "V60 data ROM bank 2 (even)", RomRole::MainCpu, 0x1200000, 0x80000, RomLayout::EvenBytes, 0xfcda2d1e},
    {"mpr-16089.11", "V60 data ROM bank 2 (odd)", RomRole::MainCpu, 0x1200001, 0x80000, RomLayout::OddBytes, 0x39befbe0},
    {"mpr-16090.12", "V60 data ROM bank 3 (even)", RomRole::MainCpu, 0x1300000, 0x80000, RomLayout::EvenBytes, 0x90c76831},
    {"mpr-16091.13", "V60 data ROM bank 3 (odd)", RomRole::MainCpu, 0x1300001, 0x80000, RomLayout::OddBytes, 0x53115448},
    {"epr-16120.7", "68000 sound program (low)", RomRole::SoundCpu, 0x00000, 0x20000, RomLayout::WordSwap, 0x2bff8378},
    {"epr-16121.8", "68000 sound program (high)", RomRole::SoundCpu, 0x20000, 0x20000, RomLayout::WordSwap, 0xff6723f9},
    {"mpr-16122.32", "MultiPCM 1 samples (low)", RomRole::MultiPcm1, 0x000000, 0x200000, RomLayout::Plain, 0x568bc64e},
    {"mpr-16123.33", "MultiPCM 1 samples (high)", RomRole::MultiPcm1, 0x200000, 0x200000, RomLayout::Plain, 0x15d78844},
    {"mpr-16124.4", "MultiPCM 2 samples (low)", RomRole::MultiPcm2, 0x000000, 0x200000, RomLayout::Plain, 0x45520ba1},
    {"mpr-16125.5", "MultiPCM 2 samples (high)", RomRole::MultiPcm2, 0x200000, 0x200000, RomLayout::Plain, 0x9b4998b6},
    {"315-5724.bin", "TGP firmware", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},
    {"mpr-16096.26", "polygon models", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},
    {"mpr-16097.27", "polygon models", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},
    {"mpr-16098.28", "polygon models", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},
    {"mpr-16099.29", "polygon models", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},
    {"mpr-16100.30", "polygon models", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},
    {"mpr-16101.31", "polygon models", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},
    {"mpr-16102.32", "polygon models", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},
    {"mpr-16103.33", "polygon models", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},
    {"epr-14869b.25", "I/O board firmware", RomRole::IoBoardCpu, 0, 0x10000, RomLayout::Plain, 0x2d093304},
    {"opr14742.bin", "TGP tables", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},
    {"opr14743.bin", "TGP tables", RomRole::NotUsed, 0, 0, RomLayout::Plain, 0},
    MODEL1_CPU_BOARD_FILES,
};

// Virtua Racing (MAME "vr").
constexpr RomFileSpec k_vr_files[] = {
    {"epr-14882.14", "V60 program ROM (even bytes)", RomRole::MainCpu, 0x200000, 0x80000, RomLayout::EvenBytes, 0x547d75ad},
    {"epr-14883.15", "V60 program ROM (odd bytes)", RomRole::MainCpu, 0x200001, 0x80000, RomLayout::OddBytes, 0x6bfad8b1},
    {"epr-14878a.4", "V60 boot ROM (low)", RomRole::MainCpu, 0xfc0000, 0x20000, RomLayout::Plain, 0x6d69e695},
    {"epr-14879a.5", "V60 boot ROM (high, reset vector)", RomRole::MainCpu, 0xfe0000, 0x20000, RomLayout::Plain, 0xd45af9dd},
    {"mpr-14880.6", "V60 data ROM bank 0 (even)", RomRole::MainCpu, 0x1000000, 0x80000, RomLayout::EvenBytes, 0xadc7c208},
    {"mpr-14881.7", "V60 data ROM bank 0 (odd)", RomRole::MainCpu, 0x1000001, 0x80000, RomLayout::OddBytes, 0xe5ab89df},
    {"mpr-14884.8", "V60 data ROM bank 1 (even)", RomRole::MainCpu, 0x1100000, 0x80000, RomLayout::EvenBytes, 0x6cf9c026},
    {"mpr-14885.9", "V60 data ROM bank 1 (odd)", RomRole::MainCpu, 0x1100001, 0x80000, RomLayout::OddBytes, 0xf65c9262},
    {"mpr-14886.10", "V60 data ROM bank 2 (even)", RomRole::MainCpu, 0x1200000, 0x80000, RomLayout::EvenBytes, 0x92868734},
    {"mpr-14887.11", "V60 data ROM bank 2 (odd)", RomRole::MainCpu, 0x1200001, 0x80000, RomLayout::OddBytes, 0x10c7c636},
    {"mpr-14888.12", "V60 data ROM bank 3 (even)", RomRole::MainCpu, 0x1300000, 0x80000, RomLayout::EvenBytes, 0x04bfdc5b},
    {"mpr-14889.13", "V60 data ROM bank 3 (odd)", RomRole::MainCpu, 0x1300001, 0x80000, RomLayout::OddBytes, 0xc49f0486},
    {"epr-14870a.7", "68000 sound program", RomRole::SoundCpu, 0x00000, 0x20000, RomLayout::WordSwap, 0x919d9b75},
    {"mpr-14873.32", "MultiPCM 1 samples", RomRole::MultiPcm1, 0x000000, 0x200000, RomLayout::Plain, 0xb1965190},
    {"mpr-14876.4", "MultiPCM 2 samples", RomRole::MultiPcm2, 0x000000, 0x200000, RomLayout::Plain, 0xba6b2327},
    {"315-5573.bin", "TGP program (MB86233)", RomRole::TgpProgram, 0, 0x2000, RomLayout::Plain, 0x3335a19b},
    {"opr14742.bin", "TGP tables (low halves)", RomRole::TgpTables, 0, 0x20000, RomLayout::Word32Low, 0x446a1085},
    {"opr14743.bin", "TGP tables (high halves)", RomRole::TgpTables, 2, 0x20000, RomLayout::Word32High, 0xe8953554},
    {"mpr-14890.26", "polygon (model) ROM", RomRole::PolygonRom, 0x000000, 0x200000, RomLayout::Word32Low, 0xdcbe006b},
    {"mpr-14891.27", "polygon (model) ROM", RomRole::PolygonRom, 0x000002, 0x200000, RomLayout::Word32High, 0x25832b38},
    {"mpr-14892.28", "polygon (model) ROM", RomRole::PolygonRom, 0x400000, 0x200000, RomLayout::Word32Low, 0x5136f3ba},
    {"mpr-14893.29", "polygon (model) ROM", RomRole::PolygonRom, 0x400002, 0x200000, RomLayout::Word32High, 0x1c531ada},
    {"mpr-14894.30", "polygon (model) ROM", RomRole::PolygonRom, 0x800000, 0x200000, RomLayout::Word32Low, 0x830a71bc},
    {"mpr-14895.31", "polygon (model) ROM", RomRole::PolygonRom, 0x800002, 0x200000, RomLayout::Word32High, 0xaf027ac5},
    {"mpr-14896.32", "polygon (model) ROM", RomRole::PolygonRom, 0xc00000, 0x200000, RomLayout::Word32Low, 0x382091dc},
    {"mpr-14897.33", "polygon (model) ROM", RomRole::PolygonRom, 0xc00002, 0x200000, RomLayout::Word32High, 0x74873195},
    {"mpr-14898.39", "TGP data ROM (byte 0 of each word)", RomRole::TgpData, 0, 0x80000, RomLayout::Byte32Lane, 0x61da2bb6},
    {"mpr-14899.40", "TGP data ROM (byte 1 of each word)", RomRole::TgpData, 1, 0x80000, RomLayout::Byte32Lane, 0x2cd58bee},
    {"mpr-14900.41", "TGP data ROM (byte 2 of each word)", RomRole::TgpData, 2, 0x80000, RomLayout::Byte32Lane, 0xaa7c017d},
    {"mpr-14901.42", "TGP data ROM (byte 3 of each word)", RomRole::TgpData, 3, 0x80000, RomLayout::Byte32Lane, 0x175b7a9a},
    {"93c45.bin", "I/O board EEPROM defaults", RomRole::IoBoardEeprom, 0, 0x80, RomLayout::Plain, 0x65aac303},
    {"epr-14869.25", "I/O board firmware", RomRole::IoBoardCpu, 0, 0x10000, RomLayout::Plain, 0x6187cd7a},
    MODEL1_CPU_BOARD_FILES,
};

#undef MODEL1_CPU_BOARD_FILES

// Older dumps known to misbehave, with what to expect.
struct KnownBadDump {
    std::string_view name;
    uint32_t crc32;
    std::string_view note;
};
constexpr KnownBadDump k_known_bad_dumps[] = {
    {"315-5573.bin", 0xec913af2,
     "the old dump of Virtua Racing's TGP program (replaced in MAME 0.197): the game hangs waiting for TGP results "
     "(MAME Testers bug 07025). Use the corrected dump, CRC32 0x3335a19b"},
};

constexpr GameSpec k_games[] = {
    {"vf", "Virtua Fighter", k_vf_files},
    {"vr", "Virtua Racing", k_vr_files},
};

std::string to_lower(std::string_view text)
{
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

const char* role_name(RomRole role)
{
    switch (role) {
    case RomRole::MainCpu:   return "main board";
    case RomRole::SoundCpu:  return "sound program";
    case RomRole::MultiPcm1: return "MultiPCM 1";
    case RomRole::MultiPcm2: return "MultiPCM 2";
    case RomRole::IoBoardCpu: return "I/O board";
    case RomRole::IoBoardEeprom: return "I/O board EEPROM";
    case RomRole::TgpProgram: return "TGP program";
    case RomRole::TgpTables:  return "TGP tables";
    case RomRole::TgpData:    return "TGP data";
    case RomRole::PolygonRom: return "polygon ROM";
    case RomRole::NotUsed:   return "not used";
    }
    return "?";
}

std::string size_text(uint32_t bytes)
{
    return bytes >= 0x100000 ? std::to_string(bytes / 0x100000) + " MB" : std::to_string(bytes / 1024) + " KB";
}

// Case-insensitive index of the regular files in `folder`.
std::map<std::string, fs::path> index_folder(const fs::path& folder, bool& has_zip)
{
    std::map<std::string, fs::path> files;
    has_zip = false;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string name = to_lower(entry.path().filename().string());
        has_zip = has_zip || entry.path().extension() == ".zip";
        files.emplace(name, entry.path());
    }
    return files;
}

// Files the set cannot run without (the I/O board firmware is optional).
bool is_optional(RomRole role)
{
    return role == RomRole::IoBoardCpu || role == RomRole::IoBoardEeprom || role == RomRole::TgpProgram
        || role == RomRole::TgpTables || role == RomRole::TgpData || role == RomRole::PolygonRom;
}

// Other names some ROM sets use for a file (same contents).
struct AlternateName {
    std::string_view name;
    std::string_view alternate;
};
constexpr AlternateName k_alternate_names[] = {
    {"mpr-14897.33", "mpr-14879.33"}, // Virtua Racing polygon ROM: digits transposed in older sets
};

// The file for `name` in the folder index (or under an alternate name).
std::map<std::string, fs::path>::const_iterator find_rom(const std::map<std::string, fs::path>& files,
                                                         std::string_view name)
{
    auto found = files.find(to_lower(name));
    for (const AlternateName& alt : k_alternate_names) {
        if (found == files.end() && alt.name == name) {
            found = files.find(to_lower(alt.alternate));
        }
    }
    return found;
}

bool is_required(RomRole role)
{
    return role != RomRole::NotUsed && !is_optional(role);
}

std::size_t required_present(const GameSpec& game, const std::map<std::string, fs::path>& files)
{
    return static_cast<std::size_t>(std::count_if(game.files.begin(), game.files.end(), [&files](const RomFileSpec& f) {
        return is_required(f.role) && find_rom(files, f.name) != files.end();
    }));
}

std::size_t required_count(const GameSpec& game)
{
    return static_cast<std::size_t>(std::count_if(game.files.begin(), game.files.end(),
                                                  [](const RomFileSpec& f) { return is_required(f.role); }));
}

bool read_file(const fs::path& path, std::vector<uint8_t>& data)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return file.gcount() == static_cast<std::streamsize>(data.size());
}

// A contiguous block of bytes for one destination, after de-interleaving.
struct Block {
    RomRole role;
    uint32_t offset;
    std::vector<uint8_t> bytes;
};

} // namespace

std::span<const GameSpec> known_games()
{
    return k_games;
}

uint32_t crc32(std::span<const uint8_t> data)
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t byte : data) {
        crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

bool load_game_directory(const std::string& folder_path, Motherboard& motherboard, std::string_view game_id)
{
    const fs::path folder(folder_path);
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        std::cerr << "[ROM] ERROR: '" << folder_path << "' is not a directory\n";
        return false;
    }
    bool has_zip = false;
    auto files = index_folder(folder, has_zip);

    // Device firmware: MAME keeps the I/O board's in its own set,
    // "model1io", next to the game's. Files in the game folder win.
    // The folder as given (symbolic links not followed), without a trailing
    // separator: its parent holds the other sets.
    fs::path given = fs::absolute(folder, ec).lexically_normal();
    if (given.filename().empty()) {
        given = given.parent_path();
    }
    const fs::path parent = given.parent_path();
    const fs::path device_folder = parent / "model1io";
    bool device_zip_only = false;
    if (fs::is_directory(device_folder, ec)) {
        bool device_has_zip = false;
        for (auto& [name, path] : index_folder(device_folder, device_has_zip)) {
            files.emplace(name, path);
        }
    } else {
        device_zip_only = fs::exists(parent / "model1io.zip", ec);
    }

    // --- Which game?
    const GameSpec* game = nullptr;
    if (!game_id.empty()) {
        for (const GameSpec& g : k_games) {
            if (g.id == game_id) {
                game = &g;
            }
        }
        if (game == nullptr) {
            std::cerr << "[ROM] ERROR: unknown game '" << game_id << "'. Known games:";
            for (const GameSpec& g : k_games) {
                std::cerr << ' ' << g.id << " (" << g.title << ')';
            }
            std::cerr << '\n';
            return false;
        }
    } else {
        // The game whose needed files are (most) present.
        std::size_t best = 0;
        for (const GameSpec& g : k_games) {
            const std::size_t present = required_present(g, files);
            if (present > best) {
                best = present;
                game = &g;
            }
        }
        if (game == nullptr) {
            std::cerr << "[ROM] ERROR: no Model 1 ROM files found in '" << folder_path << "'";
            if (has_zip) {
                std::cerr << " (found .zip files: unzip the ROM set into the folder first)";
            }
            std::cerr << '\n';
            return false;
        }
    }
    std::cerr << "[ROM] " << game->title << " (" << game->id << ") in '" << folder_path << "': "
              << required_present(*game, files) << " of " << required_count(*game) << " needed files found\n";

    // --- Phase 1a: every needed file present with the right size? Report all
    // problems at once; nothing is read or written unless the set is complete.
    std::vector<std::pair<const RomFileSpec*, fs::path>> found_files;
    bool ok = true;
    for (const RomFileSpec& spec : game->files) {
        if (spec.role == RomRole::NotUsed) {
            continue;
        }
        const auto found = find_rom(files, spec.name);
        if (is_optional(spec.role)) {
            // Optional: without it, a high-level stand-in replaces the board.
            std::error_code size_ec;
            if (found == files.end() || fs::file_size(found->second, size_ec) != spec.size || size_ec) {
                std::cerr << "[ROM] WARNING: '" << spec.name << "' (" << spec.description << ") missing or wrong size: "
                          << (spec.role == RomRole::IoBoardCpu      ? "using the high-level I/O board stand-in"
                              : spec.role == RomRole::IoBoardEeprom ? "the I/O board EEPROM starts erased"
                              : spec.role == RomRole::PolygonRom    ? "3D objects from polygon ROM will be missing"
                                                                    : "using the high-level TGP")
                          << "\n";
                if (spec.role == RomRole::IoBoardCpu && device_zip_only) {
                    std::cerr << "[ROM]          (found " << (parent / "model1io.zip").string()
                              << ": unzip it into a 'model1io' folder next to the game folder)\n";
                }
            } else {
                found_files.emplace_back(&spec, found->second);
            }
            continue;
        }
        if (found == files.end()) {
            std::cerr << "[ROM] ERROR: missing file '" << spec.name << "' (" << spec.description << ", "
                      << size_text(spec.size) << ")\n";
            ok = false;
            continue;
        }
        const std::uintmax_t size = fs::file_size(found->second, ec);
        if (ec || size != spec.size) {
            std::cerr << "[ROM] ERROR: '" << spec.name << "' is " << (ec ? 0 : size) << " bytes, expected "
                      << spec.size << " (" << spec.description << ")\n";
            ok = false;
            continue;
        }
        found_files.emplace_back(&spec, found->second);
    }
    if (!ok) {
        std::cerr << "[ROM] ERROR: ROM set incomplete, nothing loaded\n";
        return false;
    }

    // --- Phase 1b: read them all and check their CRC32.
    struct Loaded {
        const RomFileSpec* spec;
        std::vector<uint8_t> data;
    };
    std::vector<Loaded> loaded;
    for (const auto& [spec, path] : found_files) {
        Loaded entry{spec, std::vector<uint8_t>(spec->size)};
        if (!read_file(path, entry.data)) {
            std::cerr << "[ROM] ERROR: cannot read '" << path.string() << "', nothing loaded\n";
            return false;
        }
        const uint32_t crc = crc32(entry.data);
        if (crc != spec->crc32) {
            std::cerr << "[ROM] WARNING: '" << spec->name << "' CRC32 " << Hex{crc} << ", expected " << Hex{spec->crc32}
                      << " (bad dump or different revision); loading anyway\n";
            for (const KnownBadDump& bad : k_known_bad_dumps) {
                if (bad.name == spec->name && bad.crc32 == crc) {
                    std::cerr << "[ROM] WARNING: '" << spec->name << "' is " << bad.note << '\n';
                }
            }
        }
        loaded.push_back(std::move(entry));
    }

    // --- Phase 2: de-interleave into contiguous blocks, then write them.
    std::map<std::pair<RomRole, uint32_t>, Block> blocks;
    for (const Loaded& entry : loaded) {
        const RomFileSpec& spec = *entry.spec;
        switch (spec.layout) {
        case RomLayout::Plain:
            blocks[{spec.role, spec.offset}] = Block{spec.role, spec.offset, entry.data};
            break;
        case RomLayout::WordSwap: {
            std::vector<uint8_t> swapped(entry.data);
            for (std::size_t i = 0; i + 1 < swapped.size(); i += 2) {
                std::swap(swapped[i], swapped[i + 1]);
            }
            blocks[{spec.role, spec.offset}] = Block{spec.role, spec.offset, std::move(swapped)};
            break;
        }
        case RomLayout::Word32Low:
        case RomLayout::Word32High:
        case RomLayout::Byte32Lane: {
            // Interleaved into 32-bit little-endian words: file element i
            // (16-bit word or byte) goes to byte offset 4i + lane.
            const uint32_t base = spec.offset & ~3u;
            const std::size_t element = spec.layout == RomLayout::Byte32Lane ? 1 : 2;
            Block& block = blocks[{spec.role, base}];
            block.role = spec.role;
            block.offset = base;
            block.bytes.resize(entry.data.size() / element * 4, 0xFF);
            const std::size_t lane = spec.offset & 3u;
            for (std::size_t i = 0; i < entry.data.size() / element; ++i) {
                for (std::size_t b = 0; b < element; ++b) {
                    block.bytes[4 * i + lane + b] = entry.data[i * element + b];
                }
            }
            break;
        }
        case RomLayout::EvenBytes:
        case RomLayout::OddBytes: {
            const uint32_t base = spec.offset & ~1u;
            Block& block = blocks[{spec.role, base}];
            block.role = spec.role;
            block.offset = base;
            block.bytes.resize(static_cast<std::size_t>(spec.size) * 2, 0xFF);
            const std::size_t lane = spec.offset & 1u;
            for (std::size_t i = 0; i < entry.data.size(); ++i) {
                block.bytes[2 * i + lane] = entry.data[i];
            }
            break;
        }
        }
    }

    Bus& bus = motherboard.bus();
    SoundBoard& sound = motherboard.sound();
    std::size_t total = 0;
    for (const auto& [key, block] : blocks) {
        bool written = false;
        switch (block.role) {
        case RomRole::MainCpu:
            written = block.offset >= k_data_rom_region
                ? bus.load_data_rom(block.bytes, block.offset - k_data_rom_region)
                : bus.load_rom_data(block.bytes, block.offset);
            break;
        case RomRole::SoundCpu:
            written = sound.bus().load_rom(block.bytes, block.offset);
            break;
        case RomRole::MultiPcm1:
            written = sound.pcm1().load_sample_rom(block.bytes, block.offset);
            break;
        case RomRole::MultiPcm2:
            written = sound.pcm2().load_sample_rom(block.bytes, block.offset);
            break;
        case RomRole::IoBoardCpu:
            written = motherboard.io_board().load_rom(block.bytes);
            break;
        case RomRole::IoBoardEeprom: {
            std::array<uint16_t, Eeprom93c46::k_words> words{}; // 16-bit little-endian words
            for (std::size_t i = 0; i < words.size() && 2 * i + 1 < block.bytes.size(); ++i) {
                words[i] = static_cast<uint16_t>(block.bytes[2 * i] | (block.bytes[2 * i + 1] << 8));
            }
            motherboard.io_board().eeprom().set_contents(words);
            written = true;
            break;
        }
        case RomRole::TgpProgram:
            written = motherboard.tgp_copro().load_program(block.bytes);
            break;
        case RomRole::TgpTables:
            written = motherboard.tgp_copro().load_tables(block.bytes);
            break;
        case RomRole::TgpData:
            written = motherboard.tgp_copro().load_data_rom(block.bytes);
            break;
        case RomRole::PolygonRom:
            written = motherboard.polygons().load_poly_rom(block.bytes, block.offset);
            break;
        case RomRole::NotUsed:
            written = true;
            break;
        }
        if (!written) {
            // Only possible if a manifest entry is wrong.
            std::cerr << "[ROM] ERROR: could not place " << block.bytes.size() << " bytes for " << role_name(block.role)
                      << " at offset " << Hex{block.offset} << '\n';
            return false;
        }
        total += block.bytes.size();
    }

    std::size_t unused_present = 0;
    for (const RomFileSpec& spec : game->files) {
        if (spec.role == RomRole::NotUsed && files.contains(to_lower(spec.name))) {
            ++unused_present;
        }
    }
    std::cerr << "[ROM] Loaded " << loaded.size() << " files (" << total / 1024 << " KB) for " << game->title;
    if (unused_present > 0) {
        std::cerr << "; " << unused_present << " other files of the set are present but not used";
    }
    std::cerr << '\n';

    // The control panel: Virtua Racing's wheel, pedals, view buttons and
    // shifter, or the default joystick + buttons.
    motherboard.inputs().set_profile(std::string_view(game->id) == "vr" ? InputManager::Profile::VirtuaRacing
                                                                        : InputManager::Profile::VirtuaFighter);
    return true;
}

} // namespace model1
