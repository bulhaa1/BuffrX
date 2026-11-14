// What a recorded file's audio tracks are, read from the file itself and from the sidecar the recorder
// left beside it (`<file>.buffrx-audio.json`, audio_routing.h). The editor must never take a layout from
// the current capture settings: a clip is edited months later, and its tracks were decided when it was
// written.
//
// A sidecar is only believed when it names every audio stream the container really has, in order, with a
// known role, AND when its recorded media identity still matches the file on disk: its size, its
// high-resolution last-write time, and a hash of its first and last 16 KiB. Track count alone is not
// identity: a different file with the same number of streams must not inherit an old mix. A missing,
// stale, hand-edited, legacy or foreign file is an import: it gets generic labels, and no track is
// assumed to be an aggregate mix. That is the rule that keeps an arbitrary multi-track file from being
// exported as if track 1 were a mix of the others.
//
// The identity is captured only after a writer has closed and the file has its final bytes. A sidecar
// written while a file is still growing is marked pending, and reads as an import until bind_audio_layout
// puts the identity beside the same labels.
//
// The aggregate mix: BuffrX records track 1 as the whole mix and the tracks after it as the sources it
// was mixed from. When every source of that mix is one of those tracks, the mix can be recomputed from
// the edited levels exactly (audio_mix_plan); when it is not, the mix is kept as it was recorded and a
// level change is refused rather than quietly misrepresented.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bx {

// `<file>.buffrx-audio.json`, beside the media file. The same suffix audio_routing.h writes.
inline constexpr const char* kAudioLayoutSuffix = ".buffrx-audio.json";

// The bytes a sidecar is bound to. `head` and `tail` are FNV-1a 64 hashes of the first and last
// `kAudioIdentityBytes` of the file (the tail is empty when the file is that small). Size and time alone
// miss a replacement that happens to keep both; the hashes catch it without a full-file read.
struct MediaIdentity {
  int64_t size = -1;
  int64_t mtime = 0;  // FILETIME ticks (100 ns since 1601), the same value Windows reports
  uint64_t head = 0;
  uint64_t tail = 0;
  bool valid = false;
};

// Reads the identity of `path` now. False when the file cannot be opened or changed while it was read,
// which is exactly when a sidecar must not be bound to it.
bool media_identity(const std::string& path, MediaIdentity& identity);

struct AudioTrackLabel {
  int index = 0;                     // 1-based, as the sidecar and the product count tracks
  std::string role;                  // mix | desktop | microphone | application | combined | empty | track
  std::string title;                 // "BuffrX Mix", "Discord", ...; empty for an import
  std::vector<std::string> sources;  // desktop | microphone | app:<exe>
};

struct AudioLayout {
  // A well-formed BuffrX sidecar matched exactly this file's audio streams.
  bool buffrx = false;
  // Track 1 is the whole mix. Only a BuffrX sidecar can say so.
  bool aggregate = false;
  // Every source of the mix is exactly one of the tracks after it.
  bool complete = false;
  std::vector<AudioTrackLabel> tracks;  // one per audio stream, in stream order
};

// The labels of a file, for an editor that is about to show and mix it. `audio_streams` is how many
// audio streams the container reports; zero means "unknown" and is treated as an import with no tracks.
// A sidecar whose media identity does not match the file is an import.
AudioLayout read_audio_layout(const std::string& media_path, int audio_streams);

// The labels a sidecar holds, with no stream count to compare: used by a join that carries them to its
// output. True only for this product's own sidecar, version 2, bound to this exact file.
bool read_bound_audio_layout(const std::string& media_path, AudioLayout& layout);

// Generic labels for a file whose tracks nothing describes, so a caller has one shape to draw.
AudioLayout imported_audio_layout(int audio_streams);

// The gain each stream is heard and exported at: its own level, silenced by its mute, and by solo on
// every track that is not soloed. Clamped to 0..4 like the export's own volume filter.
struct AudioGains {
  std::vector<double> values;
  bool changed = false;  // any value is not exactly 1
};
AudioGains audio_gains(const std::vector<double>& volumes, const std::vector<bool>& muted,
                       const std::vector<bool>& solo, int tracks);

// What an export does with the file's first stream.
enum class AudioMixPlan {
  keep,      // write the tracks as they are, each at its own gain
  recompute, // rebuild track 1 from the edited sources (the file is an aggregate mix)
  refuse,    // the mix cannot be rebuilt and a gain was changed: an error, not a wrong file
};
AudioMixPlan audio_mix_plan(const AudioLayout& layout, const AudioGains& gains, std::string& error);

// Writes the sidecar for a layout with more than one track; a single-track file gets none. `layout`
// must be a BuffrX layout: an import is never written as one. The media's identity is captured here, so
// call it only once the file has its final bytes. False when the media cannot be read, or when a file
// this product did not write already sits at the sidecar name: that file is never opened over.
bool write_audio_layout(const std::string& media_path, const AudioLayout& layout);

// The same labels, written while a writer still owns the media, with no identity yet. It reads as an
// import until bind_audio_layout captures the final bytes beside the same labels.
bool write_audio_layout_pending(const std::string& media_path, const AudioLayout& layout);

// Captures the final identity of `media_path` and rewrites the labels already in its sidecar beside it.
// True when there is nothing to bind (no sidecar) or the bind succeeded; false (with a log line) when a
// pending sidecar could not be re-read or the media could not be measured.
bool bind_audio_layout(const std::string& media_path);

// True when the file at the sidecar name is this product's own (any version), whatever its labels say.
// Used where a sidecar is moved with its clip: a stranger's file at that name stays.
bool audio_sidecar_is_ours(const std::string& media_path);

// Deletes the sidecar beside `media_path`, but only one that is a BuffrX sidecar: a file this product
// did not write is left alone.
void remove_audio_layout(const std::string& media_path);

// True when the two layouts describe the same tracks, roles and sources: what a join needs before it can
// carry one sidecar over to its output.
bool same_audio_layout(const AudioLayout& left, const AudioLayout& right);

// What a join of several clips can honestly say about the sound of its output.
//
// A join copies each part's streams in their own order, so stream 1 is the same lane in every part: it is
// the whole mix when every input is a BuffrX aggregate, and an independent track otherwise. A sidecar is
// written only when that meaning holds across the whole output.
//
// - Every input is the same bound layout: the output carries it unchanged.
// - Every input is a BuffrX aggregate with the same stream count, but the layouts differ: the output is
//   a positional decomposition. Track 1 stays the mix; each later track keeps its ordinal. A lane whose
//   source is the same in every input keeps that identity (a desktop lane stays a desktop lane). A lane
//   whose source changes between inputs is named generically and gets one synthetic logical source id of
//   its own, `join:track:<ordinal>`: the id names the output lane, never a physical source, so no input's
//   label is claimed for another's sound. When every input is complete, the mix can still be rebuilt
//   exactly from the output's own stems.
// - Some of those aggregates are incomplete (the mix holds sound no stem carries): the mix-first meaning
//   is kept, the layout is marked incomplete with `join:unlisted` in the mix's sources, and every level
//   change stays refused. Playback uses the mix alone, so nothing is heard twice.
// - Aggregates mixed with imports (or with a sidecar that is not an aggregate) have incompatible
//   first-stream meanings, and no sidecar can describe the result on its own: the join is refused before
//   any encode. Imports joined with imports carry no sidecar, exactly as before.
// - Single-stream inputs carry no sidecar: there is no mix to describe.
struct JoinAudioLayout {
  bool carries = false;   // write `layout` beside the finished output
  AudioLayout layout;     // meaningful only when `carries`
  std::string error;      // non-empty: the join must not be made
};
JoinAudioLayout join_audio_layout(const std::vector<AudioLayout>& inputs);

}  // namespace bx
