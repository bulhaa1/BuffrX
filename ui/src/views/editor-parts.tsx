// The parts the editor's screens share: the crop frame, the fitted picture, the colour filter and the
// export choices. Nothing here knows about a clip or a sequence, so both the picker's editor and the
// image editor use the same ones.

import { useEffect, useRef, useState } from 'react'
import { mediaUrl, shell } from '../lib/bridge'
import { runEngine } from '../lib/media'
import { dragCrop, type Crop } from '../lib/sequence'

export interface AudioTrack {
  index: number
  channels: number
  title: string
}

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
    audio: AudioTrack[]
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
    audio: raw.audio ?? [],
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

/** A short, stable name for a clip's cached track files: FNV-1a over its path and size. */
function trackCacheKey(path: string, size: number): string {
  let hash = 0x811c9dc5
  for (const c of `${path.toLowerCase()}|${size}`) {
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

/**
 * The preview's sound as the save will mix it. A browser plays only a file's first audio track, and an
 * element's own volume stops at 100%. So the video's sound (track 1) goes through a gain, and every
 * further track is copied out once (`--media-audio`, cached in the data root) and played in step with
 * the picture through a gain of its own. 'failed' leaves the first track playing as before.
 */
export function useTrackMix(video: React.RefObject<HTMLVideoElement | null>, path: string, details: MediaDetails, volumes: number[], off: boolean): 'loading' | 'ready' | 'failed' {
  const [state, setState] = useState<'loading' | 'ready' | 'failed'>('loading')
  const gains = useRef<GainNode[]>([])
  useEffect(() => {
    const element = video.current
    if (!element || off || !details.audio.length) return
    let gone = false
    const source = mixSource(element)
    const context = mixContext!
    const extras: HTMLAudioElement[] = []
    const first = context.createGain()
    source.connect(first).connect(context.destination)
    gains.current = [first]
    // Follow the picture: play, pause, seek and speed, and pull a track back when it drifts.
    const follow = () => {
      for (const audio of extras) {
        if (Math.abs(audio.currentTime - element.currentTime) > 0.06) audio.currentTime = element.currentTime
        audio.playbackRate = element.playbackRate
        if (element.paused && !audio.paused) audio.pause()
        else if (!element.paused && audio.paused) void audio.play().catch(() => undefined)
      }
    }
    const resume = () => void context.resume().catch(() => undefined)
    const events = ['play', 'pause', 'seeked', 'ratechange'] as const
    for (const name of events) element.addEventListener(name, follow)
    element.addEventListener('play', resume)
    const timer = window.setInterval(() => !element.paused && follow(), 250)
    void (async () => {
      if (details.audio.length < 2) return setState('ready')
      try {
        const root = await shell<string>('get_data_root')
        const key = trackCacheKey(path, details.bytes)
        for (let track = 1; track < details.audio.length; track++) {
          const file = `${root}\\cache\\audio\\${key}-${track}.m4a`
          const answer = (await runEngine(['--media-audio', path, String(track), file])) as { ok: boolean; error?: string }
          if (!answer.ok) throw new Error(answer.error ?? 'the track could not be read')
          if (gone) return
          const audio = new Audio()
          audio.crossOrigin = 'anonymous'
          audio.preload = 'auto'
          audio.src = mediaUrl(file)
          const gain = context.createGain()
          mixSource(audio).connect(gain).connect(context.destination)
          extras.push(audio)
          gains.current.push(gain)
        }
        if (gone) return
        follow()
        setState('ready')
      } catch {
        if (!gone) setState('failed')
      }
    })()
    return () => {
      gone = true
      window.clearInterval(timer)
      for (const name of events) element.removeEventListener(name, follow)
      element.removeEventListener('play', resume)
      for (const audio of extras) {
        audio.pause()
        audio.removeAttribute('src')
        mixSources.get(audio)?.disconnect()
      }
      source.disconnect()
      for (const gain of gains.current) gain.disconnect()
      gains.current = []
    }
  }, [video, path, details, off])
  // Every gain at the track's own volume, up to the 200% the save allows.
  useEffect(() => {
    gains.current.forEach((gain, index) => (gain.gain.value = volumes[index] ?? 1))
    // `off` turns a sequence's other player down and back up: its gains are rebuilt at 1 when it
    // becomes active again, so the levels must be written then too.
  }, [volumes, state, off])
  return state
}
