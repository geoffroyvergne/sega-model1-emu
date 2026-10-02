// Digital Sound Board tests: the MPEG-1 Layer II decoder (against values
// from an independent decoder, mpg123, for the same synthetic frames), bit
// positions and limits, the board's MPEG registers, playback, looping,
// volume and pan, and its UART interrupting the Z80.

#include "test_framework.hpp"

#include "audio/mp2_decoder.hpp"
#include "core/digital_sound_board.hpp"
#include "core/i8251.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <vector>

namespace {

using model1::DigitalSoundBoard;
using model1::I8251;
using model1::Mp2Decoder;

constexpr std::size_t k_frame_bytes = 576; // 128 kbps at 32 kHz
constexpr uint64_t k_frame_bits = k_frame_bytes * 8;
constexpr uint64_t k_frame_data_bits = 556; // what the synthetic frames use

class BitWriter {
public:
    void put(uint32_t value, int bits) // bits <= 32
    {
        for (int i = bits - 1; i >= 0; --i) {
            m_bits.push_back(((value >> i) & 1u) != 0);
        }
    }
    void pad_to(std::size_t bits) { m_bits.resize(bits, false); }
    // Bytes, with the stream starting `offset` bits into the first byte.
    [[nodiscard]] std::vector<uint8_t> bytes(int offset = 0) const
    {
        std::vector<uint8_t> out((m_bits.size() + static_cast<std::size_t>(offset) + 7) / 8 + 1, 0);
        for (std::size_t i = 0; i < m_bits.size(); ++i) {
            const std::size_t p = i + static_cast<std::size_t>(offset);
            if (m_bits[i]) {
                out[p / 8] = static_cast<uint8_t>(out[p / 8] | (0x80u >> (p % 8)));
            }
        }
        return out;
    }

private:
    std::vector<bool> m_bits;
};

void put_header(BitWriter& w)
{
    w.put(0xFFF, 12); // sync
    w.put(1, 1);      // MPEG-1
    w.put(2, 2);      // layer II
    w.put(1, 1);      // no CRC
    w.put(8, 4);      // 128 kbps
    w.put(2, 2);      // 32 kHz
    w.put(0, 2);      // no padding, private
    w.put(0, 2);      // stereo
    w.put(0, 2);      // mode extension
    w.put(0, 4);      // copyright, original, emphasis
}

// One frame (table B.2a, 27 subbands): channel 0 subband 0 with 15 levels,
// channel 1 subband 1 with 3 levels (grouped), channel 0 subband 5 with 9
// levels (grouped); one scale factor each; codes varying with `seed`.
void put_synthetic_frame(BitWriter& w, int seed, std::size_t frame_index)
{
    put_header(w);
    const int nbal[27] = {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 2, 2};
    auto alloc = [](int ch, int sb) -> int {
        if (ch == 0 && sb == 0) return 3; // 15 levels
        if (ch == 1 && sb == 1) return 1; // 3 levels
        if (ch == 0 && sb == 5) return 4; // 9 levels
        return 0;
    };
    auto scf = [](int ch, int sb) { return ch == 0 ? 9 : (sb == 1 ? 12 : 0); };
    for (int sb = 0; sb < 27; ++sb) {
        for (int ch = 0; ch < 2; ++ch) w.put(static_cast<uint32_t>(alloc(ch, sb)), nbal[sb]);
    }
    for (int sb = 0; sb < 27; ++sb) {
        for (int ch = 0; ch < 2; ++ch) {
            if (alloc(ch, sb) != 0) w.put(2, 2); // one scale factor for the frame
        }
    }
    for (int sb = 0; sb < 27; ++sb) {
        for (int ch = 0; ch < 2; ++ch) {
            if (alloc(ch, sb) != 0) w.put(static_cast<uint32_t>(scf(ch, sb)), 6);
        }
    }
    for (int gr = 0; gr < 12; ++gr) {
        for (int sb = 0; sb < 27; ++sb) {
            for (int ch = 0; ch < 2; ++ch) {
                if (ch == 0 && sb == 0) {
                    for (int k = 0; k < 3; ++k) w.put(static_cast<uint32_t>((gr * 3 + k + seed) % 15), 4);
                } else if (ch == 1 && sb == 1) {
                    w.put(static_cast<uint32_t>((gr * 5 + seed) % 27), 5);
                } else if (ch == 0 && sb == 5) {
                    w.put(static_cast<uint32_t>((gr * 7 + seed * 3) % 729), 10);
                }
            }
        }
    }
    w.pad_to((frame_index + 1) * k_frame_bits);
}

std::vector<uint8_t> synthetic_stream(int frames, int offset = 0)
{
    BitWriter w;
    for (int f = 0; f < frames; ++f) {
        put_synthetic_frame(w, f, static_cast<std::size_t>(f));
    }
    return w.bytes(offset);
}

std::vector<int16_t> decode_all(std::span<const uint8_t> data, uint64_t pos, int frames)
{
    Mp2Decoder decoder;
    std::vector<int16_t> pcm;
    std::array<int16_t, Mp2Decoder::k_samples_per_frame * 2> out{};
    Mp2Decoder::FrameInfo info;
    for (int f = 0; f < frames; ++f) {
        if (!decoder.decode(data, pos, data.size() * 8, out, info)) break;
        pcm.insert(pcm.end(), out.begin(), out.end());
    }
    return pcm;
}

} // namespace

TEST_CASE(mp2_decoder_matches_reference_decoder)
{
    const std::vector<uint8_t> stream = synthetic_stream(3);
    const std::vector<int16_t> pcm = decode_all(stream, 0, 3);
    CHECK_EQ(pcm.size(), 3u * 1152 * 2);
    // Interleaved samples as mpg123 decodes the same three frames (it
    // rounds slightly differently: equal within 1).
    const std::pair<std::size_t, int> expected[] = {
        {0, 0}, {100, -19}, {1001, -752}, {1500, -15650}, {2361, -3772},
        {3415, 1809}, {6608, -4978}, {6910, -8127}, {6911, 2028},
    };
    for (const auto& [index, value] : expected) {
        CHECK(std::abs(pcm[index] - value) <= 1);
    }
    long long sum = 0;
    for (int16_t s : pcm) sum += std::abs(s);
    CHECK(std::llabs(sum - 25452154) <= 50); // mpg123: 25452154
}

TEST_CASE(mp2_decoder_bit_positions_limits_and_silence)
{
    using Out = std::array<int16_t, Mp2Decoder::k_samples_per_frame * 2>;
    // The same frames 3 bits into a byte (as Star Wars Arcade's ROM) decode
    // identically, and the position ends just after each frame's data.
    const std::vector<uint8_t> aligned = synthetic_stream(2);
    const std::vector<uint8_t> shifted = synthetic_stream(2, 3);
    CHECK(decode_all(aligned, 0, 2) == decode_all(shifted, 0, 2));

    Mp2Decoder decoder;
    Out out{};
    Mp2Decoder::FrameInfo info;
    uint64_t pos = 0;
    CHECK(decoder.decode(shifted, pos, shifted.size() * 8, out, info));
    CHECK_EQ(pos, 3u + k_frame_data_bits);
    CHECK_EQ(info.sample_rate, 32000);
    CHECK_EQ(info.channels, 2);
    CHECK_EQ(info.bitrate_kbps, 128);
    // The next search skips the zero padding to the second frame.
    CHECK(decoder.decode(shifted, pos, shifted.size() * 8, out, info));
    CHECK_EQ(pos, 3u + k_frame_bits + k_frame_data_bits);

    // A frame that would read past the limit fails and leaves pos alone.
    Mp2Decoder fresh;
    pos = 0;
    CHECK(!fresh.decode(aligned, pos, k_frame_data_bits - 1, out, info));
    CHECK_EQ(pos, 0u);
    CHECK(fresh.decode(aligned, pos, k_frame_data_bits, out, info));
    // No sync before the limit: fails.
    pos = 600;
    CHECK(!fresh.decode(aligned, pos, k_frame_bits, out, info));
    CHECK_EQ(pos, 600u);

    // A frame with nothing allocated: header + 176 allocation bits, silent.
    BitWriter w;
    put_header(w);
    w.pad_to(32 + 176); // every allocation field 0
    w.pad_to(k_frame_bits);
    const std::vector<uint8_t> silent = w.bytes();
    // (A new decoder: the filter bank would still ring with the last frame.)
    Mp2Decoder quiet;
    pos = 0;
    out.fill(123);
    CHECK(quiet.decode(silent, pos, silent.size() * 8, out, info));
    CHECK_EQ(pos, 32u + 176u);
    bool all_zero = true;
    for (int16_t s : out) all_zero = all_zero && s == 0;
    CHECK(all_zero);
}

namespace {

// A board with the synthetic music at byte 0x1000 and a Z80 program that
// sets up the UART and then idles with interrupts on; its interrupt handler
// stores each received byte at 0x8000.
struct DsbRig {
    static constexpr double k_rate = 32000.0; // output at the music's own rate: no interpolation
    static constexpr uint32_t k_music = 0x1000;
    std::unique_ptr<DigitalSoundBoard> dsb = std::make_unique<DigitalSoundBoard>(k_rate);

    explicit DsbRig(int frames = 3)
    {
        std::vector<uint8_t> program(0x100, 0);
        const std::initializer_list<uint8_t> boot = {
            0x31, 0x00, 0x00,       // LD SP, 0x0000 (top of RAM)
            0x3E, 0x4E, 0xD3, 0xF1, // LD A, 0x4E; OUT (0xF1), A: mode 8N1 x16
            0x3E, 0x15, 0xD3, 0xF1, // LD A, 0x15; OUT (0xF1), A: TX, RX enable, reset errors
            0xED, 0x56, 0xFB,       // IM 1; EI
            0x76, 0x18, 0xFD,       // loop: HALT; JR loop
        };
        std::copy(boot.begin(), boot.end(), program.begin());
        const std::initializer_list<uint8_t> isr = {
            0xDB, 0xF0,             // IN A, (0xF0)
            0x32, 0x00, 0x80,       // LD (0x8000), A
            0xFB, 0xED, 0x4D,       // EI; RETI
        };
        std::copy(isr.begin(), isr.end(), program.begin() + 0x38);
        dsb->load_program(program);
        const std::vector<uint8_t> music = synthetic_stream(frames);
        dsb->load_mpeg_rom(music, k_music);
        dsb->reset();
    }
    void set_address(uint8_t first_port, uint32_t address)
    {
        dsb->out(first_port, static_cast<uint8_t>(address >> 16));
        dsb->out(static_cast<uint8_t>(first_port + 1), static_cast<uint8_t>(address >> 8));
        dsb->out(static_cast<uint8_t>(first_port + 2), static_cast<uint8_t>(address));
    }
    std::vector<int16_t> render(std::size_t frames)
    {
        std::vector<int16_t> out(frames * 2);
        dsb->render(out);
        return out;
    }
};

long long sum_abs(const std::vector<int16_t>& v)
{
    long long s = 0;
    for (int16_t x : v) s += std::abs(x);
    return s;
}

} // namespace

TEST_CASE(dsb_plays_once_from_start_to_end)
{
    DsbRig rig;
    CHECK(rig.dsb->present());
    rig.set_address(0xE2, DsbRig::k_music);
    rig.set_address(0xE5, DsbRig::k_music + 3 * k_frame_bytes);
    rig.dsb->out(0xE8, 0x00); // volume: inverted, 0 = loudest
    CHECK(!rig.dsb->playing());
    CHECK(sum_abs(rig.render(1000)) == 0);
    rig.dsb->out(0xE0, 1); // play once
    CHECK(rig.dsb->playing());

    // Output follows the decoded frames (here at the same rate, one frame
    // late through the interpolator), at volume 0x7F/128.
    const std::vector<int16_t> out = rig.render(3 * 1152 + 100);
    CHECK(sum_abs(out) > 1000000);
    CHECK_EQ(rig.dsb->frames_decoded(), 3u);
    CHECK(!rig.dsb->playing()); // the fourth decode hit the end marker
    // Position registers: the byte reached.
    const uint32_t pos = (uint32_t{rig.dsb->in(0xE2)} << 16) | (uint32_t{rig.dsb->in(0xE3)} << 8) | rig.dsb->in(0xE4);
    CHECK_EQ(pos, DsbRig::k_music + (2 * k_frame_bits + k_frame_data_bits) / 8);
    CHECK(sum_abs(rig.render(500)) == 0);
}

TEST_CASE(dsb_loops_from_the_latched_start)
{
    DsbRig rig;
    rig.set_address(0xE2, DsbRig::k_music);
    rig.set_address(0xE5, DsbRig::k_music + 3 * k_frame_bytes);
    rig.dsb->out(0xE0, 2); // play and loop
    rig.render(1152);
    // Written while playing: latched as the loop start (frame 2).
    rig.set_address(0xE2, DsbRig::k_music + 2 * k_frame_bytes);
    rig.render(3 * 1152);
    CHECK(rig.dsb->playing());
    CHECK_EQ(rig.dsb->frames_decoded(), 4u); // frames 0, 1, 2, then 2 again
    rig.render(3 * 1152);
    CHECK_EQ(rig.dsb->frames_decoded(), 7u);
    CHECK(rig.dsb->position_bytes() >= DsbRig::k_music + 2 * k_frame_bytes);
    // Stop.
    rig.dsb->out(0xE0, 0);
    CHECK(!rig.dsb->playing());
    rig.render(2);
    CHECK(sum_abs(rig.render(100)) == 0);
}

TEST_CASE(dsb_volume_and_pan)
{
    DsbRig loud;
    DsbRig quiet;
    DsbRig muted;
    DsbRig left;
    for (DsbRig* rig : {&loud, &quiet, &muted, &left}) {
        rig->set_address(0xE2, DsbRig::k_music);
        rig->set_address(0xE5, DsbRig::k_music + 3 * k_frame_bytes);
    }
    loud.dsb->out(0xE8, 0x00);
    quiet.dsb->out(0xE8, 0x40);  // ~0x40 & 0x7F = 0x3F
    muted.dsb->out(0xE8, 0x7F);  // 0
    left.dsb->out(0xE8, 0x00);
    left.dsb->out(0xE9, 1);      // left channel on both sides
    for (DsbRig* rig : {&loud, &quiet, &muted, &left}) rig->dsb->out(0xE0, 1);
    const auto a = loud.render(2000), b = quiet.render(2000), c = muted.render(2000), d = left.render(2000);
    CHECK(sum_abs(c) == 0);
    const double ratio = static_cast<double>(sum_abs(b)) / static_cast<double>(sum_abs(a));
    CHECK(ratio > 0.48 && ratio < 0.51); // 0x3F / 0x7F
    bool mirrored = true, left_matches = true;
    for (std::size_t i = 0; i < d.size(); i += 2) {
        mirrored = mirrored && d[i] == d[i + 1];
        left_matches = left_matches && d[i] == a[i];
    }
    CHECK(mirrored);
    CHECK(left_matches);
}

TEST_CASE(dsb_uart_byte_interrupts_the_z80)
{
    DsbRig rig;
    // A sender sharing its line with a main receiver and the board, as
    // the sound board's UART does in Star Wars Arcade.
    I8251 sender("sender");
    I8251 primary("primary");
    sender.connect_transmitter_to(primary);
    sender.add_second_receiver(rig.dsb->uart());
    for (I8251* u : {&sender, &primary}) {
        u->write(1, 0x4E);
        u->write(1, 0x15);
    }
    for (int i = 0; i < 200; ++i) rig.dsb->step(); // the board programs its UART
    CHECK_EQ(rig.dsb->read(0x8000), 0u);
    sender.write(0, 0x42);
    for (int i = 0; i < 30 && !rig.dsb->uart().rx_ready(); ++i) {
        sender.tick(16);
    }
    CHECK(rig.dsb->uart().rx_ready());
    CHECK(primary.rx_ready()); // both ends of the shared line got it
    for (int i = 0; i < 50; ++i) rig.dsb->step();
    CHECK_EQ(rig.dsb->read(0x8000), 0x42u);
    CHECK(!rig.dsb->uart().rx_ready());
}
