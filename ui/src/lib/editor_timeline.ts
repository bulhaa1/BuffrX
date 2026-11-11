import type { TimelineChunk } from './timeline/model'
import type { SequenceClip } from './sequence'
import type { Part } from './segments'

/** Only a session's media directory is a Timeline launch, never a video inside that directory. */
export function timelineFolderSession(path: string): string {
  const parts = path.split(/[\\/]+/).filter(Boolean)
  const n = parts.length
  return n >= 3 && parts[n - 3].toLowerCase() === 'sessions' && parts[n - 1].toLowerCase() === 'media'
    ? parts[n - 2] : ''
}

/** Keep the snapshot's complete, published files in session order. */
export function timelineEditorChunks(chunks: TimelineChunk[]): { chunks: TimelineChunk[]; hasGap: boolean; changed: boolean } {
  const ordered = [...chunks].sort((a, b) => a.startMs - b.startMs)
  const seen = new Set<string>()
  let hasGap = false
  let changed = false
  for (let i = 0; i < ordered.length; i++) {
    const chunk = ordered[i]
    const key = chunk.file.toLowerCase()
    if (!chunk.published || !chunk.file || !Number.isFinite(chunk.startMs) ||
        !Number.isFinite(chunk.endMs) || chunk.endMs <= chunk.startMs || seen.has(key)) {
      throw new Error('The Timeline footage has an invalid chunk.')
    }
    seen.add(key)
    if (i > 0) {
      hasGap ||= chunk.startMs - ordered[i - 1].endMs > 1000
      changed ||= chunk.epoch !== ordered[i - 1].epoch
    }
  }
  if (!ordered.length) throw new Error('That Timeline session has no footage left.')
  return { chunks: ordered, hasGap, changed }
}

/** Trim one Timeline session at the active chunk, preserving other clips and existing cuts. */
export function trimTimelineAt(clips: SequenceClip[], index: number, time: number, side: 'from' | 'to'): SequenceClip[] | null {
  const selected = clips[index]
  if (!selected?.timelineSession || !Number.isFinite(time)) return null
  const id = selected.timelineSession
  const next: SequenceClip[] = []
  for (let i = 0; i < clips.length; i++) {
    const clip = clips[i]
    if (clip.timelineSession !== id) { next.push(clip); continue }
    if ((side === 'from' && i < index) || (side === 'to' && i > index)) continue
    if (i !== index) { next.push(clip); continue }
    const parts: Part[] = clip.parts.map(([start, end]): Part =>
      side === 'from' ? [Math.max(start, time), end] : [start, Math.min(end, time)],
    ).filter(([start, end]) => end > start + 0.001)
    if (parts.length) next.push({ ...clip, parts, active: 0 })
  }
  const first = next.findIndex((clip) => clip.timelineSession === id)
  if (first < 0) return null
  next[first] = { ...next[first], timelineTrimmed: true }
  return next
}
