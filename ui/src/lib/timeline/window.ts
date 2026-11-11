// The Timeline preview's window rules: one stretch of published footage is loaded at a time, and
// media behind the playhead is dropped. Pure, so the page's memory bound is testable without a
// browser; the player in src/timeline applies what this decides. Times are the session's own
// milliseconds, the numbers `timeline.get` reports.
import { chunkIndexForSeek, type TimelineChunk } from './model.ts'

/** The stretch of chunks a seek loads: the selected chunk first, then the published ones after it. */
export interface LoadedWindow {
  /** Index in the session's `chunks[]` of the first chunk loaded; -1 when there is no footage. */
  firstChunk: number
  /** Session ms where the loaded stretch begins and ends. */
  startMs: number
  endMs: number
  /** True when the seek fell in a recorded gap and moved to the next published chunk. */
  startsInGap: boolean
}

/**
 * A seam larger than this is footage that does not exist: Timeline paused (another window in front,
 * a settings change) and the picture must not pretend the two sides are continuous. The range bar
 * draws its gap hatching at the same number.
 */
export const GAP_MS = 250

/**
 * The window a seek loads: the chunk that covers `targetMs`, then at most `aheadChunks` published
 * chunks, stopping at the first recorded gap. An unpublished chunk is not footage - its writer still
 * holds the file - so it is never selected; a target inside one moves to the next published chunk.
 * A target before the first published chunk clamps to it, and a target in a gap returns the next
 * chunk with `startsInGap`, never a window that silently joins the two sides.
 */
export function windowForSeek(chunks: TimelineChunk[], targetMs: number, aheadChunks: number): LoadedWindow {
  const published: TimelineChunk[] = []
  for (const chunk of chunks) if (chunk.published) published.push(chunk)
  if (published.length === 0) return { firstChunk: -1, startMs: 0, endMs: 0, startsInGap: false }
  const firstIndex = (chunk: TimelineChunk) => {
    for (let i = 0; i < chunks.length; i++) if (chunks[i] === chunk) return i
    return -1
  }

  let at = chunkIndexForSeek(published, targetMs)
  let startsInGap = false
  const chosen = published[at]
  if (targetMs > chosen.endMs + GAP_MS) {
    // The moment sits in a hole between chunks: the next published footage is where the picture
    // starts, and the caller is told so it can say why the time jumped.
    if (at + 1 < published.length) {
      at += 1
      startsInGap = true
    } else {
      at = published.length - 1 // past the newest footage: clamp to the last chunk
    }
  }

  const first = published[at]
  const loaded: TimelineChunk[] = [first]
  const limit = Math.max(1, aheadChunks)
  for (let i = at + 1; i < published.length && loaded.length < limit; i++) {
    const previous = loaded[loaded.length - 1]
    if (published[i].startMs > previous.endMs + GAP_MS) break
    loaded.push(published[i])
  }
  return {
    firstChunk: firstIndex(first),
    startMs: first.startMs,
    endMs: loaded[loaded.length - 1].endMs,
    startsInGap,
  }
}

/**
 * The session moment before which buffered media may be thrown away: what is further than
 * `keepBehindMs` behind the playhead. Never negative, so the first minutes of a session keep
 * everything.
 */
export function evictBeforeMs(currentMs: number, keepBehindMs: number): number {
  return Math.max(0, currentMs - keepBehindMs)
}

/**
 * True when a seek can move `video.currentTime` inside the loaded stretch. Before the window (a
 * rewind beyond it), after it, in a recorded gap, or into media already evicted, the window has to
 * be rebuilt from `windowForSeek` instead.
 */
export function seekInside(targetMs: number, window: LoadedWindow | undefined, evictedToMs = 0): boolean {
  if (!window) return false
  return targetMs >= Math.max(window.startMs, evictedToMs) && targetMs <= window.endMs
}
