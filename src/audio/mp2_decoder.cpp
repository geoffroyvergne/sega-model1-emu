#include "audio/mp2_decoder.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace model1 {

namespace {

// Analysis window D[i] x 65536 (ISO/IEC 11172-3 table 3-B.3: every
// coefficient is a multiple of 2^-16).
constexpr std::array<int32_t, 512> k_window = {
    0, -1, -1, -1, -1, -1, -1, -2, -2, -2, -2, -3, -3, -4, -4, -5,
    -5, -6, -7, -7, -8, -9, -10, -11, -13, -14, -16, -17, -19, -21, -24, -26,
    -29, -31, -35, -38, -41, -45, -49, -53, -58, -63, -68, -73, -79, -85, -91, -97,
    -104, -111, -117, -125, -132, -139, -147, -154, -161, -169, -176, -183, -190, -196, -202, -208,
    213, 218, 222, 225, 227, 228, 228, 227, 224, 221, 215, 208, 200, 189, 177, 163,
    146, 127, 106, 83, 57, 29, -2, -36, -72, -111, -153, -197, -244, -294, -347, -401,
    -459, -519, -581, -645, -711, -779, -848, -919, -991, -1064, -1137, -1210, -1283, -1356, -1428, -1498,
    -1567, -1634, -1698, -1759, -1817, -1870, -1919, -1962, -2001, -2032, -2057, -2075, -2085, -2087, -2080, -2063,
    2037, 2000, 1952, 1893, 1822, 1739, 1644, 1535, 1414, 1280, 1131, 970, 794, 605, 402, 185,
    -45, -288, -545, -814, -1095, -1388, -1692, -2006, -2330, -2663, -3004, -3351, -3705, -4063, -4425, -4788,
    -5153, -5517, -5879, -6237, -6589, -6935, -7271, -7597, -7910, -8209, -8491, -8755, -8998, -9219, -9416, -9585,
    -9727, -9838, -9916, -9959, -9966, -9935, -9863, -9750, -9592, -9389, -9139, -8840, -8492, -8092, -7640, -7134,
    6574, 5959, 5288, 4561, 3776, 2935, 2037, 1082, 70, -998, -2122, -3300, -4533, -5818, -7154, -8540,
    -9975, -11455, -12980, -14548, -16155, -17799, -19478, -21189, -22929, -24694, -26482, -28289, -30112, -31947, -33791, -35640,
    -37489, -39336, -41176, -43006, -44821, -46617, -48390, -50137, -51853, -53534, -55178, -56778, -58333, -59838, -61289, -62684,
    -64019, -65290, -66494, -67629, -68692, -69679, -70590, -71420, -72169, -72835, -73415, -73908, -74313, -74630, -74856, -74992,
    75038, 74992, 74856, 74630, 74313, 73908, 73415, 72835, 72169, 71420, 70590, 69679, 68692, 67629, 66494, 65290,
    64019, 62684, 61289, 59838, 58333, 56778, 55178, 53534, 51853, 50137, 48390, 46617, 44821, 43006, 41176, 39336,
    37489, 35640, 33791, 31947, 30112, 28289, 26482, 24694, 22929, 21189, 19478, 17799, 16155, 14548, 12980, 11455,
    9975, 8540, 7154, 5818, 4533, 3300, 2122, 998, -70, -1082, -2037, -2935, -3776, -4561, -5288, -5959,
    6574, 7134, 7640, 8092, 8492, 8840, 9139, 9389, 9592, 9750, 9863, 9935, 9966, 9959, 9916, 9838,
    9727, 9585, 9416, 9219, 8998, 8755, 8491, 8209, 7910, 7597, 7271, 6935, 6589, 6237, 5879, 5517,
    5153, 4788, 4425, 4063, 3705, 3351, 3004, 2663, 2330, 2006, 1692, 1388, 1095, 814, 545, 288,
    45, -185, -402, -605, -794, -970, -1131, -1280, -1414, -1535, -1644, -1739, -1822, -1893, -1952, -2000,
    2037, 2063, 2080, 2087, 2085, 2075, 2057, 2032, 2001, 1962, 1919, 1870, 1817, 1759, 1698, 1634,
    1567, 1498, 1428, 1356, 1283, 1210, 1137, 1064, 991, 919, 848, 779, 711, 645, 581, 519,
    459, 401, 347, 294, 244, 197, 153, 111, 72, 36, 2, -29, -57, -83, -106, -127,
    -146, -163, -177, -189, -200, -208, -215, -221, -224, -227, -228, -228, -227, -225, -222, -218,
    213, 208, 202, 196, 190, 183, 176, 169, 161, 154, 147, 139, 132, 125, 117, 111,
    104, 97, 91, 85, 79, 73, 68, 63, 58, 53, 49, 45, 41, 38, 35, 31,
    29, 26, 24, 21, 19, 17, 16, 14, 13, 11, 10, 9, 8, 7, 7, 6,
    5, 5, 4, 4, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1,
};

// Matrixing coefficients N[i][k] = cos((16 + i)(2k + 1) pi / 64).
struct Matrix {
    std::array<std::array<double, 32>, 64> n{};
    Matrix()
    {
        for (int i = 0; i < 64; ++i) {
            for (int k = 0; k < 32; ++k) {
                n[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] =
                    std::cos((16 + i) * (2 * k + 1) * std::numbers::pi / 64.0);
            }
        }
    }
};
const Matrix& matrix()
{
    static const Matrix m;
    return m;
}

constexpr std::array<int, 16> k_bitrates = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0};
constexpr std::array<int, 4> k_sample_rates = {44100, 48000, 32000, 0};

// Quantization: levels per allocation code, by subband class (table B.2).
// 0 = no samples.
using Row = std::array<uint16_t, 16>;
constexpr Row k_row_a = {0, 3, 7, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767, 65535};
constexpr Row k_row_b = {0, 3, 5, 7, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 65535};
constexpr Row k_row_c = {0, 3, 5, 7, 9, 15, 31, 65535};
constexpr Row k_row_d = {0, 3, 5, 65535};
constexpr Row k_row_e = {0, 3, 5, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767};
constexpr Row k_row_f = {0, 3, 5, 9, 15, 31, 63, 127};

struct Table {
    int sblimit;
    // Per subband: allocation field width and its row.
    std::array<int, 32> nbal;
    std::array<const Row*, 32> rows;
};

Table make_table(char which)
{
    Table t{};
    auto set = [&t](int first, int last, int bits, const Row* row) {
        for (int sb = first; sb <= last; ++sb) {
            t.nbal[static_cast<std::size_t>(sb)] = bits;
            t.rows[static_cast<std::size_t>(sb)] = row;
        }
    };
    if (which == 'a' || which == 'b') {
        t.sblimit = which == 'a' ? 27 : 30;
        set(0, 2, 4, &k_row_a);
        set(3, 10, 4, &k_row_b);
        set(11, 22, 3, &k_row_c);
        set(23, t.sblimit - 1, 2, &k_row_d);
    } else {
        t.sblimit = which == 'c' ? 8 : 12;
        set(0, 1, 4, &k_row_e);
        set(2, t.sblimit - 1, 3, &k_row_f);
    }
    return t;
}

const Table& table(char which)
{
    static const Table a = make_table('a'), b = make_table('b'), c = make_table('c'), d = make_table('d');
    switch (which) {
    case 'a': return a;
    case 'b': return b;
    case 'c': return c;
    default: return d;
    }
}

// Table B.2 choice from the bit rate per channel and the sampling rate.
char choose_table(int bitrate_kbps, int channels, int sample_rate)
{
    if (bitrate_kbps == 0) {
        return sample_rate == 48000 ? 'a' : 'b'; // free format: the largest table
    }
    const int per_channel = bitrate_kbps / channels;
    if (per_channel <= 48) {
        return sample_rate == 32000 ? 'd' : 'c';
    }
    if (per_channel <= 80 || sample_rate == 48000) {
        return 'a';
    }
    return 'b';
}

// Scale factor i: 2^(1 - i/3) (table B.1); 63 is not a valid index.
double scalefactor(uint32_t index)
{
    return index >= 63 ? 0.0 : std::exp2(1.0 - static_cast<double>(index) / 3.0);
}

// Bits of one codeword: grouped codes carry three samples.
int code_bits(uint16_t levels)
{
    switch (levels) {
    case 3: return 5;
    case 5: return 7;
    case 9: return 10;
    default: return std::bit_width(static_cast<unsigned>(levels)); // 2^n - 1 levels -> n bits
    }
}

bool grouped(uint16_t levels) { return levels == 3 || levels == 5 || levels == 9; }

double requantize(uint32_t code, uint16_t levels)
{
    return (2.0 * code - levels + 1.0) / levels;
}

// MSB-first bit reader over [pos, limit); reading past the limit sets the
// overrun flag and returns 0.
struct BitReader {
    std::span<const uint8_t> data;
    uint64_t pos;
    uint64_t limit;
    bool overrun = false;

    uint32_t bit(uint64_t p) const { return (data[static_cast<std::size_t>(p >> 3)] >> (7 - (p & 7))) & 1u; }
    uint32_t get(int bits)
    {
        if (pos + static_cast<uint64_t>(bits) > limit) {
            overrun = true;
            pos = limit;
            return 0;
        }
        uint32_t v = 0;
        for (int i = 0; i < bits; ++i) {
            v = (v << 1) | bit(pos++);
        }
        return v;
    }
};

} // namespace

Mp2Decoder::Mp2Decoder()
{
    reset();
}

void Mp2Decoder::reset()
{
    for (auto& v : m_v) {
        v.fill(0.0);
    }
    m_v_offset.fill(0);
}

bool Mp2Decoder::decode(std::span<const uint8_t> data, uint64_t& pos, uint64_t limit,
                        std::span<int16_t, k_samples_per_frame * 2> out, FrameInfo& info)
{
    limit = std::min<uint64_t>(limit, uint64_t{data.size()} * 8);
    BitReader r{data, pos, limit};

    // Sync: twelve 1 bits, ID 1 (MPEG-1), layer "10" (II).
    uint32_t window = 0;
    int have = 0;
    for (;;) {
        if (r.pos >= limit) {
            return false;
        }
        window = ((window << 1) | r.bit(r.pos++)) & 0x7FFF;
        if (++have >= 15 && window == 0x7FFE) {
            break;
        }
    }
    const uint32_t protection = r.get(1);
    const uint32_t bitrate_index = r.get(4);
    const uint32_t rate_index = r.get(2);
    r.get(2); // padding, private
    const uint32_t mode = r.get(2);
    const uint32_t mode_ext = r.get(2);
    r.get(4); // copyright, original, emphasis
    if (protection == 0) {
        r.get(16); // CRC, not checked
    }
    if (r.overrun || bitrate_index == 15 || rate_index == 3) {
        return false;
    }
    const int channels = mode == 3 ? 1 : 2;
    info.sample_rate = k_sample_rates[rate_index];
    info.channels = channels;
    info.bitrate_kbps = k_bitrates[bitrate_index];
    const Table& t = table(choose_table(info.bitrate_kbps, channels, info.sample_rate));
    const int sblimit = t.sblimit;
    const int bound = mode == 1 ? std::min(static_cast<int>(mode_ext + 1) * 4, sblimit) : sblimit;

    // Bit allocation (levels per channel and subband).
    std::array<std::array<uint16_t, 32>, 2> levels{};
    for (int sb = 0; sb < sblimit; ++sb) {
        const auto s = static_cast<std::size_t>(sb);
        for (int ch = 0; ch < channels; ++ch) {
            const auto c = static_cast<std::size_t>(ch);
            if (sb < bound || ch == 0) {
                levels[c][s] = (*t.rows[s])[r.get(t.nbal[s])];
            } else {
                levels[c][s] = levels[0][s];
            }
        }
    }
    // Scale factor selection information.
    std::array<std::array<uint32_t, 32>, 2> scfsi{};
    for (int sb = 0; sb < sblimit; ++sb) {
        for (int ch = 0; ch < channels; ++ch) {
            if (levels[static_cast<std::size_t>(ch)][static_cast<std::size_t>(sb)] != 0) {
                scfsi[static_cast<std::size_t>(ch)][static_cast<std::size_t>(sb)] = r.get(2);
            }
        }
    }
    // Scale factors for the three parts of the frame (4 granules each).
    std::array<std::array<std::array<double, 3>, 32>, 2> scale{};
    for (int sb = 0; sb < sblimit; ++sb) {
        const auto s = static_cast<std::size_t>(sb);
        for (int ch = 0; ch < channels; ++ch) {
            const auto c = static_cast<std::size_t>(ch);
            if (levels[c][s] == 0) {
                continue;
            }
            auto& f = scale[c][s];
            switch (scfsi[c][s]) {
            case 0:
                f[0] = scalefactor(r.get(6));
                f[1] = scalefactor(r.get(6));
                f[2] = scalefactor(r.get(6));
                break;
            case 1:
                f[0] = f[1] = scalefactor(r.get(6));
                f[2] = scalefactor(r.get(6));
                break;
            case 2:
                f[0] = f[1] = f[2] = scalefactor(r.get(6));
                break;
            default:
                f[0] = scalefactor(r.get(6));
                f[1] = f[2] = scalefactor(r.get(6));
                break;
            }
        }
    }
    if (r.overrun) {
        return false;
    }

    // Samples: 12 granules of 3 blocks of 32 subband samples.
    std::array<std::array<std::array<double, k_subbands>, 3>, 2> blocks{};
    int16_t* o = out.data();
    for (int gr = 0; gr < 12; ++gr) {
        const auto part = static_cast<std::size_t>(gr / 4);
        for (auto& ch_blocks : blocks) {
            for (auto& b : ch_blocks) {
                b.fill(0.0);
            }
        }
        for (int sb = 0; sb < sblimit; ++sb) {
            const auto s = static_cast<std::size_t>(sb);
            std::array<double, 3> shared{};
            for (int ch = 0; ch < channels; ++ch) {
                const auto c = static_cast<std::size_t>(ch);
                const uint16_t n = levels[c][s];
                if (n == 0) {
                    continue;
                }
                std::array<double, 3> v{};
                if (sb >= bound && ch == 1) {
                    v = shared; // intensity stereo: one set of samples, two scale factors
                } else if (grouped(n)) {
                    uint32_t code = r.get(code_bits(n));
                    for (double& x : v) {
                        x = requantize(code % n, n);
                        code /= n;
                    }
                } else {
                    const int bits = code_bits(n);
                    for (double& x : v) {
                        x = requantize(r.get(bits), n);
                    }
                }
                shared = v;
                for (std::size_t i = 0; i < 3; ++i) {
                    blocks[c][i][s] = v[i] * scale[c][s][part];
                }
            }
        }
        if (r.overrun) {
            return false;
        }
        for (std::size_t i = 0; i < 3; ++i) {
            for (int ch = 0; ch < channels; ++ch) {
                synthesize(ch, blocks[static_cast<std::size_t>(ch)][i], o + ch, 2);
            }
            if (channels == 1) {
                for (std::size_t j = 0; j < k_subbands; ++j) {
                    o[j * 2 + 1] = o[j * 2];
                }
            }
            o += k_subbands * 2;
        }
    }
    pos = r.pos;
    return true;
}

void Mp2Decoder::synthesize(int channel, const std::array<double, k_subbands>& subbands, int16_t* out, std::size_t stride)
{
    // V is kept twice over (a ring of 1024 mirrored at +1024) so the 16
    // newest blocks are always contiguous from the current offset.
    auto& v = m_v[static_cast<std::size_t>(channel)];
    std::size_t& offset = m_v_offset[static_cast<std::size_t>(channel)];
    offset = (offset + 1024 - 64) % 1024;
    const auto& n = matrix().n;
    for (std::size_t i = 0; i < 64; ++i) {
        double sum = 0.0;
        for (std::size_t k = 0; k < k_subbands; ++k) {
            sum += n[i][k] * subbands[k];
        }
        v[offset + i] = sum;
        v[offset + i + 1024] = sum;
    }
    const double* vv = v.data() + offset;
    // U from V (blocks of 32 alternately taken from the first and last
    // quarters of each 128-value group), windowed by D, summed per output.
    for (std::size_t j = 0; j < k_subbands; ++j) {
        double sum = 0.0;
        for (std::size_t i = 0; i < 8; ++i) {
            sum += vv[i * 128 + j] * k_window[i * 64 + j];
            sum += vv[i * 128 + 96 + j] * k_window[i * 64 + 32 + j];
        }
        const double sample = std::round(sum / 65536.0 * 32768.0);
        out[j * stride] = static_cast<int16_t>(std::clamp(sample, -32768.0, 32767.0));
    }
}

} // namespace model1
