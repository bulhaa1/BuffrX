// The mutation harness (N4): valid inputs, then bytes flipped, truncated, extended and spliced, fed to every parser the
// engine has that reads something it did not write itself. The toolchain has no sanitizers and no libFuzzer, so what is
// checked here is what can be checked without them: no crash, no hang (a watchdog ends the process after 2 s in one
// case), no unbounded allocation (every read is bounded by the input), and the properties each parser promises.
//
// Every case is a function of its seed alone, and the seed is written to buffrx-fuzz-seed.txt before the case runs, so a
// crash names the bytes that caused it. Run with no argument for a wall-clock budget in seconds (default 1200), or with
// `buffrx-fuzz --seed N` to replay one case.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../src/engine/config.h"
#include "../src/engine/link_math.h"
#include "../src/engine/media_math.h"
#include "../src/engine/mp4_edit.h"
#include "../src/engine/packet_hash.h"
#include "../src/engine/ring_spill.h"
#include "../src/engine/session_store.h"
#include "../src/shared/json.h"
#include "../src/shared/pipe.h"

namespace {

int g_failures = 0;

void fail(const char* target, uint64_t seed, const char* what) {
  ++g_failures;
  std::printf("FAIL %s seed %llu: %s\n", target, static_cast<unsigned long long>(seed), what);
}

// The watchdog: a parser that never returns cannot report from the inside, so the case in flight lives here and a thread
// ends the process when one runs past the 2 s budget. The seed file already holds that case.
std::atomic<uint64_t> g_case_start{0};
std::atomic<uint64_t> g_case_seed{0};
const char* volatile g_case_target = "none";

[[noreturn]] void case_timeout() {
  std::printf("FAIL %s seed %llu: a case ran past 2 s (watchdog)\n", g_case_target,
              static_cast<unsigned long long>(g_case_seed.load()));
  std::fflush(stdout);
  std::_Exit(3);
}

void watchdog() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const uint64_t start = g_case_start.load(std::memory_order_relaxed);
    if (start != 0 && GetTickCount64() - start > 2000) case_timeout();
  }
}

void note_case(const char* target, uint64_t seed) {
  g_case_target = target;
  g_case_seed.store(seed, std::memory_order_relaxed);
  g_case_start.store(GetTickCount64(), std::memory_order_relaxed);
  if (FILE* file = std::fopen("buffrx-fuzz-seed.txt", "wb")) {
    std::fprintf(file, "%s %llu\n", target, static_cast<unsigned long long>(seed));
    std::fclose(file);
  }
}

std::string mutated(std::mt19937_64& random, const std::string& input) {
  std::string out = input;
  const int rounds = 1 + static_cast<int>(random() % 4);
  for (int round = 0; round < rounds; ++round) {
    switch (random() % 6) {
      case 0:
        if (!out.empty()) out[random() % out.size()] ^= static_cast<char>(1u << (random() % 8));
        break;
      case 1:
        out.resize(random() % (out.size() + 1));
        break;
      case 2: {
        const size_t count = 1 + random() % 96;
        for (size_t i = 0; i < count; ++i) out.push_back(static_cast<char>(random()));
        break;
      }
      case 3:
        if (!out.empty()) {
          const size_t at = random() % out.size();
          const size_t length = 1 + random() % (out.size() - at);
          out.insert(random() % (out.size() + 1), out.substr(at, length));
        }
        break;
      case 4:
        if (out.size() > 1) {
          const size_t at = random() % out.size();
          out.erase(at, 1 + random() % (out.size() - at));
        }
        break;
      default:
        if (!out.empty()) out[random() % out.size()] = static_cast<char>(random());
        break;
    }
  }
  return out;
}

std::string random_text(std::mt19937_64& random, size_t bytes) {
  std::string out;
  out.reserve(bytes);
  for (size_t i = 0; i < bytes; ++i) out.push_back(static_cast<char>(random()));
  return out;
}

// ---------------------------------------------------------------- the parsers

const char* kCells[] = {"hello", "status.get", "buffer.save", "timeline.get", "ui.show"};

void case_json(std::mt19937_64& random, uint64_t seed) {
  static const std::string kConfig = bx::config_to_json(bx::Config{}).dump();
  static const std::vector<std::string> kCorpus = {
      "{}", "[]", "null", "true", "0", "-1.5e-300", "\"text\\u00e9\\n\"",
      "{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":-1.5e10,\"e\":[]}}",
      "{\"cmd\":\"ui.show\",\"args\":{\"view\":\"main\"},\"id\":7}",
      std::string("{\"cmd\":\"") + kCells[3] + "\",\"args\":{}}",
      "[[[[[[[[[[0]]]]]]]]]]",
      kConfig,
  };
  const std::string input = mutated(random, kCorpus[random() % kCorpus.size()]);
  bx::Json parsed;
  std::string error;
  const bool ok = bx::Json::parse(input, parsed, error);
  if (!ok) {
    // A rejected input is a correct answer; what it must never do is accept one and then fail to write it back.
    return;
  }
  const std::string written = parsed.dump();
  bx::Json again;
  std::string again_error;
  if (!bx::Json::parse(written, again, again_error)) fail("json", seed, "a parsed document dumped to text that does not parse");
}

void case_config(std::mt19937_64& random, uint64_t seed) {
  static const std::string kConfig = bx::config_to_json(bx::Config{}).dump();
  const std::string input = mutated(random, kConfig);
  bx::Json parsed;
  std::string error;
  if (!bx::Json::parse(input, parsed, error)) return;
  bx::Config config;
  bx::config_from_json(parsed, config);
  bx::migrate_config(config);
  bx::Json again = bx::config_to_json(config);
  bx::Json check;
  std::string check_error;
  if (!bx::Json::parse(again.dump(), check, check_error)) fail("config", seed, "the engine wrote a config it cannot read back");
}

void case_frame_length(std::mt19937_64& random, uint64_t seed) {
  unsigned char header[4];
  for (int i = 0; i < 4; ++i) header[i] = static_cast<unsigned char>(random());
  uint32_t length = 0;
  if (bx::frame_length(header, length) && length > bx::kMaxFrameBytes) fail("frame_length", seed, "a length past the bound was accepted");
  const uint32_t wanted = static_cast<uint32_t>(random() % (4 * 1024 * 1024));
  header[0] = static_cast<unsigned char>(wanted & 0xFF);
  header[1] = static_cast<unsigned char>((wanted >> 8) & 0xFF);
  header[2] = static_cast<unsigned char>((wanted >> 16) & 0xFF);
  header[3] = static_cast<unsigned char>((wanted >> 24) & 0xFF);
  uint32_t decoded = 0;
  if (!bx::frame_length(header, decoded) || decoded != wanted) fail("frame_length", seed, "a legal length did not read back");
}

void case_record(std::mt19937_64& random, uint64_t seed) {
  std::string bytes = random_text(random, 48 + random() % 208);
  bx::PacketFields packet;
  if (bx::decode_record_header(reinterpret_cast<const uint8_t*>(bytes.data()), packet)) {
    uint8_t encoded[bx::kSpillRecordHeaderBytes];
    bx::encode_record_header(packet, encoded);
    bx::PacketFields again;
    if (!bx::decode_record_header(encoded, again) || again.size != packet.size || again.pts != packet.pts ||
        again.dts_usec != packet.dts_usec || again.track != packet.track || again.video != packet.video) {
      fail("record", seed, "a decoded record header did not encode back");
    }
    // The magic is the one thing that says a record is a record: break it and the decoder must say no.
    encoded[40] ^= 0x01;
    bx::PacketFields broken;
    if (bx::decode_record_header(encoded, broken)) fail("record", seed, "a broken magic was accepted");
  }
}

void case_vcl(std::mt19937_64& random, uint64_t seed) {
  const std::string bytes = random_text(random, random() % 4096);
  const bx::HashCodec codecs[] = {bx::HashCodec::other, bx::HashCodec::h264, bx::HashCodec::hevc, bx::HashCodec::av1};
  const bx::HashCodec codec = codecs[random() % 4];
  const int length_size = static_cast<int>(random() % 5);
  const bx::VclDigest first = bx::vcl_digest(codec, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), length_size);
  const bx::VclDigest second = bx::vcl_digest(codec, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), length_size);
  if (first.ok != second.ok || first.hash != second.hash || first.bytes != second.bytes) {
    fail("vcl", seed, "the same bytes gave two different digests");
  }
  if (first.bytes > bytes.size()) fail("vcl", seed, "the digest counted more bytes than it was given");
  if (codec == bx::HashCodec::other && first.ok) fail("vcl", seed, "a codec with no VCL parser claimed a digest");
}

void case_link(std::mt19937_64& random, uint64_t seed) {
  bx::LinkTicket ticket;
  for (int i = 0; i < 32; ++i) ticket.endpoint[i] = static_cast<uint8_t>(random());
  for (int i = 0; i < 16; ++i) ticket.share[i] = static_cast<uint8_t>(random());
  for (int i = 0; i < 16; ++i) ticket.token[i] = static_cast<uint8_t>(random());
  ticket.expires = static_cast<uint32_t>(random());
  ticket.relay = "https://relay.example.test/" + std::to_string(random() % 100000);
  const std::string payload = bx::link_payload(ticket);
  const std::string url = bx::link_url("https://example.test/l/", ticket, "fuzz");
  const std::vector<std::string> corpus = {payload, url, payload.substr(0, payload.size() / 2), ""};
  const std::string input = mutated(random, corpus[random() % corpus.size()]);

  bx::LinkTicket parsed;
  if (bx::parse_link_payload(input, parsed)) {
    const std::string written = bx::link_payload(parsed);
    bx::LinkTicket again;
    if (!bx::parse_link_payload(written, again)) fail("link", seed, "a parsed payload did not write back into one that parses");
  }
  bx::LinkTicket from_url;
  if (bx::parse_link_url(input, from_url)) {
    const std::string written = bx::link_url("https://example.test/l/", from_url);
    bx::LinkTicket again;
    if (!bx::parse_link_url(written, again)) fail("link", seed, "a parsed URL did not write back into one that parses");
  }
}

void case_plans(std::mt19937_64& random, uint64_t seed) {
  bx::SessionEntry entry;
  entry.id = "BuffrX 2026-11-02 01-00-00";
  entry.kind = "recording";
  entry.started_ms = 1000;
  uint32_t at = entry.started_ms;
  const int count = 1 + static_cast<int>(random() % 8);
  for (int i = 0; i < count; ++i) {
    bx::SessionChunk chunk;
    chunk.file = "chunk-" + std::to_string(i) + ".mp4";
    chunk.bytes = static_cast<int64_t>(random() % (4 * 1024 * 1024));
    chunk.start_ms = at;
    at += 500 + static_cast<uint32_t>(random() % 20000);
    chunk.end_ms = at;
    chunk.wall_start_ms = chunk.start_ms;
    chunk.wall_end_ms = chunk.end_ms;
    chunk.epoch = (random() % 8 == 0) ? 1u : 0u;
    entry.chunks.push_back(chunk);
  }
  const uint32_t begin = entry.chunks.front().start_ms + static_cast<uint32_t>(random() % 3000);
  const uint32_t finish = begin + static_cast<uint32_t>(random() % 30000);

  bx::ConcatPlan plan;
  if (bx::concat_plan_for_range(entry.chunks, begin, finish, plan)) {
    if (plan.files.empty()) fail("plans", seed, "a range that touches a chunk planned no file");
    if (plan.start_in_first < 0.0 || plan.end_in_last < 0.0) fail("plans", seed, "a concat plan has a negative cut");
  }
  std::string error;
  const bx::TimelineSavePlan saved = bx::timeline_save_plan(entry, begin, finish, at, error);
  if (!saved.files.empty()) {
    // The cuts are each inside their own file: `start_in_first_ms` counts from the first file's start and
    // `end_in_last_ms` from the last one's, so a range that starts late and ends early has end < start and that is
    // correct. Map each planned file back to its chunk and hold the cuts to those chunks.
    const bx::SessionChunk* head = nullptr;
    const bx::SessionChunk* tail = nullptr;
    bool in_order = true;
    int64_t previous = -1;
    for (const std::string& file : saved.files) {
      int64_t index = -1;
      for (size_t i = 0; i < entry.chunks.size(); ++i) {
        if (entry.chunks[i].file == file) index = static_cast<int64_t>(i);
      }
      if (index < 0 || index <= previous) in_order = false;
      previous = index;
      if (saved.files.front() == file) head = &entry.chunks[static_cast<size_t>(index)];
      tail = &entry.chunks[static_cast<size_t>(index)];
    }
    if (!in_order || !head || !tail || saved.cut_head_ms < 0) {
      fail("plans", seed, "a Timeline save plan's files are not the session's chunks in order");
    } else {
      const int64_t head_ms = static_cast<int64_t>(head->end_ms) - head->start_ms;
      const int64_t tail_ms = static_cast<int64_t>(tail->end_ms) - tail->start_ms;
      if (saved.start_in_first_ms < 0 || saved.start_in_first_ms > head_ms || saved.end_in_last_ms < 0 ||
          saved.end_in_last_ms > tail_ms || (saved.cut_head_ms > 0 && saved.start_in_first_ms != 0)) {
        fail("plans", seed, "a Timeline save plan has a cut outside its own file");
      }
    }
  }
}

void case_mp4(std::mt19937_64& random, uint64_t seed) {
  std::string bytes = random_text(random, random() % 4096);
  if (random() % 2 == 0 && bytes.size() >= 8) {
    // A box header at the front gives the parser a real shape rather than noise.
    const uint32_t size = static_cast<uint32_t>(bytes.size());
    bytes[0] = static_cast<char>((size >> 24) & 0xFF);
    bytes[1] = static_cast<char>((size >> 16) & 0xFF);
    bytes[2] = static_cast<char>((size >> 8) & 0xFF);
    bytes[3] = static_cast<char>(size & 0xFF);
    std::memcpy(&bytes[4], "moov", 4);
  }
  const std::vector<uint8_t> moov(bytes.begin(), bytes.end());
  const double head = static_cast<double>(random() % 100000) / 1000.0;
  const std::vector<uint8_t> trimmed = bx::mp4_trim_moov(moov, head);
  if (trimmed.size() > moov.size()) fail("mp4", seed, "trimming a moov grew it");
  if (moov.empty() && !trimmed.empty()) fail("mp4", seed, "an empty moov produced bytes");
}

void case_session(std::mt19937_64& random, uint64_t seed) {
  bx::SessionEntry entry;
  entry.id = "BuffrX 2026-11-02 01-00-00";
  entry.kind = "recording";
  entry.app_id = "1234";
  entry.profile = "Auto";
  entry.started_ms = 1000;
  entry.started_at = 1760000000;
  entry.ended_ms = 9000;
  entry.closed = true;
  for (int i = 0; i < 3; ++i) {
    bx::SessionChunk chunk;
    chunk.file = "C:/clips/chunk-" + std::to_string(i) + ".mp4";
    chunk.bytes = 4096;
    chunk.start_ms = 1000 + static_cast<uint32_t>(i) * 1000;
    chunk.end_ms = 2000 + static_cast<uint32_t>(i) * 1000;
    chunk.wall_start_ms = 1760000000000LL + i * 1000;
    chunk.wall_end_ms = chunk.wall_start_ms + 1000;
    entry.chunks.push_back(chunk);
  }
  bx::SessionMarker marker;
  marker.at_ms = 1500;
  marker.kind = "clip";
  marker.label = "1.2 GB";
  entry.markers.push_back(marker);

  const std::string index = bx::session_entry_json(entry).dump();
  const std::string input = mutated(random, index);
  bx::Json parsed;
  std::string error;
  if (!bx::Json::parse(input, parsed, error)) return;
  bx::SessionEntry out;
  if (!bx::session_entry_from_json(parsed, out)) return;
  const std::string again = bx::session_entry_json(out).dump();
  bx::Json check;
  std::string check_error;
  if (!bx::Json::parse(again, check, check_error)) fail("session", seed, "a loaded session entry wrote an index that does not parse");
}

// The order here is the case order: seed = case index, so every target is driven and a replay is exact.
constexpr int kTargets = 9;
const char* const kTargetNames[kTargets] = {"json", "config", "frame_length", "record", "vcl", "link", "plans", "mp4", "session"};

void run_case(uint64_t seed) {
  const int target = static_cast<int>(seed % kTargets);
  note_case(kTargetNames[target], seed);
  std::mt19937_64 random(seed * 0x9E3779B97F4A7C15ull + 1);
  switch (target) {
    case 0: case_json(random, seed); break;
    case 1: case_config(random, seed); break;
    case 2: case_frame_length(random, seed); break;
    case 3: case_record(random, seed); break;
    case 4: case_vcl(random, seed); break;
    case 5: case_link(random, seed); break;
    case 6: case_plans(random, seed); break;
    case 7: case_mp4(random, seed); break;
    default: case_session(random, seed); break;
  }
  g_case_start.store(0, std::memory_order_relaxed);
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t cases = 0;
  bool single = false;
  uint64_t seed = 1;
  int seconds = 1200;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed = std::strtoull(argv[++i], nullptr, 10);
      single = true;
    } else {
      seconds = std::atoi(argv[i]);
    }
  }
  std::thread(watchdog).detach();
  const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
  if (single) {
    run_case(seed);
    std::printf("buffrx-fuzz: 1 case, %d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
  }
  for (; GetTickCount64() < deadline; ++seed) {
    run_case(seed);
    ++cases;
  }
  std::printf("buffrx-fuzz: %llu cases in %d s, %d failures\n", static_cast<unsigned long long>(cases), seconds, g_failures);
  return g_failures == 0 ? 0 : 1;
}
