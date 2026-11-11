#include "clip_check.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace bx {
namespace {

constexpr size_t kHeadBytes = 4 * 1024 * 1024;  // Matroska keeps its header, tracks and first cluster up front

int64_t file_size_of(FILE* file) {
  if (_fseeki64(file, 0, SEEK_END) != 0) return -1;
  const int64_t size = _ftelli64(file);
  _fseeki64(file, 0, SEEK_SET);
  return size;
}

bool read_head(FILE* file, int64_t size, std::vector<uint8_t>& out) {
  const size_t wanted = static_cast<size_t>(std::min<int64_t>(size, static_cast<int64_t>(kHeadBytes)));
  out.resize(wanted);
  if (wanted == 0) return true;
  return std::fread(out.data(), 1, wanted, file) == wanted;
}

// ----------------------------------------------------------------- Matroska

// Element ids and sizes are variable-length integers; the size strips its marker bit and an all-ones
// value means "unknown", which is what a streaming muxer writes for the segment.
bool read_id(const std::vector<uint8_t>& data, size_t& at, uint32_t& id) {
  if (at >= data.size()) return false;
  const uint8_t first = data[at];
  int length = 0;
  for (int bit = 7; bit >= 0; --bit) {
    if (first & (1 << bit)) {
      length = 8 - bit;
      break;
    }
  }
  if (length == 0 || length > 4 || at + static_cast<size_t>(length) > data.size()) return false;
  id = 0;
  for (int index = 0; index < length; ++index) id = (id << 8) | data[at + index];
  at += static_cast<size_t>(length);
  return true;
}

bool read_size(const std::vector<uint8_t>& data, size_t& at, uint64_t& size, bool& unknown) {
  if (at >= data.size()) return false;
  const uint8_t first = data[at];
  int length = 0;
  for (int bit = 7; bit >= 0; --bit) {
    if (first & (1 << bit)) {
      length = 8 - bit;
      break;
    }
  }
  if (length == 0 || length > 8 || at + static_cast<size_t>(length) > data.size()) return false;
  uint64_t value = first & (0xFFu >> length);
  for (int index = 1; index < length; ++index) value = (value << 8) | data[at + index];
  const uint64_t all_ones = (length >= 8) ? ~0ull : ((1ull << (7 * length)) - 1);
  unknown = value == all_ones;
  size = value;
  at += static_cast<size_t>(length);
  return true;
}

bool matroska_track_is_video(const std::vector<uint8_t>& data, size_t begin, size_t end) {
  size_t at = begin;
  while (at < end) {
    uint32_t id = 0;
    uint64_t size = 0;
    bool unknown = false;
    if (!read_id(data, at, id) || !read_size(data, at, size, unknown)) return false;
    const size_t payload_end = unknown ? end : std::min(end, at + static_cast<size_t>(size));
    if (id == 0xAE) {  // TrackEntry
      size_t inner = at;
      while (inner < payload_end) {
        uint32_t inner_id = 0;
        uint64_t inner_size = 0;
        bool inner_unknown = false;
        if (!read_id(data, inner, inner_id) || !read_size(data, inner, inner_size, inner_unknown)) break;
        const size_t value_end = inner_unknown ? payload_end : std::min(payload_end, inner + static_cast<size_t>(inner_size));
        if (inner_id == 0x83 && inner < payload_end) {  // TrackType: 1 is video
          if (data[inner] == 1) return true;
        }
        if (value_end <= inner) break;
        inner = value_end;
      }
    }
    if (payload_end <= at) break;
    at = payload_end;
  }
  return false;
}

bool matroska_cluster_has_keyframe(const std::vector<uint8_t>& data, size_t begin, size_t end) {
  size_t at = begin;
  while (at < end) {
    uint32_t id = 0;
    uint64_t size = 0;
    bool unknown = false;
    if (!read_id(data, at, id) || !read_size(data, at, size, unknown)) return false;
    const size_t payload_end = unknown ? end : std::min(end, at + static_cast<size_t>(size));
    // A SimpleBlock's payload is: track number (varint), timecode (2 bytes), flags (1 byte), frame.
    // Bit 7 of the flags marks a keyframe. BlockGroup holds a Block plus, when it is not a keyframe,
    // a ReferenceBlock.
    if (id == 0xA3 && at + 3 < payload_end) {
      size_t flags_at = at;
      const uint8_t track_first = data[flags_at];
      int track_length = 0;
      for (int bit = 7; bit >= 0; --bit) {
        if (track_first & (1 << bit)) {
          track_length = 8 - bit;
          break;
        }
      }
      if (track_length == 0) track_length = 1;
      flags_at += static_cast<size_t>(track_length) + 2;  // track number + timecode
      if (flags_at < payload_end && (data[flags_at] & 0x80) != 0) return true;
    }
    if (id == 0xA0) {  // BlockGroup: a keyframe is a Block with no ReferenceBlock
      bool has_block = false;
      bool has_reference = false;
      size_t inner = at;
      while (inner < payload_end) {
        uint32_t inner_id = 0;
        uint64_t inner_size = 0;
        bool inner_unknown = false;
        if (!read_id(data, inner, inner_id) || !read_size(data, inner, inner_size, inner_unknown)) break;
        const size_t value_end = inner_unknown ? payload_end : std::min(payload_end, inner + static_cast<size_t>(inner_size));
        if (inner_id == 0xA1) has_block = true;
        if (inner_id == 0xFB) has_reference = true;
        if (value_end <= inner) break;
        inner = value_end;
      }
      if (has_block && !has_reference) return true;
    }
    if (payload_end <= at) break;
    at = payload_end;
  }
  return false;
}

ClipCheck check_matroska(const std::vector<uint8_t>& head) {
  ClipCheck result;
  size_t at = 0;
  uint32_t id = 0;
  uint64_t size = 0;
  bool unknown = false;
  if (!read_id(head, at, id) || id != 0x1A45DFA3) {
    result.reason = "not a Matroska file";
    return result;
  }
  if (!read_size(head, at, size, unknown)) {
    result.reason = "truncated Matroska header";
    return result;
  }
  at = unknown ? head.size() : std::min(head.size(), at + static_cast<size_t>(size));
  if (!read_id(head, at, id) || id != 0x18538067) {
    result.reason = "no Matroska segment";
    return result;
  }
  if (!read_size(head, at, size, unknown)) {
    result.reason = "truncated Matroska segment header";
    return result;
  }
  const size_t segment_end = unknown ? head.size() : std::min(head.size(), at + static_cast<size_t>(size));
  while (at < segment_end) {
    uint32_t child_id = 0;
    uint64_t child_size = 0;
    bool child_unknown = false;
    if (!read_id(head, at, child_id) || !read_size(head, at, child_size, child_unknown)) break;
    const size_t payload_end = child_unknown ? segment_end : std::min(segment_end, at + static_cast<size_t>(child_size));
    if (child_id == 0x1654AE6B && !result.has_video_track) {  // Tracks
      result.has_video_track = matroska_track_is_video(head, at, payload_end);
    } else if (child_id == 0x1F43B675 && !result.has_keyframe) {  // Cluster
      result.has_keyframe = matroska_cluster_has_keyframe(head, at, payload_end);
    }
    if (result.has_video_track && result.has_keyframe) break;
    if (payload_end <= at) break;
    at = payload_end;
  }
  if (!result.has_video_track) result.reason = "no video track in the Matroska file";
  else if (!result.has_keyframe) result.reason = "no keyframe in the Matroska file";
  else result.ok = true;
  return result;
}

// ----------------------------------------------------------------------- MP4

// MP4 is a tree of length-prefixed boxes, so the file is walked with seeks rather than loaded: a
// recording can be gigabytes and only its box headers matter here.
struct Box {
  uint32_t type = 0;
  int64_t begin = 0;  // payload
  int64_t end = 0;
};

bool read_box(FILE* file, int64_t file_size, int64_t at, Box& box) {
  uint8_t header[16] = {0};
  if (at < 0 || at + 8 > file_size) return false;
  if (_fseeki64(file, at, SEEK_SET) != 0) return false;
  if (std::fread(header, 1, 8, file) != 8) return false;
  int64_t size = (static_cast<int64_t>(header[0]) << 24) | (static_cast<int64_t>(header[1]) << 16) |
                 (static_cast<int64_t>(header[2]) << 8) | static_cast<int64_t>(header[3]);
  box.type = (static_cast<uint32_t>(header[4]) << 24) | (static_cast<uint32_t>(header[5]) << 16) |
             (static_cast<uint32_t>(header[6]) << 8) | static_cast<uint32_t>(header[7]);
  box.begin = at + 8;
  if (size == 1) {  // 64-bit size
    uint8_t extended[8] = {0};
    if (std::fread(extended, 1, 8, file) != 8) return false;
    size = 0;
    for (uint8_t byte : extended) size = (size << 8) | byte;
    box.begin = at + 16;
  } else if (size == 0) {
    size = file_size - at;  // to the end of the file
  }
  if (size < box.begin - at) return false;
  box.end = at + size;
  if (box.end > file_size) box.end = file_size;
  return true;
}

bool mp4_find_video_handler(FILE* file, int64_t file_size, int64_t begin, int64_t end, int depth) {
  if (depth > 6) return false;
  int64_t at = begin;
  while (at + 8 <= end) {
    Box box;
    if (!read_box(file, file_size, at, box)) return false;
    if (box.type == 0x68646C72u) {  // hdlr: version+flags, pre_defined, then the handler type
      uint8_t handler[12] = {0};
      if (_fseeki64(file, box.begin, SEEK_SET) == 0 && std::fread(handler, 1, 12, file) == 12) {
        if (handler[8] == 'v' && handler[9] == 'i' && handler[10] == 'd' && handler[11] == 'e') return true;
      }
    } else if (box.type == 0x6D6F6F76u || box.type == 0x7472616Bu || box.type == 0x6D646961u ||
               box.type == 0x6D696E66u || box.type == 0x7374626Cu) {  // moov, trak, mdia, minf, stbl
      if (mp4_find_video_handler(file, file_size, box.begin, box.end, depth + 1)) return true;
    }
    if (box.end <= at) return false;
    at = box.end;
  }
  return false;
}

ClipCheck check_mp4(FILE* file, int64_t file_size) {
  ClipCheck result;
  bool has_ftyp = false;
  bool has_media = false;
  int64_t at = 0;
  while (at + 8 <= file_size) {
    Box box;
    if (!read_box(file, file_size, at, box)) break;
    if (box.type == 0x66747970u) has_ftyp = true;                                             // ftyp
    if (box.type == 0x6D646174u && box.end > box.begin) has_media = true;                     // mdat
    if (box.type == 0x6D6F6F76u) {                                                            // moov
      result.has_video_track = mp4_find_video_handler(file, file_size, box.begin, box.end, 0);
    }
    if (box.end <= at) break;
    at = box.end;
  }
  if (!has_ftyp) result.reason = "not an MP4 file";
  else if (!result.has_video_track) result.reason = "no video track in the MP4 file";
  else if (!has_media) result.reason = "no media data in the MP4 file";
  else {
    result.has_keyframe = has_media;  // the media data itself is the payload this container holds
    result.ok = true;
  }
  return result;
}

}  // namespace

ClipCheck check_clip_file(const std::string& path) {
  ClipCheck result;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) {
    result.reason = "the file could not be opened";
    return result;
  }
  const int64_t size = file_size_of(file);
  if (size <= 0) {
    result.reason = "the file is empty";
    std::fclose(file);
    return result;
  }
  result.bytes = size;

  uint8_t magic[4] = {0};
  const size_t read = std::fread(magic, 1, sizeof(magic), file);
  _fseeki64(file, 0, SEEK_SET);
  if (read == sizeof(magic) && magic[0] == 0x1A && magic[1] == 0x45 && magic[2] == 0xDF && magic[3] == 0xA3) {
    std::vector<uint8_t> head;
    if (!read_head(file, size, head)) {
      result.reason = "the Matroska header could not be read";
      std::fclose(file);
      return result;
    }
    const ClipCheck matroska = check_matroska(head);
    result.ok = matroska.ok;
    result.reason = matroska.reason;
    result.has_video_track = matroska.has_video_track;
    result.has_keyframe = matroska.has_keyframe;
    std::fclose(file);
    return result;
  }

  const ClipCheck mp4 = check_mp4(file, size);
  result.ok = mp4.ok;
  result.reason = mp4.reason;
  result.has_video_track = mp4.has_video_track;
  result.has_keyframe = mp4.has_keyframe;
  std::fclose(file);
  return result;
}

}  // namespace bx
