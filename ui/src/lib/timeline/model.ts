// The Timeline preview's page model: the pure parts of scrubbing and seeking, so the page's rules are
// testable without a browser. Times are milliseconds on the session's own clock, the numbers
// `timeline.get` reports.

/** One finished segment of a Timeline session (`timeline.get.chunks[]`). */
export interface TimelineChunk {
  file: string
  startMs: number
  endMs: number
  published: boolean
  epoch: number
}

/** The session the preview plays (`timeline.get` for the selected session). */
export interface TimelineSession {
  id: string
  title: string
  availableStartMs: number
  availableEndMs: number
  chunks: TimelineChunk[]
}

/**
 * The Timeline session a launch path belongs to: `timeline.preview` hands the preview the session's media
 * folder (`<data root>\sessions\<id>\media`), so the id is the component after `sessions`. Empty for an
 * ordinary clip, which is every other path.
 */
export function sessionIdOf(path: string): string {
  const parts = path.split(/[\\/]/)
  const at = parts.findIndex((part) => part.toLowerCase() === 'sessions')
  return at >= 0 && at + 1 < parts.length ? parts[at + 1] : ''
}

/** One row of the preview's session list (`timeline.get.sessions[]`). */
export interface TimelineSessionSummary {
  id: string
  title: string
  startedAt: number
  endedAt: number
  bytes: number
  publishedChunks: number
}

/** A point the writer marked in a session (`session.get.markers[]`, moved onto the session clock). */
export interface TimelineMarker {
  atMs: number
  kind: string
  label: string
}

/** What Save copies: two moments on the session's own clock, IN before OUT. */
export interface Selection {
  inMs: number
  outMs: number
}

/** A completed save may clear only the unchanged selection in the session where it started. */
export function clipSaveDisposition(
  succeeded: boolean, started: Selection | undefined, current: Selection | undefined,
  startedSession: number, currentSession: number,
): 'failed' | 'stale' | 'clear' | 'keep' {
  if (!succeeded) return 'failed'
  if (startedSession !== currentSession) return 'stale'
  return started && started === current ? 'clear' : 'keep'
}

/** A position after scrubbing, never outside the footage that exists. */
export function scrubBy(currentMs: number, deltaSeconds: number, lo: number, hi: number): number {
  const next = currentMs + deltaSeconds * 1000
  return Math.min(Math.max(next, lo), hi)
}

/**
 * One wheel notch is 100 pixels (`deltaY`), and a notch moves the picture against the scroll: rolling
 * down (`+100`) walks back a second, rolling up walks forward.
 */
export function wheelSeconds(deltaY: number, shift = false): number {
  return deltaY === 0 ? 0 : (-deltaY / 100) * (shift ? 10 : 1)
}

/**
 * The selected range, kept inside the footage that exists. A range has no negative length, so IN that
 * has moved past OUT becomes OUT: the two ends swap and the selection is what lies between them.
 */
export function selectionClamp(inMs: number, outMs: number, lo: number, hi: number): Selection {
  const a = Math.min(Math.max(inMs, lo), hi)
  const b = Math.min(Math.max(outMs, lo), hi)
  return a <= b ? { inMs: a, outMs: b } : { inMs: b, outMs: a }
}

/**
 * What Esc does: a narrowed range goes back to the whole session first, and only a second press closes the
 * window. Closing on the first press would throw away work the person is in the middle of.
 */
export function escapeAction(hasRange: boolean): 'clear' | 'close' {
  return hasRange ? 'clear' : 'close'
}

/**
 * The range a drag across the film selects: its two ends in order, clamped to the footage. A drag that
 * ends before it started selects the same range, so a leftwards sweep works like a rightwards one.
 */
export function trimRange(anchorMs: number, pointerMs: number, lo: number, hi: number): Selection {
  return selectionClamp(Math.min(anchorMs, pointerMs), Math.max(anchorMs, pointerMs), lo, hi)
}

/** Move both ends together, stopping at the available footage without changing the duration. */
export function moveSelection(selection: Selection, deltaMs: number, lo: number, hi: number): Selection {
  const current = selectionClamp(selection.inMs, selection.outMs, lo, hi)
  const delta = Math.max(lo - current.inMs, Math.min(deltaMs, hi - current.outMs))
  return { inMs: current.inMs + delta, outMs: current.outMs + delta }
}

/**
 * Whether the pointer has travelled far enough to mean a range rather than a click. Judged in pixels,
 * not milliseconds, so a still press on a long session is still a seek.
 */
export function dragMoved(anchorMs: number, pointerMs: number, spanMs: number, widthPx: number): boolean {
  if (spanMs <= 0 || widthPx <= 0) return false
  return Math.abs(((pointerMs - anchorMs) / spanMs) * widthPx) >= 4
}

/**
 * The marker `[` and `]` step to: the first marker after `ms` when `direction` is forward, the last
 * one before it when it is backward. A marker exactly on the playhead is stepped over, so a second
 * press keeps walking; no marker in that direction is `undefined` and the picture stays.
 */
export function markerAtOrAfter(markers: TimelineMarker[], ms: number, direction: number): TimelineMarker | undefined {
  const ordered = [...markers].sort((a, b) => a.atMs - b.atMs)
  if (direction >= 0) return ordered.find((marker) => marker.atMs > ms)
  for (let i = ordered.length - 1; i >= 0; i--) if (ordered[i].atMs < ms) return ordered[i]
  return undefined
}

/**
 * Why this range cannot be saved, or '' when it can. Checked before `timeline.save` is sent: the
 * newest chunk's writer still holds its file, so a range crossing the published edge is refused here
 * with the moment the footage stops, instead of after a command that would wait ten seconds to say so.
 */
export function saveRefusal(selection: Selection, availableStartMs: number, availableEndMs: number): string {
  if (availableEndMs <= availableStartMs) return 'There is no Timeline footage to save yet.'
  if (selection.outMs > availableEndMs) {
    return `The newest moment is not saved yet: the footage ends at ${clock(availableEndMs)}.`
  }
  if (selection.inMs < availableStartMs) return 'The range starts before the footage that is kept.'
  if (selection.outMs <= selection.inMs) return 'The selected range is empty.'
  return ''
}

/**
 * A position on the session's clock: "1:04" in the minutes and seconds the rest of BuffrX uses,
 * "1:02:03" once an hour is on the clock, and "0:04.2" under ten seconds so a short step is visible.
 * One function, so the playhead, the IN/OUT readout, the markers and the filmstrip never disagree.
 */
/**
 * Distance from the live edge, as a ruler reads it: "-1:30", "NOW". A sentence per label ("1 minute 30
 * sec ago") said the same thing seven times across one ruler and buried the marks it was labelling.
 */
export function relativeTime(ms: number): string {
  const seconds = Math.max(0, Math.round(ms / 1000))
  if (!seconds) return 'NOW'
  const hours = Math.floor(seconds / 3600)
  const minutes = Math.floor((seconds % 3600) / 60)
  const rest = String(seconds % 60).padStart(2, '0')
  const base = hours > 0 ? `${hours}:${String(minutes).padStart(2, '0')}:${rest}` : `${minutes}:${rest}`
  return `-${base}`
}

export function clock(ms: number): string {
  const tenths = Math.round(Math.max(0, ms) / 100)
  const whole = Math.floor(tenths / 10)
  const hours = Math.floor(whole / 3600)
  const minutes = Math.floor((whole % 3600) / 60)
  const seconds = String(whole % 60).padStart(2, '0')
  const base = hours > 0 ? `${hours}:${String(minutes).padStart(2, '0')}:${seconds}` : `${minutes}:${seconds}`
  return tenths < 100 ? `${base}.${tenths % 10}` : base
}

/** A distance in words a person reads at a glance: "12 SEC", "7 MIN 51 SEC", "1 HR 2 MIN 3 SEC"; a zero part is left out. */
export function spokenSpan(ms: number): string {
  const whole = Math.round(Math.max(0, ms) / 1000)
  const hours = Math.floor(whole / 3600)
  const minutes = Math.floor((whole % 3600) / 60)
  const seconds = whole % 60
  const parts = [hours ? `${hours} HR` : '', minutes ? `${minutes} MIN` : '', seconds || !whole ? `${seconds} SEC` : '']
  return parts.filter(Boolean).join(' ')
}

/**
 * How long ago the picture was, said the way a person would: "NOW" at the live edge, else "7 MIN 51 SEC AGO".
 * "7:51 AGO" read as a clock time, and "BEFORE NOW" as a place rather than a distance.
 */
export function agoText(behindMs: number): string {
  return behindMs <= 1000 ? 'NOW' : `${spokenSpan(behindMs)} AGO`
}

/** A distance in whole seconds as the ruler words it: "0:07", "1:05", "1:02:00". */
function wholeClock(ms: number): string {
  const whole = Math.round(Math.max(0, ms) / 1000)
  const hours = Math.floor(whole / 3600)
  const minutes = Math.floor((whole % 3600) / 60)
  const seconds = String(whole % 60).padStart(2, '0')
  return hours > 0 ? `${hours}:${String(minutes).padStart(2, '0')}:${seconds}` : `${minutes}:${seconds}`
}

/**
 * How far before now a moment is, in the ruler's few characters: "NOW" at the live edge, else "-1:07". No unit
 * word: the ruler's own caption says what the numbers are, and "AGO" on every tick was most of its ink.
 */
export function rulerAge(nowMs: number, startedAtSeconds: number, atMs: number, availableEndMs: number): string {
  const behind = startedAtSeconds > 0
    ? Math.max(0, nowMs - (startedAtSeconds * 1000 + atMs))
    : Math.max(0, availableEndMs - atMs)
  return behind <= 1000 ? 'NOW' : `-${wholeClock(behind)}`
}

/** How many equal parts the ruler splits one labelled step into for its small ticks: 1 draws none. */
export function rulerMinors(stepMs: number): number {
  const seconds = Math.round(stepMs / 1000)
  switch (seconds) {
    case 2: return 2
    case 5: return 5
    case 10: return 5
    case 15: return 3
    case 30: return 3
    case 60: return 6
    case 120: return 4
    case 300: return 5
    case 600: return 5
    case 900: return 3
    case 1800: return 3
    case 3600: return 6
    default: return 1
  }
}

/** Age against the wall clock, including while the writer is stopped; no unknown timestamp becomes NOW. */
export function footageAge(nowMs: number, startedAtSeconds: number, atMs: number, availableEndMs: number): string {
  return startedAtSeconds > 0 ? agoText(Math.max(0, nowMs - (startedAtSeconds * 1000 + atMs)))
    : `${spokenSpan(Math.max(0, availableEndMs - atMs))} BEFORE END`
}

/** Session time and retained span use different origins after older footage expires. */
export function timelineReadout(atMs: number, startMs: number, endMs: number) {
  return { position: clock(atMs), end: clock(endMs), available: clock(endMs - startMs) }
}

/**
 * The selected range after the footage moved under it, and whether that moved it. Retention slides
 * `availableStartMs` forward and the oldest footage goes with it, so a range reaching into what has
 * gone is cut to what is still here; the page says so instead of saving a moment that no longer exists.
 */
export function selectionOnSpan(selection: Selection, lo: number, hi: number): { selection: Selection; trimmed: boolean } {
  const next = selectionClamp(selection.inMs, selection.outMs, lo, hi)
  return { selection: next, trimmed: next.inMs !== selection.inMs || next.outMs !== selection.outMs }
}

/**
 * What to say when the footage's start moved forward between two polls: the older footage expired and
 * the strip no longer shows it. Empty when the start did not move, so the page can call it on every
 * poll without a word to spare.
 */
export function retentionNotice(previousStartMs: number, startMs: number): string {
  if (startMs <= previousStartMs) return ''
  return `Older footage expired: the timeline now starts at ${clock(startMs)}.`
}

/**
 * The session the page shows: the one it was opened on, or - when that one has gone (expired, erased,
 * a new game's session replaced it) - the newest one, so the preview falls back instead of dying.
 * `fellBack` says the page must explain the swap.
 */
export function pickSession(sessions: TimelineSessionSummary[], wantedId: string): { id: string; fellBack: boolean } {
  if (sessions.length === 0) return { id: '', fellBack: false }
  if (sessions.some((entry) => entry.id === wantedId)) return { id: wantedId, fellBack: false }
  let newest = sessions[0]
  for (const entry of sessions) if (entry.startedAt > newest.startedAt) newest = entry
  return { id: newest.id, fellBack: wantedId !== '' }
}

/**
 * The chunk a moment belongs to: the last chunk that starts at or before it, so a seek lands in the
 * segment that actually covers it. Before the first chunk (or with none) the first chunk answers,
 * which is where a preview clamps; -1 when there is no footage at all.
 */
export function chunkIndexForSeek(chunks: TimelineChunk[], targetMs: number): number {
  if (chunks.length === 0) return -1
  let found = 0
  for (let i = 0; i < chunks.length; i++) {
    if (chunks[i].startMs > targetMs) break
    found = i
  }
  return found
}

/**
 * Whether a moment has footage behind it. The end of a chunk counts: it is the live edge, and a page
 * that treated it as a hole refused to play the newest second it had.
 */
export function isRecorded(chunks: TimelineChunk[], targetMs: number): boolean {
  for (const chunk of chunks) if (targetMs >= chunk.startMs && targetMs <= chunk.endMs) return true
  return false
}

/**
 * The nearest edge of recorded content in one direction, or null: forward is the next chunk's start,
 * backward is the last recorded millisecond before the target (never its exclusive end, which no chunk
 * covers). A hole is skipped, never played.
 */
export function recordedEdge(chunks: TimelineChunk[], targetMs: number, direction: -1 | 1): number | null {
  if (chunks.length === 0) return null
  if (direction > 0) {
    for (const chunk of chunks) if (chunk.startMs > targetMs) return chunk.startMs
    return null
  }
  let found: number | null = null
  for (const chunk of chunks) {
    if (chunk.endMs <= targetMs && (found === null || chunk.endMs > found)) found = chunk.endMs
  }
  return found === null ? null : found - 1
}

/** Resolve an empty moment to footage. Directional gestures cross the gap in their direction. */
export function seekableMoment(chunks: TimelineChunk[], targetMs: number, direction: -1 | 0 | 1): number | null {
  const published = chunks.filter((chunk) => chunk.published)
  if (isRecorded(published, targetMs)) return targetMs
  const before = recordedEdge(published, targetMs, -1)
  const after = recordedEdge(published, targetMs, 1)
  if (direction < 0) return before ?? after
  if (direction > 0) return after ?? before
  if (before === null) return after
  if (after === null) return before
  return targetMs - (before + 1) < after - targetMs ? before : after
}

/**
 * The next footage after a moment: the start of the earliest published chunk that begins ahead of it,
 * or null when there is none. Crossing a gap is a seek to this.
 */
export function nextPlayable(chunks: TimelineChunk[], atMs: number): number | null {
  let next: number | null = null
  for (const chunk of chunks) {
    if (!chunk.published || chunk.startMs <= atMs) continue
    if (next === null || chunk.startMs < next) next = chunk.startMs
  }
  return next
}

/** The first run after a real gap, or null when the next chunk continues this run. */
export function gapSuccessor(chunks: TimelineChunk[], atMs: number, gapMs: number): number | null {
  let next: number | null = null
  for (const chunk of chunks) {
    if (!chunk.published || chunk.endMs <= atMs) continue
    if (chunk.startMs <= atMs + gapMs) return null
    if (next === null || chunk.startMs < next) next = chunk.startMs
  }
  return next
}

/** The label the engine writes when recording resumed because a game came back (`kTimelineGamePausedLabel`). */
const GAME_PAUSED = 'Game paused'

/**
 * What a gap says: the game was not running, or the writer was not writing. A `gap` marker labelled
 * "Game paused" anywhere within two seconds of either end of the hole is the writer naming the game as
 * the reason; a sleep gap carries its own label and must not borrow this one. The engine stamps the
 * marker when recording RESUMES, so it sits at the hole's end: judging it against the start alone left
 * every hole longer than two seconds reading NO FOOTAGE.
 */
export function gapLabel(markers: TimelineMarker[], gap: { startMs: number; endMs: number }): string {
  const from = gap.startMs - 2000
  const to = gap.endMs + 2000
  return markers.some((marker) =>
    marker.kind === 'gap' && marker.label === GAME_PAUSED && marker.atMs >= from && marker.atMs <= to)
    ? 'NO GAME' : 'NO FOOTAGE'
}

/**
 * Where `[` or `]` lands: the first marker or the edge of the footage, whichever comes first. A marker
 * a long way off must not hide the edge just past the playhead, which is why both are compared.
 */
export function stepTarget(
  chunks: TimelineChunk[],
  markersAt: number[],
  fromMs: number,
  direction: -1 | 1,
): number | null {
  let best: number | null = null
  const consider = (candidate: number | null) => {
    if (candidate === null) return
    if (direction > 0 ? candidate <= fromMs : candidate >= fromMs) return
    if (best === null || Math.abs(candidate - fromMs) < Math.abs(best - fromMs)) best = candidate
  }
  consider(recordedEdge(chunks, fromMs, direction))
  for (const at of markersAt) consider(at)
  return best
}

/** How long a clip starts out when Clip is pressed: Steam's habit, and long enough for most moments. */
export const CLIP_DEFAULT_MS = 30000

/**
 * The range Clip opens with: `lengthMs` ending at the playhead, the moment just watched. Near the start
 * of the footage it runs forward from the start instead, and it is never longer than the footage.
 */
export function clipAround(playheadMs: number, lo: number, hi: number, lengthMs = CLIP_DEFAULT_MS): Selection {
  if (hi <= lo) return { inMs: lo, outMs: lo }
  const length = Math.min(lengthMs, hi - lo)
  const outMs = Math.min(hi, Math.max(lo + length, playheadMs))
  return { inMs: outMs - length, outMs }
}

/** The clip's length, which is what the band says: the bar already shows where it sits. */
export function clipLabel(selection: Selection): string {
  return `CLIP ${clock(selection.outMs - selection.inMs)}`
}

/**
 * I and O: the clip's start or end set to the playhead, which is how a clip is cut while watching.
 * With no clip, the other end follows at the usual length. With one, the far end stays where it was,
 * unless the playhead passes it: then the clip keeps its length and moves, instead of turning empty.
 */
export function markEdge(selection: Selection | undefined, which: 'in' | 'out', atMs: number, lo: number, hi: number, lengthMs = CLIP_DEFAULT_MS): Selection {
  const at = Math.min(Math.max(atMs, lo), hi)
  const length = selection ? Math.max(1, selection.outMs - selection.inMs) : Math.min(lengthMs, Math.max(0, hi - lo))
  let next: Selection
  if (which === 'in') {
    next = !selection || at >= selection.outMs ? { inMs: at, outMs: at + length } : { inMs: at, outMs: selection.outMs }
    if (next.outMs > hi) next = { inMs: Math.max(lo, hi - length), outMs: hi }
  } else {
    next = !selection || at <= selection.inMs ? { inMs: at - length, outMs: at } : { inMs: selection.inMs, outMs: at }
    if (next.inMs < lo) next = { inMs: lo, outMs: Math.min(hi, lo + length) }
  }
  return selectionClamp(next.inMs, next.outMs, lo, hi)
}

/**
 * A dragged clip end sticks to a marker or the old playhead when it comes within `px` pixels of one, so
 * a cut lands on the moment that was marked without a steady hand. Judged in pixels, like dragMoved.
 */
export function snapMs(ms: number, targets: number[], spanMs: number, widthPx: number, px = 8): number {
  if (spanMs <= 0 || widthPx <= 0) return ms
  const reach = (px / widthPx) * spanMs
  let best = ms
  let gap = reach
  for (const target of targets) {
    const distance = Math.abs(target - ms)
    if (distance <= gap) {
      best = target
      gap = distance
    }
  }
  return best
}

/**
 * Where a playing clip goes next: playing through its end wraps to its start, so Space previews exactly
 * what Save keeps. A delayed playback event must still wrap after the end.
 */
export function clipLoopTarget(selection: Selection | undefined, atMs: number, playing: boolean): number | undefined {
  if (!selection || !playing || selection.outMs <= selection.inMs) return undefined
  return atMs >= selection.outMs ? selection.inMs : undefined
}

/** The stretch of the footage the bar shows: all of it, or a zoomed part. */
export interface View {
  fromMs: number
  toMs: number
}

/** The narrowest view: five seconds across the bar, which is frame-exact on any screen. */
export const VIEW_MIN_MS = 5000

/** A view kept inside the footage without changing its length (unless the footage is shorter). */
export function clampView(view: View, lo: number, hi: number): View {
  const span = Math.min(Math.max(view.toMs - view.fromMs, Math.min(VIEW_MIN_MS, hi - lo)), hi - lo)
  const from = Math.min(Math.max(view.fromMs, lo), hi - span)
  return { fromMs: from, toMs: from + span }
}

/**
 * The view that shows a whole clip, or undefined when `view` already does (both ends clear of its edges).
 * The current scale is kept when the clip fits in it; a clip wider than the view widens it. A short clip
 * is centred with some context either side, so its ends are never cut off at a border.
 */
export function viewHoldingClip(view: View, selection: Selection, lo: number, hi: number): View | undefined {
  const span = view.toMs - view.fromMs
  const margin = span * 0.01
  if (selection.inMs >= view.fromMs + margin && selection.outMs <= view.toMs - margin) return undefined
  const length = Math.max(1, selection.outMs - selection.inMs)
  const context = Math.max(500, length * 0.25)
  const wanted = Math.max(span, length + context * 2)
  const middle = (selection.inMs + selection.outMs) / 2
  return clampView({ fromMs: middle - wanted / 2, toMs: middle + wanted / 2 }, lo, hi)
}

/** The stretch `progress` (0..1) of the way from one view to another, easing out: the zoom glide's frame. */
export function glideView(from: View, to: View, progress: number): View {
  const eased = 1 - (1 - Math.min(1, Math.max(0, progress))) ** 3
  return {
    fromMs: from.fromMs + (to.fromMs - from.fromMs) * eased,
    toMs: from.toMs + (to.toMs - from.toMs) * eased,
  }
}

/** How far a loudness column has risen (0..1) when the whole profile is `progress` along: a wave from the left. */
export function loudRise(progress: number, column: number, columns: number): number {
  const wave = Math.min(1, Math.max(0, progress * 1.6 - (column / Math.max(1, columns)) * 0.6))
  return 1 - (1 - wave) ** 3
}

/**
 * Zooms by `factor` (above 1 narrows) around `anchorMs`, which stays under the same point of the bar -
 * the pointer for a wheel, the playhead for a key - so the eye never loses the moment it was on.
 */
export function zoomView(view: View, factor: number, anchorMs: number, lo: number, hi: number): View {
  const span = view.toMs - view.fromMs
  if (span <= 0 || hi <= lo) return { fromMs: lo, toMs: hi }
  const next = Math.min(hi - lo, Math.max(Math.min(VIEW_MIN_MS, hi - lo), span / factor))
  const fraction = Math.min(1, Math.max(0, (anchorMs - view.fromMs) / span))
  return clampView({ fromMs: anchorMs - fraction * next, toMs: anchorMs - fraction * next + next }, lo, hi)
}

/**
 * The stretch the bar draws: an explicit zoom, else the review view frozen when the owner left live,
 * else the fallback that grows with the footage. Once the owner pauses, scrubs or picks a clip, new
 * footage must not move the scale under them.
 */
export function viewFor(zoom: View | null | undefined, frozen: View | null | undefined, fallback: View): View {
  return zoom ?? frozen ?? fallback
}

/**
 * The moment the ruler counts back from: the newest footage while live, the right edge of the view
 * otherwise. Measured from the live edge, a frozen bar relabelled itself as the writer grew.
 */
export function rulerBase(live: boolean, visualEdge: number, view: View): number {
  return live ? visualEdge : view.toMs
}

/**
 * Whether a moment asked for is the newest footage: within `toleranceMs` of the live edge, or past it.
 * The edge arrives in fragments, so a drag that reaches the right of the bar lands a hair short of it
 * and used to park there, paused, while the writer kept growing the bar. Asking the edge means Live.
 */
export function nearLiveEdge(targetMs: number, edgeMs: number, toleranceMs = 1000): boolean {
  return targetMs >= edgeMs - toleranceMs
}

/**
 * Whether a movement asks for the newest footage: it lands on the live edge and it came from behind it.
 * A head that already sits on the edge and steps a frame is stepping, not asking to play Live, so the
 * gesture's own starting moment decides - the position it was at, never the one it passed through.
 */
export function liveSnap(fromMs: number, targetMs: number, edgeMs: number, toleranceMs = 1000): boolean {
  return nearLiveEdge(targetMs, edgeMs, toleranceMs) && !nearLiveEdge(fromMs, edgeMs, toleranceMs)
}

/**
 * The right end of a frozen review bar: the end it was drawn to, extended to the moment the picture is
 * already on. Live plays the writer's open chunk, so pausing can leave the picture past the published
 * edge the bar ended at - the bar must hold it, or the playhead has nowhere to be drawn. It never
 * shrinks: what the owner is reviewing may not pull back under them.
 */
export function frozenBarEnd(view: View, wantMs: number): number {
  return Math.max(view.toMs, wantMs)
}

/** How much footage the bar shows by default: the newest five minutes, the way Steam's timeline opens. */
export const WINDOW_DEFAULT_MS = 5 * 60_000

/** The lengths the window button steps through; Infinity is the whole session. */
export const WINDOW_STEPS_MS = [60_000, 5 * 60_000, 15 * 60_000, 60 * 60_000, Infinity]

/** The bar with no zoom and no held review: the newest `windowMs` of footage, or all of it when that is shorter. */
export function defaultView(lo: number, edge: number, windowMs: number): View {
  const to = Math.max(lo + 1, edge)
  return { fromMs: Math.max(lo, to - windowMs), toMs: to }
}

/** The window length a view stands for: Infinity when it holds all the footage, else its own span. */
export function windowOf(view: View, lo: number, hi: number): number {
  const span = view.toMs - view.fromMs
  return span >= hi - lo - 1 ? Infinity : span
}

/** The next length on the window button: the first step wider than the current one, else back to the shortest. */
export function nextWindow(currentMs: number): number {
  return WINDOW_STEPS_MS.find((step) => step > currentMs * 1.01) ?? WINDOW_STEPS_MS[0]
}

/** The window button's text: ALL, or the span in minutes (seconds under two minutes). */
export function windowLabel(spanMs: number, all: boolean): string {
  if (all) return 'ALL'
  const seconds = Math.round(spanMs / 1000)
  return seconds < 120 ? `${seconds} S` : `${Math.round(seconds / 60)} MIN`
}

/** Whether the view shows less than the whole footage. */
export function isZoomed(view: View, lo: number, hi: number): boolean {
  return view.fromMs > lo + 1 || view.toMs < hi - 1
}

/**
 * The view that keeps the playhead on screen while it plays: unchanged while it is inside, else moved
 * so the playhead sits a tenth in from the edge it is heading for.
 */
export function followView(view: View, headMs: number, lo: number, hi: number): View {
  if (headMs >= view.fromMs && headMs <= view.toMs) return view
  const span = view.toMs - view.fromMs
  const from = headMs > view.toMs ? headMs - span * 0.9 : headMs - span * 0.1
  return clampView({ fromMs: from, toMs: from + span }, lo, hi)
}

const RULER_STEPS_MS = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600].map((s) => s * 1000)

/** The ruler's spacing: the shortest round step that leaves at least `minPx` between two labels. */
export function rulerStep(spanMs: number, widthPx: number, minPx = 90): number {
  if (spanMs <= 0 || widthPx <= 0) return RULER_STEPS_MS[RULER_STEPS_MS.length - 1]
  for (const step of RULER_STEPS_MS) if ((step / spanMs) * widthPx >= minPx) return step
  return RULER_STEPS_MS[RULER_STEPS_MS.length - 1]
}

/**
 * How a session reads in the chooser: the day and the clock it began, then the game it was named after.
 * `startedAt` and `nowSeconds` are Unix seconds; the day is the viewer's own.
 */
export function sessionLabel(summary: Pick<TimelineSessionSummary, 'startedAt' | 'title'>, nowSeconds: number): string {
  if (!summary.startedAt) return summary.title || 'Session'
  const start = new Date(summary.startedAt * 1000)
  const now = new Date(nowSeconds * 1000)
  const dayOf = (d: Date) => new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime()
  const days = Math.round((dayOf(now) - dayOf(start)) / 86400000)
  const day = days === 0 ? 'Today' : days === 1 ? 'Yesterday' : start.toLocaleDateString('en-GB', { day: 'numeric', month: 'short' })
  const time = `${String(start.getHours()).padStart(2, '0')}:${String(start.getMinutes()).padStart(2, '0')}`
  return `${day} ${time} · ${summary.title || 'Desktop'}`
}

/** One chunk's sound as the engine reports it: a level byte (0..255) per bucket, from `startMs` on the session clock. */
export interface LoudChunk {
  startMs: number
  levels: number[]
}

/**
 * The level that counts as "as loud as it gets" for this session: the 95th percentile of the sounding seconds, never
 * below 32. A game's one explosion cannot flatten everything else, and a quiet session still shows its shape.
 */
export function loudScale(chunks: LoudChunk[]): number {
  const sounding: number[] = []
  for (const chunk of chunks) for (const level of chunk.levels) if (level > 0) sounding.push(level)
  if (sounding.length === 0) return 255
  sounding.sort((a, b) => a - b)
  return Math.max(32, sounding[Math.min(sounding.length - 1, Math.floor(sounding.length * 0.95))])
}

/**
 * The bar's loudness profile: `columns` values (0..1) across [fromMs, toMs), each the loudest bucket that falls in it,
 * squared so the quiet fades and the loud moments stand out. A column with no footage is 0.
 */
export function loudColumns(
  chunks: LoudChunk[],
  fromMs: number,
  toMs: number,
  columns: number,
  bucketMs: number,
  scale: number,
): number[] {
  const out = new Array<number>(Math.max(0, columns)).fill(0)
  const span = toMs - fromMs
  if (columns <= 0 || span <= 0 || bucketMs <= 0 || scale <= 0) return out
  for (const chunk of chunks) {
    for (let i = 0; i < chunk.levels.length; i++) {
      const start = chunk.startMs + i * bucketMs
      const end = start + bucketMs
      if (end <= fromMs || start >= toMs) continue
      const value = Math.min(1, chunk.levels[i] / scale) ** 2
      if (value <= 0) continue
      const first = Math.max(0, Math.floor(((start - fromMs) / span) * columns))
      const last = Math.min(columns - 1, Math.ceil(((end - fromMs) / span) * columns) - 1)
      for (let column = first; column <= last; column++) out[column] = Math.max(out[column], value)
    }
  }
  return out
}

/** Pick a few distinct loud regions for the accent; the full profile remains visible underneath. */
export function loudHighlights(profile: number[], limit = 8, minGap = 12): number[] {
  const candidates: { at: number; level: number }[] = []
  let best = -1
  for (let i = 0; i <= profile.length; i++) {
    const level = profile[i] ?? 0
    if (level >= 0.8) {
      if (best < 0 || level > profile[best]) best = i
    } else if (best >= 0) {
      candidates.push({ at: best, level: profile[best] })
      best = -1
    }
  }
  candidates.sort((a, b) => b.level - a.level || a.at - b.at)
  const picked: number[] = []
  for (const candidate of candidates) {
    if (picked.length >= limit) break
    if (picked.every((at) => Math.abs(at - candidate.at) >= minGap)) picked.push(candidate.at)
  }
  return picked.sort((a, b) => a - b)
}

/** What the transport button should show: the picture, not a stale flag. */
export type TransportState = 'paused' | 'waiting' | 'playing'

export function transportState(input: { paused: boolean; waiting?: boolean; seeking?: boolean }): TransportState {
  if (input.paused) return 'paused'
  if (input.waiting || input.seeking) return 'waiting'
  return 'playing'
}
