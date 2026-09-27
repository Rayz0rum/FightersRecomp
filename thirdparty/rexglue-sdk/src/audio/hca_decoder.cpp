/**
 * @file        audio/hca_decoder.cpp
 * @brief       Decoding of CRI HCA audio with FFmpeg's HCA decoder
 */

#include <rex/audio/hca_decoder.h>

#include <cstring>

#include <rex/logging.h>

extern "C" {
#if REX_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "libavcodec/avcodec.h"
#if REX_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

namespace rex::audio {

namespace {

uint16_t ReadBE16(const uint8_t* p) {
  return uint16_t((p[0] << 8) | p[1]);
}
uint32_t ReadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Header chunk name (the high bits may be set as a mask).
uint32_t ChunkName(const uint8_t* p) {
  return ReadBE32(p) & 0x7F7F7F7F;
}

constexpr uint32_t Tag(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) |
         (uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d));
}

}  // namespace

bool DecodeHca(const uint8_t* data, size_t size, HcaAudio& out) {
  out = HcaAudio();
  if (size < 8 || ChunkName(data) != Tag('H', 'C', 'A', 0)) {
    return false;
  }
  uint32_t header_size = ReadBE16(data + 6);
  if (header_size > size) {
    return false;
  }
  // The header's chunks (unmasked for the decoder).
  std::vector<uint8_t> header(data, data + header_size);
  uint32_t block_count = 0, block_size = 0;
  uint32_t loop_start_block = 0, loop_end_block = 0;
  uint16_t encoder_delay = 0;
  for (size_t p = 8; p + 4 <= header_size;) {
    uint32_t name = ChunkName(&header[p]);
    header[p] &= 0x7F;
    header[p + 1] &= 0x7F;
    header[p + 2] &= 0x7F;
    header[p + 3] &= 0x7F;
    if (name == Tag('f', 'm', 't', 0)) {
      out.channels = header[p + 4];
      out.sample_rate = (uint32_t(header[p + 5]) << 16) | (uint32_t(header[p + 6]) << 8) |
                        header[p + 7];
      block_count = ReadBE32(&header[p + 8]);
      encoder_delay = ReadBE16(&header[p + 12]);
      p += 16;
    } else if (name == Tag('c', 'o', 'm', 'p')) {
      block_size = ReadBE16(&header[p + 4]);
      p += 16;
    } else if (name == Tag('d', 'e', 'c', 0)) {
      block_size = ReadBE16(&header[p + 4]);
      p += 12;
    } else if (name == Tag('l', 'o', 'o', 'p')) {
      out.loops = true;
      loop_start_block = ReadBE32(&header[p + 4]);
      loop_end_block = ReadBE32(&header[p + 8]);
      p += 16;
    } else if (name == Tag('v', 'b', 'r', 0) || name == Tag('r', 'v', 'a', 0)) {
      p += 8;
    } else if (name == Tag('a', 't', 'h', 0) || name == Tag('c', 'i', 'p', 'h')) {
      if (name == Tag('c', 'i', 'p', 'h') && ReadBE16(&header[p + 4]) != 0) {
        REXLOG_WARN("DecodeHca: encrypted HCA isn't supported");
        return false;
      }
      p += 6;
    } else if (name == Tag('c', 'o', 'm', 'm')) {
      p += 5 + header[p + 4];
    } else {
      break;
    }
  }
  if (!out.channels || !out.sample_rate || !block_size || !block_count ||
      header_size + size_t(block_size) * block_count > size) {
    return false;
  }

  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HCA);
  if (!codec) {
    REXLOG_ERROR("DecodeHca: the HCA decoder isn't available");
    return false;
  }
  AVCodecContext* context = avcodec_alloc_context3(codec);
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  bool ok = context && packet && frame;
  if (ok) {
    context->channels = int(out.channels);
    context->sample_rate = int(out.sample_rate);
    context->extradata =
        static_cast<uint8_t*>(av_mallocz(header_size + AV_INPUT_BUFFER_PADDING_SIZE));
    ok = context->extradata != nullptr;
    if (ok) {
      std::memcpy(context->extradata, header.data(), header_size);
      context->extradata_size = int(header_size);
      ok = avcodec_open2(context, codec, nullptr) >= 0;
    }
  }
  std::vector<uint8_t> block(block_size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
  if (ok) {
    out.samples.reserve(size_t(block_count) * 1024 * out.channels);
    for (uint32_t i = 0; i < block_count && ok; ++i) {
      std::memcpy(block.data(), data + header_size + size_t(i) * block_size, block_size);
      packet->data = block.data();
      packet->size = int(block_size);
      if (avcodec_send_packet(context, packet) < 0) {
        ok = false;
        break;
      }
      while (avcodec_receive_frame(context, frame) >= 0) {
        for (int s = 0; s < frame->nb_samples; ++s) {
          for (uint32_t c = 0; c < out.channels; ++c) {
            out.samples.push_back(reinterpret_cast<const float*>(frame->extended_data[c])[s]);
          }
        }
      }
    }
  }
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&context);
  if (!ok) {
    out.samples.clear();
    return false;
  }
  // Skip the encoder delay.
  size_t delay = std::min(size_t(encoder_delay) * out.channels, out.samples.size());
  out.samples.erase(out.samples.begin(), out.samples.begin() + delay);
  if (out.loops) {
    uint32_t frames = uint32_t(out.samples.size() / out.channels);
    out.loop_start = std::min(loop_start_block * 1024, frames);
    out.loop_end = std::min((loop_end_block + 1) * 1024, frames);
    if (out.loop_end <= out.loop_start) {
      out.loops = false;
    }
  }
  return true;
}

}  // namespace rex::audio
