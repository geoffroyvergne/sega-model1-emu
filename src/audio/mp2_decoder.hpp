#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace model1 {

// MPEG-1 Audio Layer II decoder (ISO/IEC 11172-3), for the Digital Sound
// Board's music. Emulated hardware support: no host dependencies.
//
// The board's decoder plays a bit stream straight out of ROM, from a start
// to an end position given in bytes; frames need not be byte-aligned (Star
// Wars Arcade's start 3 bits into a byte). As MAME's decoder, decode()
// therefore works on bit positions:
//   - it searches for the next sync word (twelve 1 bits, then ID = 1 and
//     layer = II: "110") at any bit position from `pos`;
//   - it reads only the bits the frame's header, side information and
//     samples need (padding and ancillary data are skipped by the next
//     search), and leaves `pos` just after them;
//   - it fails, leaving `pos` alone, if no frame starts or a frame would
//     read a bit at or beyond `limit`. That is how the board notices the
//     end of a piece of music.
// Supported: MPEG-1 (32, 44.1, 48 kHz), mono, stereo, dual channel and
// joint (intensity) stereo, CRC (skipped, not checked), every bit rate
// including free format. Each frame gives 1152 samples per channel.
//
// Decoding follows the standard: bit allocation (tables B.2a-d), scale
// factor selection information, scale factors (table B.1), grouped and
// ungrouped samples requantized as in table B.4 (equivalently
// (2v - n + 1) / n for an n-level code v), then the polyphase synthesis
// filter bank with the analysis window D (table B.3).
class Mp2Decoder {
public:
    static constexpr std::size_t k_samples_per_frame = 1152;
    static constexpr std::size_t k_subbands = 32;

    struct FrameInfo {
        int sample_rate = 0;   // Hz
        int channels = 0;      // 1 or 2 in the stream (output is always stereo)
        int bitrate_kbps = 0;  // 0 = free format
    };

    Mp2Decoder();

    // Clears the synthesis filter history (as at power-on).
    void reset();

    // Decodes the next frame of `data` (bits MSB first) at or after bit
    // `pos`, reading nothing at or beyond bit `limit`. On success writes
    // k_samples_per_frame interleaved stereo samples to `out` (a mono stream
    // is copied to both sides), fills `info`, moves `pos` past the frame and
    // returns true.
    bool decode(std::span<const uint8_t> data, uint64_t& pos, uint64_t limit,
                std::span<int16_t, k_samples_per_frame * 2> out, FrameInfo& info);

private:
    // Polyphase synthesis: one block of 32 subband samples for `channel`
    // gives 32 output samples (scaled to 16 bits).
    void synthesize(int channel, const std::array<double, k_subbands>& subbands, int16_t* out, std::size_t stride);

    // Per channel: the last 16 blocks of 64 matrixed values (the standard's
    // V vector, 1024 values), newest first starting at m_v_offset.
    std::array<std::array<double, 1024 * 2>, 2> m_v{};
    std::array<std::size_t, 2> m_v_offset{};
};

} // namespace model1
