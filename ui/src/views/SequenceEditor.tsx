// The sequence editor, all of it on one screen: the clips in order on the left, the picture under the
// playhead in the middle, the clip's own edit and the export in the tabs on the right, and the timeline
// across the bottom. Playback runs from one clip into the next; the export is one job. With no clip in
// the edit the same panes stand empty, and the picture area is the drop target.

import clsx from 'clsx'
import { useCallback, useEffect, useMemo, useRef, useState, type Dispatch, type SetStateAction } from 'react'
import { Icon } from '../components/icons'
import { Button, ContextMenu, DialogBox, IconButton, Slider, Tabs } from '../components/ui'
import { engine, type MediaItem } from '../lib/api'
import { audioLayout, audibleRows, joinSoundProblem, mixIsDerived, mixLockReason, monitorGains, monitorMode } from '../lib/audio'
import { mediaUrl, onShell, shell } from '../lib/bridge'
import { trimTimelineAt } from '../lib/editor_timeline'
import { canShareSaved, shareThenLeave } from '../lib/editor-share'
import { duration, fileName, stem, timecode } from '../lib/format'
import { exportFileName } from '../lib/filename'
import { setFullscreen as setWindowFullscreen } from '../lib/window'
import { mediaWork } from '../lib/media'
import type { MediaProbe } from '../lib/media_queue'
import { displaySource, holdInSpan, inSpan, lastShownSource, needsSeek, partSpan, playbackStep, playerSlot, preloadTime, previewClock, restartAt, slotClipIndex, spanAt, spanSource } from '../lib/playback'
import { loadExportDefaults, saveExportDefaults } from '../lib/preferences'
import { moveAll, moveEdge, movePart, partAt, playbackJump, remove, split, SHORTEST, type Part } from '../lib/segments'
import {
  clipExportInput,
  clipOutputLength,
  sameCrop,
  sameGrade,
  scaleCrop,
  clipStart,
  locate,
  newClip,
  outputOffsetOfSource,
  reorder,
  sequenceLength,
  type SequenceClip,
} from '../lib/sequence'
import { attempt, toast } from '../lib/store'
import { clampView, spanOfZoom, viewAtSpan, zoomOfSpan, zoomView, type TimelineView } from '../lib/timeline_view'
import { editorAction, type EditorAction } from '../lib/editor_keys'
import {
  ASPECTS,
  CropOverlay,
  centredCrop,
  gradeFilter,
  loadDetails,
  uniquePath,
  useFitted,
  useTrackMix,
  type MediaDetails,
} from './editor-parts'
import { EmptyStage, Transport } from './editor-panels'
import { ColorTab, EditTab, ExportTab, NothingToEdit, SpeedTab, type LookPatch } from './inspector-tabs'
import { ShortcutsDialog } from './ShortcutsDialog'
import { ClipTimeline, GUTTER, SequenceLanes } from './SequenceTimeline'

/** What the panes read while the edit holds no clip: a clip with nothing in it. */
const EMPTY_CLIP: SequenceClip = newClip({ path: '', name: '', duration: 0, width: 0, height: 0, fps: 0, bytes: 0, audio: [] })

/** Said when the clips cannot be joined as they are: the engine refuses a join of unlike sound. */
const TRACK_MISMATCH = 'These clips do not have the same sound tracks. A join needs every clip to match, so remove one.'
/** Said when equal track counts still disagree on what the first stream means (a mix, or an import). */
const SOUND_KIND_MISMATCH = 'These clips hold their sound differently: one starts with a full mix and the other has only separate tracks. Export the clips separately, or join clips that hold their sound the same way.'

/** The details a player that holds no clip is given: the mix hook wants one, and this one is silent. */
const NO_DETAILS: MediaDetails = { ok: false, duration: 0, width: 0, height: 0, fps: 0, videoCodec: '', videoKbps: 0, bytes: 0, modified: 0, audio: [] }
const NO_VOLUMES: number[] = []
/** What the timeline's wave reads before any file has been read. */
const NO_PROBE: MediaProbe = {}

/** The engine's even slices of a clip's sound, as the waveform and the trim lane read them. */
const PEAKS = 800

/** What a drop area says while a clip is over it or on its way: the whole area answers, not only its frame. */
function DropNotice() {
  return (
    <div className="drop-zone bx-fade-in pointer-events-none absolute inset-0 z-10 grid place-items-center border-2 border-dashed border-accent bg-canvas/85 transition-colors">
      <span className="flex items-center gap-2 bg-accent px-3 py-1.5 text-[14px] font-extrabold text-on-accent">
        <Icon name="plus" size={16} />
        Drop to add
      </span>
    </div>
  )
}

/** What `media-done` reports when an export ends (src-tauri/src/media.rs). */
interface ExportResult {
  id: number
  ok: boolean
  output: string
  error: string
  encoder: string
}

export function SequenceEditor({ clips, setClips, hover, dragging, suspended, clipsDir, focusClip, sourceProblem, onBrowse, onForget, onExportingChange, onShared }: {
  clips: SequenceClip[]
  setClips: Dispatch<SetStateAction<SequenceClip[]>>
  /** A file is over the window. */
  hover: boolean
  /** A library row is being dragged: the stage and the timeline answer it. */
  dragging: boolean
  /** The screenshot editor is in front: this one stays mounted (an export in flight reports to it) but is quiet. */
  suspended: boolean
  clipsDir: string
  focusClip: { id: string; serial: number } | null
  sourceProblem: string
  /** The native picker, for files outside the BuffrX library. */
  onBrowse: () => void
  /** A clip left the sequence: the window may add the same file again. */
  onForget: (id: string) => void
  /** True while an export is writing: closing the window now stops it and deletes its partial file. */
  onExportingChange?: (busy: boolean) => void
  /** The panel's Share sheet is open on this file: leave the editor for it. */
  onShared?: () => void
}) {
  const [playhead, setPlayhead] = useState(0)
  const [playing, setPlaying] = useState(false)
  const wantPlay = useRef(false)
  // A move the person makes (a seek, a cut, a clip change) places the players exactly. Only the playing sequence's own ticks
  // leave a drift to the tolerance, so the flag is set by every other writer of the playhead.
  const placeExact = useRef(false)
  // A placement the playhead already holds renders nothing by itself, so each one also bumps this count: the sync effect runs
  // for it, and the players are placed (a new cut at the same second is not skipped).
  const [placeTick, setPlaceTick] = useState(0)
  const place = useCallback((seconds: number) => {
    placeExact.current = true
    setPlayhead(seconds)
    setPlaceTick((tick) => tick + 1)
  }, [])
  // Two players take turns: the clip on screen in one, the clip after it loaded in the other.
  const players = [useRef<HTMLVideoElement>(null), useRef<HTMLVideoElement>(null)] as const
  const stage = useRef<HTMLDivElement>(null)
  const [failedPath, setFailedPath] = useState<string | null>(null)
  const [focus, setFocus] = useState<'preview' | 'trim'>('trim')
  const [tab, setTab] = useState<'edit' | 'color' | 'speed' | 'export'>('edit')
  const replacingRef = useRef(false)
  const [replacing, setReplacing] = useState(false)
  const [confirmReplace, setConfirmReplace] = useState(false)
  const [rangeUndo, setRangeUndo] = useState<{ before: SequenceClip[]; after: SequenceClip[]; at: number } | null>(null)
  // The Selection: the id of the clip whose active part the preview is held to. Null is the whole sequence. Only the id lives here;
  // the part is the clip's own `active`, so a split, a delete or an undo keeps it valid without a second index.
  const [selectedClip, setSelectedClip] = useState<string | null>(null)
  const focusedSerial = useRef(0)
  useEffect(() => {
    if (!focusClip || focusedSerial.current === focusClip.serial) return
    const found = clips.findIndex((entry) => entry.id === focusClip.id)
    if (found < 0) return
    focusedSerial.current = focusClip.serial
    setSelectedClip(null)
    setPlaying(false)
    wantPlay.current = false
    place(clipStart(clips, found))
  }, [clips, focusClip])

  const total = sequenceLength(clips)
  // A clip that has left the sequence takes its Selection with it: the id finds nothing, and the preview is the sequence again.
  const selectedIndex = selectedClip === null ? -1 : clips.findIndex((entry) => entry.id === selectedClip)
  const selectedPart = selectedIndex >= 0 ? Math.min(clips[selectedIndex].active, clips[selectedIndex].parts.length - 1) : -1
  const span = useMemo(() => (selectedIndex >= 0 ? partSpan(clips, selectedIndex, selectedPart) : null), [clips, selectedIndex, selectedPart])
  // The playhead stays in sequence seconds everywhere. A Selection holds it inside its part (the end included), so it never reads
  // the sequence lookup for its own end: a boundary there belongs to the next clip.
  const at = span ? holdInSpan(span, playhead) : Math.min(Math.max(0, playhead), total)
  const located = locate(clips, at)
  const index = span ? span.index : Math.max(0, Math.min(clips.length - 1, located.index))
  const empty = clips.length === 0
  const clip = clips[index] ?? EMPTY_CLIP
  const clock = previewClock(span, at, total)
  // One fold for the whole audio view: the inspector and the timeline show the same state, and a clip
  // that was not on screen yet starts folded into its one combined lane.
  const [audioFold, setAudioFold] = useState({ id: clip.id, expanded: false })
  const audioExpanded = audioFold.id === clip.id && audioFold.expanded
  const setAudioExpanded = useCallback((open: boolean) => setAudioFold({ id: clip.id, expanded: open }), [clip.id])
  const sourceTime = span ? spanSource(span, at) : located.sourceTime
  const sourceRef = useRef(sourceTime)
  sourceRef.current = sourceTime
  const activeSlot = playerSlot(index)
  const slotClip = (slot: 0 | 1): SequenceClip | null => {
    const i = slotClipIndex(index, slot, clips.length)
    return i < 0 ? null : clips[i]
  }
  const slotA = slotClip(0)
  const slotB = slotClip(1)
  const detailsOf = (entry: SequenceClip | null): MediaDetails =>
    entry
      ? {
          ok: true,
          duration: entry.duration,
          width: entry.width,
          height: entry.height,
          fps: entry.fps,
          videoCodec: '',
          videoKbps: 0,
          bytes: entry.bytes,
          modified: entry.modified ?? 0,
          audio: entry.audio,
        }
      : NO_DETAILS
  const detailsA = useMemo(() => detailsOf(slotA), [slotA])
  const detailsB = useMemo(() => detailsOf(slotB), [slotB])
  const gainsOf = (entry: SequenceClip | null) =>
    entry ? monitorGains(entry.volumes, entry.muted, entry.solo, entry.audio.length, mixIsDerived(audioLayout(entry.audio)), entry.master) : NO_VOLUMES
  const gainsA = useMemo(() => gainsOf(slotA), [slotA])
  const gainsB = useMemo(() => gainsOf(slotB), [slotB])
  const failed = !empty && failedPath === clip.path

  // Keyframes and sound, once per file, for as long as the window is open. The reads are shared and
  // capped (media_queue.ts): a walk through the clips leaves one process at a time, and leaving a clip
  // ends the read it had out once another needs its place, so nothing is scanned for a clip nobody is
  // looking at. A source replaced in place keeps its path, so its size and write time are what ask for
  // the new bytes to be read.
  const [media, setMedia] = useState<{ path: string; probe: MediaProbe; sound: 'loading' | 'ready' | 'failed' }>({ path: '', probe: NO_PROBE, sound: 'loading' })
  const [mediaEpoch, setMediaEpoch] = useState(0)
  const stamps = useRef(new Map<string, string>())
  useEffect(() => {
    const path = clip.path
    if (!path) {
      setMedia({ path: '', probe: NO_PROBE, sound: 'loading' })
      return
    }
    let live = true
    const stamp = `${clip.bytes}|${clip.modified}`
    // A file rewritten under the same name would otherwise keep the wave of its old bytes: the queue
    // caches by path, so the read in hand is dropped and asked for again.
    if (stamps.current.has(path) && stamps.current.get(path) !== stamp) mediaWork.forget(path)
    stamps.current.set(path, stamp)
    // A file already read draws at once, with no empty frame in between. The state names the path it
    // describes, so a switched clip never shows the previous clip's wave for a frame.
    const show = (patch: Partial<{ probe: MediaProbe; sound: 'loading' | 'ready' | 'failed' }>) =>
      setMedia((current) => (current.path === path ? { ...current, ...patch } : { path, probe: NO_PROBE, sound: 'loading', ...patch }))
    const known = mediaWork.cached(path, PEAKS)
    setMedia({ path, probe: known, sound: known.peaks ? 'ready' : 'loading' })
    void mediaWork.keyframes(path).then((keyframes) => {
      if (live && keyframes) show({ probe: { ...mediaWork.cached(path, PEAKS), keyframes } })
    })
    // A failed read is not cached, so it is said rather than left looking like a silent track.
    void mediaWork.peaks(path, PEAKS).then((tracks) => {
      if (!live) return
      if (tracks) show({ probe: mediaWork.cached(path, PEAKS), sound: 'ready' })
      else show({ sound: 'failed' })
    })
    return () => {
      live = false
      mediaWork.release(path)
    }
  }, [clip.path, clip.bytes, clip.modified, mediaEpoch])
  // The probe and the sound state belong to the path that asked for them.
  const probe = media.path === clip.path ? media.probe : NO_PROBE
  const sound = media.path === clip.path ? media.sound : 'loading'

  const update = useCallback((patch: Partial<SequenceClip>) => {
    setClips((list) => list.map((entry, i) => (i === index ? { ...entry, ...patch } : entry)))
  }, [index, setClips])

  // A lane and its fader are one control: picking a lane brings the Edit tab forward and puts the
  // keyboard on that track's level. The mixer is read from the document, so no shared selection state
  // has to be threaded through the pane.
  const focusMixer = useCallback((trackIndex: number) => {
    setTab('edit')
    window.requestAnimationFrame(() => {
      document.querySelector<HTMLInputElement>(`[data-audio-track="${trackIndex}"]`)?.focus()
    })
  }, [])

  // The colour grade and the crop are one look for the whole sequence: a change on the active clip is made on every
  // clip, a clip whose value already matches is left as it is (no new object, nothing to redraw), and a frame placed
  // on this clip lands at the same share of each other clip's picture.
  const look = useCallback((patch: LookPatch) => {
    setClips((list) => {
      const source = list[index]
      if (!source) return list
      return list.map((entry) => {
        let next = entry
        if (patch.grade && !sameGrade(entry.grade, patch.grade)) next = { ...next, grade: patch.grade }
        if (patch.aspect !== undefined) next = { ...next, aspect: patch.aspect, crop: centredCrop(entry.width, entry.height, patch.ratio ?? null) }
        else if (patch.crop) {
          const crop = scaleCrop(patch.crop, source, entry)
          if (!sameCrop(entry.crop, crop)) next = { ...next, crop }
        }
        return next
      })
    })
  }, [index, setClips])

  const setParts = useCallback((next: Part[], record = true) => {
    placeExact.current = true
    update({
      parts: next,
      history: record ? [...clip.history.slice(-49), clip.parts] : clip.history,
      future: record ? [] : clip.future,
    })
  }, [update, clip.history, clip.parts, clip.future])

  const undo = useCallback(() => {
    const previous = clip.history[clip.history.length - 1]
    if (!previous) return
    placeExact.current = true
    update({ parts: previous, history: clip.history.slice(0, -1), future: [...clip.future, clip.parts], active: Math.min(clip.active, previous.length - 1) })
  }, [clip, update])
  const redo = useCallback(() => {
    const next = clip.future[clip.future.length - 1]
    if (!next) return
    placeExact.current = true
    update({ parts: next, history: [...clip.history, clip.parts], future: clip.future.slice(0, -1), active: Math.min(clip.active, next.length - 1) })
  }, [clip, update])

  const current = Math.min(clip.active, clip.parts.length - 1)
  const cropOn = clip.aspect !== 'off'
  const cropRatio = ASPECTS.find((entry) => entry.value === clip.aspect)?.ratio ?? null
  const untouched = clip.parts.length === 1 && clip.parts[0][0] <= 0.001 && clip.parts[0][1] >= clip.duration - 0.001
  const fitted = useFitted(stage, clip.width, clip.height, cropOn ? clip.crop : undefined)
  // The part of the clip the cut lane shows. The wheel and the toolbar's zoom write it; a clip starts whole.
  const [view, setView] = useState<TimelineView>({ start: 0, span: Math.max(0.001, clip.duration) })
  useEffect(() => setView({ start: 0, span: Math.max(0.001, clip.duration) }), [clip.id, clip.duration])
  // Each player carries its own clip's tracks: only the one on screen is connected to the mix, and a
  // screenshot in front of the editor leaves no copy running behind it.
  const mixA = useTrackMix(players[0], slotA?.path ?? '', detailsA, gainsA, !slotA || activeSlot !== 0 || suspended || failedPath === slotA.path)
  const mixB = useTrackMix(players[1], slotB?.path ?? '', detailsB, gainsB, !slotB || activeSlot !== 1 || suspended || failedPath === slotB.path)
  const mix = activeSlot === 0 ? mixA : mixB
  // What the picture's sound is: the file's own layout decides, never the capture settings of today.
  const layout = useMemo(() => audioLayout(clip.audio), [clip.audio])
  const monitor = monitorMode(layout, clip.audio.length)
  // The lanes whose sound is actually heard, in the mixer's order: a derived mix is not drawn beside
  // the sources it is made from, and a mix that cannot be rebuilt is drawn alone.
  const audible = useMemo(() => audibleRows(clip.audio, layout), [clip.audio, layout])

  // ---------------------------------------------------------------- playback

  const playFrom = (seconds: number) => {
    place(Math.min(Math.max(0, seconds), total))
  }
  // The sequence's own places leave any Selection: a lane, the library, a move or a removal means the whole sequence again.
  const sequenceFrom = (seconds: number) => {
    setSelectedClip(null)
    playFrom(seconds)
  }
  const pausePreview = () => {
    wantPlay.current = false
    setPlaying(false)
    for (const player of players) player.current?.pause()
  }

  // The parts the player on screen is held to: the selected part alone in a Selection, the clip's kept parts otherwise.
  const playParts = (entry: SequenceClip, slot: 0 | 1): Part[] => (span && slot === activeSlot ? [[span.from, span.to]] : entry.parts)

  const togglePlay = useCallback(() => {
    if (wantPlay.current) {
      wantPlay.current = false
      for (const player of players) player.current?.pause()
      setPlaying(false)
      return
    }
    // From the end, the preview starts again: the sequence on its first kept frame, a Selection on the start of its part.
    // The ended player is not played here: it would restart at the removed head of its file. The sync effect places the
    // players first, then starts the one on screen.
    if (span ? restartAt(span, at) !== at : at >= total - 0.05) {
      place(span ? span.start : 0)
      wantPlay.current = true
      setPlaying(true)
      return
    }
    const element = players[playerSlot(index)].current
    if (!element) return
    wantPlay.current = true
    void element.play().catch(() => undefined)
    setPlaying(true)
  }, [at, total, index, place, span])

  // A loaded player takes its place: the active one the playhead, the waiting one its clip's first
  // kept frame, so the boundary swaps two ready pictures instead of loading one.
  const prepare = (slot: 0 | 1) => {
    const element = players[slot].current
    const entry = slotClip(slot)
    if (!entry || !element) return
    element.playbackRate = entry.speed
    if (slot === activeSlot) element.currentTime = displaySource(playParts(entry, slot), Math.max(0, Math.min(entry.duration, sourceRef.current)))
    else element.currentTime = preloadTime(entry)
    if (slot === activeSlot && wantPlay.current) void element.play().catch(() => undefined)
  }

  // While a cut handle is dragged the players do not chase it: every frame of the drag moved the source time under the playhead, and
  // each move past a quarter second seeked the video, which is what made the trim bar lag. One seek follows the release.
  const trimming = useRef(false)
  const [releaseTick, setReleaseTick] = useState(0)
  const onTrimDrag = useCallback((on: boolean) => {
    trimming.current = on
    if (!on) setReleaseTick((tick) => tick + 1)
  }, [])
  // The Selection's own reconcile, in one place. A change to the held part (another part or clip, its source range, its place on the
  // clock) stops the preview, and a playhead outside the part goes to its start. Declared before the sync effect, so the pause is in
  // place before any player is asked to play. The playhead is read through its ref: a tick never restarts this check.
  const selectionKey = span ? `${clip.id}|${span.part}|${span.from}|${span.to}` : ''
  const headRef = useRef(playhead)
  headRef.current = playhead
  useEffect(() => {
    pausePreview()
    if (!span) return
    if (!inSpan(span, headRef.current)) place(span.start)
    // It re-runs when the held part, its source range or its place on the clock changes; the playhead is read through its ref.
  }, [selectionKey, span?.start, span?.end]) // eslint-disable-line react-hooks/exhaustive-deps -- see the comment above

  // Both players follow the playhead: the active one plays from the sequence's second, the other
  // waits on its clip's first frame until the boundary makes it active.
  useEffect(() => {
    if (trimming.current) return
    // Paused, or placed by a move or a committed edit, the players go to the exact kept frame: a speed change while paused moves
    // the sequence's source second with no placement, and the picture has to follow it. A playing one is only corrected once it
    // drifts (needsSeek). The flag is spent here, so the next tick is a drift again. A placement made before a player has its
    // metadata is not lost: prepare places that player when the metadata arrives.
    const exact = placeExact.current || !wantPlay.current
    placeExact.current = false
    players.forEach((ref, slot) => {
      const element = ref.current
      const entry = slotClip(slot as 0 | 1)
      if (!element || !entry) return
      element.playbackRate = entry.speed
      if (element.readyState < 1) return
      const target = slot === activeSlot ? displaySource(playParts(entry, slot as 0 | 1), Math.min(entry.duration, sourceTime)) : preloadTime(entry)
      if (needsSeek(element.currentTime, target, exact)) element.currentTime = target
      if (slot === activeSlot) {
        if (wantPlay.current && element.paused) void element.play().catch(() => undefined)
      } else if (!element.paused) element.pause()
    })
  }, [activeSlot, sourceTime, slotA, slotB, releaseTick, placeTick, span?.from, span?.to]) // eslint-disable-line react-hooks/exhaustive-deps -- playParts reads the span, which its bounds name

  // The clip plays only what it keeps, and the end of its last part hands over to the next clip,
  // whose player is already loaded and waiting. A player the browser paused mid-clip is started
  // again: an occluded or throttled window must not leave the sequence stopped in its tracks.
  // 3.4: the loop runs only while the sequence plays and stops itself when it is paused, so a paused editor does no work per frame.
  useEffect(() => {
    if (!playing) return
    let frame = 0
    const tick = () => {
      const element = players[activeSlot].current
      if (element) {
        // A Selection is one bounded part: its end finishes the preview on its own last kept image, and never hands over.
        const parts: Part[] = span ? [[span.from, span.to]] : clip.parts
        const step = playbackStep({
          wantPlay: wantPlay.current,
          paused: element.paused,
          ended: element.ended,
          currentTime: element.currentTime,
          end: parts[parts.length - 1][1],
          last: span ? true : index + 1 >= clips.length,
        })
        if (step === 'restart') void element.play().catch(() => undefined)
        else if (step === 'advance') {
          const jump = playbackJump(parts, element.currentTime)
          if (jump !== null) element.currentTime = jump
          setPlayhead(span ? spanAt(span, element.currentTime) : clipStart(clips, index) + outputOffsetOfSource(clip, element.currentTime))
        } else if (step === 'handover') place(clipStart(clips, index + 1))
        else if (step === 'finish') {
          wantPlay.current = false
          setPlaying(false)
          element.pause()
          place(span ? span.end : total)
        }
      }
      if (wantPlay.current) frame = requestAnimationFrame(tick)
    }
    frame = requestAnimationFrame(tick)
    return () => cancelAnimationFrame(frame)
  }, [playing, clip, index, clips, total, activeSlot, place, span])

  // A cut part is skipped when playback starts, as on the stage of the single-clip editor.
  // The playhead and the player go to a second of the clip under the given parts. A committed cut passes its own parts:
  // the render's clip still holds the parts that were just replaced, and mapping through them placed the playhead wrongly.
  const placeIn = useCallback((parts: Part[], t: number) => {
    const jump = playbackJump(parts, t)
    const next = jump === null ? t : jump
    place(clipStart(clips, index) + outputOffsetOfSource({ ...clip, parts }, next))
    const element = players[playerSlot(index)].current
    if (element) element.currentTime = displaySource(parts, next)
  }, [clip, clips, index, place])
  // A seek from the cut lane's pointer. In a Selection it selects the part the second lands in, so the lane and the preview agree.
  // It writes `active` only when that part changes: a seek drag reports every move, and the same part is not written again.
  const seekSource = useCallback((t: number) => {
    const jump = playbackJump(clip.parts, t)
    const part = partAt(clip.parts, jump === null ? t : jump)
    if (span && part >= 0 && part !== span.part) update({ active: part })
    placeIn(clip.parts, t)
  }, [clip.parts, span, placeIn, update])
  // The keys (the arrows, the frame steps, Home and End) never leave a Selection: they are held to its part. In the sequence they seek as before.
  const seekInSpan = (t: number) => {
    if (!span) return seekSource(t)
    const source = Math.min(Math.max(t, span.from), span.to)
    place(spanAt(span, source))
    const element = players[playerSlot(index)].current
    if (element) element.currentTime = displaySource([[span.from, span.to]], source)
  }

  const [splitFlash, setSplitFlash] = useState<{ at: number; id: number } | null>(null)
  const splitHere = useCallback(() => {
    const result = split(clip.parts, sourceTime)
    if (!result) return
    setSplitFlash({ at: sourceTime, id: Date.now() })
    setParts(result.parts)
    update({ active: result.active })
  }, [clip.parts, sourceTime, setParts, update])
  const deletePartAt = useCallback((part: number) => {
    const result = remove(clip.parts, part)
    if (!result) return
    setParts(result.parts)
    update({ active: result.active })
    placeIn(result.parts, result.parts[result.active][0])
  }, [clip.parts, setParts, update, placeIn])
  const deletePart = useCallback(() => deletePartAt(current), [deletePartAt, current])
  const canSplit = split(clip.parts, sourceTime) !== null
  const canDelete = clip.parts.length > 1

  // A committed trim selects its clip's part: the Selection follows the edit. The reconcile effect then holds the playhead.
  const applyEdge = useCallback((part: number, edge: 'in' | 'out', t: number, first: boolean, live: boolean) => {
    if (!clip.parts[part]) return
    setParts(moveEdge(clip.parts, part, edge, t, clip.duration, live ? 0 : SHORTEST), first)
    setSelectedClip(clip.id)
  }, [clip.id, clip.parts, clip.duration, setParts])
  const applyPart = useCallback((part: number, from: number, first: boolean) => {
    const entry = clip.parts[part]
    if (!entry) return
    setParts(movePart(clip.parts, part, from - entry[0], clip.duration), first)
    setSelectedClip(clip.id)
  }, [clip.id, clip.parts, clip.duration, setParts])
  const applyBar = useCallback((from: number, first: boolean) => {
    if (!clip.parts.length) return
    setParts(moveAll(clip.parts, from - clip.parts[0][0], clip.duration), first)
    setSelectedClip(clip.id)
  }, [clip.id, clip.parts, clip.duration, setParts])

  const moveClip = useCallback((from: number, to: number) => {
    const next = reorder(clips, from, to)
    if (next === clips) return
    setSelectedClip(null)
    setClips(next)
    place(clipStart(next, to))
  }, [clips, setClips])

  // ---------------------------------------------------------------- export

  const [defaults] = useState(loadExportDefaults)
  const [quality, setQuality] = useState(defaults.quality)
  const [resolution, setResolution] = useState(defaults.resolution)
  // The saved copy's name. It follows the generated default (the clip's name and "edit"/"sequence") until a
  // person types one; from then on their name stays, across cuts, a second save and to the Share sheet.
  const generatedName = `${stem(clips[0]?.path ?? '') || 'buffrx'}-${clips.length > 1 ? 'sequence' : 'edit'}.mp4`
  const [typedName, setTypedName] = useState<string | null>(null)
  const exportName = typedName ?? generatedName
  const nameCheck = exportFileName(exportName)
  const [exportBusy, setExportBusy] = useState(false)
  const [exporting, setExporting] = useState<{ id: number; value: number }[]>([])
  const [done, setDone] = useState<string | null>(null)
  const [doneSignature, setDoneSignature] = useState('')
  const [doneShareable, setDoneShareable] = useState(false)
  const settles = useRef(new Map<number, (result: ExportResult) => void>())
  const endedEarly = useRef(new Map<number, ExportResult>())
  const shareAfter = useRef(false)
  const saveSignature = useRef('')
  const shareSignature = useRef('')
  const shareIsCopy = useRef(false)

  const exportSignature = JSON.stringify([clips.map(clipExportInput), quality, resolution])
  const signatureRef = useRef(exportSignature)
  signatureRef.current = exportSignature

  useEffect(() => {
    const offProgress = onShell('media-progress', (data) => {
      const { id, value } = data as { id: number; value: number }
      setExporting((list) => list.map((entry) => (entry.id === id ? { id, value } : entry)))
    })
    const offDone = onShell('media-done', (data) => {
      const result = data as ExportResult
      setExportBusy(false)
      setExporting((list) => list.filter((entry) => entry.id !== result.id))
      const settle = settles.current.get(result.id)
      if (!settle) {
        endedEarly.current.set(result.id, result)
        return
      }
      settles.current.delete(result.id)
      settle(result)
    })
    return () => {
      offProgress()
      offDone()
    }
  }, [])

  // The panel draws the sheet; the editor leaves only after the engine accepted the file, so a refused share
  // does not take the person away from the edit. Save & share and the saved card's Share both end here.
  const shareFile = useCallback((file: string) => {
    void attempt('The share sheet did not open',
                 () => shareThenLeave(() => engine.share(file), () => onShared?.()))
  }, [onShared])

  const settle = useCallback((result: ExportResult) => {
    onExportingChange?.(false)
    const wasReplacing = replacingRef.current
    replacingRef.current = false
    setReplacing(false)
    if (result.ok) {
      setDone(result.output)
      setDoneSignature(saveSignature.current)
      setDoneShareable(shareIsCopy.current)
      toast('success', 'Saved', `${fileName(result.output)}${result.encoder === 'copy' ? ' · lossless copy' : ''}`)
      // A save that goes straight to the Share sheet does not also open Explorer: the sheet is the destination.
      if (!shareAfter.current) void attempt('Not shown', () => engine.reveal({ file: result.output, kind: 'clip' } as MediaItem))
      void engine.notifyLibrary().catch(() => undefined)
      if (wasReplacing) {
        // The source has changed on disk. Reset its edit only if nothing arrived or changed while saving.
        const signature = saveSignature.current
        void loadDetails(result.output).then((details) => {
          if (!details.ok || signatureRef.current !== signature) {
            toast('info', 'Original replaced', 'Reopen the video before making further edits.')
            return
          }
          mediaWork.forget(result.output)
          // Same path, new bytes: the reads of the old file are gone, so the wave and cuts are read again.
          setSelectedClip(null)
          setMediaEpoch((epoch) => epoch + 1)
          setClips([newClip({ path: result.output, name: fileName(result.output), duration: details.duration,
            width: details.width, height: details.height, fps: details.fps, bytes: details.bytes,
            modified: details.modified, audio: details.audio })])
          place(0)
          setFailedPath(null)
          // The source was rewritten: its signature is no longer the one the save was measured
          // against, so the old card would immediately claim the edit changed. Start clean.
          setDone(null)
          players.forEach((ref) => ref.current?.load())
        }).catch(() => toast('info', 'Original replaced', 'Reopen the video before making further edits.'))
      }
      if (shareAfter.current && canShareSaved(shareSignature.current, signatureRef.current, shareIsCopy.current))
        shareFile(result.output)
      else if (shareAfter.current) toast('info', 'Clip saved', 'Your edit changed while saving. Save it again to share the latest version.')
    } else if (result.error !== 'cancelled') {
      toast('error', 'The export failed', result.error)
    }
    shareAfter.current = false
  }, [setClips, onExportingChange, shareFile])

  const outHeight = resolution === 'original' ? 0 : Number(resolution)
  const base = clips[0] ?? EMPTY_CLIP
  // The picture that sets the output shape is the first clip's frame as it is kept: a portrait crop
  // exported at 720p is 720 tall and narrow, not a 16:9 stretch of it.
  const frame = base.crop.width > 0 && (base.crop.width < base.width || base.crop.height < base.height) ? base.crop : base
  const outSize = outHeight && outHeight < frame.height
    ? { width: Math.round((frame.width * outHeight) / frame.height / 2) * 2, height: outHeight }
    : null
  const soundProblem = joinSoundProblem(clips)
  const soundWarning = soundProblem === 'tracks' ? TRACK_MISMATCH : soundProblem === 'kinds' ? SOUND_KIND_MISMATCH : null
  const canReplace = clips.length === 1 && !clips[0].timelineSession
  // A source problem belongs to the Timeline clip it names, so it only blocks an export the sequence would
  // actually read it for: an ordinary sequence of library clips exports while a session is still preparing.
  const exportable = total >= 0.5 && !!clipsDir && clips.length > 0 && !soundProblem && (!sourceProblem || !clips.some((entry) => entry.timelineSession))

  const runExport = async (share: boolean, replace = false) => {
    if (exportBusy || !exportable || (replace && !canReplace)) return
    const chosen = exportFileName(exportName)
    if (!replace && chosen.error) {
      toast('error', 'The copy needs a name', chosen.error)
      return
    }
    setTab('export')
    setExportBusy(true)
    onExportingChange?.(true)
    shareAfter.current = share
    saveSignature.current = exportSignature
    shareSignature.current = exportSignature
    shareIsCopy.current = !replace
    if (replace) {
      replacingRef.current = true
      setReplacing(true)
      wantPlay.current = false
      setPlaying(false)
      players.forEach((ref) => ref.current?.pause())
    }
    setDone(null)
    const target = replace ? clips[0].path : await attempt('The export did not start', () => uniquePath(`${clipsDir}\\Edits\\${chosen.name}`))
    if (target === undefined) { setExportBusy(false); onExportingChange?.(false); replacingRef.current = false; setReplacing(false); return }
    const inputs = clips.map(clipExportInput)
    const job = clips.length === 1
      ? { ...inputs[0], output: target, replace_original: replace, fail_if_exists: !replace, quality, out_width: outSize?.width ?? 0, out_height: outSize?.height ?? 0 }
      : { output: target, inputs, fail_if_exists: true, quality, out_width: outSize?.width ?? 0, out_height: outSize?.height ?? 0, fit_pad: true }
    const id = await attempt('The export did not start', () => shell<number>('media_export', { job }))
    if (id === undefined) { setExportBusy(false); onExportingChange?.(false); replacingRef.current = false; setReplacing(false); return }
    const ended = endedEarly.current.get(id)
    if (ended) {
      endedEarly.current.delete(id)
      settle(ended)
    } else {
      settles.current.set(id, settle)
      setExporting((list) => [...list, { id, value: 0 }])
    }
  }

  const takeOut = (id: string) => {
    const removed = clips.findIndex((entry) => entry.id === id)
    const next = clips.filter((entry) => entry.id !== id)
    setSelectedClip(null)
    setClips(next)
    onForget(id)
    if (next.length) place(clipStart(next, Math.min(removed, next.length - 1)))
    else place(0)
  }

  const trimTimeline = (side: 'from' | 'to') => {
    const next = trimTimelineAt(clips, index, sourceTime, side)
    if (!next || !clip.timelineSession) return
    setRangeUndo({ before: clips, after: next, at })
    setSelectedClip(null)
    setClips(next)
    wantPlay.current = false
    setPlaying(false)
    const matching = next.map((entry, i) => entry.timelineSession === clip.timelineSession ? i : -1).filter((i) => i >= 0)
    const target = side === 'from' ? matching[0] : matching[matching.length - 1]
    const boundary = clipStart(next, target) + (side === 'to' ? Math.max(0, clipOutputLength(next[target]) - 0.001) : 0)
    place(boundary)
  }

  // The toolbar's zoom reads and writes the cut lane's view, around the playhead when it is on screen.
  const zoom = zoomOfSpan(view.span, clip.duration)
  const setZoom = (level: number) => {
    const anchor = sourceTime >= view.start && sourceTime <= view.start + view.span ? sourceTime : view.start + view.span / 2
    setView((current) => viewAtSpan(current, clip.duration, anchor, spanOfZoom(level, clip.duration)))
  }
  const stepZoom = (direction: 1 | -1) => setZoom(Math.min(1, Math.max(0, zoom + direction * 0.15)))
  // Clip navigation returns to the sequence; selecting a cut is the explicit way to preview one part again.
  const goPrevious = () => {
    const offsetInClip = at - clipStart(clips, index)
    sequenceFrom(offsetInClip > 0.5 || index === 0 ? clipStart(clips, index) : clipStart(clips, index - 1))
  }
  const goNext = () => {
    if (index + 1 < clips.length) sequenceFrom(clipStart(clips, index + 1))
  }

  // ---------------------------------------------------------------- keyboard and wheel

  const [help, setHelp] = useState(false)
  // Full screen: the picture and its transport take the whole window, and the window itself goes full screen.
  const [fullscreen, setFullscreen] = useState(false)
  useEffect(() => {
    void setWindowFullscreen(fullscreen)
    return () => void setWindowFullscreen(false)
  }, [fullscreen])
  const suspendedRef = useRef(suspended)
  suspendedRef.current = suspended
  // Behind the screenshot editor nothing keeps playing.
  useEffect(() => {
    if (!suspended) return
    setFullscreen(false)
    wantPlay.current = false
    setPlaying(false)
    for (const player of players) player.current?.pause()
  }, [suspended]) // eslint-disable-line react-hooks/exhaustive-deps -- the players are refs
  // A right click: on a clip's block in the sequence lane, or on a part of the active clip's trim bar.
  const [menu, setMenu] = useState<{ kind: 'clip' | 'part'; index: number; x: number; y: number } | null>(null)
  const menuItems = (() => {
    if (!menu) return []
    if (menu.kind === 'part') {
      // The same rule as the Delete key: the part goes, and the clip with it when it is the only one.
      return [{ id: 'delete', label: 'Delete', onAction: () => (canDelete ? deletePartAt(menu.index) : takeOut(clip.id)) }]
    }
    const target = clips[menu.index]
    if (!target) return []
    const whole = target.parts.length === 1 && target.parts[0][0] <= 0.001 && target.parts[0][1] >= target.duration - 0.001
    return [
      { id: 'earlier', label: 'Move earlier', isDisabled: menu.index === 0, onAction: () => moveClip(menu.index, menu.index - 1) },
      { id: 'later', label: 'Move later', isDisabled: menu.index === clips.length - 1, onAction: () => moveClip(menu.index, menu.index + 1) },
      {
        id: 'reset',
        label: 'Reset the trim',
        isDisabled: whole,
        onAction: () => setClips((list) => list.map((entry, i) => (i === menu.index
          ? { ...entry, parts: [[0, entry.duration]], active: 0, history: [...entry.history.slice(-49), entry.parts], future: [] }
          : entry))),
      },
      { id: 'remove', label: 'Remove clip', onAction: () => takeOut(target.id) },
    ]
  })()
  const frameStep = 1 / (clip.fps || 60)
  const perform = (action: EditorAction) => {
    if (action === 'help') return setHelp(true)
    if (action === 'browse') return onBrowse()
    if (action === 'exitFullscreen' && !fullscreen) return
    if (empty) return
    switch (action) {
      case 'play': return togglePlay()
      case 'fullscreen': return setFullscreen((on) => !on)
      case 'exitFullscreen': return setFullscreen(false)
      case 'split': return splitHere()
      // The selected part goes; when it is the only one the clip does, as in any editor.
      case 'delete': return canDelete ? deletePart() : takeOut(clip.id)
      case 'removeClip': return takeOut(clip.id)
      case 'undo': return undo()
      case 'redo': return redo()
      case 'earlier': return moveClip(index, index - 1)
      case 'later': return moveClip(index, index + 1)
      case 'back1': return seekInSpan(sourceTime - 1)
      case 'forward1': return seekInSpan(sourceTime + 1)
      case 'back5': return seekInSpan(sourceTime - 5)
      case 'forward5': return seekInSpan(sourceTime + 5)
      case 'frameBack': return seekInSpan(sourceTime - frameStep)
      case 'frameForward': return seekInSpan(sourceTime + frameStep)
      // Home and End are the selected part's own start and end in a Selection, the clip's in the sequence.
      case 'clipStart': return seekInSpan(span ? span.from : clip.parts[0][0])
      // The last frame, not the end itself: the end is where the next clip starts.
      case 'clipEnd': return seekInSpan(span ? span.to : lastShownSource(clip.parts[clip.parts.length - 1]))
      case 'previousClip': return goPrevious()
      case 'nextClip': return goNext()
      // Ctrl+Home and Ctrl+End are the whole sequence, whatever the preview holds.
      case 'sequenceStart': return sequenceFrom(0)
      case 'sequenceEnd': return sequenceFrom(total)
      case 'markIn':
        setSelectedClip(clip.id)
        return setParts(moveEdge(clip.parts, current, 'in', sourceTime, clip.duration))
      case 'markOut':
        setSelectedClip(clip.id)
        return setParts(moveEdge(clip.parts, current, 'out', sourceTime, clip.duration))
      case 'zoomIn': return stepZoom(1)
      case 'zoomOut': return stepZoom(-1)
      case 'zoomFit': return setView({ start: 0, span: Math.max(0.001, clip.duration) })
      case 'resetTrim':
        if (untouched) return
        setSelectedClip(clip.id)
        setParts([[0, clip.duration]])
        return update({ active: 0 })
      case 'saveCopy': return void runExport(false)
      case 'saveShare': return void runExport(true)
    }
  }
  const performRef = useRef(perform)
  performRef.current = perform
  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (replacingRef.current || suspendedRef.current) return
      const target = event.target instanceof Element ? event.target : null
      // Tabs, menus and lists move on their own arrows; the rest of the window keeps the keys.
      if (target?.closest('input, textarea, select, a, [role="dialog"], [role="slider"], [role="tab"], [role="menu"], [role="menuitem"], [role="listbox"], [role="option"]')) return
      // A clicked button keeps focus; only the keys that press it stay its own, so S, I, O, the
      // arrows and Delete still edit after a click on Split or a Crop shape.
      if (target?.closest('button') && (event.key === ' ' || event.key === 'Enter')) return
      const action = editorAction(event)
      if (!action) return
      event.preventDefault()
      performRef.current(action)
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [])

  // The wheel over the picture or the timeline moves the playhead: a notch is a fixed stretch of the timeline
  // as drawn, so a close zoom scrubs finely. Shift is a frame a notch, Ctrl zooms, Alt leaves the wheel to the
  // timeline's own scrolling. A native listener, because React's is passive and cannot stop the page scrolling.
  const shellRef = useRef<HTMLDivElement>(null)
  // J7: the wheel's zoom steps are a goal the view eases toward (fast token, a frame-rate exponential), so a burst of notches reads as one
  // smooth zoom instead of jumps. Reduced motion jumps.
  const viewRef = useRef(view)
  viewRef.current = view
  const viewGoal = useRef<TimelineView | null>(null)
  const viewFrame = useRef(0)
  const glideView = (compute: (current: TimelineView) => TimelineView) => {
    const goal = compute(viewGoal.current ?? viewRef.current)
    viewGoal.current = goal
    if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
      viewGoal.current = null
      setView(goal)
      return
    }
    if (viewFrame.current) return
    let last = performance.now()
    const stepView = (now: number) => {
      const target = viewGoal.current
      if (!target) { viewFrame.current = 0; return }
      const k = 1 - Math.exp(-((now - last) / 1000) * 32)
      last = now
      setView((current) => {
        const close = Math.abs(target.span - current.span) < target.span * 0.004 && Math.abs(target.start - current.start) < target.span * 0.004
        if (close) return target
        return { start: current.start + (target.start - current.start) * k, span: current.span + (target.span - current.span) * k }
      })
      const live = viewRef.current
      const done = Math.abs(target.span - live.span) < target.span * 0.004 && Math.abs(target.start - live.start) < target.span * 0.004
      if (done) { viewGoal.current = null; viewFrame.current = 0; return }
      viewFrame.current = requestAnimationFrame(stepView)
    }
    viewFrame.current = requestAnimationFrame(stepView)
  }
  useEffect(() => () => {
    if (viewFrame.current) cancelAnimationFrame(viewFrame.current)
    viewFrame.current = 0
  }, [])
  const onWheel = (event: WheelEvent) => {
    const target = event.target instanceof Element ? event.target : null
    if (!target || replacingRef.current || suspendedRef.current || empty) return
    if (!target.closest('.area-timeline, .area-stage') || target.closest('[role="dialog"]')) return
    if (event.altKey && target.closest('.area-timeline')) return
    // Shift turns the wheel sideways on some systems: take whichever way it moved.
    const raw = Math.abs(event.deltaY) >= Math.abs(event.deltaX) ? event.deltaY : event.deltaX
    const delta = raw * (event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? 400 : 1)
    if (!delta) return
    event.preventDefault()
    if (event.ctrlKey || event.metaKey) {
      glideView((current) => zoomView(current, clip.duration, sourceTime, delta < 0 ? 0.85 : 1.15))
      return
    }
    const speed = clip.speed > 0 ? clip.speed : 1
    let moved: number
    if (event.shiftKey) moved = (Math.sign(delta) * frameStep) / speed
    else {
      const lane = Math.max(200, (shellRef.current?.querySelector('.area-timeline')?.clientWidth ?? 800) - GUTTER)
      moved = (delta * (view.span / lane) * 0.25) / speed
    }
    // Playing, the picture is what drives the playhead every frame, so the wheel moves the picture too: the playhead
    // alone would be overwritten by the next frame. The ref keeps a burst of notches adding up before a render.
    const next = Math.min(Math.max(span ? span.start : 0, playheadRef.current + moved), span ? span.end : total)
    playheadRef.current = next
    place(next)
    const element = players[playerSlot(index)].current
    if (span) {
      if (element) element.currentTime = displaySource([[span.from, span.to]], spanSource(span, next))
      return
    }
    const where = locate(clips, next)
    if (element && where.index === index) element.currentTime = displaySource(clip.parts, where.sourceTime)
  }
  const playheadRef = useRef(at)
  playheadRef.current = at
  const wheelRef = useRef(onWheel)
  wheelRef.current = onWheel
  useEffect(() => {
    const element = shellRef.current
    if (!element) return
    const handler = (event: WheelEvent) => wheelRef.current(event)
    element.addEventListener('wheel', handler, { passive: false })
    return () => element.removeEventListener('wheel', handler)
  }, [])

  // A zoomed cut lane follows the playhead: scrubbing or playing past its edge pans it along.
  useEffect(() => {
    if (empty || view.span >= clip.duration - 1e-6) return
    if (sourceTime >= view.start - 1e-6 && sourceTime <= view.start + view.span + 1e-6) return
    setView((current) => clampView({ start: sourceTime < current.start ? sourceTime - current.span * 0.1 : sourceTime - current.span * 0.9, span: current.span }, clip.duration))
  }, [sourceTime, view, clip.duration, empty])

  const editPanel = (content: React.ReactNode) => (empty ? <NothingToEdit /> : <div inert={replacing}>{content}</div>)

  /** A file from the shell or a row from the library: either way every drop area says it is ready. */
  const droppable = hover || dragging

  return (
    <div ref={shellRef} className="editor-workspace editor-shell h-full min-h-0">
      {/* the picture, and the transport under it */}
      <div inert={replacing} data-drop className={clsx('pane area-stage flex min-h-0 flex-col', (focus === 'preview' || droppable) && 'is-focused')} style={{ viewTransitionName: 'clip-pane', ...(fullscreen /* inset leaves room for the pane title, which sits half above the pane */ ? { position: 'fixed', inset: 14, zIndex: 40 } : null) }} onPointerDown={() => setFocus('preview')}>
        <h3 className="pane-title">preview</h3>
        <div ref={stage} className="relative m-1 min-h-0 flex-1 overflow-hidden bg-canvas p-2">
          {empty ? (
            <EmptyStage hover={droppable} onBrowse={onBrowse} />
          ) : (
            // The picture sits inside the stage padding, where its fit was measured. Placed on the stage itself it was one
            // padding too far left, with the spare room all on its right.
            <div className="absolute inset-2">
            <div className="absolute" style={{ left: fitted.left, top: fitted.top, width: fitted.width, height: fitted.height }}>
              {([0, 1] as const).map((slot) => {
                const entry = slotClip(slot)
                if (!entry) return null
                const here = slot === activeSlot
                return (
                  <video
                    key={entry.path}
                    ref={players[slot]}
                    src={mediaUrl(entry.path)}
                    crossOrigin="anonymous"
                    className={clsx('absolute inset-0 h-full w-full border border-line-strong bg-black', !here && 'pointer-events-none opacity-0')}
                    style={{ filter: gradeFilter(entry.grade) }}
                    onLoadedMetadata={() => prepare(slot)}
                    onError={() => setFailedPath(entry.path)}
                    onPointerDown={cropOn || !here ? undefined : (event) => { if (event.button === 0) togglePlay() }}
                    onClick={cropOn || !here ? undefined : (event) => { if (event.detail === 0) togglePlay() }}
                    preload="auto"
                    playsInline
                  />
                )
              })}
              {failed && (
                <div className="absolute inset-0 grid place-items-center border border-line bg-surface text-center text-[14px] text-muted">
                  <div><Icon name="alert" size={22} className="mx-auto mb-2 text-warn" />Preview unavailable for this file. Trimming and exporting still work.</div>
                </div>
              )}
              {cropOn && <CropOverlay frame={{ width: clip.width, height: clip.height }} crop={clip.crop} onCrop={(crop) => look({ crop })} ratio={cropRatio} />}
            </div>
            </div>
          )}
          {droppable && !empty && <DropNotice />}
        </div>
        {!empty && (
          <Transport
            playing={playing}
            fullscreen={fullscreen}
            time={clock.time}
            total={clock.total}
            scope={span ? 'selection' : 'sequence'}
            showScope={clips.length > 1 || clip.parts.length > 1}
            onScope={(next) => {
              // Choosing the Selection holds the active part; choosing the sequence leaves it. A choice already in force changes
              // nothing. Otherwise the preview pauses, and the playhead stays where it is, read on the sequence's clock.
              if ((next === 'selection') === (span !== null)) return
              setSelectedClip(next === 'selection' && clip.parts.length ? clip.id : null)
              pausePreview()
            }}
            hasNext={index + 1 < clips.length}
            clip={clip}
            onToggle={togglePlay}
            onPrev={goPrevious}
            onNext={goNext}
            onFullscreen={() => setFullscreen((on) => !on)}
          />
        )}
      </div>

      {/* the clip's own edit, then the export */}
      <aside className="pane area-inspector flex flex-col">
        <h3 className="pane-title">{empty ? 'clip' : `clip ${String(index + 1).padStart(2, '0')}`}</h3>
        <Tabs
          label="Editor tools"
          value={tab}
          onChange={setTab}
          className="mt-1"
          tabs={[
            { id: 'edit', label: 'Edit', icon: 'sliders', content: editPanel(<EditTab clip={clip} update={update} look={look} audioExpanded={audioExpanded} onAudioExpanded={setAudioExpanded} />) },
            { id: 'color', label: 'Color', icon: 'palette', content: editPanel(<ColorTab clip={clip} look={look} />) },
            { id: 'speed', label: 'Speed', icon: 'gauge', content: editPanel(<SpeedTab clip={clip} update={update} />) },
            {
              id: 'export',
              label: 'Export',
              icon: 'share',
              content: empty ? <NothingToEdit /> : (
                <ExportTab
                  clips={clips.length}
                  canReplace={canReplace}
                  name={exportName}
                  onName={setTypedName}
                  nameError={nameCheck.error}
                  quality={quality}
                  onQuality={(next) => {
                    setQuality(next)
                    saveExportDefaults({ quality: next, resolution })
                  }}
                  resolution={resolution}
                  onResolution={(next) => {
                    setResolution(next)
                    saveExportDefaults({ quality, resolution: next })
                  }}
                  frameWidth={frame.width}
                  frameHeight={frame.height}
                  exportable={exportable}
                  busy={exportBusy}
                  clipsDir={clipsDir}
                  warning={sourceProblem || soundWarning}
                  replacing={replacing}
                  progress={exporting}
                  onCancel={(id) => void shell('media_cancel', { id })}
                  onSave={(share) => void runExport(share)}
                  onReplace={() => setConfirmReplace(true)}
                  done={done}
                  doneStale={doneSignature !== exportSignature}
                  doneShareable={doneShareable}
                  shareable={canShareSaved(doneSignature, exportSignature, doneShareable)}
                  onShare={() => done && shareFile(done)}
                  onShow={() => done && void attempt('Not shown', () => engine.reveal({ file: done, kind: 'clip' } as MediaItem))}
                  onCopy={() => done && void attempt('Not copied', () => engine.copy([done])).then((result) => result !== undefined && toast('success', 'Copied'))}
                />
              ),
            },
          ]}
        />
      </aside>

      {/* the timeline: the toolbar, the sequence's own lane, and the active clip's cut under it */}
      <div inert={replacing} data-drop className={clsx('pane area-timeline flex flex-col', focus === 'trim' && 'is-focused')} onPointerDown={() => setFocus('trim')}>
        <h3 className="pane-title">sequence</h3>
        <span className="pane-title num !left-auto right-[10px]">{duration(total)} total</span>
        {!empty && (
          <div className="mt-1 grid h-[52px] shrink-0 grid-cols-[1fr_auto_1fr] items-center gap-2 border-b border-line px-3">
            <div className="flex items-center">
              <IconButton size={19} className="!h-9 !w-9" icon="undo" label="Undo (Ctrl+Z)" isDisabled={clip.history.length === 0} onPress={undo} />
              <IconButton size={19} icon="undo" label="Redo (Ctrl+Y)" className="!h-9 !w-9 -scale-x-100" isDisabled={clip.future.length === 0} onPress={redo} />
              <span className="mx-2 h-6 w-px bg-line" aria-hidden="true" />
              <IconButton size={19} className="!h-9 !w-9" icon="scissors" label="Cut at the playhead (S or C)" isDisabled={!canSplit} onPress={splitHere} />
              <IconButton size={19} className="!h-9 !w-9" icon="trash" label="Remove this clip (Shift+Delete)" onPress={() => takeOut(clip.id)} />
            </div>
            <span className="num min-w-[124px] border border-line bg-canvas px-3 py-1.5 text-center text-[17px] font-bold text-ink">{timecode(at)}</span>
            <div className="flex items-center justify-end gap-1">
              <IconButton size={19} className="!h-9 !w-9" icon="zoomOut" label="Zoom out (-)" isDisabled={zoom <= 0} onPress={() => stepZoom(-1)} />
              <Slider label="Timeline zoom" value={Math.round(zoom * 100)} min={0} max={100} step={1} onChange={(value) => setZoom(value / 100)} onCommit={(value) => setZoom(value / 100)} className="w-[180px]" />
              <IconButton size={19} className="!h-9 !w-9" icon="zoomIn" label="Zoom in (+)" isDisabled={zoom >= 1} onPress={() => stepZoom(1)} />
              <span className="mx-2 h-6 w-px bg-line" aria-hidden="true" />
              <IconButton size={19} className="!h-9 !w-9" icon="keyboard" label="Keyboard shortcuts (?)" onPress={() => setHelp(true)} />
            </div>
          </div>
        )}
        <div className="flex min-h-0 flex-col gap-3 overflow-y-auto p-3" style={{ maxHeight: '46vh' }}>
          {empty ? (
            <div className="grid h-[88px] place-items-center border border-line bg-canvas px-4 text-center text-[14px] text-faint">Added clips appear here.</div>
          ) : (
            <>
              <SequenceLanes clips={clips} active={index} time={at} onSelect={(i) => sequenceFrom(clipStart(clips, i))} onMenu={(i, x, y) => setMenu({ kind: 'clip', index: i, x, y })} onSeek={sequenceFrom} onReorder={moveClip} />
              {clip.timelineSession && (
                <div className="flex flex-wrap items-center gap-2">
                  <span className="text-[13px] text-muted">Trim Timeline at playhead</span>
                  <Button variant="ghost" isDisabled={!!sourceProblem} onPress={() => trimTimeline('from')}>Keep from here</Button>
                  <Button variant="ghost" isDisabled={!!sourceProblem} onPress={() => trimTimeline('to')}>Keep to here</Button>
                  {rangeUndo?.after === clips && <Button variant="ghost" onPress={() => {
                    setSelectedClip(null)
                    setClips(rangeUndo.before)
                    place(rangeUndo.at)
                    setRangeUndo(null)
                  }}>Undo Timeline trim</Button>}
                </div>
              )}
              <ClipTimeline
                duration={clip.duration}
                time={sourceTime}
                parts={clip.parts}
                active={current}
                tracks={audible.map((row) => row.track)}
                trackIndexes={audible.map((row) => row.index)}
                gains={gainsOf(clip)}
                muted={clip.muted}
                solo={clip.solo}
                sound={sound}
                view={view}
                onView={setView}
                onSelect={(part) => {
                  if (part !== clip.active) update({ active: part })
                  setSelectedClip(clip.id)
                }}
                onPartMenu={(part, x, y) => setMenu({ kind: 'part', index: part, x, y })}
                onEdge={applyEdge}
                onPart={applyPart}
                onBar={applyBar}
                onSeek={seekSource}
                onDrag={onTrimDrag}
                flash={splitFlash}
                peaks={probe.peaks}
                undecoded={probe.undecoded}
                keyframes={probe.keyframes}
                onFocusTrack={focusMixer}
                lockReason={mixLockReason(layout)}
                expanded={audioExpanded}
                onExpanded={setAudioExpanded}
              />
              {soundWarning && <p className="text-[13px] leading-snug text-warn">{soundWarning}</p>}
              {clip.audio.length > 1 && (() => {
                const reason = monitor === 'mix' ? mixLockReason(layout)
                  : mix.state === 'loading'
                    ? (monitor === 'isolated'
                      ? "Preparing this clip's own tracks. The original mix plays until they are ready, so your edits are not audible yet."
                      : "Preparing this clip's tracks. The further tracks join when they are ready.")
                    : mix.state === 'failed'
                      ? (monitor === 'isolated'
                        ? "Your audio edits are not audible in the preview: the original mix is playing. The saved file still uses every track and your edits."
                        : "Some of this file's tracks could not be opened, so the preview is not playing every track. The saved file still uses every track and your edits.")
                      : ''
                return reason ? (
                  <div className="flex flex-wrap items-center gap-2">
                    <p className={clsx('text-[13px] leading-snug', mix.state === 'failed' ? 'text-warn' : 'text-muted')}>{reason}</p>
                    {mix.state === 'failed' && <Button variant="ghost" onPress={mix.retry}>Try again</Button>}
                  </div>
                ) : null
              })()}
            </>
          )}
        </div>
      </div>

      <DialogBox isOpen={confirmReplace} onClose={() => setConfirmReplace(false)} title="Replace the original?"
        actions={<><Button variant="ghost" onPress={() => setConfirmReplace(false)}>Cancel</Button>
          <Button variant="danger" onPress={() => { setConfirmReplace(false); void runExport(false, true) }}>Replace</Button></>}>
        This replaces the original video with your edit. The old file is sent to the Recycle Bin when possible. Replace works only inside BuffrX folders.
      </DialogBox>
      <ContextMenu at={menu ? { x: menu.x, y: menu.y } : null} label={menu?.kind === 'part' ? 'Trim part' : 'Clip'} items={menuItems} onClose={() => setMenu(null)} />
      <ShortcutsDialog isOpen={help} onClose={() => setHelp(false)} />
    </div>
  )
}
