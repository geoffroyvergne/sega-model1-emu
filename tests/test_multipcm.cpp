// MultiPCM tests: register interface, sample decoding, pitch, looping, pan
// and attenuation, banking, sound bus mapping, the 440 Hz demo tone, and the
// lockstep audio rate inside the Motherboard.

#include "test_framework.hpp"

#include "audio/multipcm.hpp"
#include "core/motherboard.hpp"
#include "core/sound_board.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using model1::Motherboard;
using model1::MultiPCM;
using model1::SoundBoard;
using model1::SoundBus;

// Writes a 12-byte sample header for sample `index`.
void put_header(MultiPCM& pcm, uint32_t index, uint32_t start, uint32_t loop, uint32_t length)
{
    std::array<uint8_t, 12> h{};
    h[0] = static_cast<uint8_t>(start >> 16);
    h[1] = static_cast<uint8_t>(start >> 8);
    h[2] = static_cast<uint8_t>(start);
    h[3] = static_cast<uint8_t>(loop >> 8);
    h[4] = static_cast<uint8_t>(loop);
    h[5] = static_cast<uint8_t>((0x10000 - length) >> 8);
    h[6] = static_cast<uint8_t>(0x10000 - length);
    pcm.load_sample_rom(h, index * 12);
}

void reg(MultiPCM& pcm, uint8_t slot, uint8_t r, uint8_t value)
{
    pcm.write(1, slot);
    pcm.write(2, r);
    pcm.write(0, value);
}

// Plays sample 0 on slot 0 at 1 ROM sample per output sample (octave 1,
// pitch 0), with the given pan and attenuation, and returns `frames` of
// output (interleaved).
std::vector<int16_t> play(MultiPCM& pcm, std::size_t frames, uint8_t pan = 0, uint8_t attenuation = 0)
{
    reg(pcm, 0, 0, static_cast<uint8_t>(pan << 4));
    reg(pcm, 0, 2, 0);
    reg(pcm, 0, 1, 0);
    reg(pcm, 0, 3, 0x10); // octave 1, pitch 0
    reg(pcm, 0, 5, static_cast<uint8_t>((attenuation << 1) | 1));
    reg(pcm, 0, 4, 0x80);
    std::vector<int16_t> out(frames * 2);
    pcm.generate(out, frames);
    return out;
}

} // namespace

TEST_CASE(multipcm_slot_select_and_key)
{
    auto pcm = std::make_unique<MultiPCM>("PCM");
    put_header(*pcm, 0, 0x100, 0, 16);
    reg(*pcm, 8, 4, 0x80); // slot value 8 -> slot 7
    CHECK(pcm->slot_playing(7));
    CHECK_EQ(pcm->active_slots(), 1);
    reg(*pcm, 30, 4, 0x80); // value 30 -> slot 27
    CHECK(pcm->slot_playing(27));
    reg(*pcm, 8, 4, 0x00); // key off
    CHECK(!pcm->slot_playing(7));

    reg(*pcm, 7, 4, 0x80); // value 7 is not a slot: ignored, logged
    CHECK_EQ(pcm->active_slots(), 1);
    CHECK(model1_test::captured_log().find("invalid slot number") != std::string::npos);
}

TEST_CASE(multipcm_pitch_step)
{
    // step = 2^(octave - 1) x (1 + pitch / 1024), 12-bit fraction (4096 = 1.0)
    auto pcm = std::make_unique<MultiPCM>("PCM");
    reg(*pcm, 0, 3, 0x10); // octave 1, pitch 0
    CHECK_EQ(pcm->slot_step(0), 4096u);
    reg(*pcm, 0, 3, 0x20); // octave 2
    CHECK_EQ(pcm->slot_step(0), 8192u);
    reg(*pcm, 0, 3, 0xF0); // octave -1: x 1/4
    CHECK_EQ(pcm->slot_step(0), 1024u);
    reg(*pcm, 0, 2, static_cast<uint8_t>((268 & 0x3F) << 2));
    reg(*pcm, 0, 3, static_cast<uint8_t>(0x00 | (268 >> 6))); // octave 0, pitch 268
    CHECK_EQ(pcm->slot_step(0), (1024u + 268) * 4 / 2);
}

TEST_CASE(multipcm_8bit_sample_level_pan_and_attenuation)
{
    // Constant sample value 0x40 (8-bit) -> 0x4000; gain at full level is
    // 1/4 (mixing headroom): 0x4000 / 4 = 4096 per channel.
    auto pcm = std::make_unique<MultiPCM>("PCM");
    put_header(*pcm, 0, 0x1000, 0, 32);
    std::vector<uint8_t> wave(32, 0x40);
    pcm->load_sample_rom(wave, 0x1000);

    auto centre = play(*pcm, 8);
    CHECK_EQ(centre[0], 0);    // first output interpolates from silence
    CHECK_EQ(centre[10], 4096); // frame 5, left
    CHECK_EQ(centre[11], 4096); // frame 5, right

    auto muted = play(*pcm, 8, 0x8);
    CHECK_EQ(muted[10], 0);
    CHECK_EQ(muted[11], 0);

    auto right_only = play(*pcm, 8, 0x7); // pan 7: left side muted
    CHECK_EQ(right_only[10], 0);
    CHECK_EQ(right_only[11], 4096);

    auto quiet = play(*pcm, 8, 0, 64); // 64 steps x 0.375 dB = -24 dB
    CHECK(quiet[10] >= 255 && quiet[10] <= 260);
}

TEST_CASE(multipcm_12bit_sample_decoding)
{
    // 12-bit samples are packed in pairs: bytes 0x12 0x34 0x56 hold
    // 0x123 and 0x563 (-> 0x1240 and 0x5630 as 16-bit).
    auto pcm = std::make_unique<MultiPCM>("PCM");
    put_header(*pcm, 0, 0x400000 | 0x2000, 0, 2); // bit 22: 12-bit format
    const std::array<uint8_t, 3> packed = {0x12, 0x34, 0x56};
    pcm->load_sample_rom(packed, 0x2000);
    auto out = play(*pcm, 3);
    // Output lags one sample (interpolation from the previous sample).
    CHECK_EQ(out[2], 0x1240 / 4);
    CHECK_EQ(out[4], 0x5630 / 4);
}

TEST_CASE(multipcm_loop_point)
{
    // Length 4, loop at 2: positions 0 1 2 3 2 3 2 3 ...
    auto pcm = std::make_unique<MultiPCM>("PCM");
    put_header(*pcm, 0, 0x3000, 2, 4);
    const std::array<uint8_t, 4> wave = {0x08, 0x10, 0x20, 0x40};
    pcm->load_sample_rom(wave, 0x3000);
    auto out = play(*pcm, 9);
    std::vector<int16_t> left;
    for (std::size_t i = 1; i < 9; ++i) {
        left.push_back(out[i * 2]);
    }
    const int16_t s0 = 0x0800 / 4, s1 = 0x1000 / 4, s2 = 0x2000 / 4, s3 = 0x4000 / 4;
    const std::vector<int16_t> expected = {s0, s1, s2, s3, s2, s3, s2, s3};
    CHECK(left == expected);
}

TEST_CASE(multipcm_bank_register_selects_second_megabyte)
{
    auto pcm = std::make_unique<MultiPCM>("PCM");
    // Sample at 0x100010, in the banked window (offset 0x10 keeps clear of
    // the header table at the start of the ROM).
    put_header(*pcm, 0, 0x100010, 0, 8);
    std::vector<uint8_t> bank0(8, 0x10), bank2(8, 0x20);
    pcm->load_sample_rom(bank0, 0x000010); // bank 0 maps the first megabyte again
    pcm->load_sample_rom(bank2, 0x200010);
    pcm->set_bank(2);
    auto out = play(*pcm, 4);
    CHECK_EQ(out[6], 0x2000 / 4);
    pcm->set_bank(0);
    out = play(*pcm, 4);
    CHECK_EQ(out[6], 0x1000 / 4);
}

TEST_CASE(multipcm_sound_bus_mapping)
{
    // The 68000 sees each chip's ports at odd addresses (low byte lane).
    auto board = std::make_unique<SoundBoard>();
    SoundBus& bus = board->bus();
    put_header(board->pcm2(), 0, 0x100, 0, 8);
    bus.write_byte(SoundBus::k_pcm2_base + 3, 2);    // slot 2
    bus.write_byte(SoundBus::k_pcm2_base + 5, 4);    // key register
    bus.write_byte(SoundBus::k_pcm2_base + 1, 0x80); // key on
    CHECK(board->pcm2().slot_playing(2));
    CHECK_EQ(board->pcm1().active_slots(), 0);
    bus.write_byte(SoundBus::k_pcm2_base + 0, 0x00); // even address: not connected
    CHECK(board->pcm2().slot_playing(2));
    CHECK_EQ(bus.read_byte(SoundBus::k_pcm1_base + 1), 0u); // reads return 0
    bus.write_byte(SoundBus::k_ym_base + 1, 0x28);  // YM3438 key on/off: FM not synthesized, logged once
    bus.write_byte(SoundBus::k_ym_base + 3, 0xF0);
    CHECK_EQ(board->ym().register_value(0, 0x28), 0xF0u); // stored for later
    const std::string log = model1_test::captured_log();
    CHECK(log.find("[YM3438] WARNING: FM synthesis not emulated") != std::string::npos);
    CHECK(log.find("unmapped") == std::string::npos);
}

TEST_CASE(multipcm_demo_tone_is_440_hz)
{
    auto board = std::make_unique<Motherboard>();
    board->reset();
    board->load_sound_demo();
    board->update_sound_demo(0); // key on

    // One second of chip output: count rising zero crossings.
    const auto rate = static_cast<std::size_t>(SoundBoard::k_audio_rate_hz);
    std::vector<int16_t> out(rate * 2);
    board->sound().pcm1().generate(out, rate);
    int crossings = 0;
    for (std::size_t i = 1; i < rate; ++i) {
        if (out[(i - 1) * 2] < 0 && out[i * 2] >= 0) {
            ++crossings;
        }
    }
    CHECK(crossings >= 438 && crossings <= 442);

    board->update_sound_demo(30); // key off half a second later
    CHECK_EQ(board->sound().pcm1().active_slots(), 0);
}

TEST_CASE(multipcm_audio_generated_in_lockstep)
{
    auto board = std::make_unique<Motherboard>();
    board->reset();
    uint64_t taken = 0;
    for (int frame = 0; frame < 60; ++frame) {
        board->run_frame();
        taken += board->sound().pending_audio().size() / 2;
        board->sound().clear_audio();
    }
    // One emulated second: 10 MHz / 224 = 44,642.86 samples.
    CHECK(taken >= 44'640 && taken <= 44'645);
    CHECK_EQ(board->sound().audio_frames_dropped(), 0u);

    // If nobody takes the audio, the buffer caps and the excess is counted.
    for (int frame = 0; frame < 10; ++frame) {
        board->run_frame();
    }
    CHECK_EQ(board->sound().pending_audio().size() / 2, SoundBoard::k_audio_buffer_frames);
    CHECK(board->sound().audio_frames_dropped() > 0);
}
