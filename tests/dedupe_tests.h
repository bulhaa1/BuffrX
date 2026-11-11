// The Timeline dedupe's checks (timeline_link.h, ring_index.h's chunk tier, ring_store.h, ring_parts.h). Included by engine_tests.cpp after the ring's test helpers
// (SpillStore, SpillRig, spill_collect), so it shares them. A "chunk file" in these tests is a range of the original stream: the part-wise save is compared
// with the single-pipe save of the same footage.
#pragma once

#include "../src/engine/dedupe_policy.h"

#include <chrono>
#include <cstdio>
#include <functional>

// ---------------------------------------------------------------- the link rule (timeline_link.h)

namespace dedupe_test {

constexpr int64_t kFrameUs = 50000;  // 20 fps

std::vector<bx::RingKey> ring_keys(const std::vector<int>& distinct, uint64_t gop, uint64_t first_seq = 0) {
  std::vector<bx::RingKey> keys;
  for (size_t i = 0; i < distinct.size(); ++i) {
    bx::RingKey key;
    key.seq = first_seq + i * gop;
    key.dts_usec = static_cast<int64_t>(key.seq) * kFrameUs;
    key.hash = 0x1000 + static_cast<uint64_t>(distinct[i]);
    key.vcl_bytes = 500;
    keys.push_back(key);
  }
  return keys;
}

// A chunk of `gops` groups of `gop` packets whose keyframes carry the hashes of ring keys starting at `from` (index into `distinct`).
bx::ChunkFacts chunk_of(const std::vector<int>& distinct, size_t from, size_t gops, uint64_t gop, bool follows) {
  bx::ChunkFacts chunk;
  chunk.video_count = static_cast<uint32_t>(gops * gop);
  chunk.follows_previous = follows;
  for (size_t g = 0; g < gops; ++g) {
    bx::ChunkKey key;
    key.index = static_cast<uint32_t>(g * gop);
    key.dts_us = static_cast<int64_t>(g * gop) * kFrameUs;
    key.hash = 0x1000 + static_cast<uint64_t>(distinct[from + g]);
    key.vcl_bytes = 500;
    chunk.keys.push_back(key);
  }
  return chunk;
}

}  // namespace dedupe_test

void test_timeline_link() {
  using namespace dedupe_test;
  // Ring: 12 keyframes of 20 packets, all different. Chunks of two groups each.
  const std::vector<int> distinct = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  const std::vector<bx::RingKey> keys = ring_keys(distinct, 20);
  const uint64_t end = 12 * 20 + 1;
  std::vector<bx::ChunkFacts> chunks = {chunk_of(distinct, 2, 2, 20, false), chunk_of(distinct, 4, 2, 20, true)};
  std::vector<bx::ChunkLink> links = bx::link_chunks(keys, end, chunks);
  check(links[0].linked && links[0].at == 40 && links[1].linked && links[1].at == 80, "chunks whose keyframes are unique in the ring are linked at their place");

  chunks[0].video_count = 41;  // one packet too many: the end is not on a keyframe
  check(!bx::link_chunks(keys, end, chunks)[0].linked, "a chunk whose count is off by one is not linked (it would shift every neighbour)");
  chunks[0].video_count = 40;

  std::vector<bx::ChunkFacts> late = {chunk_of(distinct, 10, 2, 20, false)};  // runs to the ring's end: the packet after it is not known
  check(!bx::link_chunks(keys, 12 * 20, late)[0].linked, "a chunk the ring has not got past yet is not linked");
  late[0] = chunk_of(distinct, 9, 2, 20, false);
  check(bx::link_chunks(keys, end, late)[0].linked && bx::link_chunks(keys, end, late)[0].at == 180, "and the same chunk is linked once the ring has the keyframe after it");

  std::vector<bx::ChunkFacts> skewed = {chunk_of(distinct, 2, 2, 20, false)};
  skewed[0].keys[1].dts_us += 20000;  // the second keyframe is 20 ms off where the ring has it
  check(!bx::link_chunks(keys, end, skewed)[0].linked, "keyframes at the right places but the wrong times are not a match");

  std::vector<bx::ChunkFacts> missing = {chunk_of(distinct, 2, 2, 20, false)};
  missing[0].keys.pop_back();  // the chunk lacks a keyframe the ring has inside the same span
  check(!bx::link_chunks(keys, end, missing)[0].linked, "a ring keyframe inside the span that the chunk does not have is not a match");

  std::vector<bx::ChunkFacts> unknown = {chunk_of(distinct, 2, 2, 20, false)};
  unknown[0].keys[0].hash ^= 0xff;
  check(!bx::link_chunks(keys, end, unknown)[0].linked, "a chunk whose first picture is not in the ring is not linked");

  // A still picture: every keyframe the same. Alone, the chunk could be anywhere; with a distinct neighbour it is placed by the chain.
  const std::vector<int> still = {1, 1, 1, 1, 1, 1, 1, 1};
  const std::vector<bx::RingKey> still_keys = ring_keys(still, 20);
  std::vector<bx::ChunkFacts> alone = {chunk_of(still, 0, 2, 20, false)};
  check(!bx::link_chunks(still_keys, 8 * 20 + 1, alone)[0].linked, "identical keyframes make a chunk ambiguous: it is not linked");

  std::vector<int> mixed = {1, 1, 1, 1, 1, 7, 8, 1};  // the ring saw something other than a still picture at 5-6
  const std::vector<bx::RingKey> mixed_keys = ring_keys(mixed, 20);
  std::vector<bx::ChunkFacts> chain = {chunk_of(mixed, 3, 2, 20, false), chunk_of(mixed, 5, 2, 20, true)};  // [3,4] is still, [5,6] is distinct
  const std::vector<bx::ChunkLink> chained = bx::link_chunks(mixed_keys, 8 * 20 + 1, chain);
  check(chained[1].linked && chained[1].at == 100 && chained[0].linked && chained[0].at == 60,
        "the distinct chunk anchors the still one before it: the chunk starts where the next one ends");
  chain[1].follows_previous = false;
  check(!bx::link_chunks(mixed_keys, 8 * 20 + 1, chain)[0].linked, "without a promise that nothing came between them, the chain decides nothing");

  // Settled links stand while the ring's front moves on, and they anchor their neighbours.
  std::vector<bx::ChunkLink> known(2);
  known[1].linked = true;
  known[1].at = 100;
  const std::vector<bx::RingKey> trimmed(mixed_keys.begin() + 5, mixed_keys.end());  // the front is past the still chunk and its keys
  std::vector<bx::ChunkFacts> sticky = {chunk_of(mixed, 3, 2, 20, false), chunk_of(mixed, 5, 2, 20, true)};
  const std::vector<bx::ChunkLink> kept = bx::link_chunks(trimmed, 8 * 20 + 1, sticky, known);
  check(kept[1].linked && kept[1].at == 100 && !kept[0].linked, "a settled link is not asked again, and one whose keys left the ring stays unlinked rather than guessed");

  // Chunks that claim to follow each other but sit in the wrong places: neither is trusted.
  std::vector<bx::ChunkFacts> wrong = {chunk_of(distinct, 4, 2, 20, false), chunk_of(distinct, 2, 2, 20, true)};
  const std::vector<bx::ChunkLink> disagree = bx::link_chunks(keys, end, wrong);
  check(!disagree[0].linked && !disagree[1].linked, "chunks out of order (a later one placed before an earlier one) are both refused");
  check(!bx::link_chunks(keys, end, {}).size(), "no chunks, no links");
  bx::ChunkFacts empty;
  check(!bx::link_chunks(keys, end, {empty})[0].linked, "a chunk with nothing in it is not linked");
}

// ---------------------------------------------------------------- the index's chunk tier

void test_ring_index_chunks() {
  using bx::RingEntry;
  using bx::SpillStep;
  const int64_t mb = 1000000;
  auto entry = [](int64_t seconds, bool video, bool key, uint32_t size) {
    RingEntry e;
    e.dts_usec = seconds * 1000000;
    e.size = size;
    e.video = video;
    e.video_keyframe = video && key;
    return e;
  };
  size_t position = 0;
  int64_t offset = 0;

  // Video seqs 0-9, a keyframe every other second. A chunk covers seqs 2-5, then a pause, then another covers 8-9.
  bx::RingIndex ring;
  ring.enable_disk(8 * bx::kSpillBlockBytes);
  for (int t = 0; t < 12; ++t) {
    const RingEntry e = entry(t, true, t % 2 == 0, mb);
    ring.room_for(e, 1000 * 1000000LL, 0);
    ring.push(e);
  }
  check(ring.first_video_seq() == 0 && ring.end_video_seq() == 12, "video packets are numbered from the start of the ring");
  bx::CoverRange a{7, 2, 6, 2 * 1000000, 6 * 1000000};
  bx::CoverRange b{8, 8, 10, 8 * 1000000, 10 * 1000000};
  ring.set_cover({a, b});
  // seq 0 and 1 are not covered: they stage. Spill start 0 means everything is due.
  check(ring.step_spill(0, position, offset) == SpillStep::staged && ring.step_spill(0, position, offset) == SpillStep::staged, "records no chunk holds are staged for the file as before");
  check(ring.step_spill(0, position, offset) == SpillStep::flush,
        "the first covered record behind staged ones asks for the block to be padded and written: the part out of RAM must stay a prefix");
  const int64_t head_before = ring.head();
  ring.note_padded(bx::kSpillBlockBytes);
  check(ring.head() == bx::kSpillBlockBytes && ring.head() > head_before, "the padded block moves the next record to a block edge");
  check(ring.block_written(bx::kSpillBlockBytes) == 2 && ring.disk_count() == 2 && ring.nonram_count() == 2 && ring.staged_count() == 0, "the staged records leave RAM with the block");
  size_t flipped = 0;
  while (ring.step_spill(0, position, offset) == SpillStep::flipped) ++flipped;
  check(flipped == 4 && ring.chunk_count() == 4 && ring.chunk_bytes() == 4 * mb && ring.deduped_total() == 4 * mb, "the four covered records leave RAM without being written");
  check(ring.ram_bytes() == 6 * mb && ring.bytes() == 12 * mb && ring.spilled_total() == 2 * (mb + bx::kSpillRecordHeaderBytes),
        "RAM drops by what left, the footage held does not change, and only the two disk records count as written");
  check(ring.tail() == 0 && ring.nonram_count() == 6, "the file's tail is its oldest disk record; chunk records have none");
  // seqs 6 and 7 are not covered: staged; 8 is covered behind them: flush again.
  check(ring.staged_count() == 1 && ring.step_spill(0, position, offset) == SpillStep::staged, "records after the chunk are staged again (seq 6 was taken by the loop that ended the run)");
  check(ring.step_spill(0, position, offset) == SpillStep::flush, "and the next chunk asks for another flush");
  const std::vector<RingEntry> view = ring.snapshot();
  check(view[0].on_disk && view[2].on_chunk && view[5].on_chunk && view[6].staged && !view[7].on_chunk && view[2].chunk_id == 7 && view[2].video_seq == 2,
        "a snapshot says where each record is, and which chunk holds the ones that left for one");
  ring.release_pin();
  ring.note_padded(2 * bx::kSpillBlockBytes);
  ring.block_written(2 * bx::kSpillBlockBytes);
  while (ring.step_spill(0, position, offset) == SpillStep::flipped) {
  }
  check(ring.chunk_count() == 6 && ring.disk_count() == 4, "disk, chunk, disk, chunk: the part out of RAM alternates");
  check(ring.tail() == 0 && ring.first_video_seq() == 0, "and the file's tail is still its oldest disk record");

  // Time eviction takes chunk records like any other: nothing to release, no space in the file freed.
  bx::RingIndex evict;
  evict.enable_disk(4 * bx::kSpillBlockBytes);
  for (int t = 0; t < 6; ++t) {
    const RingEntry e = entry(t, true, true, mb);
    evict.room_for(e, 1000 * 1000000LL, 0);
    evict.push(e);
  }
  evict.set_cover({bx::CoverRange{1, 0, 3, 0, 3 * 1000000}});
  while (evict.step_spill(0, position, offset) == SpillStep::flipped) {
  }
  check(evict.chunk_count() == 3 && evict.ram_bytes() == 3 * mb, "setup: three records are in a chunk");
  RingEntry late = entry(10, true, true, mb);
  const size_t gone = evict.room_for(late, 7500000, 0);  // keep 7.5 s: exactly the three chunk records leave
  check(gone == 3 && evict.removed_ram() == 0 && evict.chunk_count() == 0 && evict.chunk_bytes() == 0, "leaving by time, chunk records have no payload to release");

  // The ceiling backstop gives up the whole part that is out of RAM, chunk records included.
  bx::RingIndex ceiling;
  ceiling.enable_disk(4 * bx::kSpillBlockBytes);
  for (int t = 0; t < 10; ++t) {
    const RingEntry e = entry(t, true, t % 2 == 0, mb);
    ceiling.room_for(e, 1000 * 1000000LL, 0);
    ceiling.push(e);
  }
  ceiling.set_cover({bx::CoverRange{1, 0, 6, 0, 6 * 1000000}});
  while (ceiling.step_spill(2 * mb, position, offset) == SpillStep::flipped) {
  }
  check(ceiling.chunk_count() > 0 && ceiling.ram_bytes() <= 4 * mb + mb, "setup: records are in a chunk and RAM is down");
  ceiling.room_for(entry(10, false, false, mb), 1000 * 1000000LL, 3 * mb);
  check(ceiling.chunk_count() == 0 && ceiling.backstop_events() == 1, "at the RAM ceiling the part that left RAM goes as a whole, once");

  // No cover, no change: step_spill is next_to_stage.
  bx::RingIndex plain;
  plain.enable_disk(4 * bx::kSpillBlockBytes);
  for (int t = 0; t < 4; ++t) plain.push(entry(t, true, true, mb));
  check(plain.step_spill(0, position, offset) == SpillStep::staged && plain.chunk_count() == 0, "with no chunk linked a record is staged exactly as before");
}

// ---------------------------------------------------------------- the whole store against fake chunk files

namespace dedupe_test {

// An H.264 packet the hash can read: a start code, a slice header byte, then bytes that are never zero. `seed` makes a keyframe's picture its own.
SpillTestPacket packet(bool video, bool key, int64_t dts_usec, uint32_t size, uint32_t seed) {
  SpillTestPacket p;
  p.bytes = std::make_shared<std::vector<uint8_t>>(size);
  std::vector<uint8_t>& b = *p.bytes;
  uint32_t state = seed * 2654435761u + 12345u;
  for (uint8_t& byte : b) {
    state = state * 1664525u + 1013904223u;
    byte = static_cast<uint8_t>(1 + (state >> 24) % 250);
  }
  if (video) {
    b[0] = b[1] = b[2] = 0;
    b[3] = 1;
    b[4] = key ? 0x65 : 0x41;
  }
  p.fields.video = video;
  p.fields.keyframe = video && key;
  p.fields.dts_usec = dts_usec;
  p.fields.timebase_num = 1;
  p.fields.timebase_den = video ? 1000000 : 48000;
  p.fields.dts = video ? dts_usec : dts_usec * 48 / 1000;
  p.fields.pts = p.fields.dts;
  return p;
}

// `seconds` of 20 fps video with a keyframe every `gop` packets and an audio packet at every frame (arriving after its picture).
std::vector<SpillTestPacket> stream(int seconds, int gop) {
  std::vector<SpillTestPacket> out;
  for (int frame = 0; frame < seconds * 20; ++frame) {
    const int64_t at = frame * kFrameUs;
    const bool key = frame % gop == 0;
    out.push_back(packet(true, key, at, key ? 160 * 1024 : 40 * 1024, static_cast<uint32_t>(frame)));
    out.push_back(packet(false, false, at, 3 * 1024, 100000u + static_cast<uint32_t>(frame)));
  }
  return out;
}

struct Clip {
  std::vector<std::vector<uint8_t>> video, audio;
  std::vector<bool> key;
  size_t parts = 0;
  bool seams_on_keyframes = true;
};

Clip collect_plain(SpillStore& store, bx::SaveReport* report = nullptr) {
  Clip clip;
  const bx::SaveReport done = store.save([&](const bx::PacketFields& f) {
    std::vector<uint8_t> bytes(f.data, f.data + f.size);
    if (f.video) {
      clip.video.push_back(bytes);
      clip.key.push_back(f.keyframe);
    } else {
      clip.audio.push_back(bytes);
    }
    return true;
  });
  if (report) *report = done;
  return clip;
}

// The save of ring_output.cpp, with the chunk files replaced by ranges of the original stream. `available` says which chunks still exist.
Clip collect_parts(SpillStore& store, const std::vector<SpillTestPacket>& all, std::function<bool(const bx::ChunkRec&)> available, bx::SaveReport* report,
                   bx::SavePlan* plan_out = nullptr) {
  Clip clip;
  std::unique_ptr<SpillStore::SaveJob> job = store.begin_save();
  const bx::SavePlan plan = store.plan_save(*job, available);
  if (plan_out) *plan_out = plan;
  std::vector<int64_t> video_dts;
  std::vector<const SpillTestPacket*> videos, audios;
  for (const SpillTestPacket& p : all) {
    if (p.fields.video) {
      video_dts.push_back(p.fields.dts_usec);
      videos.push_back(&p);
    } else {
      audios.push_back(&p);
    }
  }
  bx::SaveReport total;
  total.ok = true;
  std::vector<uint32_t> everything(job->entries.size());
  for (uint32_t i = 0; i < everything.size(); ++i) everything[i] = i;
  const std::vector<bx::SavePart> parts = plan.valid ? plan.parts : std::vector<bx::SavePart>{bx::SavePart{false, everything, {}, 0}};
  bool first_part = true;
  for (const bx::SavePart& part : parts) {
    ++clip.parts;
    if (part.file) {
      for (size_t c = 0; c < part.chunks.size(); ++c) {
        const bx::ChunkRec& rec = job->chunks.at(part.chunks[c]);
        uint64_t s0 = rec.at;
        if (first_part && c == 0 && part.start_in_first > 0) s0 += static_cast<uint64_t>(part.start_in_first / 0.05 + 0.5);
        const uint64_t s1 = rec.at + rec.count;
        const int64_t t0 = video_dts[s0], t1 = video_dts[s1];
        if (!videos[s0]->fields.keyframe) clip.seams_on_keyframes = false;
        for (uint64_t s = s0; s < s1; ++s) {
          clip.video.push_back(*videos[s]->bytes);
          clip.key.push_back(videos[s]->fields.keyframe);
        }
        for (const SpillTestPacket* a : audios) {
          if (a->fields.dts_usec >= t0 && a->fields.dts_usec < t1) clip.audio.push_back(*a->bytes);
        }
      }
    } else {
      bool first_video = true;
      const bx::SaveReport r = store.run_part(*job, part.entries, [&](const bx::PacketFields& f) {
        std::vector<uint8_t> bytes(f.data, f.data + f.size);
        if (f.video) {
          if (first_video && !f.keyframe && !first_part) clip.seams_on_keyframes = false;
          first_video = false;
          clip.video.push_back(bytes);
          clip.key.push_back(f.keyframe);
        } else {
          clip.audio.push_back(bytes);
        }
        return true;
      });
      total.skipped += r.skipped;
      total.unreadable = total.unreadable || r.unreadable;
      total.ok = total.ok && r.ok;
    }
    first_part = false;
  }
  if (report) *report = total;
  return clip;
}

bx::ChunkProbe probe_of(const std::vector<SpillTestPacket>& all, uint64_t seq0, uint32_t count, bool follows, const std::string& name) {
  bx::ChunkProbe probe;
  probe.file = name;
  probe.facts.video_count = count;
  probe.facts.follows_previous = follows;
  uint64_t seq = 0;
  int64_t base = 0;
  for (const SpillTestPacket& p : all) {
    if (!p.fields.video) continue;
    if (seq == seq0) base = p.fields.dts_usec;
    if (seq >= seq0 && seq < seq0 + count && p.fields.keyframe) {
      bx::ChunkKey key;
      key.index = static_cast<uint32_t>(seq - seq0);
      key.dts_us = p.fields.dts_usec - base;
      const bx::VclDigest d = bx::vcl_digest(bx::HashCodec::h264, p.bytes->data(), p.bytes->size(), 0);
      key.hash = d.hash;
      key.vcl_bytes = d.bytes;
      probe.facts.keys.push_back(key);
      probe.key_time_s.push_back(key.index * 0.05);
    }
    ++seq;
  }
  return probe;
}

// Pushes `all` into the rig and gives it each chunk (100 packets, five groups) the moment the ring has the keyframe after it. `timeline_on(c)` says whether
// Timeline was writing chunk c (a chunk it did not write is never given). `session` is the Timeline session the files belong to, as the host hands it over.
struct DedupeRun {
  uint32_t chunk_ids[64] = {};
  int chunks = 0;
};
DedupeRun feed(SpillRig& rig, const std::vector<SpillTestPacket>& all, const std::function<bool(int)>& timeline_on, const std::function<bool(int)>& usable,
               const std::string& session = std::string()) {
  DedupeRun run;
  uint64_t seq = 0;
  int next_chunk = 0;
  for (const SpillTestPacket& p : all) {
    rig.push(p);
    if (!p.fields.video) continue;
    if (seq == static_cast<uint64_t>(next_chunk + 1) * 100) {  // the first packet after chunk `next_chunk`
      if (timeline_on(next_chunk)) {
        bx::ChunkProbe probe =
            probe_of(all, static_cast<uint64_t>(next_chunk) * 100, 100, next_chunk > 0 && timeline_on(next_chunk - 1), "chunk" + std::to_string(next_chunk));
        probe.session_id = session;
        const uint32_t id = rig.store.add_chunk(probe);
        if (usable(next_chunk)) rig.store.set_chunk_usable(id, true);
        run.chunk_ids[next_chunk] = id;
      }
      ++run.chunks;
      ++next_chunk;
    }
    ++seq;
    while (rig.store.spill_due() && rig.store.spill_step()) {
    }
  }
  return run;
}

}  // namespace dedupe_test

void test_dedupe_store() {
  using namespace dedupe_test;
  const int64_t file = 12 * bx::kSpillBlockBytes;
  const int64_t limit = 16 * kMiB;
  const int64_t window_us = 45 * 1000000LL;
  const int64_t no_ceiling = 1LL << 40;
  const std::vector<SpillTestPacket> all = stream(80, 20);  // 80 s, a keyframe a second, about 1 MB a second

  SpillStore reference;
  reference.configure(window_us, no_ceiling);
  SpillRig plain(file, limit, window_us, no_ceiling);  // the same ring with the disk tier and no dedupe
  for (const SpillTestPacket& p : all) {
    reference.push(p);
    plain.push(p);
  }
  const Clip oracle = collect_plain(reference);
  const bx::StoreInfo plain_info = plain.store.info();
  check(!oracle.video.empty() && plain_info.spilled_total > 0, "setup: the footage passes the RAM limit and the plain ring spills");

  // Dedupe configured, Timeline on for every chunk and every chunk usable.
  SpillRig on(file, limit, window_us, no_ceiling);
  on.store.configure_dedupe(bx::HashCodec::h264);
  const DedupeRun run = feed(on, all, [](int) { return true; }, [](int) { return true; });
  const bx::StoreInfo info = on.store.info();
  check(run.chunks >= 7 && info.chunks_linked >= 1, "chunks were handed over and matched to the ring");
  check(info.deduped_total > 0 && info.chunk_bytes > 0, "records left RAM through chunks");
  check(info.spilled_total < plain_info.spilled_total / 4, "and the ring file was written a fraction of what a plain ring writes");
  check(on.max_ram_seen <= limit, "RAM stays under its limit");
  bool sequential = true;
  for (size_t i = 0; i < on.disk.writes.size(); ++i) sequential = sequential && on.disk.writes[i] == static_cast<int64_t>(i) * bx::kSpillBlockBytes;
  check(sequential, "whatever is written is still written strictly in order");
  bx::SaveReport report;
  bx::SavePlan plan;
  const Clip parts = collect_parts(on.store, all, [](const bx::ChunkRec&) { return true; }, &report, &plan);
  check(plan.valid && plan.file_parts() >= 1 && report.ok && report.skipped == 0 && !report.unreadable, "the save is planned in parts and every packet part reads");
  check(parts.video == oracle.video && parts.key == oracle.key, "the video of a part-wise clip is packet for packet the video of the single-pipe clip");
  check(parts.audio == oracle.audio, "and so is the audio");
  check(parts.seams_on_keyframes, "every seam is on a video keyframe");

  // Timeline paused for chunks 8-9 (40-50 s, inside the ring's last 45 s): that part spills as today, and the clip is the same.
  SpillRig paused(file, limit, window_us, no_ceiling);
  paused.store.configure_dedupe(bx::HashCodec::h264);
  feed(paused, all, [](int c) { return c != 8 && c != 9; }, [](int) { return true; });
  const bx::StoreInfo paused_info = paused.store.info();
  bx::SavePlan paused_plan;
  bx::SaveReport paused_report;
  const Clip paused_clip = collect_parts(paused.store, all, [](const bx::ChunkRec&) { return true; }, &paused_report, &paused_plan);
  check(paused_info.spilled_total > info.spilled_total && paused_info.spilled_total < plain_info.spilled_total, "a pause costs the uncovered part's writes, and no more");
  check(paused_plan.valid && paused_report.ok && paused_report.skipped == 0 && paused_clip.video == oracle.video && paused_clip.audio == oracle.audio,
        "a Timeline that paused mid-buffer still gives the whole clip, gap-free");
  check(paused.max_ram_seen <= limit, "and RAM stayed bounded across the switches between the file and the chunks");
  bool packet_between = false;
  for (size_t i = 1; i + 1 < paused_plan.parts.size(); ++i) packet_between = packet_between || (!paused_plan.parts[i].file && paused_plan.parts[i - 1].file && paused_plan.parts[i + 1].file);
  check(packet_between, "the plan has a packet part between two file parts");

  // A chunk file that is gone at save time: the records that left RAM for it are lost, up to the next keyframe, and the report says so.
  bx::SaveReport lost_report;
  bx::SavePlan lost_plan;
  const std::string gone = "chunk9";
  const Clip lost = collect_parts(on.store, all, [&](const bx::ChunkRec& rec) { return rec.file != gone; }, &lost_report, &lost_plan);
  check(lost_plan.valid && lost_plan.lost > 0 && lost_report.ok, "a missing chunk file is found at save time and counted, and the save goes on");
  check(oracle.video.size() - lost.video.size() == 100, "the clip is short by exactly that chunk's pictures (100: five whole groups)");
  check(lost.key.size() == lost.video.size() && lost.audio.size() < oracle.audio.size(), "and its sound");

  // Chunks probed but never trusted (not usable): nothing leaves RAM for them, the ring spills as before.
  SpillRig untrusted(file, limit, window_us, no_ceiling);
  untrusted.store.configure_dedupe(bx::HashCodec::h264);
  feed(untrusted, all, [](int) { return true; }, [](int) { return false; });
  const bx::StoreInfo untrusted_info = untrusted.store.info();
  check(untrusted_info.deduped_total == 0 && untrusted_info.spilled_total == plain_info.spilled_total, "a chunk the host has not pinned is not relied on: the ring writes what a plain ring writes");
  bx::SavePlan no_plan;
  const Clip untrusted_clip = collect_parts(untrusted.store, all, [](const bx::ChunkRec&) { return true; }, nullptr, &no_plan);
  check(no_plan.file_parts() == 0 && untrusted_clip.video == oracle.video && untrusted_clip.audio == oracle.audio, "and its clip is the plain clip");

  // Dedupe configured but no chunk ever arrives: the same writes as a plain ring, the same clip, today's single pipe.
  SpillRig idle(file, limit, window_us, no_ceiling);
  idle.store.configure_dedupe(bx::HashCodec::h264);
  for (const SpillTestPacket& p : all) idle.push(p);
  check(idle.disk.writes == plain.disk.writes && idle.store.info().spilled_total == plain_info.spilled_total, "hashing keyframes with no chunk changes nothing that is written");
  bx::SaveReport idle_report;
  const Clip idle_clip = collect_plain(idle.store, &idle_report);
  std::unique_ptr<SpillStore::SaveJob> idle_job = idle.store.begin_save();
  check(!bx::snapshot_has_chunks(idle_job->entries) && idle_clip.video == oracle.video && idle_clip.audio == oracle.audio && idle_report.skipped == 0,
        "a snapshot with no chunk records takes the single-pipe path and gives the plain clip");

  // The pure rules of the fallback.
  bx::SavePlan pretend;
  pretend.valid = true;
  bx::SavePart packet_old, file_part, packet_new;
  file_part.file = true;
  pretend.parts = {packet_old, file_part, packet_new};
  check(bx::newest_packet_part(pretend) == 2, "when the join is refused the newest packet part is the clip");
  pretend.parts = {file_part};
  check(bx::newest_packet_part(pretend) == -1, "and with no packet part there is no clip to fall back to");
}

// What hashing a keyframe costs the packet thread: the numbers are printed, the check is only a ceiling.
void test_key_hash_cost() {
  const size_t sizes[] = {256 * 1024, 2 * 1024 * 1024};
  for (const size_t size : sizes) {
    std::vector<uint8_t> data(size, 0x5a);
    data[0] = data[1] = data[2] = 0;
    data[3] = 1;
    data[4] = 0x65;
    const int rounds = 40;
    const auto start = std::chrono::steady_clock::now();
    uint64_t sink = 0;
    for (int i = 0; i < rounds; ++i) {
      data[100] = static_cast<uint8_t>(i);
      sink += bx::vcl_digest(bx::HashCodec::h264, data.data(), data.size(), 0).hash;
    }
    const double per_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / rounds;
    std::printf("  [key hash] %zu KiB keyframe: %.3f ms each (%llu)\n", size / 1024, per_ms, static_cast<unsigned long long>(sink & 1));
    check(per_ms < 500.0, "hashing a keyframe takes well under a frame time");
  }
  // 200 Mbps AV1: a 1.5 MB keyframe every 2-4 s among ~25 KB inter frames at 60 fps. The per-packet average is the keyframe's cost over the packets of its group.
  std::vector<uint8_t> av1(1536 * 1024, 0x33);
  av1[0] = static_cast<uint8_t>((6 << 3) | 2);
  const size_t payload = av1.size() - 4;
  av1[1] = static_cast<uint8_t>((payload & 0x7f) | 0x80);
  av1[2] = static_cast<uint8_t>(((payload >> 7) & 0x7f) | 0x80);
  av1[3] = static_cast<uint8_t>(payload >> 14);
  const auto start = std::chrono::steady_clock::now();
  bx::VclDigest digest;
  uint64_t folded = 0;
  for (int i = 0; i < 40; ++i) {
    av1[100] = static_cast<uint8_t>(i);  // a different packet each time, so none of the calls can be folded away
    digest = bx::vcl_digest(bx::HashCodec::av1, av1.data(), av1.size(), 0);
    folded ^= digest.hash;
  }
  const double av1_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 40;
  std::printf("  [key hash] 1.5 MiB AV1 keyframe: %.3f ms each; over a 2 s group of 120 packets that is %.4f ms a packet on average (%u picture bytes hashed, %llu)\n", av1_ms, av1_ms / 120.0, digest.bytes, static_cast<unsigned long long>(folded & 1));
  check(digest.ok && av1_ms / 120.0 < 2.0, "averaged over the packets of its group, a keyframe's hash is far under a millisecond a packet");
}

// The host's rules (dedupe_policy.h).
void test_dedupe_policy() {
  check(bx::dedupe_window_ok(30, 300) && bx::dedupe_window_ok(120, 600), "Timeline keeping 30 minutes covers a 5 minute buffer, 120 a 10 minute one");
  check(!bx::dedupe_window_ok(5, 300) && !bx::dedupe_window_ok(10, 600) && !bx::dedupe_window_ok(0, 60),
        "Timeline keeping less than the buffer plus two chunks and a minute does not: retention could delete what the ring points at");
  check(bx::dedupe_window_ok(10, 500) && !bx::dedupe_window_ok(10, 521), "the line is the buffer plus 20 s plus a minute");
  check(bx::dedupe_chunk_follows(true, 5000, 5000) && bx::dedupe_chunk_follows(true, 5000, 5060) && !bx::dedupe_chunk_follows(true, 5000, 5500) && !bx::dedupe_chunk_follows(false, 5000, 5000),
        "a file the splitter opened on the previous one's boundary follows it; a pause or the first of a segment does not");
  check(bx::dedupe_chunk_follows(true, 0xFFFFFFF0u, 4), "the tick clock wraps");

  bx::ChunkReport report;
  report.ok = true;
  report.video_count = 240;
  report.duration_seconds = 8;
  report.keys = {bx::ChunkKeyFact{0, 0, 0.0, 11, 500}, bx::ChunkKeyFact{120, 4000000, 4.0, 12, 500}};
  bx::ChunkProbe probe;
  check(bx::dedupe_probe_from_report("a.mp4", report, true, probe) && probe.facts.video_count == 240 && probe.facts.follows_previous && probe.facts.keys.size() == 2 &&
            probe.key_time_s.size() == 2 && probe.key_time_s[1] == 4.0 && probe.facts.keys[1].dts_us == 4000000 && probe.file == "a.mp4",
        "a chunk's reading becomes the store's facts");
  bx::ChunkReport bad = report;
  bad.keys[0].index = 3;
  check(!bx::dedupe_probe_from_report("a.mp4", bad, false, probe), "a chunk that does not begin on a keyframe is not offered");
  bad = report;
  bad.keys[1].vcl_bytes = 0;
  check(!bx::dedupe_probe_from_report("a.mp4", bad, false, probe), "nor one with a keyframe whose picture could not be read");
  bad = report;
  bad.ok = false;
  check(!bx::dedupe_probe_from_report("a.mp4", bad, false, probe), "nor one that could not be read");
}

void test_save_part_names() {
  check(bx::save_part_pid("part-1234-2.mp4") == 1234 && bx::save_part_pid("part-7-0.mkv") == 7, "a part of a save in parts names the engine that made it");
  check(bx::save_part_pid("replay-1234.ring") == 0 && bx::save_part_pid("part-.mp4") == 0 && bx::save_part_pid("part-12.mp4") == 0 && bx::save_part_pid("part-x-1.mp4") == 0,
        "no other file name is taken for one");
  check(bx::save_part_pid("part-99999999999-1.mp4") == 0, "nor a number too long to be a process id");
}

// A chunk the ring has left, with sound that was taken out for it still at the ring's front: not a gap, not a refusal to plan.
void test_plan_evicted_chunk() {
  std::vector<bx::RingEntry> entries;
  auto video = [](uint64_t seq, int64_t dts, bool key) {
    bx::RingEntry e;
    e.video = true;
    e.video_keyframe = key;
    e.video_seq = seq;
    e.dts_usec = dts;
    e.size = 10;
    return e;
  };
  bx::RingEntry leftover;
  leftover.on_chunk = true;
  leftover.chunk_id = 1;
  leftover.dts_usec = 900000;  // before the first picture
  leftover.size = 5;
  entries.push_back(leftover);
  for (uint64_t s = 10; s < 14; ++s) entries.push_back(video(s, 1000000 + static_cast<int64_t>(s - 10) * 50000, s == 10));
  std::map<uint32_t, bx::ChunkRec> chunks;
  bx::ChunkRec rec;
  rec.id = 1;
  rec.file = "old.mp4";
  rec.at = 0;
  rec.count = 5;
  rec.key_index = {0};
  rec.key_time_s = {0.0};
  chunks[1] = rec;
  const bx::SavePlan plan = bx::plan_save_parts(entries, chunks, [](const bx::ChunkRec&) { return true; });
  check(plan.valid && plan.lost == 0 && plan.file_parts() == 0 && plan.parts.size() == 1 && plan.parts[0].entries.size() == 4,
        "sound left at the front for a chunk the ring has left is dropped quietly: the plan stands and is one packet part");
}

// A ring ceiling under two spill blocks cannot work with the disk part (overnight finding, 2026-11-02): it is lifted to two blocks, not obeyed.
void test_ceiling_floor() {
  const int64_t window_us = 30 * 1000000LL;
  SpillRig tiny(8 * bx::kSpillBlockBytes, 12 * kMiB, window_us, 4 * kMiB);
  for (const SpillTestPacket& p : spill_stream(10, 0)) tiny.push(p);
  const bx::StoreInfo info = tiny.store.info();
  check(info.backstop_events == 0 && info.spilled_total > 0 && info.packets == 10 * 20 * 2,
        "a ceiling of 4 MiB under the disk part is lifted to two blocks: nothing is purged silently and the footage goes to the file");
}
