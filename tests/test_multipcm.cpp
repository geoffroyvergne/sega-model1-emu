// MultiPCM tests: register interface, sample decoding, pitch, looping, pan
// and attenuation, envelopes, attenuation glides, LFOs, banking, sound bus
// mapping, the 440 Hz demo tone, and the lockstep audio rate inside the
// Motherboard.

#include "test_framework.hpp"

#include "audio/multipcm.hpp"
#include "core/motherboard.hpp"
#include "core/sound_board.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

using model1::Motherboard;
using model1::MultiPCM;
using model1::SoundBoard;
using model1::SoundBus;

// Envelope header bytes 8-10. The default plays at full level at once and
// stops at once at key-off: attack 15, no decay, key rate scaling off,
// release 15.
struct EnvelopeBytes {
    uint8_t attack_decay1 = 0xF0;
    uint8_t level_decay2 = 0x00;
    uint8_t krs_release = 0xFF;
};

// Writes a 12-byte sample header for sample `index`.
void put_header(MultiPCM& pcm, uint32_t index, uint32_t start, uint32_t loop, uint32_t length,
                EnvelopeBytes envelope = {}, uint8_t lfo = 0, uint8_t tremolo = 0)
{
    std::array<uint8_t, 12> h{};
    h[0] = static_cast<uint8_t>(start >> 16);
    h[1] = static_cast<uint8_t>(start >> 8);
    h[2] = static_cast<uint8_t>(start);
    h[3] = static_cast<uint8_t>(loop >> 8);
    h[4] = static_cast<uint8_t>(loop);
    h[5] = static_cast<uint8_t>((0x10000 - length) >> 8);
    h[6] = static_cast<uint8_t>(0x10000 - length);
    h[7] = lfo;
    h[8] = envelope.attack_decay1;
    h[9] = envelope.level_decay2;
    h[10] = envelope.krs_release;
    h[11] = tremolo;
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

// Output for a 16-bit sample value `v` at full envelope (0x3FF: -0.09 dB,
// gain 4052 / 4096), centre pan and no attenuation, as MAME computes it:
// the envelope's x4 and the attenuation table's /4 cancel out.
int16_t full_level(int32_t v)
{
    return static_cast<int16_t>((1024 * ((v * 4052) >> 10)) >> 12);
}

} // namespace

TEST_CASE(multipcm_slot_select_and_key)
{
    auto pcm = std::make_unique<MultiPCM>("PCM");
    put_header(*pcm, 0, 0x100, 0, 16); // release rate 15: key-off stops at once
    reg(*pcm, 8, 1, 0);    // slot value 8 -> slot 7: sample 0
    reg(*pcm, 30, 1, 0);   // slot value 30 -> slot 27
    reg(*pcm, 8, 4, 0x80); // key on slot 7
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
    // Constant sample value 0x40 (8-bit) -> 0x4000, at full envelope.
    auto pcm = std::make_unique<MultiPCM>("PCM");
    put_header(*pcm, 0, 0x1000, 0, 32);
    std::vector<uint8_t> wave(32, 0x40);
    pcm->load_sample_rom(wave, 0x1000);

    auto centre = play(*pcm, 8);
    CHECK_EQ(centre[0], 0);    // first output interpolates from silence
    CHECK_EQ(centre[10], full_level(0x4000)); // frame 5, left (16208)
    CHECK_EQ(centre[11], full_level(0x4000)); // frame 5, right

    auto muted = play(*pcm, 8, 0x8);
    CHECK_EQ(muted[10], 0);
    CHECK_EQ(muted[11], 0);

    auto right_only = play(*pcm, 8, 0x7); // pan 7: left side muted
    CHECK_EQ(right_only[10], 0);
    CHECK_EQ(right_only[11], full_level(0x4000));

    auto quiet = play(*pcm, 8, 0, 64); // 64 steps x 0.375 dB = -24 dB: 1/16
    CHECK(quiet[10] >= 1010 && quiet[10] <= 1016);
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
    CHECK_EQ(out[2], full_level(0x1240));
    CHECK_EQ(out[4], full_level(0x5630));
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
    const int16_t s0 = full_level(0x0800), s1 = full_level(0x1000), s2 = full_level(0x2000), s3 = full_level(0x4000);
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
    CHECK_EQ(out[6], full_level(0x2000));
    pcm->set_bank(0);
    out = play(*pcm, 4);
    CHECK_EQ(out[6], full_level(0x1000));
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
    bus.write_byte(SoundBus::k_ym_base + 1, 0x28);  // YM3438 key on/off register
    bus.write_byte(SoundBus::k_ym_base + 3, 0xF0);
    CHECK_EQ(board->ym().register_value(0, 0x28), 0xF0u);
    CHECK(model1_test::captured_log().find("unmapped") == std::string::npos);
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

    board->update_sound_demo(30); // key off half a second later: the note fades out
    CHECK_EQ(board->sound().pcm1().active_slots(), 1);
    board->sound().pcm1().generate(out, rate / 2);
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

// ---------------------------------------------------------------------------
// Envelope, attenuation glide, LFOs (MAME gew_pcm behaviour)
// ---------------------------------------------------------------------------

namespace {

// Plays a constant 8-bit 0x40 loop on slot 0 with the given header settings.
std::unique_ptr<MultiPCM> constant_tone(EnvelopeBytes envelope, uint8_t lfo = 0, uint8_t tremolo = 0)
{
    auto pcm = std::make_unique<MultiPCM>("PCM");
    put_header(*pcm, 0, 0x1000, 0, 64, envelope, lfo, tremolo);
    std::vector<uint8_t> wave(64, 0x40);
    pcm->load_sample_rom(wave, 0x1000);
    return pcm;
}

std::vector<int16_t> run(MultiPCM& pcm, std::size_t frames)
{
    std::vector<int16_t> out(frames * 2);
    pcm.generate(out, frames);
    return out;
}

} // namespace

TEST_CASE(multipcm_envelope_attack_decay_release)
{
    // Attack 10 (rate 40: 12.15 ms on the 44.1 kHz timebase), decay 1 rate 8
    // down to decay level 4 (6 dB per step: -24 dB, envelope level 0x2FF),
    // then hold (decay 2 rate 0); release 8. Key rate scaling off.
    auto pcm = constant_tone({0xA8, 0x40, 0xF8});
    reg(*pcm, 0, 1, 0);
    reg(*pcm, 0, 3, 0x10);
    reg(*pcm, 0, 5, 1);
    reg(*pcm, 0, 4, 0x80);
    CHECK_EQ(pcm->slot_envelope_level(0), 0x3FF - 0x2A0); // the attack starts at -63 dB

    // The attack rises smoothly: no sample jumps to full level at once.
    auto out = run(*pcm, 2000);
    int largest_jump = 0;
    for (std::size_t i = 2; i < 2000; ++i) {
        largest_jump = std::max(largest_jump, std::abs(out[i * 2] - out[(i - 1) * 2]));
    }
    CHECK(largest_jump < 2000);
    CHECK(pcm->slot_envelope_level(0) < 0x3FF); // decaying after the peak

    // Decay 1 stops at the decay level (4: top 4 bits of the level = 15 - 4).
    run(*pcm, 44643);
    CHECK_EQ(pcm->slot_envelope_level(0) >> 6, 15 - 4);
    const int held = pcm->slot_envelope_level(0);
    run(*pcm, 4464);
    CHECK_EQ(pcm->slot_envelope_level(0), held); // decay 2 rate 0: holds

    // Key off: the release fades the voice out, then it stops.
    reg(*pcm, 0, 4, 0x00);
    CHECK(pcm->slot_playing(0));
    out = run(*pcm, 100);
    CHECK(out[198] < out[2]); // getting quieter
    run(*pcm, 44643 * 2);
    CHECK(!pcm->slot_playing(0));
    CHECK_EQ(pcm->slot_envelope_level(0), 0);
}

TEST_CASE(multipcm_envelope_rate_zero_holds_and_fifteen_is_instant)
{
    // Attack 0 never rises: the voice stays at the -63 dB starting level.
    auto pcm = constant_tone({0x00, 0x00, 0xF0});
    reg(*pcm, 0, 1, 0);
    reg(*pcm, 0, 3, 0x10);
    reg(*pcm, 0, 4, 0x80);
    run(*pcm, 1000);
    CHECK_EQ(pcm->slot_envelope_level(0), 0x3FF - 0x2A0);
    // Release 0 never ends: the voice keeps playing after key-off.
    reg(*pcm, 0, 4, 0x00);
    run(*pcm, 1000);
    CHECK(pcm->slot_playing(0));

    // Attack 15 reaches full level on the first sample.
    auto fast = constant_tone({});
    reg(*fast, 0, 1, 0);
    reg(*fast, 0, 3, 0x10);
    reg(*fast, 0, 4, 0x80);
    run(*fast, 1);
    CHECK_EQ(fast->slot_envelope_level(0), 0x3FF);
}

TEST_CASE(multipcm_envelope_registers_override_the_header)
{
    // Registers 7-9 are loaded from the header, then can be rewritten.
    auto pcm = constant_tone({});
    reg(*pcm, 0, 1, 0);
    reg(*pcm, 0, 3, 0x10);
    reg(*pcm, 0, 7, 0x00); // attack 0 instead of the header's 15
    reg(*pcm, 0, 4, 0x80);
    run(*pcm, 100);
    CHECK_EQ(pcm->slot_envelope_level(0), 0x3FF - 0x2A0);
}

TEST_CASE(multipcm_attenuation_glides_when_bit_0_is_clear)
{
    auto pcm = constant_tone({});
    reg(*pcm, 0, 1, 0);
    reg(*pcm, 0, 3, 0x10);
    reg(*pcm, 0, 5, 1);          // full level, set at once
    reg(*pcm, 0, 4, 0x80);
    run(*pcm, 10);
    reg(*pcm, 0, 5, 0x40 << 1);  // glide to attenuation 0x40
    CHECK_EQ(pcm->slot_attenuation(0), 0);
    // Raising the attenuation (quieter) glides at 128 steps per 156.4 ms
    // (lowering it is twice as fast): 10 ms -> about 8 steps.
    run(*pcm, 441);
    CHECK(pcm->slot_attenuation(0) >= 7 && pcm->slot_attenuation(0) <= 9);
    run(*pcm, 4410);
    CHECK_EQ(pcm->slot_attenuation(0), 0x40); // arrived, and stays there
    reg(*pcm, 0, 5, (0x20 << 1) | 1);         // bit 0 set: jump at once
    CHECK_EQ(pcm->slot_attenuation(0), 0x20);
}

TEST_CASE(multipcm_vibrato_and_tremolo)
{
    // LFO speed 7 (7.066 Hz), vibrato depth 7 (+-79 cents) on a ramp
    // sample: the playback position drifts from the unmodulated voice's.
    std::vector<uint8_t> ramp(64);
    for (std::size_t i = 0; i < ramp.size(); ++i) {
        ramp[i] = static_cast<uint8_t>(i * 2);
    }
    auto steady = constant_tone({});
    auto wobbly = constant_tone({}, (7 << 3) | 7);
    std::vector<std::vector<int16_t>> outputs;
    for (MultiPCM* pcm : {steady.get(), wobbly.get()}) {
        pcm->load_sample_rom(ramp, 0x1000);
        reg(*pcm, 0, 1, 0);
        reg(*pcm, 0, 3, 0x10);
        reg(*pcm, 0, 4, 0x80);
        outputs.push_back(run(*pcm, 2000));
    }
    CHECK_EQ(steady->slot_step(0), wobbly->slot_step(0)); // the base step is the same
    CHECK_EQ(outputs[0][20], outputs[1][20]);              // in step at first...
    CHECK(outputs[0] != outputs[1]);                       // ...then not

    // Tremolo depth 7 (up to -24 dB) at LFO speed 7: the level varies.
    auto tremolo = constant_tone({}, 7 << 3, 7);
    reg(*tremolo, 0, 1, 0);
    reg(*tremolo, 0, 3, 0x10);
    reg(*tremolo, 0, 4, 0x80);
    auto out = run(*tremolo, 44643 / 7); // one LFO period
    int lowest = 32767, highest = 0;
    for (std::size_t i = 10; i < out.size() / 2; ++i) {
        lowest = std::min<int>(lowest, out[i * 2]);
        highest = std::max<int>(highest, out[i * 2]);
    }
    CHECK(highest >= full_level(0x4000) - 200);
    CHECK(lowest < highest / 8); // close to -24 dB at the bottom
}
