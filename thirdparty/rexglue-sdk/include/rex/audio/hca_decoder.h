/**
 * @file        rex/audio/hca_decoder.h
 * @brief       Decoding of CRI HCA audio (for apps' own sounds)
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rex::audio {

struct HcaAudio {
  uint32_t sample_rate = 0;
  uint32_t channels = 0;
  // Interleaved samples.
  std::vector<float> samples;
  // Loop region in sample frames (if loops).
  bool loops = false;
  uint32_t loop_start = 0;
  uint32_t loop_end = 0;
};

// Decodes a whole (unencrypted) HCA stream. Returns false if it's not valid.
bool DecodeHca(const uint8_t* data, size_t size, HcaAudio& out);

}  // namespace rex::audio
