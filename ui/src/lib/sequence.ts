// The editor's sequence: an ordered list of clips that stay separate until the export joins them.
// Each clip keeps its own parts, crop, colour, speed and volumes; the sequence only decides the order
// and how long each clip is on the timeline after its speed. Pure functions, tested on their own.

import type { Part } from './segments.ts'
import { keptLength } from './segments.ts'

export interface Crop {
  x: number
  y: number
  width: number
  height: number
}
/** Percentage of the picture's own brightness, contrast and saturation; 100 is the file untouched. */
export interface Grade {
  brightness: number
  contrast: number
  saturation: number
}
export const FLAT: Grade = { brightness: 100, contrast: 100, saturation: 100 }

/** One audio track of a source, as the cut lane names it and the preview mixes it. */
export interface ClipTrack {
  index: number
  channels: number
  title: string
}

/** One clip as the sequence holds it: a reference to the file and the edits made to it. */
export interface SequenceClip {
  /** A temporary Timeline chunk must never be replaced in place. */
  timelineSession?: string
  /** A Timeline range removed whole chunks, even if this chunk itself is unchanged. */
  timelineTrimmed?: boolean
  /** The file's path, lower case: one clip appears once in a sequence. */
  id: string
  path: string
  name: string
  /** The source's own length in seconds. */
  duration: number
  width: number
  height: number
  fps: number
  /** The file's size, which names its cached preview tracks. */
  bytes: number
  audio: ClipTrack[]
  /** The parts of the source that are kept, in order. A split and a delete leave gaps. */
  parts: Part[]
  /** The part under the playhead in the cut lane. */
  active: number
  history: Part[][]
  future: Part[][]
  aspect: string
  crop: Crop
  /** Per audio track, 1 is unchanged. */
  volumes: number[]
  grade: Grade
  /** 2 plays twice as fast, so the clip is half as long on the sequence. */
  speed: number
}

/** What the engine needs to export one clip of the sequence. */
export interface ExportInput {
  input: string
  segments: Part[]
  crop_x: number
  crop_y: number
  crop_width: number
  crop_height: number
  grade_brightness: number
  grade_contrast: number
  grade_saturation: number
  speed: number
  volumes: number[]
}

/** What the picker knows about a file before it is a clip of the sequence. */
export interface ClipSource {
  path: string
  name: string
  duration: number
  width: number
  height: number
  fps: number
  bytes: number
  audio: ClipTrack[]
}

export function newClip(source: ClipSource): SequenceClip {
  return {
    id: source.path.toLowerCase(),
    path: source.path,
    name: source.name,
    duration: source.duration,
    width: source.width,
    height: source.height,
    fps: source.fps,
    bytes: source.bytes,
    audio: source.audio,
    parts: [[0, source.duration]],
    active: 0,
    history: [],
    future: [],
    aspect: 'off',
    crop: { x: 0, y: 0, width: source.width, height: source.height },
    volumes: Array.from({ length: source.audio.length }, () => 1),
    grade: FLAT,
    speed: 1,
  }
}

const speedOf = (clip: SequenceClip) => (clip.speed > 0 ? clip.speed : 1)

/** How long the clip is on the sequence: what it keeps, divided by its speed. */
export function clipOutputLength(clip: SequenceClip): number {
  return keptLength(clip.parts) / speedOf(clip)
}

/** The whole sequence, in seconds of output time. */
export function sequenceLength(clips: SequenceClip[]): number {
  return clips.reduce((sum, clip) => sum + clipOutputLength(clip), 0)
}

/** Where a clip begins on the sequence. */
export function clipStart(clips: SequenceClip[], index: number): number {
  let start = 0
  for (let i = 0; i < index && i < clips.length; i++) start += clipOutputLength(clips[i])
  return start
}

/** The source second a clip shows at `offset` seconds into it on the sequence. */
export function sourceTimeAt(clip: SequenceClip, offset: number): number {
  const speed = speedOf(clip)
  let cursor = 0
  for (let i = 0; i < clip.parts.length; i++) {
    const [from, to] = clip.parts[i]
    const length = Math.max(0, to - from) / speed
    const last = i === clip.parts.length - 1
    if (offset < cursor + length || last) return Math.min(to, from + Math.max(0, offset - cursor) * speed)
    cursor += length
  }
  return 0
}

/** Where in the clip's output a source second sits. A second inside a cut gap sits at the gap. */
export function outputOffsetOfSource(clip: SequenceClip, sourceTime: number): number {
  const speed = speedOf(clip)
  let cursor = 0
  for (const [from, to] of clip.parts) {
    if (sourceTime < from) return cursor
    if (sourceTime < to) return cursor + (sourceTime - from) / speed
    cursor += Math.max(0, to - from) / speed
  }
  return cursor
}

/** The clip the sequence playhead is in, and where it is inside it. */
export function locate(clips: SequenceClip[], outputTime: number): { index: number; offset: number; sourceTime: number } {
  let start = 0
  for (let i = 0; i < clips.length; i++) {
    const length = clipOutputLength(clips[i])
    const last = i === clips.length - 1
    if (outputTime < start + length || last) {
      const offset = Math.min(Math.max(0, outputTime - start), length)
      return { index: i, offset, sourceTime: sourceTimeAt(clips[i], offset) }
    }
    start += length
  }
  return { index: -1, offset: 0, sourceTime: 0 }
}

/** A clip as the join's input: its parts and its own edit. */
export function clipExportInput(clip: SequenceClip): ExportInput {
  const cropped = clip.crop.width > 0 && (clip.crop.width < clip.width || clip.crop.height < clip.height)
  return {
    input: clip.path,
    segments: clip.parts,
    crop_x: cropped ? clip.crop.x : 0,
    crop_y: cropped ? clip.crop.y : 0,
    crop_width: cropped ? clip.crop.width : 0,
    crop_height: cropped ? clip.crop.height : 0,
    grade_brightness: clip.grade.brightness,
    grade_contrast: clip.grade.contrast,
    grade_saturation: clip.grade.saturation,
    speed: clip.speed,
    volumes: clip.volumes,
  }
}

export const sameGrade = (a: Grade, b: Grade) => a.brightness === b.brightness && a.contrast === b.contrast && a.saturation === b.saturation
export const sameCrop = (a: Crop, b: Crop) => a.x === b.x && a.y === b.y && a.width === b.width && a.height === b.height

/** The crop after dragging the frame (`move`) or one of its eight handles by a source-pixel delta. */
export function dragCrop(
  frame: { width: number; height: number },
  start: Crop,
  mode: string,
  dx: number,
  dy: number,
  ratio: number | null,
): Crop {
  let { x, y, width, height } = start
  const min = 32
  if (mode === 'move') {
    x = Math.min(Math.max(0, x + dx), frame.width - width)
    y = Math.min(Math.max(0, y + dy), frame.height - height)
  } else {
    if (mode.includes('e')) width = Math.max(min, Math.min(frame.width - x, width + dx))
    if (mode.includes('s')) height = Math.max(min, Math.min(frame.height - y, height + dy))
    if (mode.includes('w')) {
      const nx = Math.min(Math.max(0, x + dx), x + width - min)
      width += x - nx
      x = nx
    }
    if (mode.includes('n')) {
      const ny = Math.min(Math.max(0, y + dy), y + height - min)
      height += y - ny
      y = ny
    }
    if (ratio) {
      // Keep the shape: the width leads, the height follows, clamped inside the frame.
      height = Math.round(width / ratio)
      if (y + height > frame.height) {
        height = frame.height - y
        width = Math.round(height * ratio)
      }
    }
  }
  return { x: Math.round(x), y: Math.round(y), width: Math.round(width), height: Math.round(height) }
}

/** A crop as the same share of another frame, so a frame placed on one clip lands in the same place on every clip. */
export function scaleCrop(crop: Crop, from: { width: number; height: number }, to: { width: number; height: number }): Crop {
  if (!from.width || !from.height || (from.width === to.width && from.height === to.height)) return crop
  const sx = to.width / from.width
  const sy = to.height / from.height
  const width = Math.min(to.width, Math.max(1, Math.round(crop.width * sx)))
  const height = Math.min(to.height, Math.max(1, Math.round(crop.height * sy)))
  return {
    x: Math.min(Math.max(0, Math.round(crop.x * sx)), to.width - width),
    y: Math.min(Math.max(0, Math.round(crop.y * sy)), to.height - height),
    width,
    height,
  }
}

/** A clip joining the sequence takes the look the others already have: one colour grade and one crop for all. */
export function inheritLook(clip: SequenceClip, reference: SequenceClip | undefined): SequenceClip {
  if (!reference) return clip
  return { ...clip, grade: reference.grade, aspect: reference.aspect, crop: scaleCrop(reference.crop, reference, clip) }
}

/** Moves a clip to another slot. The clips keep their own trims; only the order changes. */
export function reorder(clips: SequenceClip[], from: number, to: number): SequenceClip[] {
  if (from === to || from < 0 || to < 0 || from >= clips.length || to >= clips.length) return clips
  const next = clips.slice()
  const [moved] = next.splice(from, 1)
  next.splice(to, 0, moved)
  return next
}

export function removeClip(clips: SequenceClip[], id: string): SequenceClip[] {
  return clips.filter((clip) => clip.id !== id)
}

/** The selected slot after a removal, never past the end. -1 when nothing is left. */
export function clampActive(clips: SequenceClip[], index: number): number {
  if (!clips.length) return -1
  return Math.min(Math.max(0, index), clips.length - 1)
}

/** True when the clip has been changed from the file as it arrived: closing would throw an edit away. */
export function isEdited(clip: SequenceClip): boolean {
  const whole = clip.parts.length === 1 && clip.parts[0][0] <= 0.001 && clip.parts[0][1] >= clip.duration - 0.001
  const cropped = clip.crop.width > 0 && (clip.crop.width < clip.width || clip.crop.height < clip.height)
  const graded = clip.grade.brightness !== 100 || clip.grade.contrast !== 100 || clip.grade.saturation !== 100
  const levelled = clip.volumes.some((volume) => Math.abs(volume - 1) > 0.005)
  return !!clip.timelineTrimmed || !whole || cropped || graded || levelled || clip.speed !== 1
}

export function editedCount(clips: SequenceClip[]): number {
  return clips.filter(isEdited).length
}
