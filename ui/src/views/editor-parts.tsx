// The parts the editor's screens share: the crop frame, the fitted picture, the colour filter and the
// export choices. Nothing here knows about a clip or a sequence, so both the picker's editor and the
// image editor use the same ones.

import { useCallback, useEffect, useRef, useState } from 'react'
import { mediaUrl, shell } from '../lib/bridge'
import { audioLayout, elementGain, extraGain, extraTracks, monitorMode, whenPlayable, type AudioTrack } from '../lib/audio'
import { cancelEngine, runEngine } from '../lib/media'
import { dragCrop, type Crop } from '../lib/sequence'

/** One audio track of a file, as the engine reports it (src/engine/media_export.cpp). */
export type { AudioTrack }

/** What `--media-info` reports about a file (src/engine/media_export.cpp). */
export interface MediaDetails {
  ok: boolean
  error?: string
  duration: number
  width: number
  height: number
  fps: number
  videoCodec: string
  videoKbps: number
  bytes: number
  /** Last write time, seconds since 1970: a replaced clip keeps its path and may keep its size. */
  modified: number
  audio: AudioTrack[]
}

export const VIDEO = /\.(mp4|mkv|mov|webm|m4v)$/i
export const IMAGE = /\.(png|jpe?g)$/i

/** A path nothing else uses. A failure is an error, never the path that was asked for: that one may exist,
 *  and a save onto it would overwrite a clip the person still wanted. */
export async function uniquePath(path: string): Promise<string> {
  return shell<string>('unique_path', { path })
}

/** The file's own figures, read by a short-lived engine process. */
export async function loadDetails(path: string): Promise<MediaDetails> {
  const raw = (await runEngine(['--media-info', path])) as {
    ok: boolean
    error?: string
    duration: number
    width: number
    height: number
    fps: number
    video_codec: string
    video_kbps: number
    bytes: number
    modified?: number
    audio: { index: number; channels: number; title?: string; role?: string; sources?: string[] }[]
  }
  return {
    ok: raw.ok,
    error: raw.error,
    duration: raw.duration ?? 0,
    width: raw.width ?? 0,
    height: raw.height ?? 0,
    fps: raw.fps ?? 0,
    videoCodec: raw.video_codec ?? '',
    videoKbps: raw.video_kbps ?? 0,
    bytes: raw.bytes ?? 0,
    modified: raw.modified ?? 0,
    // The labels and roles come from the file's own sidecar, read by the engine; an imported file's
    // tracks arrive as 'track' with no sources (lib/audio.ts).
    audio: (raw.audio ?? []).map((track) => ({
      index: track.index,
      channels: track.channels,
      title: track.title ?? '',
      role: track.role ?? 'track',
      sources: track.sources ?? [],
    })),
  }
}

export const ASPECTS: { value: string; label: string; ratio: number | null }[] = [
  { value: 'off', label: 'Off', ratio: null },
  { value: 'free', label: 'Free', ratio: null },
  { value: '16:9', label: '16:9', ratio: 16 / 9 },
  { value: '9:16', label: '9:16', ratio: 9 / 16 },
  { value: '1:1', label: '1:1', ratio: 1 },
  { value: '4:3', label: '4:3', ratio: 4 / 3 },
]

/** The largest rectangle of `ratio` centred in the frame. */
export function centredCrop(width: number, height: number, ratio: number | null): Crop {
  if (!ratio) return { x: 0, y: 0, width, height }
  let w = width
  let h = Math.round(w / ratio)
  if (h > height) {
    h = height
    w = Math.round(h * ratio)
  }
  return { x: Math.round((width - w) / 2), y: Math.round((height - h) / 2), width: w, height: h }
}

export const gradeFilter = (grade: { brightness: number; contrast: number; saturation: number }) =>
  `brightness(${grade.brightness}%) contrast(${grade.contrast}%) saturate(${grade.saturation}%)`

export function CropOverlay({ frame, crop, onCrop, ratio }: {
  frame: { width: number; height: number } // source pixels
  crop: Crop
  onCrop: (crop: Crop) => void
  ratio: number | null
}) {
  const box = useRef<HTMLDivElement>(null)
  const drag = useRef<{ mode: string; startX: number; startY: number; start: Crop } | null>(null)
  const toSource = (dx: number, dy: number) => {
    const rect = box.current!.getBoundingClientRect()
    return { x: (dx / rect.width) * frame.width, y: (dy / rect.height) * frame.height }
  }
  const onDown = (mode: string) => (event: React.PointerEvent) => {
    event.stopPropagation()
    ;(event.target as HTMLElement).setPointerCapture(event.pointerId)
    drag.current = { mode, startX: event.clientX, startY: event.clientY, start: crop }
  }
  const onMove = (event: React.PointerEvent) => {
    const d = drag.current
    if (!d) return
    const delta = toSource(event.clientX - d.startX, event.clientY - d.startY)
    onCrop(dragCrop(frame, d.start, d.mode, delta.x, delta.y, ratio))
  }
  const onUp = () => (drag.current = null)
  // A focused frame or handle takes the arrows (ten source pixels with Shift), so a crop is reachable
  // without a pointer. Every press is its own edit, as one drag is.
  const onKey = (mode: string) => (event: React.KeyboardEvent) => {
    if (event.altKey || event.ctrlKey || event.metaKey) return
    const step = event.shiftKey ? 10 : 1
    const horizontal = mode === 'move' || mode.includes('e') || mode.includes('w')
    const vertical = mode === 'move' || mode.includes('n') || mode.includes('s')
    let dx = 0
    let dy = 0
    if (horizontal && event.key === 'ArrowLeft') dx = -step
    else if (horizontal && event.key === 'ArrowRight') dx = step
    else if (vertical && event.key === 'ArrowUp') dy = -step
    else if (vertical && event.key === 'ArrowDown') dy = step
    if (!dx && !dy) {
      // This handle owns the other axis: the key must not fall through to the frame's move handler.
      if (event.key.startsWith('Arrow')) {
        event.preventDefault()
        event.stopPropagation()
      }
      return
    }
    event.preventDefault()
    event.stopPropagation()
    onCrop(dragCrop(frame, crop, mode, dx, dy, ratio))
  }
  const pct = (v: number, of: number) => `${(v / of) * 100}%`
  const handles = ['nw', 'n', 'ne', 'e', 'se', 's', 'sw', 'w']
  // The area outside the crop is dimmed with four panels rather than one enormous box shadow: a
  // shadow that big spills over the whole window and paints the editor's own controls, which is
  // what "the video is clipping through the interface" was.
  const dim = 'pointer-events-none absolute'
  // What the crop removes is dimmed the same way a cut in time is: one mark, "this is not in the
  // export", whichever direction it was taken in.
  const dimStyle = { ...CUT_STYLE, backgroundColor: 'color-mix(in srgb, var(--bx-canvas) 74%, transparent)' }
  return (
    <div ref={box} className="absolute inset-0" onPointerMove={onMove} onPointerUp={onUp} onPointerCancel={onUp}>
      <div className={dim} style={{ ...dimStyle, left: 0, top: 0, width: '100%', height: pct(crop.y, frame.height) }} />
      <div
        className={dim}
        style={{ ...dimStyle, left: 0, top: pct(crop.y + crop.height, frame.height), width: '100%', height: pct(frame.height - crop.y - crop.height, frame.height) }}
      />
      <div className={dim} style={{ ...dimStyle, left: 0, top: pct(crop.y, frame.height), width: pct(crop.x, frame.width), height: pct(crop.height, frame.height) }} />
      <div
        className={dim}
        style={{
          ...dimStyle,
          left: pct(crop.x + crop.width, frame.width),
          top: pct(crop.y, frame.height),
          width: pct(frame.width - crop.x - crop.width, frame.width),
          height: pct(crop.height, frame.height),
        }}
      />
      <div
        role="group"
        tabIndex={0}
        aria-label="Move crop"
        className="absolute cursor-move border-2 border-accent bx-pop-in"
        style={{
          left: pct(crop.x, frame.width),
          top: pct(crop.y, frame.height),
          width: pct(crop.width, frame.width),
          height: pct(crop.height, frame.height),
        }}
        onPointerDown={onDown('move')}
        onKeyDown={onKey('move')}
      >
        <div className="pointer-events-none absolute inset-0 grid grid-cols-3 grid-rows-3">
          {Array.from({ length: 9 }, (_, i) => <div key={i} className="border border-white/10" />)}
        </div>
        {handles.map((h) => (
          <span
            key={h}
            role="button"
            tabIndex={0}
            aria-label={`Resize crop ${h}`}
            onPointerDown={onDown(h)}
            onKeyDown={onKey(h)}
            className="absolute h-3 w-3 border-2 border-accent bg-canvas"
            style={{
              left: h.includes('w') ? -7 : h.includes('e') ? 'calc(100% - 5px)' : 'calc(50% - 6px)',
              top: h.includes('n') ? -7 : h.includes('s') ? 'calc(100% - 5px)' : 'calc(50% - 6px)',
              cursor: `${h}-resize`,
            }}
          />
        ))}
        <span className="num absolute bottom-0 left-0 bg-accent px-1.5 text-[13px] font-bold text-on-accent">{crop.width} × {crop.height}</span>
      </div>
    </div>
  )
}

/** Fits the media inside the stage while keeping its aspect, so the crop overlay lines up with it.
 *
 *  With a crop it is the crop that fits the stage: the picture is then drawn larger than the stage
 *  and overflows it, dimmed past the crop's frame. A 16:9 picture in a taller pane leaves a wide band
 *  above and below it - that band becomes the board the crop is cut on, and a 9:16 crop gets the
 *  pane's whole height instead of a strip of it. Where the picture is not cropping, what is centred
 *  is the picture itself and the result is the plain fit it always was. */
export function useFitted(container: React.RefObject<HTMLDivElement | null>, width: number, height: number, crop?: Crop) {
  const [box, setBox] = useState({ width: 0, height: 0, left: 0, top: 0 })
  const cropX = crop?.x ?? 0
  const cropY = crop?.y ?? 0
  const cropWidth = crop?.width ?? 0
  const cropHeight = crop?.height ?? 0
  useEffect(() => {
    const element = container.current
    if (!element || !width || !height) return
    const measure = () => {
      const rect = element.getBoundingClientRect()
      // The border box is not the space the picture may use: the stage has a padding, and sizing the
      // picture to the border box makes it wider than the stage, so it reaches over the inspector.
      // That is what "the video is clipping through the interface" was.
      const style = window.getComputedStyle(element)
      const availableWidth = rect.width - parseFloat(style.paddingLeft || '0') - parseFloat(style.paddingRight || '0')
      const availableHeight = rect.height - parseFloat(style.paddingTop || '0') - parseFloat(style.paddingBottom || '0')
      if (availableWidth <= 0 || availableHeight <= 0) return
      const frameWidth = cropWidth > 0 ? cropWidth : width
      const frameHeight = cropHeight > 0 ? cropHeight : height
      const scale = Math.min(availableWidth / frameWidth, availableHeight / frameHeight)
      setBox({
        width: Math.floor(width * scale),
        height: Math.floor(height * scale),
        left: Math.round((availableWidth - frameWidth * scale) / 2 - cropX * scale),
        top: Math.round((availableHeight - frameHeight * scale) / 2 - cropY * scale),
      })
    }
    measure()
    const observer = new ResizeObserver(measure)
    observer.observe(element)
    return () => observer.disconnect()
  }, [container, width, height, cropX, cropY, cropWidth, cropHeight])
  return box
}

/** The cut's own mark: a flat light tint on the ground, no stripes. A dark fill on a dark lane read as a band of
 *  nothing rather than as the gap it is. */
export const CUT_STYLE = {
  backgroundColor: 'color-mix(in srgb, var(--bx-fg) 7%, transparent)',
}

export const QUALITY: { value: number; label: string; hint: string }[] = [
  { value: 0, label: 'Original', hint: 'No quality loss' },
  { value: 0.9, label: 'High', hint: '90% of the bitrate' },
  { value: 0.65, label: 'Medium', hint: '65%, about a third smaller' },
  { value: 0.4, label: 'Low', hint: '40%, smallest for sharing' },
]

/** One speed for a whole clip: a clip that ran fast halfway is two edits, not one. */
export const SPEEDS = [0.5, 0.75, 1, 1.25, 1.5, 2]

// ---------------------------------------------------------------- the preview's sound

/** A short, stable name for a clip's cached track files: FNV-1a over its path, size and write time. The
 *  write time is part of it because a replaced clip keeps its path and may keep its size. */
function trackCacheKey(path: string, size: number, modified: number): string {
  let hash = 0x811c9dc5
  for (const c of `${path.toLowerCase()}|${size}|${modified}`) {
    hash ^= c.charCodeAt(0)
    hash = Math.imul(hash, 0x01000193) >>> 0
  }
  return hash.toString(16).padStart(8, '0')
}

// One audio graph for the window, and one source per element: an element can be routed into Web Audio
// only once, so a second run of the effect (React's StrictMode, a remount) reuses what the first made.
let mixContext: AudioContext | undefined
const mixSources = new WeakMap<HTMLMediaElement, MediaElementAudioSourceNode>()
function mixSource(element: HTMLMediaElement): MediaElementAudioSourceNode {
  mixContext ??= new AudioContext()
  let source = mixSources.get(element)
  if (!source) {
    source = mixContext.createMediaElementSource(element)
    mixSources.set(element, source)
  }
  return source
}

// Every track copy this window has asked for, so a run that is left behind can end its process by name.
let readSequence = 0

/**
 * The preview's sound as the save will mix it. A browser plays only a file's first audio track, and an
 * element's own volume stops at 100%, so the tracks the editor must represent are played through a gain
 * each: the element's own track, and every further track copied out once (`--media-audio`, cached in the
 * data root).
 *
 * Which tracks those are comes from the file's own layout (`lib/audio.ts`), never from the current
 * capture settings: a BuffrX recording whose track 1 is the mix of the tracks after it is monitored
 * through those sources alone, so nothing is heard twice; when the mix holds sound no track carries, the
 * mix is what plays, so nothing is dropped. 'failed' falls back to the mix and says so.
 *
 * The work is per clip and cancellable: leaving a clip, pausing the editor or closing the window kills
 * a copy that is still being made, and no process is left running behind a window nobody is looking at.
 */
export interface TrackMix {
  state: 'loading' | 'ready' | 'failed'
  /** Ask again after a failed copy: the reads are not cached, so a busy engine is tried afresh. */
  retry: () => void
}

export function useTrackMix(video: React.RefObject<HTMLVideoElement | null>, path: string, details: MediaDetails, gains: number[], off: boolean): TrackMix {
  const [state, setState] = useState<'loading' | 'ready' | 'failed'>('loading')
  const [attempt, setAttempt] = useState(0)
  const retry = useCallback(() => setAttempt((count) => count + 1), [])
  const levels = useRef<Map<number, GainNode>>(new Map())
  const gainsRef = useRef(gains)
  gainsRef.current = gains
  const mode = monitorMode(audioLayout(details.audio), details.audio.length)
  const modeRef = useRef(mode)
  modeRef.current = mode
  const stateRef = useRef(state)
  stateRef.current = state

  // The levels are written from one place, so a gain made after a mute was set starts muted: the async
  // copies used to be added at full volume and only picked up the level on the next unrelated change.
  const applyLevels = useCallback(() => {
    for (const [index, gain] of levels.current) {
      gain.gain.value = index === 0
        ? elementGain(modeRef.current, gainsRef.current, stateRef.current)
        : extraGain(gainsRef.current, index, stateRef.current)
    }
  }, [])

  // A replaced clip keeps its path, so its size and write time are what say its cached copies are stale.
  const signature = `${path}|${details.bytes}|${details.modified}|${details.audio.length}`

  useEffect(() => {
    const element = video.current
    levels.current = new Map()
    if (!element || !details.audio.length) {
      setState('ready')
      return
    }
    if (off) return
    let gone = false
    const context = mixContext ?? (mixContext = new AudioContext())
    const source = mixSource(element)
    const own = context.createGain()
    source.connect(own).connect(context.destination)
    levels.current.set(0, own)
    // A graph made while the picture is already playing never sees a play event: start it here.
    void context.resume().catch(() => undefined)
    // A retry after a failed copy starts from loading again, so an aggregate mix is not heard beside
    // the sources that are being made ready.
    stateRef.current = 'loading'
    setState('loading')
    applyLevels()
    const extras: HTMLAudioElement[] = []
    // Every copy this effect made, ready or still loading: leaving the clip owns all of them, so a wait
    // that is still running leaves no src, listener or timer behind.
    const owned: HTMLAudioElement[] = []
    // Cancels a readiness wait that is still running when the clip is left or replaced.
    const readiness = new AbortController()
    const ids: string[] = []
    let failedOnce = false
    // The drift check runs only while the picture plays: a paused clip holds no repeating timer.
    let ticker = 0
    const stopTicker = () => {
      if (ticker) window.clearInterval(ticker)
      ticker = 0
    }
    // Only copies that are ready need following: an element or mix-only file has none, and used to make
    // a 250 ms interval on every play that did nothing at all.
    const startTicker = () => {
      if (gone || failedOnce || !extras.length) return
      if (!ticker) ticker = window.setInterval(() => follow(), 250)
    }
    // A copy that cannot play must never be counted as ready: the mix falls back and says so, with Retry.
    const fail = () => {
      if (gone || failedOnce) return
      failedOnce = true
      stopTicker()
      for (const audio of extras) audio.pause()
      stateRef.current = 'failed'
      setState('failed')
    }
    // A copy that fails after it was ready (a decode error mid-play) is still owned by this graph.
    const onCopyError = () => fail()
    // Follow the picture: play, pause, seek and speed, and pull a track back when it drifts.
    const follow = () => {
      if (gone || failedOnce) return
      for (const audio of extras) {
        if (Math.abs(audio.currentTime - element.currentTime) > 0.06) audio.currentTime = element.currentTime
        audio.playbackRate = element.playbackRate
        if (element.paused && !audio.paused) audio.pause()
        else if (!element.paused && audio.paused) {
          // A play the browser refuses (an autoplay policy, a stalled copy) is a real failure of the
          // preview, not a silent success: fall back to the mix rather than play one track of several.
          void audio.play().catch(() => {
            if (!element.paused && stateRef.current === 'ready') fail()
          })
        }
      }
    }
    const resume = () => void context.resume().catch(() => undefined)
    const onPlay = () => {
      if (gone || failedOnce) return
      resume()
      startTicker()
      follow()
    }
    const onPause = () => {
      stopTicker()
      follow()
    }
    element.addEventListener('play', onPlay)
    element.addEventListener('pause', onPause)
    element.addEventListener('seeked', follow)
    element.addEventListener('ratechange', follow)
    // A graph made while the picture is already playing never sees a play event: start it here.
    if (!element.paused) startTicker()
    void (async () => {
      const wanted = extraTracks(mode, details.audio.length)
      if (!wanted.length) {
        stopTicker()
        if (!gone) setState('ready')
        return
      }
      try {
        const root = await shell<string>('get_data_root')
        if (gone) return
        const key = trackCacheKey(path, details.bytes, details.modified)
        for (const track of wanted) {
          const file = `${root}\\cache\\audio\\${key}-${track}.m4a`
          const id = `track-${++readSequence}`
          ids.push(id)
          const answer = (await runEngine(['--media-audio', path, String(track), file], id)) as { ok: boolean; error?: string }
          if (gone) return
          if (!answer.ok) throw new Error(answer.error ?? 'the track could not be read')
          const audio = new Audio()
          audio.crossOrigin = 'anonymous'
          audio.preload = 'auto'
          // The copy is owned from the moment it exists, so a decode failure after it is ready is still
          // seen (the readiness wait takes its own listeners back) and leaving mid-wait can clean it.
          audio.addEventListener('error', onCopyError)
          owned.push(audio)
          audio.src = mediaUrl(file)
          // Assigning a src is not a playable track: an unsupported codec or a truncated copy reports its
          // error here, and the wait is bounded so a copy that never arrives cannot read as ready.
          await whenPlayable(audio, 12000, readiness.signal)
          if (gone || failedOnce) return
          // The seek and the speed are written once the copy can take them, never on an empty element.
          audio.currentTime = element.currentTime
          audio.playbackRate = element.playbackRate
          const gain = context.createGain()
          mixSource(audio).connect(gain).connect(context.destination)
          extras.push(audio)
          levels.current.set(track, gain)
          applyLevels()
        }
        if (gone || failedOnce) return
        // A play promise can reject before React renders the state update. Publish readiness to
        // the callback first, so that rejection cannot leave a silent copy reported as ready.
        stateRef.current = 'ready'
        setState('ready')
        if (!element.paused) startTicker()
        follow()
      } catch {
        // A cancelled wait is the view leaving, not a copy that failed.
        if (gone) return
        // The mix is playing instead; stop following copies that will never carry sound.
        fail()
      }
    })()
    return () => {
      gone = true
      // A wait still running ends now, and every copy this effect made lets go of its src and listeners,
      // ready or not: a clip left mid-read used to keep its element alive for the whole wait.
      readiness.abort()
      // A copy still being read is a process of its own: it ends with the view that wanted it.
      for (const id of ids) cancelEngine(id)
      stopTicker()
      element.removeEventListener('play', onPlay)
      element.removeEventListener('pause', onPause)
      element.removeEventListener('seeked', follow)
      element.removeEventListener('ratechange', follow)
      for (const audio of owned) {
        audio.removeEventListener('error', onCopyError)
        audio.pause()
        audio.removeAttribute('src')
        audio.load()
        mixSources.get(audio)?.disconnect()
      }
      source.disconnect()
      for (const gain of levels.current.values()) gain.disconnect()
      levels.current = new Map()
    }
    // The clip and its bytes decide the graph; a level is written by the effect below, so changing one
    // never restarts a copy.
  }, [video, signature, off, applyLevels, mode, attempt]) // eslint-disable-line react-hooks/exhaustive-deps -- details are read through refs

  useEffect(() => {
    applyLevels()
    // `off` turns a sequence's other player down and back up: its gains are rebuilt when it becomes
    // active again, so the levels must be written then too.
  }, [gains, state, off, mode, applyLevels])
  return { state, retry }
}
