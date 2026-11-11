// A hash of a video packet's picture data that does not depend on how a container frames it. The replay ring holds the encoder's packets (H.264 and HEVC with
// start codes, AV1 as OBUs); a Timeline chunk holds the same packets after FFmpeg's mp4 muxer has re-framed them (length-prefixed NAL units, parameter sets moved
// to the codec's extradata, temporal delimiters dropped). Only the slice (VCL) data survives that unchanged, so the hash covers exactly that: the bytes of every
// VCL NAL unit (H.264 types 1-5, HEVC types 0-31) or of every frame-header / tile-group / frame OBU's payload (AV1 types 3, 4, 6), without the framing around
// them. Pure, so it is tested without a codec. Used to tell that a chunk's packet and a ring record are the same packet (docs/review-timeline-mux-2026-11-02.md).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bx {

enum class HashCodec { other, h264, hevc, av1 };

inline HashCodec hash_codec_from_name(const char* name) {
  if (!name) return HashCodec::other;
  if (!std::strcmp(name, "h264") || !std::strcmp(name, "avc") || !std::strcmp(name, "avc1")) return HashCodec::h264;
  if (!std::strcmp(name, "hevc") || !std::strcmp(name, "h265") || !std::strcmp(name, "hev1")) return HashCodec::hevc;
  if (!std::strcmp(name, "av1") || !std::strcmp(name, "av01")) return HashCodec::av1;
  return HashCodec::other;
}

struct VclDigest {
  bool ok = false;      // the packet parsed and held picture data
  uint32_t bytes = 0;   // the picture data hashed
  uint64_t hash = 0;    // FNV-1a 64 over it
};

// FNV-1a's multiply-and-xor, taken eight bytes at a time (a keyframe is megabytes and is hashed on the encoder's packet thread; byte at a time cost
// 2 ms per 2 MB), fed in pieces. Only ever compared with itself - the same pieces on both sides - so it need not be FNV's published value.
class Fnv64 {
public:
  void add(const uint8_t* data, size_t size) {
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
      uint64_t word;
      std::memcpy(&word, data + i, 8);
      hash_ = (hash_ ^ word) * 1099511628211ull;
      hash_ ^= hash_ >> 29;
    }
    for (; i < size; ++i) {
      hash_ ^= data[i];
      hash_ *= 1099511628211ull;
    }
  }
  uint64_t value() const { return hash_; }

private:
  uint64_t hash_ = 14695981039346656037ull;
};

// The whole payload, for packets no container re-frames (an AAC frame).
inline uint64_t payload_hash(const uint8_t* data, size_t size) {
  Fnv64 fnv;
  fnv.add(data, size);
  return fnv.value();
}

namespace packet_hash_detail {

inline bool is_vcl_nal(HashCodec codec, uint8_t first) {
  if (codec == HashCodec::h264) {
    const int type = first & 0x1f;
    return type >= 1 && type <= 5;
  }
  const int type = (first >> 1) & 0x3f;  // HEVC
  return type <= 31;
}

// Position of the next 00 00 01 at or after `from`, or `size`.
inline size_t next_start_code(const uint8_t* data, size_t size, size_t from) {
  for (size_t i = from; i + 2 < size; ++i) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return i;
  }
  return size;
}

inline void add_nal(Fnv64& fnv, VclDigest& digest, HashCodec codec, const uint8_t* nal, size_t size) {
  if (size == 0 || !is_vcl_nal(codec, nal[0])) return;
  fnv.add(nal, size);
  digest.bytes += static_cast<uint32_t>(size);
}

// leb128: up to 8 bytes. False when it runs off the end.
inline bool read_leb128(const uint8_t* data, size_t size, size_t& at, uint64_t& value) {
  value = 0;
  for (int i = 0; i < 8; ++i) {
    if (at >= size) return false;
    const uint8_t byte = data[at++];
    value |= static_cast<uint64_t>(byte & 0x7f) << (i * 7);
    if (!(byte & 0x80)) return true;
  }
  return false;
}

}  // namespace packet_hash_detail

// `length_size` 0: start codes (what the encoder hands the ring); 1-4: big-endian length prefixes of that many bytes (an mp4 sample, whose length size the
// codec's extradata names). AV1 is an OBU stream either way and ignores it.
inline VclDigest vcl_digest(HashCodec codec, const uint8_t* data, size_t size, int length_size) {
  using namespace packet_hash_detail;
  VclDigest digest;
  if (!data || size == 0 || codec == HashCodec::other) return digest;
  Fnv64 fnv;
  if (codec == HashCodec::av1) {
    size_t at = 0;
    while (at < size) {
      const uint8_t header = data[at++];
      const int type = (header >> 3) & 0x0f;
      const bool extension = (header >> 2) & 1;
      const bool has_size = (header >> 1) & 1;
      if (extension) {
        if (at >= size) return VclDigest{};
        ++at;
      }
      uint64_t payload = size - at;
      if (has_size && !read_leb128(data, size, at, payload)) return VclDigest{};
      if (payload > size - at) return VclDigest{};
      if (type == 3 || type == 4 || type == 6) {
        fnv.add(data + at, static_cast<size_t>(payload));
        digest.bytes += static_cast<uint32_t>(payload);
      }
      at += static_cast<size_t>(payload);
    }
  } else if (length_size == 0) {
    size_t start = next_start_code(data, size, 0);
    while (start < size) {
      const size_t begin = start + 3;
      size_t end = next_start_code(data, size, begin);
      size_t last = end;
      while (last > begin && data[last - 1] == 0) --last;  // the zero byte of a four-byte start code belongs to the next one
      if (last > begin) add_nal(fnv, digest, codec, data + begin, last - begin);
      start = end;
    }
  } else if (length_size >= 1 && length_size <= 4) {
    size_t at = 0;
    while (at < size) {
      if (size - at < static_cast<size_t>(length_size)) return VclDigest{};
      uint32_t length = 0;
      for (int i = 0; i < length_size; ++i) length = (length << 8) | data[at + static_cast<size_t>(i)];
      at += static_cast<size_t>(length_size);
      if (length > size - at) return VclDigest{};
      add_nal(fnv, digest, codec, data + at, length);
      at += length;
    }
  } else {
    return VclDigest{};
  }
  digest.ok = digest.bytes > 0;
  digest.hash = fnv.value();
  return digest;
}

}  // namespace bx
