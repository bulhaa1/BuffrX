// The sequence editor, all of it on one screen: the clips in order on the left, the picture under the
// playhead in the middle, the clip's own edit and the export in the tabs on the right, and the timeline
// across the bottom. Playback runs from one clip into the next; the export is one job. With no clip in
// the edit the same panes stand empty, and the picture area is the drop target.

import clsx from 'clsx'
import { useCallback, useEffect, useMemo, useRef, useState, type Dispatch, type SetStateAction } from 'react'
import { Icon } from '../components/icons'
import { Button, ContextMenu, DialogBox, IconButton, Slider, Tabs } from '../components/ui'
import { engine, type MediaItem } from '../lib/api'
import { mediaUrl, onShell, shell } from '../lib/bridge'
import { trimTimelineAt } from '../lib/editor_timeline'
import { canShareSaved } from '../lib/editor-share'
import { duration, fileName, stem, timecode } from '../lib/format'
import { setFullscreen as setWindowFullscreen } from '../lib/window'
import { mediaWork } from '../lib/media'
import type { MediaProbe } from '../lib/media_queue'
import { playbackStep, playerSlot, preloadTime, slotClipIndex } from '../lib/playback'
import { loadExportDefaults, saveExportDefaults } from '../lib/preferences'
import { moveAll, moveEdge, movePart, playbackJump, remove, split, SHORTEST, type Part } from '../lib/segments'
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

/** The details a player that holds no clip is given: the mix hook wants one, and this one is silent. */
const NO_DETAILS: MediaDetails = { ok: false, duration: 0, width: 0, height: 0, fps: 0, videoCodec: '', videoKbps: 0, bytes: 0, audio: [] }
const NO_VOLUMES: number[] = []

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

export function SequenceEditor({ clips, setClips, hover, dragging, suspended, clipsDir, focusClip, sourceProblem, onBrowse, onForget, onExportingChange }: {
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
}) {
  const [playhead, setPlayhead] = useState(0)
  const [playing, setPlaying] = useState(false)
  const wantPlay = useRef(false)
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
  const focusedSerial = useRef(0)
  useEffect(() => {
    if (!focusClip || focusedSerial.current === focusClip.serial) return
    const found = clips.findIndex((entry) => entry.id === focusClip.id)
    if (found < 0) return
    focusedSerial.current = focusClip.serial
    setPlaying(false)
    wantPlay.current = false
    setPlayhead(clipStart(clips, found))
  }, [clips, focusClip])

  const total = sequenceLength(clips)
  const at = Math.min(Math.max(0, playhead), total)
  const located = locate(clips, at)
  const index = Math.max(0, Math.min(clips.length - 1, located.index))
  const empty = clips.length === 0
  const clip = clips[index] ?? EMPTY_CLIP
  const sourceTime = located.sourceTime
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
          audio: entry.audio,
        }
      : NO_DETAILS
  const detailsA = useMemo(() => detailsOf(slotA), [slotA])
  const detailsB = useMemo(() => detailsOf(slotB), [slotB])
  const failed = !empty && failedPath === clip.path

  // Keyframes and sound, once per file, for as long as the window is open. The reads are shared and
  // capped (media_queue.ts): a walk through the clips leaves one process at a time, and leaving a clip
  // ends the read it had out once another needs its place, so nothing is scanned for a clip nobody is
  // looking at. A source replaced in place keeps its path, so `mediaEpoch` is what asks for the new
  // bytes to be read.
  const [media, setMedia] = useState<MediaProbe>({})
  const [mediaEpoch, setMediaEpoch] = useState(0)
  useEffect(() => {
    const path = clip.path
    if (!path) {
      setMedia({})
      return
    }
    let live = true
    const show = () => {
      if (live) setMedia(mediaWork.cached(path, PEAKS))
    }
    // A file already read draws at once, with no empty frame in between.
    show()
    void mediaWork.keyframes(path).then(show)
    void mediaWork.peaks(path, PEAKS).then(show)
    return () => {
      live = false
      mediaWork.release(path)
    }
  }, [clip.path, clip.bytes, mediaEpoch])

  const update = useCallback((patch: Partial<SequenceClip>) => {
    setClips((list) => list.map((entry, i) => (i === index ? { ...entry, ...patch } : entry)))
  }, [index, setClips])

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
    update({
      parts: next,
      history: record ? [...clip.history.slice(-49), clip.parts] : clip.history,
      future: record ? [] : clip.future,
    })
  }, [update, clip.history, clip.parts, clip.future])

  const undo = useCallback(() => {
    const previous = clip.history[clip.history.length - 1]
    if (!previous) return
    update({ parts: previous, history: clip.history.slice(0, -1), future: [...clip.future, clip.parts], active: Math.min(clip.active, previous.length - 1) })
  }, [clip, update])
  const redo = useCallback(() => {
    const next = clip.future[clip.future.length - 1]
    if (!next) return
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
  // Each player carries its own clip's tracks: only the one on screen is connected to the mix.
  const mixA = useTrackMix(players[0], slotA?.path ?? '', detailsA, slotA?.volumes ?? NO_VOLUMES, !slotA || activeSlot !== 0 || failedPath === slotA.path)
  const mixB = useTrackMix(players[1], slotB?.path ?? '', detailsB, slotB?.volumes ?? NO_VOLUMES, !slotB || activeSlot !== 1 || failedPath === slotB.path)
  const mix = activeSlot === 0 ? mixA : mixB

  // ---------------------------------------------------------------- playback

  const playFrom = (seconds: number) => {
    setPlayhead(Math.min(Math.max(0, seconds), total))
  }

  const togglePlay = useCallback(() => {
    const element = players[playerSlot(index)].current
    if (!element) return
    if (wantPlay.current) {
      wantPlay.current = false
      for (const player of players) player.current?.pause()
      setPlaying(false)
      return
    }
    if (at >= total - 0.05) setPlayhead(0)
    wantPlay.current = true
    void element.play().catch(() => undefined)
    setPlaying(true)
  }, [at, total, index])

  // A loaded player takes its place: the active one the playhead, the waiting one its clip's first
  // kept frame, so the boundary swaps two ready pictures instead of loading one.
  const prepare = (slot: 0 | 1) => {
    const element = players[slot].current
    const entry = slotClip(slot)
    if (!element || !entry) return
    element.playbackRate = entry.speed
    if (slot === activeSlot) element.currentTime = Math.max(0, Math.min(entry.duration, sourceRef.current))
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
  // Both players follow the playhead: the active one plays from the sequence's second, the other
  // waits on its clip's first frame until the boundary makes it active.
  useEffect(() => {
    if (trimming.current) return
    players.forEach((ref, slot) => {
      const element = ref.current
      const entry = slotClip(slot as 0 | 1)
      if (!element || !entry) return
      element.playbackRate = entry.speed
      if (element.readyState < 1) return
      const target = slot === activeSlot ? Math.min(entry.duration, sourceTime) : preloadTime(entry)
      if (Math.abs(element.currentTime - target) > 0.25) element.currentTime = target
      if (slot === activeSlot) {
        if (wantPlay.current && element.paused) void element.play().catch(() => undefined)
      } else if (!element.paused) element.pause()
    })
  }, [activeSlot, sourceTime, slotA, slotB, releaseTick])

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
        const end = clip.parts[clip.parts.length - 1][1]
        const step = playbackStep({
          wantPlay: wantPlay.current,
          paused: element.paused,
          ended: element.ended,
          currentTime: element.currentTime,
          end,
          last: index + 1 >= clips.length,
        })
        if (step === 'restart') void element.play().catch(() => undefined)
        else if (step === 'advance') {
          const jump = playbackJump(clip.parts, element.currentTime)
          if (jump !== null) element.currentTime = jump
          setPlayhead(clipStart(clips, index) + outputOffsetOfSource(clip, element.currentTime))
        } else if (step === 'handover') setPlayhead(clipStart(clips, index + 1))
        else if (step === 'finish') {
          wantPlay.current = false
          setPlaying(false)
          element.pause()
          setPlayhead(total)
        }
      }
      if (wantPlay.current) frame = requestAnimationFrame(tick)
    }
    frame = requestAnimationFrame(tick)
    return () => cancelAnimationFrame(frame)
  }, [playing, clip, index, clips, total, activeSlot])

  // A cut part is skipped when playback starts, as on the stage of the single-clip editor.
  const seekSource = useCallback((t: number) => {
    const jump = playbackJump(clip.parts, t)
    const next = jump === null ? t : jump
    setPlayhead(clipStart(clips, index) + outputOffsetOfSource(clip, next))
    const element = players[playerSlot(index)].current
    if (element) element.currentTime = next
  }, [clip, clips, index])

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
    seekSource(result.parts[result.active][0])
  }, [clip.parts, setParts, update, seekSource])
  const deletePart = useCallback(() => deletePartAt(current), [deletePartAt, current])
  const canSplit = split(clip.parts, sourceTime) !== null
  const canDelete = clip.parts.length > 1

  const applyEdge = useCallback((part: number, edge: 'in' | 'out', t: number, first: boolean, live: boolean) => {
    if (!clip.parts[part]) return
    setParts(moveEdge(clip.parts, part, edge, t, clip.duration, live ? 0 : SHORTEST), first)
  }, [clip.parts, clip.duration, setParts])
  const applyPart = useCallback((part: number, from: number, first: boolean) => {
    const entry = clip.parts[part]
    if (!entry) return
    setParts(movePart(clip.parts, part, from - entry[0], clip.duration), first)
  }, [clip.parts, clip.duration, setParts])
  const applyBar = useCallback((from: number, first: boolean) => {
    if (!clip.parts.length) return
    setParts(moveAll(clip.parts, from - clip.parts[0][0], clip.duration), first)
  }, [clip.parts, clip.duration, setParts])

  const moveClip = useCallback((from: number, to: number) => {
    const next = reorder(clips, from, to)
    if (next === clips) return
    setClips(next)
    setPlayhead(clipStart(next, to))
  }, [clips, setClips])

  // ---------------------------------------------------------------- export

  const [defaults] = useState(loadExportDefaults)
  const [quality, setQuality] = useState(defaults.quality)
  const [resolution, setResolution] = useState(defaults.resolution)
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
      // The save ends in Explorer, on the file: the card can say it, the folder shows it.
      void attempt('Not shown', () => engine.reveal({ file: result.output, kind: 'clip' } as MediaItem))
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
          setMediaEpoch((epoch) => epoch + 1)
          setClips([newClip({ path: result.output, name: fileName(result.output), duration: details.duration,
            width: details.width, height: details.height, fps: details.fps, bytes: details.bytes, audio: details.audio })])
          setPlayhead(0)
          setFailedPath(null)
          // The source was rewritten: its signature is no longer the one the save was measured
          // against, so the old card would immediately claim the edit changed. Start clean.
          setDone(null)
          players.forEach((ref) => ref.current?.load())
        }).catch(() => toast('info', 'Original replaced', 'Reopen the video before making further edits.'))
      }
      if (shareAfter.current && canShareSaved(shareSignature.current, signatureRef.current, shareIsCopy.current))
        void attempt('The share sheet did not open', () => engine.share(result.output))
      else if (shareAfter.current) toast('info', 'Clip saved', 'Your edit changed while saving. Save it again to share the latest version.')
    } else if (result.error !== 'cancelled') {
      toast('error', 'The export failed', result.error)
    }
    shareAfter.current = false
  }, [setClips, onExportingChange])

  const outHeight = resolution === 'original' ? 0 : Number(resolution)
  const base = clips[0] ?? EMPTY_CLIP
  // The picture that sets the output shape is the first clip's frame as it is kept: a portrait crop
  // exported at 720p is 720 tall and narrow, not a 16:9 stretch of it.
  const frame = base.crop.width > 0 && (base.crop.width < base.width || base.crop.height < base.height) ? base.crop : base
  const outSize = outHeight && outHeight < frame.height
    ? { width: Math.round((frame.width * outHeight) / frame.height / 2) * 2, height: outHeight }
    : null
  const mismatch = new Set(clips.map((entry) => entry.audio.length)).size > 1
  const canReplace = clips.length === 1 && !clips[0].timelineSession
  // A source problem belongs to the Timeline clip it names, so it only blocks an export the sequence would
  // actually read it for: an ordinary sequence of library clips exports while a session is still preparing.
  const exportable = total >= 0.5 && !!clipsDir && clips.length > 0 && !mismatch && (!sourceProblem || !clips.some((entry) => entry.timelineSession))

  const runExport = async (share: boolean, replace = false) => {
    if (exportBusy || !exportable || (replace && !canReplace)) return
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
    const suffix = clips.length === 1 ? 'edit' : 'sequence'
    const target = replace ? clips[0].path : await attempt('The export did not start', () => uniquePath(`${clipsDir}\\Edits\\${stem(clips[0].path)}-${suffix}.mp4`))
    if (target === undefined) { setExportBusy(false); onExportingChange?.(false); replacingRef.current = false; setReplacing(false); return }
    const inputs = clips.map(clipExportInput)
    const job = clips.length === 1
      ? { ...inputs[0], output: target, replace_original: replace, quality, out_width: outSize?.width ?? 0, out_height: outSize?.height ?? 0 }
      : { output: target, inputs, quality, out_width: outSize?.width ?? 0, out_height: outSize?.height ?? 0, fit_pad: true }
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
    setClips(next)
    onForget(id)
    if (next.length) setPlayhead(clipStart(next, Math.min(removed, next.length - 1)))
    else setPlayhead(0)
  }

  const trimTimeline = (side: 'from' | 'to') => {
    const next = trimTimelineAt(clips, index, sourceTime, side)
    if (!next || !clip.timelineSession) return
    setRangeUndo({ before: clips, after: next, at })
    setClips(next)
    wantPlay.current = false
    setPlaying(false)
    const matching = next.map((entry, i) => entry.timelineSession === clip.timelineSession ? i : -1).filter((i) => i >= 0)
    const target = side === 'from' ? matching[0] : matching[matching.length - 1]
    const boundary = clipStart(next, target) + (side === 'to' ? Math.max(0, clipOutputLength(next[target]) - 0.001) : 0)
    setPlayhead(boundary)
  }

  // The toolbar's zoom reads and writes the cut lane's view, around the playhead when it is on screen.
  const zoom = zoomOfSpan(view.span, clip.duration)
  const setZoom = (level: number) => {
    const anchor = sourceTime >= view.start && sourceTime <= view.start + view.span ? sourceTime : view.start + view.span / 2
    setView((current) => viewAtSpan(current, clip.duration, anchor, spanOfZoom(level, clip.duration)))
  }
  const stepZoom = (direction: 1 | -1) => setZoom(Math.min(1, Math.max(0, zoom + direction * 0.15)))
  const offsetInClip = at - clipStart(clips, index)
  const goPrevious = () => playFrom(offsetInClip > 0.5 || index === 0 ? clipStart(clips, index) : clipStart(clips, index - 1))
  const goNext = () => playFrom(clipStart(clips, index + 1))

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
      case 'back1': return seekSource(sourceTime - 1)
      case 'forward1': return seekSource(sourceTime + 1)
      case 'back5': return seekSource(sourceTime - 5)
      case 'forward5': return seekSource(sourceTime + 5)
      case 'frameBack': return seekSource(sourceTime - frameStep)
      case 'frameForward': return seekSource(sourceTime + frameStep)
      case 'clipStart': return seekSource(clip.parts[0][0])
      // The last frame, not the end itself: the end is where the next clip starts.
      case 'clipEnd': return seekSource(Math.max(clip.parts[0][0], clip.parts[clip.parts.length - 1][1] - frameStep))
      case 'previousClip': return goPrevious()
      case 'nextClip': return goNext()
      case 'sequenceStart': return playFrom(0)
      case 'sequenceEnd': return playFrom(total)
      case 'markIn': return setParts(moveEdge(clip.parts, current, 'in', sourceTime, clip.duration))
      case 'markOut': return setParts(moveEdge(clip.parts, current, 'out', sourceTime, clip.duration))
      case 'zoomIn': return stepZoom(1)
      case 'zoomOut': return stepZoom(-1)
      case 'zoomFit': return setView({ start: 0, span: Math.max(0.001, clip.duration) })
      case 'resetTrim':
        if (untouched) return
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
    const next = Math.min(Math.max(0, playheadRef.current + moved), total)
    playheadRef.current = next
    setPlayhead(next)
    const where = locate(clips, next)
    const element = players[playerSlot(index)].current
    if (element && where.index === index) element.currentTime = where.sourceTime
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
            time={at}
            total={total}
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
            { id: 'edit', label: 'Edit', icon: 'sliders', content: editPanel(<EditTab clip={clip} update={update} look={look} />) },
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
                  warning={sourceProblem || (mismatch ? TRACK_MISMATCH : null)}
                  replacing={replacing}
                  progress={exporting}
                  onCancel={(id) => void shell('media_cancel', { id })}
                  onSave={(share) => void runExport(share)}
                  onReplace={() => setConfirmReplace(true)}
                  done={done}
                  doneStale={doneSignature !== exportSignature}
                  doneShareable={doneShareable}
                  shareable={canShareSaved(doneSignature, exportSignature, doneShareable)}
                  onShare={() => done && void attempt('The share sheet did not open', () => engine.share(done))}
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
          <div className="mt-1 grid h-[40px] shrink-0 grid-cols-[1fr_auto_1fr] items-center gap-2 border-b border-line px-2">
            <div className="flex items-center">
              <IconButton icon="undo" label="Undo (Ctrl+Z)" isDisabled={clip.history.length === 0} onPress={undo} />
              <IconButton icon="undo" label="Redo (Ctrl+Y)" className="-scale-x-100" isDisabled={clip.future.length === 0} onPress={redo} />
              <span className="mx-2 h-5 w-px bg-line" aria-hidden="true" />
              <IconButton icon="scissors" label="Cut at the playhead (S or C)" isDisabled={!canSplit} onPress={splitHere} />
              <IconButton icon="trash" label="Remove this clip (Shift+Delete)" onPress={() => takeOut(clip.id)} />
            </div>
            <span className="num min-w-[96px] border border-line bg-canvas px-3 py-1 text-center text-[14px] font-bold text-ink">{timecode(at)}</span>
            <div className="flex items-center justify-end gap-1">
              <IconButton icon="zoomOut" label="Zoom out (-)" isDisabled={zoom <= 0} onPress={() => stepZoom(-1)} />
              <Slider label="Timeline zoom" value={Math.round(zoom * 100)} min={0} max={100} step={1} onChange={(value) => setZoom(value / 100)} onCommit={(value) => setZoom(value / 100)} className="w-[140px]" />
              <IconButton icon="zoomIn" label="Zoom in (+)" isDisabled={zoom >= 1} onPress={() => stepZoom(1)} />
              <span className="mx-2 h-5 w-px bg-line" aria-hidden="true" />
              <IconButton icon="keyboard" label="Keyboard shortcuts (?)" onPress={() => setHelp(true)} />
            </div>
          </div>
        )}
        <div className="flex min-h-0 flex-col gap-2 overflow-y-auto p-2" style={{ maxHeight: '46vh' }}>
          {empty ? (
            <div className="grid h-[88px] place-items-center border border-line bg-canvas px-4 text-center text-[14px] text-faint">Added clips appear here.</div>
          ) : (
            <>
              <SequenceLanes clips={clips} active={index} time={at} onSelect={(i) => playFrom(clipStart(clips, i))} onMenu={(i, x, y) => setMenu({ kind: 'clip', index: i, x, y })} onSeek={playFrom} onReorder={moveClip} />
              {clip.timelineSession && (
                <div className="flex flex-wrap items-center gap-2">
                  <span className="text-[13px] text-muted">Trim Timeline at playhead</span>
                  <Button variant="ghost" isDisabled={!!sourceProblem} onPress={() => trimTimeline('from')}>Keep from here</Button>
                  <Button variant="ghost" isDisabled={!!sourceProblem} onPress={() => trimTimeline('to')}>Keep to here</Button>
                  {rangeUndo?.after === clips && <Button variant="ghost" onPress={() => {
                    setClips(rangeUndo.before)
                    setPlayhead(rangeUndo.at)
                    setRangeUndo(null)
                  }}>Undo Timeline trim</Button>}
                </div>
              )}
              <ClipTimeline
                duration={clip.duration}
                time={sourceTime}
                parts={clip.parts}
                active={current}
                tracks={clip.audio}
                volumes={clip.volumes}
                view={view}
                onView={setView}
                onSelect={(part) => update({ active: part })}
                onPartMenu={(part, x, y) => setMenu({ kind: 'part', index: part, x, y })}
                onEdge={applyEdge}
                onPart={applyPart}
                onBar={applyBar}
                onSeek={seekSource}
                onDrag={onTrimDrag}
                flash={splitFlash}
                peaks={media.peaks}
                keyframes={media.keyframes}
              />
              {mismatch && <p className="text-[13px] leading-snug text-warn">{TRACK_MISMATCH}</p>}
              {mix !== 'ready' && clip.audio.length > 1 && (
                <p className="text-[13px] text-muted">{mix === 'loading' ? 'Preparing the tracks for the preview…' : 'The preview plays the first track only. The save uses every track.'}</p>
              )}
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
