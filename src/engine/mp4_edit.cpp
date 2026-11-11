#include "mp4_edit.h"

#include <cmath>
#include <cstddef>
#include <cstdio>

namespace bx {
namespace {

constexpr uint32_t tag(const char* name) {
  return (uint32_t(static_cast<uint8_t>(name[0])) << 24) | (uint32_t(static_cast<uint8_t>(name[1])) << 16) |
         (uint32_t(static_cast<uint8_t>(name[2])) << 8) | uint32_t(static_cast<uint8_t>(name[3]));
}

constexpr uint32_t kMoov = tag("moov");
constexpr uint32_t kTrak = tag("trak");
constexpr uint32_t kMdia = tag("mdia");
constexpr uint32_t kEdts = tag("edts");
constexpr uint32_t kMvhd = tag("mvhd");
constexpr uint32_t kTkhd = tag("tkhd");
constexpr uint32_t kMdhd = tag("mdhd");
constexpr uint32_t kHdlr = tag("hdlr");
constexpr uint32_t kElst = tag("elst");
constexpr uint32_t kFree = tag("free");
constexpr uint32_t kSkip = tag("skip");
constexpr uint32_t kVideo = tag("vide");
constexpr uint32_t kSound = tag("soun");

uint32_t read32(const uint8_t* at) {
  return (uint32_t(at[0]) << 24) | (uint32_t(at[1]) << 16) | (uint32_t(at[2]) << 8) | uint32_t(at[3]);
}

uint64_t read64(const uint8_t* at) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value = (value << 8) | at[i];
  return value;
}

void write32(uint8_t* at, uint32_t value) {
  at[0] = static_cast<uint8_t>(value >> 24);
  at[1] = static_cast<uint8_t>(value >> 16);
  at[2] = static_cast<uint8_t>(value >> 8);
  at[3] = static_cast<uint8_t>(value);
}

void write64(uint8_t* at, uint64_t value) {
  for (int i = 7; i >= 0; --i) {
    at[i] = static_cast<uint8_t>(value);
    value >>= 8;
  }
}

// Only the boxes this module walks are children. Everything else (sample tables included) is kept as
// bytes and written back as it arrived, so no offset inside the file is ever rebuilt.
bool is_container(uint32_t type) { return type == kMoov || type == kTrak || type == kMdia || type == kEdts; }

struct Box {
  uint32_t type = 0;
  bool container = false;
  std::vector<uint8_t> payload;
  std::vector<Box> children;
};

bool parse(const uint8_t* data, size_t from, size_t to, Box& out, size_t& used) {
  if (to < from || to - from < 8) return false;
  uint64_t size = read32(data + from);
  out.type = read32(data + from + 4);
  size_t header = 8;
  if (size == 1) {  // 64-bit size
    if (to - from < 16) return false;
    size = read64(data + from + 8);
    header = 16;
  } else if (size == 0) {  // to the end of the parent
    size = to - from;
  }
  if (size < header || size > to - from) return false;
  used = static_cast<size_t>(size);
  out.container = is_container(out.type);
  if (!out.container) {
    out.payload.assign(data + from + header, data + from + used);
    return true;
  }
  const size_t end = from + used;
  size_t at = from + header;
  while (at < end) {
    Box child;
    size_t child_size = 0;
    if (!parse(data, at, end, child, child_size)) return false;
    out.children.push_back(std::move(child));
    at += child_size;
  }
  return true;
}

bool write(const Box& box, std::vector<uint8_t>& out) {
  const size_t start = out.size();
  out.resize(start + 8);
  if (box.container) {
    for (const Box& child : box.children) {
      if (!write(child, out)) return false;
    }
  } else {
    out.insert(out.end(), box.payload.begin(), box.payload.end());
  }
  const size_t size = out.size() - start;
  if (size > 0xFFFFFFFFu) return false;
  write32(out.data() + start, static_cast<uint32_t>(size));
  write32(out.data() + start + 4, box.type);
  return true;
}

Box* child(Box& parent, uint32_t type) {
  for (Box& box : parent.children) {
    if (box.type == type) return &box;
  }
  return nullptr;
}

size_t child_index(const Box& parent, const Box* box) {
  for (size_t i = 0; i < parent.children.size(); ++i) {
    if (&parent.children[i] == box) return i;
  }
  return parent.children.size();
}

// mvhd and mdhd share one layout: a timescale and a length in that timescale.
struct Clock {
  int version = 0;
  uint32_t timescale = 0;
  int64_t duration = 0;
};

bool read_clock(const Box& box, Clock& out) {
  if (box.payload.size() < 1) return false;
  out.version = box.payload[0];
  const uint8_t* p = box.payload.data();
  if (out.version == 1) {
    if (box.payload.size() < 32) return false;
    out.timescale = read32(p + 20);
    out.duration = static_cast<int64_t>(read64(p + 24));
  } else {
    if (box.payload.size() < 20) return false;
    out.timescale = read32(p + 12);
    out.duration = read32(p + 16);
  }
  return out.timescale != 0;
}

bool read_track_duration(const Box& tkhd, int64_t& duration) {
  if (tkhd.payload.size() < 1) return false;
  const uint8_t* p = tkhd.payload.data();
  if (p[0] == 1) {
    if (tkhd.payload.size() < 36) return false;
    duration = static_cast<int64_t>(read64(p + 28));
  } else {
    if (tkhd.payload.size() < 24) return false;
    duration = read32(p + 20);
  }
  return true;
}

void write_track_duration(Box& tkhd, int64_t duration) {
  uint8_t* p = tkhd.payload.data();
  if (p[0] == 1) {
    write64(p + 28, static_cast<uint64_t>(duration));
  } else {
    write32(p + 20, static_cast<uint32_t>(duration));
  }
}

void write_movie_duration(Box& mvhd, int64_t duration) {
  uint8_t* p = mvhd.payload.data();
  if (p[0] == 1) {
    write64(p + 24, static_cast<uint64_t>(duration));
  } else {
    write32(p + 16, static_cast<uint32_t>(duration));
  }
}

// One edit: present `duration` seconds of the track from `media_time` on, at normal speed.
Box make_elst(int64_t duration, int64_t media_time) {
  Box elst;
  elst.type = kElst;
  elst.payload.assign(20, 0);
  write32(elst.payload.data() + 4, 1);
  write32(elst.payload.data() + 8, static_cast<uint32_t>(duration > 0 ? duration : 0));
  write32(elst.payload.data() + 12, static_cast<uint32_t>(static_cast<int32_t>(media_time)));
  elst.payload[17] = 1;  // media rate 1.0, fraction 0
  return elst;
}

// How wide one edit entry is in a list of this version.
size_t edit_width(int version) { return version == 1 ? 20 : 12; }

int64_t edit_duration(const uint8_t* entry, int version) {
  return version == 1 ? static_cast<int64_t>(read64(entry)) : read32(entry);
}

int64_t edit_media_time(const uint8_t* entry, int version) {
  return version == 1 ? static_cast<int64_t>(read64(entry + 8)) : static_cast<int32_t>(read32(entry + 4));
}

void write_edit(uint8_t* entry, int version, int64_t duration, int64_t media_time) {
  if (version == 1) {
    write64(entry, static_cast<uint64_t>(duration));
    write64(entry + 8, static_cast<uint64_t>(media_time));
  } else {
    write32(entry, static_cast<uint32_t>(duration));
    write32(entry + 4, static_cast<uint32_t>(static_cast<int32_t>(media_time)));
  }
}

// The head folds into the edit list the track already has. A sound track primed by the encoder moves
// by 1024 samples, and replacing that entry would move the sound against the picture by as much.
void compose_elst(Box& elst, int64_t head_ticks, int64_t move_ticks, int64_t kept) {
  const int version = elst.payload.empty() ? 0 : elst.payload[0];
  const size_t width = edit_width(version);
  const uint32_t count = elst.payload.size() >= 8 ? read32(elst.payload.data() + 4) : 0;
  if (count == 0 || elst.payload.size() < 8 + size_t(count) * width) {
    elst = make_elst(kept, head_ticks);  // nothing usable to keep: a list of our own
    return;
  }
  uint8_t* base = elst.payload.data() + 8;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t* entry = base + i * width;
    const int64_t media_time = edit_media_time(entry, version);
    if (media_time == -1) continue;  // an empty edit stays empty and keeps its place
    const int64_t span = edit_duration(entry, version) - move_ticks;
    write_edit(entry, version, span > 0 ? span : 0, media_time + head_ticks);
    return;
  }
  // Every edit is empty, so nothing presents the media: the head arrives as a new first entry.
  std::vector<uint8_t> entry(width, 0);
  write_edit(entry.data(), version, kept, head_ticks);
  entry[version == 1 ? 17 : 9] = 1;  // media rate 1.0, fraction 0
  elst.payload.insert(elst.payload.begin() + 8, entry.begin(), entry.end());
  write32(elst.payload.data() + 4, count + 1);
}

// A box header and where its payload ends, read from the file with seeks: a clip can be gigabytes
// and only the headers matter here.
struct FileBox {
  uint32_t type = 0;
  int64_t begin = 0;
  int64_t end = 0;
};

bool read_file_box(FILE* file, int64_t file_size, int64_t at, FileBox& box) {
  if (at < 0 || at + 8 > file_size) return false;
  if (_fseeki64(file, at, SEEK_SET) != 0) return false;
  uint8_t header[16] = {0};
  if (std::fread(header, 1, 8, file) != 8) return false;
  int64_t size = int64_t(read32(header));
  box.type = read32(header + 4);
  box.begin = at + 8;
  if (size == 1) {  // 64-bit size
    if (std::fread(header, 1, 8, file) != 8) return false;
    size = static_cast<int64_t>(read64(header));
    box.begin = at + 16;
  } else if (size == 0) {
    size = file_size - at;
  }
  if (size < box.begin - at) return false;
  box.end = at + size;
  if (box.end > file_size) box.end = file_size;
  return true;
}

}  // namespace

MoovBox mp4_find_moov(const std::string& path) {
  MoovBox result;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) return result;
  if (_fseeki64(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return result;
  }
  const int64_t file_size = _ftelli64(file);
  int64_t at = 0;
  while (at + 8 <= file_size) {
    FileBox box;
    if (!read_file_box(file, file_size, at, box)) break;
    if (!result.found && box.type == kMoov) {
      result.found = true;
      result.begin = at;
      result.size = box.end - at;
      result.last = true;  // until something that is not padding follows it
    } else if (result.found && box.type != kFree && box.type != kSkip) {
      result.last = false;
    }
    if (box.end <= at) break;
    at = box.end;
  }
  std::fclose(file);
  return result;
}

std::vector<uint8_t> mp4_trim_moov(const std::vector<uint8_t>& moov, double head_seconds) {
  if (!(head_seconds > 0.001)) return moov;  // nothing to hide: the header is already right
  if (moov.size() < 8) return {};

  Box root;
  size_t used = 0;
  if (!parse(moov.data(), 0, moov.size(), root, used) || used != moov.size()) return {};
  if (root.type != kMoov || !root.container) return {};

  Box* mvhd = child(root, kMvhd);
  Clock movie;
  if (!mvhd || !read_clock(*mvhd, movie)) return {};
  const int64_t move_ticks = std::llround(head_seconds * movie.timescale);
  if (move_ticks <= 0) return moov;

  bool trimmed = false;
  for (Box& track : root.children) {
    if (track.type != kTrak || !track.container) continue;
    Box* tkhd = child(track, kTkhd);
    Box* mdia = child(track, kMdia);
    if (!tkhd || !mdia || !mdia->container) continue;
    Box* hdlr = child(*mdia, kHdlr);
    Box* mdhd = child(*mdia, kMdhd);
    if (!hdlr || !mdhd || hdlr->payload.size() < 12) continue;
    const uint32_t handler = read32(hdlr->payload.data() + 8);
    if (handler != kVideo && handler != kSound) continue;
    Clock media;
    int64_t track_duration = 0;
    if (!read_clock(*mdhd, media) || !read_track_duration(*tkhd, track_duration)) continue;

    // The media keeps its head -- that is what the copy paid for -- and the edit list starts the
    // picture past it, so the cut is sample-exact to the nearest tick.
    const int64_t head_ticks = std::llround(head_seconds * media.timescale);
    const int64_t kept = track_duration - move_ticks > 0 ? track_duration - move_ticks : 0;
    write_track_duration(*tkhd, kept);

    const size_t tkhd_at = child_index(track, tkhd);
    Box* edts = child(track, kEdts);
    if (!edts) {
      track.children.insert(track.children.begin() + static_cast<std::ptrdiff_t>(tkhd_at + 1), Box{});
      edts = &track.children[tkhd_at + 1];
      edts->type = kEdts;
      edts->container = true;
    }
    if (!edts->container) {
      edts->container = true;
      edts->payload.clear();
      edts->children.clear();
    }
    Box* elst = child(*edts, kElst);
    if (elst) {
      compose_elst(*elst, head_ticks, move_ticks, kept);
    } else {
      edts->children.push_back(make_elst(kept, head_ticks));
    }
    trimmed = true;
  }
  if (!trimmed) return {};  // no picture and no sound: not a header this module can trim

  write_movie_duration(*mvhd, movie.duration - move_ticks > 0 ? movie.duration - move_ticks : 0);

  std::vector<uint8_t> out;
  if (!write(root, out)) return {};
  return out;
}

bool mp4_head_trim(const std::string& path, double head_seconds, std::string& error) {
  if (!(head_seconds > 0.001)) return true;  // the copy already starts where the cut is

  const MoovBox index = mp4_find_moov(path);
  if (!index.found) {
    error = "the file has no index to trim";
    return false;
  }
  if (!index.last) {
    // Media after the index means its sample offsets point past it: growing it would move them.
    error = "the index is not at the end of the file";
    return false;
  }

  FILE* file = std::fopen(path.c_str(), "r+b");
  if (!file) {
    error = "the file has no index to trim";
    return false;
  }
  std::vector<uint8_t> moov(static_cast<size_t>(index.size));
  if (_fseeki64(file, index.begin, SEEK_SET) != 0 || std::fread(moov.data(), 1, moov.size(), file) != moov.size()) {
    std::fclose(file);
    error = "the file has no index to trim";
    return false;
  }

  const std::vector<uint8_t> trimmed = mp4_trim_moov(moov, head_seconds);
  if (trimmed.empty()) {
    std::fclose(file);
    error = "the file has no index to trim";
    return false;
  }

  const int64_t end = index.begin + static_cast<int64_t>(trimmed.size());
  const bool written = _fseeki64(file, index.begin, SEEK_SET) == 0 &&
                       std::fwrite(trimmed.data(), 1, trimmed.size(), file) == trimmed.size() &&
                       std::fflush(file) == 0 && _chsize_s(_fileno(file), end) == 0;
  std::fclose(file);
  if (!written) {
    error = "the trimmed index is larger than the file can hold";
    return false;
  }
  return true;
}

}  // namespace bx
