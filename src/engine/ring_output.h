// BuffrX's own replay ring: an encoded-packet output that holds the last stretch of footage in RAM and writes it out on a save. It is a
// port of OBS's replay buffer (the same packets, the same muxer process, the same file), with what BuffrX needs on top: the ring's own
// byte and span readout (`ring_info`) instead of a second output that only counted bytes, and the ring's limits under BuffrX's control
// (src/engine/ring_index.h). Same procs and signal as `replay_buffer` (`save`, `save_seconds`,
// `get_last_replay`, `saved`), plus `ring_info`, `spill_problem`, `link_chunk`, `chunk_usable`, `chunk_states` and
// `save_result`: the verdict of the last save (a clip, a clip with a gap, or a failure), because `saved` says a save
// ended and not that it worked.
#pragma once

#include <cstdint>
#include <string>

namespace bx {

// Registers the output type "buffrx_ring" with libobs. Once per obs_startup: obs_shutdown forgets every registered type.
void register_ring_output();

// What the ring's `chunk_states` proc reports of a Timeline chunk it was handed (Timeline dedupe, obs_host_dedupe.cpp).
struct RingChunkState {
  uint32_t id = 0;
  std::string file;
  bool linked = false;   // matched to a place in the ring
  bool usable = false;   // the host has pinned it and found its file: the spill may rely on it
  uint64_t at = 0;       // the ring's video ordinal of the chunk's first packet
  uint32_t count = 0;    // its video packets
};

}  // namespace bx
