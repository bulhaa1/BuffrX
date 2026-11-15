// The speaker a program's row draws on Settings > Audio. Pure: what the meter says, and how long a loud
// sample is shown for after a silent one, so a 250-500 ms poll cannot blink the waves off between two.
#pragma once

#include <cstdint>

namespace bx::ui {

// About -48 dBFS: a stream that is running but silent does not count.
inline constexpr float kAudiblePeak = 0.004f;
inline constexpr uint32_t kAudioHoldMs = 700;    // longer than the slowest peak poll (500 ms)
inline constexpr uint32_t kAudioWaveMs = 125;    // 8 frames a second while it sounds: motion, not a strobe

struct AudioSpeakerState {
  float peak = -1.0f;  // -1: no session exposes a meter, which is unknown, never silence
  bool sounding = false;
};

// One sample for one program. A loud reading (re)arms `hold_until`; a silent one keeps the waves on until the
// hold runs out; an unknown meter is never called sound or silence.
inline AudioSpeakerState audio_speaker_sample(float peak, uint32_t& hold_until, uint32_t now_ms) {
  AudioSpeakerState state;
  state.peak = peak;
  if (peak >= kAudiblePeak) {
    hold_until = now_ms + kAudioHoldMs;
    state.sounding = true;
  } else {
    state.sounding = hold_until != 0 && now_ms < hold_until;
    if (!state.sounding) hold_until = 0;
  }
  return state;
}

// The wave frame drawn now: two states, alternating on the same beat for every speaker on the page.
inline int audio_speaker_frame(uint32_t now_ms) { return static_cast<int>((now_ms / kAudioWaveMs) % 2); }

// Whether a glyph at `(x, y)` of `size` is inside the rectangle it is drawn into. A row scrolled out of its list,
// or under a sheet, must not ask the panel for frames: an animation nobody can see only costs the GPU.
inline bool audio_speaker_visible(float x, float y, float size, float clip_x0, float clip_y0, float clip_x1,
                                  float clip_y1) {
  return x + size > clip_x0 && x < clip_x1 && y + size > clip_y0 && y < clip_y1;
}

}  // namespace bx::ui
