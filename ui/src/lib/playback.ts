// The two players that hand a sequence from one clip to the next: which slot holds the clip on
// screen, which holds the one after it, and where the next one is parked before it is needed.
// Pure, so the handoff's arithmetic is tested without a browser.

import type { SequenceClip } from './sequence.ts'

/** The player on screen for a clip: the two slots take turns, so a one-clip step needs no reload. */
export function playerSlot(index: number): 0 | 1 {
  return index % 2 === 0 ? 0 : 1
}

/** The clip a slot holds: the active slot holds `index`, the other holds the clip after it.
 *  -1 is an empty slot, and a slot never holds a clip beyond the sequence. */
export function slotClipIndex(index: number, slot: 0 | 1, count: number): number {
  if (index < 0 || index >= count) return -1
  const held = playerSlot(index) === slot ? index : index + 1
  return held < count ? held : -1
}

/** Where the next clip is parked while it waits: its first kept frame, not its file's first. */
export function preloadTime(clip: SequenceClip): number {
  return clip.parts[0]?.[0] ?? 0
}

/** What the tick does with the player on screen this frame. A player that stopped on its own - the
 *  end of the clip, a decode stall, a throttled window - is restarted or handed over, never left
 *  frozen with the sequence believing it plays. */
export type PlaybackStep = 'idle' | 'restart' | 'advance' | 'handover' | 'finish'

export function playbackStep(state: {
  wantPlay: boolean
  paused: boolean
  ended: boolean
  currentTime: number
  /** The clip's last kept source second. */
  end: number
  /** The last clip of the sequence. */
  last: boolean
}): PlaybackStep {
  if (!state.wantPlay) return 'idle'
  if (state.ended || state.currentTime >= state.end - 0.02) return state.last ? 'finish' : 'handover'
  return state.paused ? 'restart' : 'advance'
}
