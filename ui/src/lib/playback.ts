// The two players that hand a sequence from one clip to the next: which slot holds the clip on
// screen, which holds the one after it, and where the next one is parked before it is needed.
// Pure, so the handoff's arithmetic is tested without a browser.

import { clipStart, type SequenceClip } from './sequence.ts'
import type { Part } from './segments.ts'

/** How far a playing player may drift from the sequence before it is moved back. */
export const DRIFT_SECONDS = 0.25
/** The gap an exact placement moves for. A player's time is kept to the microsecond, so only float noise is inside it: a 1 ms
 *  head or a 240 fps frame (4.2 ms) is a different place and is moved to. */
export const PLACE_SECONDS = 0.000001
/** A seek lands on the frame a time falls in. A part's exclusive end is shown this far inside it, so the picture is the last
 *  kept frame at any frame rate, with no frame times guessed for a variable-rate file. */
export const END_INSET_SECONDS = 0.000001

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

/** Whether a player must move to its target. A committed edit, a seek or a paused playhead places it exactly; a playing
 *  player is moved only once it has drifted past the tolerance, so ordinary playback is never chased. */
export function needsSeek(current: number, target: number, exact: boolean): boolean {
  return Math.abs(current - target) > (exact ? PLACE_SECONDS : DRIFT_SECONDS)
}

/** The last second a part shows: its exclusive end one microsecond inside it, never before the part's own start. A seek shows
 *  the frame a time falls in, so this is the last kept frame whatever the frame rate, and the aligned and unaligned ends agree. */
export function lastShownSource(part: Part): number {
  return Math.max(part[0], part[1] - END_INSET_SECONDS)
}

/** The second a player shows for a sequence source second. Inside a kept part it is the same second, except at the part's
 *  exclusive end, which shows the last kept frame. A second in a cut moves to the next kept part, as playbackJump does, and a
 *  second past the last part shows the last kept frame.
 *  shortcut: a cut whose start falls inside a frame, or that is shorter than one, shows the frame before its start (frame times
 *  are neither read nor guessed); upgrade when the source's frame times are read. */
export function displaySource(parts: Part[], source: number): number {
  if (!parts.length) return source
  for (const part of parts) {
    const [from, to] = part
    if (source >= from && source < to) return source < to - END_INSET_SECONDS ? source : lastShownSource(part)
  }
  const next = parts.find(([from]) => from > source)
  return next ? next[0] : lastShownSource(parts[parts.length - 1])
}

/** One kept part as the Selection preview holds the player to: its place on the sequence's clock, and its own source range. */
export interface PreviewSpan {
  index: number
  part: number
  /** Where the part starts and ends on the sequence's output clock, after the clip's speed. */
  start: number
  end: number
  /** The part's own source seconds, [from, to), and the clip's speed. */
  from: number
  to: number
  speed: number
}

/** The span of one kept part on the sequence's clock: after the clips and the parts before it in its own clip, and as long
 *  as it plays at the clip's speed. Null when there is no such clip or part, so a stale selection shows nothing. */
export function partSpan(clips: SequenceClip[], index: number, part: number): PreviewSpan | null {
  const clip = clips[index]
  const range = clip?.parts[part]
  if (!clip || !range) return null
  const speed = clip.speed > 0 ? clip.speed : 1
  let before = 0
  for (let i = 0; i < part; i++) before += Math.max(0, clip.parts[i][1] - clip.parts[i][0])
  const start = clipStart(clips, index) + before / speed
  const [from, to] = range
  return { index, part, start, end: start + Math.max(0, to - from) / speed, from, to, speed }
}

/** A clock second inside a span, the end included to the microsecond: the float arithmetic of a seek and a trim can leave a hair
 *  past either end, and that is the same place, not the next one. A millisecond past the end is outside. */
export function inSpan(span: PreviewSpan, at: number): boolean {
  return at >= span.start - PLACE_SECONDS && at <= span.end + PLACE_SECONDS
}

/** Where a Selection's clock sits: the playhead while it is inside the part, the part's start otherwise. The end counts as
 *  inside, so a paused end stays on its last kept image instead of moving on to the next part. */
export function holdInSpan(span: PreviewSpan, at: number): number {
  return inSpan(span, at) ? Math.min(Math.max(at, span.start), span.end) : span.start
}

/** The source second a Selection shows at a clock second, held inside the part. */
export function spanSource(span: PreviewSpan, at: number): number {
  const offset = Math.min(Math.max(at - span.start, 0), span.end - span.start)
  return span.from + offset * span.speed
}

/** The clock second a source second sits at, held inside the part. */
export function spanAt(span: PreviewSpan, source: number): number {
  return Math.min(Math.max(span.start + (source - span.from) / span.speed, span.start), span.end)
}

/** Play from a paused end starts the selected part again; from anywhere else, play carries on from here. */
export function restartAt(span: PreviewSpan, at: number): number {
  return at >= span.end - 0.05 ? span.start : at
}

/** The transport's clock: the selected part's own time and length while a Selection is held, the sequence's otherwise. */
export function previewClock(span: PreviewSpan | null, at: number, total: number): { time: number; total: number } {
  if (!span) return { time: at, total }
  return { time: Math.min(Math.max(at - span.start, 0), span.end - span.start), total: span.end - span.start }
}