// What a clip's audio tracks are, and how the preview and the export treat them.
//
// The engine reads a recording's own sidecar (`<file>.buffrx-audio.json`) and reports each track's role
// and sources (`--media-info`, src/engine/audio_layout.h). Nothing here looks at the current capture
// settings: a clip's tracks were decided when it was written. A file without a sidecar is an import: its
// tracks are generic and no track is assumed to be a mix of the others. An unlabelled multitrack import
// auditions track 1 alone using preview-only solo: a copy that lost its sidecar may hold a mix beside
// its stems, and summing every stream at unity would play that sound twice. Every track is preserved
// on export unless the person explicitly changes its gain or mute.
//
// BuffrX records track 1 as the whole mix and the tracks after it as the sources it was mixed from. When
// every source of the mix is exactly one of those tracks the mix can be rebuilt from the edited levels;
// the preview then monitors the sources alone (never the mix as well, which would double every sound),
// and the export rebuilds track 1 so an ordinary player hears the edit.

export type AudioRole = 'mix' | 'desktop' | 'microphone' | 'application' | 'combined' | 'empty' | 'track' | ''

/** One audio track of a source, as the engine reports it and the editor holds it. */
export interface AudioTrack {
  index: number
  channels: number
  title: string
  role: string
  sources: string[]
}

export interface AudioLayout {
  /** A BuffrX sidecar described this file: its roles and titles are real. */
  buffrx: boolean
  /** Track 1 is the whole mix of the tracks after it. */
  aggregate: boolean
  /** Every source of the mix is exactly one of the tracks after it. */
  complete: boolean
}

/** What the sidecar says about a file, judged the same way the engine judges it before an export. */
export function audioLayout(tracks: AudioTrack[]): AudioLayout {
  const aggregate = tracks.length > 1 && tracks[0]?.role === 'mix'
  let complete = false
  if (aggregate) {
    const mix = tracks[0].sources
    const rest = tracks.slice(1)
    complete =
      mix.length > 0 &&
      rest.every((track) => track.sources.every((source) => mix.includes(source))) &&
      // Counted, not just present: one source listed twice on one track is not a decomposition either.
      mix.every((source) => rest.reduce((n, track) => n + track.sources.filter((one) => one === source).length, 0) === 1)
  }
  return { buffrx: tracks.some((track) => track.role !== 'track' && track.role !== ''), aggregate, complete }
}

/**
 * Why a sequence's clips cannot be joined as they are, or '' when they can.
 *
 * A join writes every part's streams in order, so stream 1 means the same thing in each part: the whole
 * mix for a BuffrX recording, an independent track for an import. Two clips with the same number of
 * tracks can still disagree there, and the engine refuses that join; saying so here keeps the export
 * from starting (src/engine/audio_layout.h, join_audio_layout).
 */
export function joinSoundProblem(clips: { audio: AudioTrack[] }[]): '' | 'tracks' | 'kinds' {
  if (clips.length < 2) return ''
  if (new Set(clips.map((clip) => clip.audio.length)).size > 1) return 'tracks'
  const aggregates = clips.filter((clip) => audioLayout(clip.audio).aggregate).length
  return aggregates > 0 && aggregates < clips.length ? 'kinds' : ''
}

/** One stored overall level, clamped to the range the engine's own gains accept. */
const overallLevel = (master: number) => Math.min(4, Math.max(0, master))

/** The level each track is exported at: its own gain, its mute, then the one overall level. Solo is
 *  preview-only and never reaches a file. A derived mix's row stays at unity because the engine rebuilds
 *  it from the same sources at their gains: multiplying that row as well would apply the overall level
 *  twice. Every other track carries the overall level exactly once. */
export function exportGains(volumes: number[], muted: boolean[], tracks: number, derivedMix = false, master = 1): number[] {
  const level = overallLevel(master)
  return Array.from({ length: tracks }, (_, i) => {
    if (derivedMix && i === 0) return 1
    const gain = Math.min(4, Math.max(0, volumes[i] ?? 1))
    return muted[i] ? 0 : Math.min(4, gain * level)
  })
}

/** The level each track is heard at in the preview: its own gain, the overall level, silenced by its
 *  mute or by solo on every track it does not carry. A mute is an edit and stays the final word, so
 *  soloing a muted track does not quietly play it. Exiting solo restores every track's stored mute and
 *  level unchanged. A derived mix's own row carries the overall level too, because it is what plays
 *  before the source copies are ready. */
export function monitorGains(volumes: number[], muted: boolean[], solo: boolean[], tracks: number, derivedMix = false, master = 1): number[] {
  const anySolo = solo.some(Boolean)
  const level = overallLevel(master)
  return Array.from({ length: tracks }, (_, i) => {
    // A derived mix is an output, not a source to silence when one of its sources is soloed.
    if (derivedMix && i === 0) return level
    const gain = Math.min(4, Math.max(0, volumes[i] ?? 1))
    if (muted[i] || (anySolo && !solo[i])) return 0
    return Math.min(4, gain * level)
  })
}

/** True when a file has several tracks and nothing says what they are: a copy that lost its sidecar, an
 *  import, or labels that belonged to other bytes. Summing every stream at unity could play a mix and its
 *  stems together, so the first track is the one heard until another is chosen. */
export function isUnknownMultitrack(tracks: AudioTrack[]): boolean {
  return tracks.length > 1 && !audioLayout(tracks).buffrx
}

/** What a track's controls start from, on a new clip or after Reset audio. An unknown multitrack import
 *  auditions track 1 alone using preview-only solo; all tracks remain unmuted for export. */
export function audioDefaults(tracks: AudioTrack[]): { volumes: number[]; muted: boolean[]; solo: boolean[]; master: number } {
  const firstOnly = isUnknownMultitrack(tracks)
  return {
    volumes: Array.from({ length: tracks.length }, () => 1),
    muted: Array.from({ length: tracks.length }, () => false),
    solo: Array.from({ length: tracks.length }, (_, index) => firstOnly && index === 0),
    master: 1,
  }
}

/** True when a saved level, mute or overall level differs from what the file starts with. Solo only
 *  auditions the preview, including the initial solo for an unknown multitrack import. */
export function audioIsEdited(tracks: AudioTrack[], volumes: number[], muted: boolean[], master = 1): boolean {
  const defaults = audioDefaults(tracks)
  return Math.abs(master - defaults.master) > 0.005 || tracks.some((_, index) =>
    Math.abs((volumes[index] ?? 1) - defaults.volumes[index]) > 0.005 || (muted[index] ?? false) !== defaults.muted[index])
}

/** What the preview's other elements must do to represent the edit exactly once. */
export type MonitorMode =
  /** One track: the video element's own sound, through its gain. */
  | 'element'
  /** Independent tracks: the element's first plus one element per further track. */
  | 'streams'
  /** An aggregate mix with a complete decomposition: the sources alone; the mix stays silent. */
  | 'isolated'
  /** An aggregate mix that cannot be rebuilt: the mix alone, so nothing is dropped. */
  | 'mix'

export function monitorMode(layout: AudioLayout, tracks: number): MonitorMode {
  if (tracks <= 1) return 'element'
  if (layout.aggregate) return layout.complete ? 'isolated' : 'mix'
  return 'streams'
}

/** The level the video element's own (first) track is played at for a monitoring mode. An isolated mix
 *  is silent once its sources are ready; before that, and after a failed copy, the original mix is the
 *  fallback and carries the same overall level. */
export function elementGain(mode: MonitorMode, gains: number[], state: 'loading' | 'ready' | 'failed'): number {
  if (mode === 'isolated') return state === 'ready' ? 0 : (gains[0] ?? 1)
  return gains[0] ?? 1
}

/** Until every track is ready, or after one fails, do not play a partial mix beside the fallback. */
export function extraGain(gains: number[], index: number, state: 'loading' | 'ready' | 'failed'): number {
  return state === 'ready' ? (gains[index] ?? 1) : 0
}

/** Which tracks are copied out and played beside the picture. */
export function extraTracks(mode: MonitorMode, tracks: number): number[] {
  if (mode !== 'streams' && mode !== 'isolated') return []
  return Array.from({ length: Math.max(0, tracks - 1) }, (_, i) => i + 1)
}

/** The name a track is shown under: the sidecar's title without its product prefix, else what its role
 *  means. "BuffrX Desktop" is the row a person calls Desktop; the prefix names the writer, not the source.
 *  The mix is the output a person hears, so it is called Combined rather than after the writer's own
 *  "Mix" label; a file with no sidecar keeps its fallback Track names. */
export function trackName(track: AudioTrack | undefined, index: number): string {
  if (!track) return `Track ${index + 1}`
  if (track.role === 'mix') return 'Combined'
  const title = (track.title ?? '').replace(/^BuffrX\s+/i, '').trim()
  // A combined track whose only title is the writer's ordinal placeholder is the combined output, not
  // a source called "Track 3"; a real title on it still wins.
  if (track.role === 'combined' && (!title || /^Track \d+$/i.test(title))) return 'Combined'
  if (title) return title
  switch (track.role) {
    case 'desktop': return 'Desktop'
    case 'microphone': return 'Microphone'
    case 'application': return 'Application'
    case 'combined': return 'Combined'
    case 'empty': return 'Empty'
    default: return `Track ${index + 1}`
  }
}

/** True when the mix can be rebuilt, so its own row is the output and its controls mean nothing. */
export function mixIsDerived(layout: AudioLayout): boolean {
  return layout.aggregate && layout.complete
}

/** True when this file's level controls cannot reach the export. The engine refuses a gain change on
 *  any track of an aggregate mix it cannot rebuild, because the mix holds sound no track carries: the
 *  controls are locked for every track, the mix included, or the export would be refused after the edit. */
export function trackIsLocked(layout: AudioLayout): boolean {
  return layout.aggregate && !layout.complete
}

/** Why an aggregate mix cannot be levelled, shown where the lock is, or '' when it is not locked. */
export function mixLockReason(layout: AudioLayout): string {
  return trackIsLocked(layout)
    ? "Levels are locked for this file: its mix holds sound no single track carries. Record it with separate tracks to mix it here. Playing and saving still work."
    : ''
}

/**
 * One row of the mixer. `index` is the track's own ordinal in the file, never its place in this list:
 * a hidden row still owns its level, mute and solo, and the export and preview arrays are indexed by it.
 */
export interface AudioRow {
  track: AudioTrack
  index: number
  /** A normal track with its own controls; `derived` is the mix that follows its sources; `locked` is a
   *  track of an aggregate mix that cannot be rebuilt, so no level reaches the export - the mix itself is
   *  locked too, because the engine refuses a gain change on any track of such a file. */
  kind: 'source' | 'derived' | 'locked'
}

/** The tracks a file's mixer names, in the file's own order. The derived mix is kept as its own row so
 *  the UI can say it follows the sources; an empty role has no sound and is left out of the display,
 *  while every remaining row keeps the file's ordinal. */
export function audioRows(tracks: AudioTrack[], layout: AudioLayout): AudioRow[] {
  return tracks.flatMap((track, index): AudioRow[] => {
    if (track.role === 'empty') return []
    if (index === 0 && mixIsDerived(layout)) return [{ track, index, kind: 'derived' as const }]
    if (trackIsLocked(layout)) return [{ track, index, kind: 'locked' as const }]
    return [{ track, index, kind: 'source' as const }]
  })
}

/** The rows whose sound is actually heard, so the timeline draws each audible stream once: the sources
 *  when the mix is derived, the mix alone when it cannot be rebuilt, every track otherwise. */
export function audibleRows(tracks: AudioTrack[], layout: AudioLayout): AudioRow[] {
  const rows = audioRows(tracks, layout)
  if (trackIsLocked(layout)) return rows.filter((row) => row.index === 0)
  return rows.filter((row) => row.kind === 'source')
}

/**
 * One waveform for everything actually heard, read off the audible rows' cached peaks and weighted by
 * the gain each row is heard at. The rows are passed in already chosen by `audibleRows`, so a mix and
 * the stems it was made from are never added together. A gain of zero (a mute, or another row being
 * heard alone) leaves that row's sound out, which is what the preview plays. Returns undefined until a
 * lane has been read, so the lane says it is still reading rather than looking silent.
 */
export function heardPeaks(peaks: (number[] | undefined)[], gains: number[], indexes: number[]): number[] | undefined {
  let length = 0
  for (const index of indexes) length = Math.max(length, peaks[index]?.length ?? 0)
  if (length === 0) return undefined
  const out = new Array<number>(length).fill(0)
  for (const index of indexes) {
    const lane = peaks[index]
    if (!lane) continue
    const gain = Math.max(0, gains[index] ?? 1)
    for (let i = 0; i < lane.length; i++) out[i] += lane[i] * gain
  }
  return out
}

// ---------------------------------------------------------------- the level wheel

/** One wheel notch's scroll, in the pixels a wheel and a trackpad both report. */
const WHEEL_NOTCH = 100
/** How far one notch moves a level, in the percentage points the mixer shows. */
const LEVEL_WHEEL_STEP = 5
/** A notch is at most two steps, and no gesture banks more than two notches of scroll. */
const WHEEL_MAX_NOTCHES = 2
const WHEEL_CARRY_LIMIT = WHEEL_MAX_NOTCHES * WHEEL_NOTCH

/**
 * What one wheel event does to a level, in the percentages the fader shows. A mouse reports a notch as
 * a single event of about 100 px (or three lines, or a page), and is answered one step per notch whatever
 * its exact size; a trackpad reports a few pixels at a time, so its scroll is carried to the next event
 * instead of every event being rounded to nothing. The value comes back unchanged when no notch crossed:
 * the caller must not write a level the gesture did not move.
 */
export function wheelLevel(value: number, carried: number, deltaY: number, deltaMode = 0): { value: number; carried: number } {
  // WheelEvent.deltaMode: 0 pixels, 1 lines (a notch is about three), 2 pages.
  const pixels = deltaMode === 1 ? (deltaY * WHEEL_NOTCH) / 3 : deltaMode === 2 ? deltaY * WHEEL_NOTCH : deltaY
  const sum = Math.max(-WHEEL_CARRY_LIMIT, Math.min(WHEEL_CARRY_LIMIT, carried + pixels))
  // A whole notch in one event is not banked: 120 px and 100 px are both one turn of the wheel.
  const whole = Math.abs(pixels) >= WHEEL_NOTCH
  const notches = whole
    ? Math.max(-WHEEL_MAX_NOTCHES, Math.min(WHEEL_MAX_NOTCHES, Math.round(pixels / WHEEL_NOTCH)))
    : Math.trunc(sum / WHEEL_NOTCH)
  const next = Math.min(200, Math.max(0, value - notches * LEVEL_WHEEL_STEP))
  // A notch turned against the end of the fader is spent, not banked: the first notch back must answer.
  const rest = whole || (next === value && notches !== 0) ? 0 : sum - notches * WHEEL_NOTCH
  return { value: next, carried: rest }
}

/** The part of an HTMLAudioElement a readiness wait needs, so the wait is tested without a browser. */
export interface PlayableSource {
  /** HAVE_NOTHING 0, HAVE_METADATA 1, HAVE_CURRENT_DATA 2, HAVE_FUTURE_DATA 3. */
  readyState: number
  error: { message?: string } | null
  addEventListener(type: 'canplay' | 'error', listener: () => void): void
  removeEventListener(type: 'canplay' | 'error', listener: () => void): void
}

/** Resolves once a copied track can play, and rejects when the element reports an error, the bounded
 *  wait runs out, or `signal` is aborted. Assigning a src is not a track that plays: a copy of an
 *  unsupported codec used to be counted as ready at once, so the preview showed every track ready and
 *  played silence instead. Cancelling ends the wait at once and takes its listeners and timer with it,
 *  so leaving a clip mid-read leaves nothing holding the element. */
export function whenPlayable(source: PlayableSource, timeoutMs = 12000, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    let settled = false
    const finish = (failure?: string) => {
      if (settled) return
      settled = true
      clearTimeout(timer)
      source.removeEventListener('canplay', ready)
      source.removeEventListener('error', failed)
      signal?.removeEventListener('abort', cancelled)
      if (failure) reject(new Error(failure))
      else resolve()
    }
    const ready = () => finish()
    const failed = () => finish(source.error?.message || 'the track could not be played')
    const cancelled = () => finish('the readiness wait was cancelled')
    const timer = setTimeout(() => finish('the track did not become playable in time'), timeoutMs)
    source.addEventListener('canplay', ready)
    source.addEventListener('error', failed)
    signal?.addEventListener('abort', cancelled)
    // A copy the browser already buffered sends no `canplay` after this listener was added.
    if (signal?.aborted) cancelled()
    else if (source.error) failed()
    else if (source.readyState >= 3) finish()
  })
}
