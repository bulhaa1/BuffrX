// The Timeline preview's player: one MediaSource, one SourceBuffer, one bounded stretch of published
// footage at a time. A seek inside the loaded stretch moves the picture; a seek outside it tears the
// window down and loads the chunks around the new moment. Media more than a minute behind the playhead
// is removed, and a full buffer is answered by loading a smaller window, so a two-hour session never
// costs two hours of RAM. The rules themselves are in lib/timeline/window (tested); this file applies
// them to a <video>.
//
// Segments come from lib/mp4: each chunk's moof+mdat copied with its decode time moved onto the
// session's clock. A chunk's index `startMs` is where it sits, so shifts are self-explanatory; the
// engine's own chunk boundaries are the mapping (no per-packet time mapping yet).
import { codecString, initBytes, segments } from '../lib/mp4.ts'
import { fetchChunkBytes } from '../lib/timeline/chunk-fetch.ts'
import type { TimelineChunk, TimelineSession } from '../lib/timeline/model.ts'
import { evictBeforeMs, GAP_MS, seekInside, windowForSeek, type LoadedWindow } from '../lib/timeline/window.ts'

export type PlayerState = 'buffering' | 'ready' | 'codec' | 'error' | 'missing' | 'empty' | 'append-failed'

export interface PlayerOptions {
  /** What the page should draw: preparing, playing, a codec it cannot decode, or no footage. */
  onState?: (state: PlayerState, detail?: string) => void
  /** How far the page got, for the shell's log (`timeline_log`). */
  onLog?: (message: string) => void
}

/** A chunk loaded is out of the buffer once this much media is behind the playhead. */
const KEEP_BEHIND_MS = 60000
/** How many chunks one window loads. Four is about half a minute: a rewind starts on the first one. */
const AHEAD_CHUNKS = 4
/** The audio codec every Timeline chunk carries (`timeline.get` only names the picture). */
const AUDIO_CODEC = 'mp4a.40.2'

import { mediaUrl } from './url.ts'

/**
 * The end of the last complete fragment in a file the writer is still growing; 0 until one lands.
 * lib/mp4 throws on a truncated box by design, so the live reader trims to whole moof+mdat pairs first.
 */
const completePrefix = (bytes: Uint8Array): number => {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  let at = 0
  let end = 0
  while (at + 8 <= bytes.length) {
    let size = view.getUint32(at)
    const type = String.fromCharCode(bytes[at + 4], bytes[at + 5], bytes[at + 6], bytes[at + 7])
    let header = 8
    if (size === 1) {
      if (at + 16 > bytes.length) break
      const large = view.getBigUint64(at + 8)
      if (large > BigInt(Number.MAX_SAFE_INTEGER)) break
      size = Number(large)
      header = 16
    }
    if (size < header || at + size > bytes.length) break
    at += size
    if (type === 'mdat') end = at
  }
  return end
}

const quotaExceeded = (error: unknown) => {
  const name = (error as { name?: string } | null)?.name
  return name === 'QuotaExceededError' || String(error).includes('QuotaExceededError')
}

/** Wait for enough media to absorb normal fragment delivery jitter. */
const LIVE_START_BUFFER_SECONDS = 1.5

/**
 * Live's playback rate for how far the picture is behind the newest fragment, measured right after an
 * append: a little faster (unnoticeable on game footage) from 4 s behind until it is back at 3 s, and a
 * jump past 10 s (a stall, a hidden window), where speeding up would take too long. The thresholds sit
 * above how the edge arrives: in 64 KB writes, which on quiet footage is a step of about 2 s, and a
 * 2-3 s target ran the buffer dry between steps (measured on a still desktop).
 */
export function liveCatchUp(lagSeconds: number, rate: number): { rate: number; jumpTo?: number } {
  if (lagSeconds > 10) return { rate: 1, jumpTo: 3 }
  if (lagSeconds > 4) return { rate: 1.08 }
  if (rate > 1 && lagSeconds > 3) return { rate }
  return { rate: 1 }
}

export class TimelinePlayer {
  private readonly video: HTMLVideoElement
  private readonly options: PlayerOptions
  private session?: TimelineSession
  private loadedWindow?: LoadedWindow
  private sourceBuffer?: SourceBuffer
  private objectUrl?: string
  private controller = new AbortController()
  private queue: Promise<void> = Promise.resolve()
  private generation = 0
  private sequence = 0
  private aheadChunks = AHEAD_CHUNKS
  private quotaRetries = 0
  private following = true
  private closed = false
  /** Session ms up to which buffered media has already been removed. */
  private evictedToMs = 0
  private lastEvictCheck = 0
  /** The window's end that a missing stretch of footage stopped: 0 when nothing is blocking. */
  private gapAfterMs = 0
  private loadStartedAt = 0
  private askedMs = 0
  private loadedChunks = 0
  /** The chunk a writer holds right now, and what of it has reached the SourceBuffer. */
  private liveFile = ''
  private liveStartMs = 0
  private liveWindowStart = 0
  private liveBytes = new Uint8Array(0)
  private liveFetched = 0
  private liveAppended = 0
  private liveRequested = false
  private liveVersion = 0
  private liveJumpRequested = false
  private extending = false
  private seekVersion = 0
  /** Whether the last chunk append failed, and why: the page shows it and the tick stops asking. */
  appendFailed = false
  appendError = ''

  constructor(video: HTMLVideoElement, options: PlayerOptions = {}) {
    this.video = video
    this.options = options
    video.addEventListener('loadeddata', this.onFirstFrame)
    video.addEventListener('seeked', this.onSeeked)
    video.addEventListener('timeupdate', this.onTime)
  }

  /** Where the picture is now, on the session's clock. */
  positionMs(): number {
    if (!this.loadedWindow) return this.session?.availableStartMs ?? 0
    return this.loadedWindow.startMs + this.video.currentTime * 1000
  }

  /** Whether the picture is the writer's own open chunk (Live), whose end is past the published edge. */
  isLive(): boolean {
    return this.liveFile !== ''
  }

  /** The stretch the player holds, for the page's range bar and its seek decision. */
  loaded(): LoadedWindow | undefined {
    return this.loadedWindow
  }

  /**
   * Moves the picture inside the stretch already loaded, for a scrub that is still moving: true when the
   * target is in the loaded window (the page then keeps easing), false when it is not (the page settles
   * with a real `load`, which rebuilds the window around it). No buffering state is reported here: a
   * frame or two of the decoder catching up is the point of the ease.
   */
  seekPreview(targetMs: number, approximate = false): boolean {
    const window = this.loadedWindow
    if (!window || this.closed) return false
    if (targetMs < Math.max(window.startMs, this.evictedToMs) || targetMs >= window.endMs) return false
    this.following = false
    if (approximate && this.video.seeking) return true
    const time = (targetMs - window.startMs) / 1000
    if (approximate && typeof this.video.fastSeek === 'function') this.video.fastSeek(time)
    else this.video.currentTime = time
    return true
  }

  /** Loads the window around `targetMs` and parks the picture there. */
  load(session: TimelineSession, targetMs: number): Promise<void> {
    this.updateSession(session)
    const id = session.id
    return this.enqueue(() => this.session?.id === id ? this.rebuild(targetMs) : Promise.resolve())
  }

  /** Keep the published range current while Live consumes the writer's separate open chunk. */
  updateSession(session: TimelineSession): void {
    if (this.session && this.session.id !== session.id) this.clearSession()
    this.session = session
  }

  /** Invalidate all work and clear the picture before switching sessions. */
  clearSession(): void {
    this.generation += 1
    this.seekVersion += 1
    this.stopLive()
    this.release()
    this.loadedWindow = undefined
    this.session = undefined
  }

  /** Appends published chunks that continue the loaded window while following the live edge. */
  append(chunks: TimelineChunk[]): Promise<void> {
    const id = this.session?.id
    return this.enqueue(async () => {
      if (!this.session || this.session.id !== id) return
      this.session = this.withChunks(chunks)
      if (!this.loadedWindow || !this.sourceBuffer) return
      const generation = this.generation
      try {
        const known = new Set(this.chunksIn(this.loadedWindow).map((chunk) => chunk.file))
        // The window reaches a few seconds past the playhead and no further: appending the rest of the
        // session would download footage nobody is about to watch, and the picture would sit behind it.
        const ahead = this.positionMs() + 30000
        for (const chunk of chunks) {
          if (!chunk.published || known.has(chunk.file)) continue
          const stretch: LoadedWindow = this.loadedWindow!
          if (chunk.endMs <= stretch.endMs) continue
          if (chunk.startMs > ahead) break
          if (chunk.startMs > stretch.endMs + GAP_MS) {
            // The writer paused and footage is missing: joining would show a still picture for the gap.
            // Remembered, because `onTime` asks again four times a second and the line would repeat with
            // it; the window stays shut until a chunk past the gap is published.
            if (this.gapAfterMs !== stretch.endMs) {
              this.log(`live edge: a gap before ${Math.round(chunk.startMs)} ms ends the window`)
              this.gapAfterMs = stretch.endMs
            }
            break
          }
          // A chunk that failed is tried again here, because a growth append means footage has landed since:
          // the file may have become readable. Only `onTime` is shut out (see `appendFailed`).
          const appended = await this.appendChunk(chunk)
          if (!appended || generation !== this.generation) return
          known.add(chunk.file)
          this.loadedWindow = { ...stretch, endMs: chunk.endMs }
          this.log(`live edge: +${Math.round(chunk.endMs - chunk.startMs)} ms, now ${Math.round(chunk.endMs)} ms`)
        }
      } catch (error) {
        // A seek or a close aborted this fetch on purpose: nothing failed.
        if (generation !== this.generation || this.closed) return
        // The tick asks again four times a second; the page hears of one episode once.
        if (!this.appendFailure(error)) return
        // Deleted footage is not a hiccup: the page can offer another moment or a retry.
        if (/\b(404|410)\b/.test(String(error))) this.report(generation, error)
        return
      }
      this.appendWorked()
    })
  }

  /** Moves the picture; outside the loaded window the window is rebuilt around the moment. */
  seek(targetMs: number, resume?: () => boolean): Promise<void> {
    const version = ++this.seekVersion
    const session = this.session
    const wanted = session && Math.min(Math.max(targetMs, session.availableStartMs), session.availableEndMs)
    const stretch = this.loadedWindow
    const ranges = this.sourceBuffer?.buffered
    const seconds = stretch && wanted !== undefined ? (wanted - stretch.startMs) / 1000 : -1
    let buffered = false
    if (!this.liveFile && ranges && seekInside(wanted ?? -1, stretch, this.evictedToMs)) {
      for (let i = 0; i < ranges.length; i++) {
        if (seconds >= ranges.start(i) && seconds < ranges.end(i)) buffered = true
      }
    }
    if (buffered && wanted !== undefined && session) {
      if (this.controller.signal.aborted) this.controller = new AbortController()
      this.following = wanted >= session.availableEndMs - 2000
      this.askedMs = wanted
      this.video.currentTime = seconds
      return Promise.resolve()
    }
    if (wanted !== undefined) {
      // Fetching the old window must not hold a new seek behind it in the SourceBuffer queue.
      this.generation += 1
      this.controller.abort()
      // The next append must not inherit the dead signal: it belongs to the seek that is about to start.
      this.controller = new AbortController()
    }
    return this.enqueue(async () => {
      if (version !== this.seekVersion) return
      const session = this.session
      if (!session || this.closed) return
      const wanted = Math.min(Math.max(targetMs, session.availableStartMs), session.availableEndMs)
      this.following = wanted >= session.availableEndMs - 2000
      this.askedMs = wanted
      // An open writer chunk is not part of the published seek window. Rebuild so later published
      // chunks can extend playback normally after the user leaves Live.
      if (this.liveFile) {
        await this.rebuild(wanted, resume)
        return
      }
      this.log(`seek: ${Math.round(wanted)} ms is outside the loaded window; rebuilding`)
      if (!this.loadedWindow) this.aheadChunks = AHEAD_CHUNKS
      await this.rebuild(wanted, resume)
    })
  }

  /** Start a fresh writer stream after a seek or pause, even when the writer is still on the same chunk. */
  startLive(): Promise<void> {
    this.liveRequested = true
    const version = ++this.liveVersion
    this.seekVersion += 1
    if (this.liveFile && this.sourceBuffer && this.loadedWindow) {
      // A pause keeps this stream and its buffered frames. Reuse it when Live is requested again.
      // The generation moves first: the abort below is this switch's own doing, so an append still in
      // flight must land in `append`'s guard instead of reporting a read the writer did not cause.
      this.generation += 1
      this.controller.abort()
      this.controller = new AbortController()
      return this.enqueue(async () => {
        if (this.closed || !this.liveRequested || version !== this.liveVersion) return
        this.following = true
        this.liveJumpRequested = true
        if (this.loadedChunks > 0) {
          this.video.currentTime = Math.max(0, this.bufferedEnd() - LIVE_START_BUFFER_SECONDS)
          void this.video.play().catch(() => {})
        }
      })
    }
    this.generation += 1
    this.controller.abort()
    this.controller = new AbortController()
    return this.enqueue(async () => {
      if (this.closed || !this.session || !this.liveRequested || version !== this.liveVersion) return
      this.generation += 1
      this.release()
      this.loadedWindow = undefined
      this.following = true
      this.options.onState?.('buffering')
    })
  }

  /** Cancel pending Live work before a pause or seek can be undone by a late fragment. */
  stopLive(): void {
    this.liveRequested = false
    this.liveVersion += 1
    this.following = false
    this.video.playbackRate = 1
  }

  /**
   * Resume the writer's stream after a pause at the position the picture already holds: keep the
   * chunk and its buffered frames, and let the next poll append what has arrived since.
   */
  resumeLive(): void {
    if (!this.liveFile) return
    this.liveRequested = true
    this.following = true
    this.liveJumpRequested = false
  }

  /**
   * Plays the chunk the writer still holds: fetches only the bytes flushed since the last call, trims
   * them to whole fragments, and appends what is new. The first chunk of a live stretch builds the
   * MediaSource; a later one in the same session keeps it and is shifted onto the same clock, so a
   * chunk rotation costs one append, not a reload.
   */
  live(file: string, startMs: number, bytes: number): Promise<void> {
    const version = this.liveVersion
    return this.enqueue(async () => {
      if (this.liveFile && file !== this.liveFile) {
        await this.updateLive(this.liveFile, this.liveStartMs, Number.POSITIVE_INFINITY, version)
      }
      await this.updateLive(file, startMs, bytes, version)
    })
  }

  private async updateLive(file: string, startMs: number, bytes: number, version: number): Promise<void> {
      if (this.closed || !this.session || !this.liveRequested || version !== this.liveVersion) return
      if (file !== this.liveFile) {
        const first = this.liveFile === ''
        if (first) {
          this.release()
          this.controller = new AbortController()
          this.generation += 1
          this.sequence = 0
          this.liveWindowStart = startMs
          this.following = true
          this.askedMs = startMs
        }
        this.liveFile = file
        this.liveStartMs = startMs
        this.liveBytes = new Uint8Array(0)
        this.liveFetched = 0
        this.liveAppended = 0
        this.log(`live: ${first ? 'opening' : 'next chunk'} ${file} at ${Math.round(startMs)} ms`)
      }
      const generation = this.generation
      if (bytes > this.liveFetched) {
        const tail = await fetchChunkBytes(mediaUrl(file), this.controller.signal, fetch, this.liveFetched).catch(
          (error: unknown) => {
            if (!this.liveRequested || version !== this.liveVersion) return undefined
            if (bytes === Number.POSITIVE_INFINITY && /\b416\b/.test(String(error))) return undefined
            this.report(generation, error)
            return undefined
          },
        )
        if (!tail || !tail.length || generation !== this.generation || !this.liveRequested || version !== this.liveVersion) return
        const grown = new Uint8Array(this.liveBytes.length + tail.length)
        grown.set(this.liveBytes, 0)
        grown.set(tail, this.liveBytes.length)
        this.liveBytes = grown
        this.liveFetched += tail.length
      }
      const whole = completePrefix(this.liveBytes)
      if (!whole) return
      const prefix = this.liveBytes.subarray(0, whole)
      if (!this.sourceBuffer) {
        let mime = ''
        try {
          mime = `video/mp4; codecs="${codecString(initBytes(prefix))}, ${AUDIO_CODEC}"`
        } catch (error) {
          // The first fragment is not on disk yet: the next tick tries again.
          this.log(`live: the chunk is not readable yet (${String(error)})`)
          return
        }
        if (!MediaSource.isTypeSupported(mime)) {
          this.fail(generation, 'codec', `This Timeline footage is encoded in a way the preview cannot decode (${mime}).`)
          return
        }
        const media = new MediaSource()
        this.objectUrl = URL.createObjectURL(media)
        this.video.src = this.objectUrl
        await new Promise<void>((resolve) => {
          media.addEventListener('sourceopen', () => resolve(), { once: true })
          this.controller.signal.addEventListener('abort', () => resolve(), { once: true })
        })
        if (this.closed || generation !== this.generation || !this.liveRequested || version !== this.liveVersion) return
        this.sourceBuffer = media.addSourceBuffer(mime)
        await this.push(initBytes(prefix), generation)
      }
      if (this.closed || generation !== this.generation || !this.liveRequested || version !== this.liveVersion) return
      const parts = segments(prefix, this.liveStartMs - this.liveWindowStart, this.sequence, this.liveAppended)
      for (const part of parts) {
        await this.push(part.bytes, generation)
        this.liveAppended += 1
        this.sequence += 1
        if (!this.liveRequested || version !== this.liveVersion) return
      }
      if (parts.length) {
        // Keep the init and what is not appended yet, nothing more. The open chunk grows to hundreds of
        // MB, and copying and re-parsing all of it on every poll got slower the longer Live ran.
        const init = initBytes(prefix)
        const rest = this.liveBytes.subarray(whole)
        const kept = new Uint8Array(init.length + rest.length)
        kept.set(init, 0)
        kept.set(rest, init.length)
        this.liveBytes = kept
        this.liveAppended = 0
      }
      const end = this.liveWindowStart + this.bufferedEnd() * 1000
      this.loadedWindow = { startMs: this.liveWindowStart, endMs: end, firstChunk: 0, startsInGap: false }
      if (this.liveJumpRequested && this.bufferedEnd() >= LIVE_START_BUFFER_SECONDS) {
        this.video.currentTime = Math.max(0, this.bufferedEnd() - LIVE_START_BUFFER_SECONDS)
        this.liveJumpRequested = false
      }
      if (this.loadedChunks === 0 && this.bufferedEnd() >= LIVE_START_BUFFER_SECONDS) {
        this.loadedChunks = 1
        this.evictedToMs = this.liveWindowStart
        this.lastEvictCheck = 0
        this.video.currentTime = Math.max(0, this.bufferedEnd() - LIVE_START_BUFFER_SECONDS)
        void this.video.play().catch(() => {})
        this.options.onState?.('ready')
        this.log(`live: playing ${Math.round((end - this.liveWindowStart) / 1000)} s buffered`)
      }
      // Reaching the buffered edge can pause the element. New fragments should resume Live; an
      // explicit user pause leaves Live and therefore stops calling this method.
      if (this.loadedChunks > 0 && this.following && this.video.paused) void this.video.play().catch(() => {})
      if (this.loadedChunks > 0 && this.following && !this.video.paused) {
        // Stay close to the edge: without this the picture drifted to 3-4 s behind and stayed there.
        const lag = this.bufferedEnd() - this.video.currentTime
        const step = liveCatchUp(lag, this.video.playbackRate)
        if (step.jumpTo !== undefined) {
          this.video.currentTime = Math.max(0, this.bufferedEnd() - step.jumpTo)
          this.log(`live: ${lag.toFixed(1)} s behind; jumped to the edge`)
        }
        this.video.playbackRate = step.rate
      }
  }

  /** Stops the picture, aborts fetches, drops the buffers and releases the object URL. */
  close(): void {
    if (this.closed) return
    this.closed = true
    this.generation += 1
    this.controller.abort()
    this.video.removeEventListener('loadeddata', this.onFirstFrame)
    this.video.removeEventListener('seeked', this.onSeeked)
    this.video.removeEventListener('timeupdate', this.onTime)
    this.release()
    this.log('closed')
  }

  // The window is built here; every public method reaches it through the queue, so two appends can
  // never interleave on one SourceBuffer.
  private async rebuild(targetMs: number, resume?: () => boolean): Promise<void> {
    if (this.closed) return
    const session = this.session
    if (!session) return
    this.generation += 1
    const generation = this.generation
    const startedAt = performance.now()
    this.release()
    this.controller = new AbortController()
    const stretch = windowForSeek(session.chunks, targetMs, this.aheadChunks)
    if (stretch.firstChunk < 0) {
      this.loadedWindow = undefined
      this.options.onState?.('empty')
      return
    }
    this.loadedWindow = stretch
    this.evictedToMs = stretch.startMs
    this.gapAfterMs = 0
    this.sequence = 0
    this.loadedChunks = 0
    this.askedMs = Math.min(Math.max(targetMs, session.availableStartMs), session.availableEndMs)
    this.options.onState?.('buffering')
    this.log(
      `open: chunks from ${stretch.firstChunk}, ${Math.round(stretch.startMs)}-${Math.round(stretch.endMs)} ms` +
        `${stretch.startsInGap ? ' (the seek landed in a gap)' : ''}`,
    )
    const chunks = this.chunksIn(stretch)
    if (chunks.length === 0) {
      this.options.onState?.('empty')
      return
    }
    // The planned window may include files still being fetched. Only completed appends are seekable.
    this.loadedWindow = { ...stretch, endMs: stretch.startMs }
    this.loadStartedAt = performance.now()
    let firstFile: Uint8Array
    try {
      firstFile = await this.bytes(chunks[0])
    } catch (error) {
      this.report(generation, error)
      return
    }
    if (generation !== this.generation) return
    let mime: string
    try {
      // The parser names the picture; Timeline chunks always carry one AAC track beside it.
      mime = `video/mp4; codecs="${codecString(initBytes(firstFile))}, ${AUDIO_CODEC}"`
    } catch (error) {
      this.fail(generation, 'error', `This Timeline footage could not be read by the preview (${String(error)}). The footage can still be saved.`)
      return
    }
    if (typeof MediaSource === 'undefined' || !MediaSource.isTypeSupported(mime)) {
      this.fail(
        generation,
        'codec',
        `This codec cannot play in the preview (${mime}). The footage can still be saved.`,
      )
      return
    }
    const mediaSource = new MediaSource()
    this.objectUrl = URL.createObjectURL(mediaSource)
    const opened = new Promise<void>((resolve) => {
      mediaSource.addEventListener('sourceopen', () => resolve(), { once: true })
      this.controller.signal.addEventListener('abort', () => resolve(), { once: true })
    })
    this.video.src = this.objectUrl
    await opened
    if (generation !== this.generation) return
    let buffer: SourceBuffer
    try {
      buffer = mediaSource.addSourceBuffer(mime)
    } catch (error) {
      this.fail(generation, 'error', `This codec cannot play in the preview (${String(error)}). The footage can still be saved.`)
      return
    }
    this.sourceBuffer = buffer
    try {
      await this.push(initBytes(firstFile), generation)
      for (const chunk of chunks) {
        const file = chunk === chunks[0] ? firstFile : await this.bytes(chunk)
        if (generation !== this.generation) return
        const parts = segments(file, chunk.startMs - stretch.startMs, this.sequence)
        this.sequence += parts.length
        for (const part of parts) await this.push(part.bytes, generation)
        this.loadedChunks += 1
        if (generation !== this.generation) return
        this.loadedWindow = { ...stretch, endMs: chunk.endMs }
        // The picture starts on the chunk under the moment asked for; the rest of the window keeps
        // arriving behind it. Waiting for all of it held the first frame back a second or more.
        if (this.loadedChunks === 1) {
          this.seekToLoaded()
          if (resume?.()) void this.video.play().catch(() => {})
        }
      }
    } catch (error) {
      if (generation !== this.generation) return
      if (quotaExceeded(error)) {
        // A window that still does not fit is not retried for ever: one chunk is the floor, and after
        // two rounds the footage itself is larger than this decoder's buffer.
        const next = Math.max(1, Math.floor(this.loadedChunks / 2))
        const changed = next < this.aheadChunks
        this.aheadChunks = next
        if (!changed || this.quotaRetries >= 2) {
          this.fail(generation, 'error', 'This Timeline footage is too large for the preview. The footage can still be saved.')
          return
        }
        this.quotaRetries += 1
        this.log(`the buffer is full; loading ${this.aheadChunks} chunk(s) after a rebuild`)
        await this.rebuild(this.askedMs, resume)
        return
      }
      this.fail(generation, 'error', `This Timeline footage could not be read by the preview (${String(error)}). The footage can still be saved.`)
      return
    }
    this.quotaRetries = 0
    this.appendWorked()
    this.log(
      `loaded: ${chunks.length} chunk(s) in ${Math.round(performance.now() - startedAt)} ms, ` +
        `${Math.round(this.bufferedEnd() * 1000)} ms buffered`,
    )
  }

  /** Put the playhead on the moment asked for, clamped to what the buffer holds so far. */
  private seekToLoaded(): void {
    const stretch = this.loadedWindow
    if (!stretch) return
    const at = Math.max((this.askedMs - stretch.startMs) / 1000, 0)
    const duration = this.video.duration
    this.video.currentTime = Number.isFinite(duration) ? Math.min(at, duration) : at
  }

  /** Where the buffer ends, in seconds: MSE reports `duration` as Infinity until the stream ends. */
  private bufferedEnd(): number {
    const ranges = this.sourceBuffer?.buffered
    return ranges && ranges.length ? ranges.end(ranges.length - 1) : 0
  }

  private chunksIn(window: LoadedWindow): TimelineChunk[] {
    const chunks = this.session?.chunks ?? []
    const inside = chunks.filter((chunk) => chunk.published && chunk.startMs < window.endMs && chunk.endMs > window.startMs)
    inside.sort((a, b) => a.startMs - b.startMs)
    return inside
  }

  private withChunks(chunks: TimelineChunk[]): TimelineSession {
    const session = this.session!
    let start = Number.POSITIVE_INFINITY
    let end = 0
    for (const chunk of chunks) {
      if (!chunk.published) continue
      start = Math.min(start, chunk.startMs)
      end = Math.max(end, chunk.endMs)
    }
    return {
      ...session,
      chunks,
      availableStartMs: Number.isFinite(start) ? start : session.availableStartMs,
      availableEndMs: end || session.availableEndMs,
    }
  }

  private async appendChunk(chunk: TimelineChunk): Promise<boolean> {
    const stretch = this.loadedWindow!
    const generation = this.generation
    // A chunk that is gone (retention, an erase, a delete the writer retried) must be named, never left
    // as a player that waits for a fetch that will not finish: `append` reports it once per episode.
    const file = await this.bytes(chunk)
    if (generation !== this.generation) return false
    const parts = segments(file, chunk.startMs - stretch.startMs, this.sequence)
    if (parts.length === 0) return false
    this.sequence += parts.length
    for (const part of parts) await this.push(part.bytes, generation)
    return generation === this.generation
  }

  private async bytes(chunk: TimelineChunk): Promise<Uint8Array> {
    return fetchChunkBytes(mediaUrl(chunk.file), this.controller.signal)
  }

  private push(bytes: Uint8Array, generation: number): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      const buffer = this.sourceBuffer
      if (!buffer || generation !== this.generation) return resolve()
      const signal = this.controller.signal
      if (signal.aborted) return reject(new Error('the media buffer was replaced'))
      const cleanup = () => {
        buffer.removeEventListener('updateend', done)
        buffer.removeEventListener('error', failed)
        signal.removeEventListener('abort', aborted)
      }
      const done = () => {
        cleanup()
        resolve()
      }
      const failed = () => {
        cleanup()
        reject(new Error('the media buffer refused a segment'))
      }
      // A seek or a close lets go of the buffer. `updateend` may never come for an append that was
      // abandoned, and the queue would wait on it forever: the abort is the answer.
      const aborted = () => {
        cleanup()
        reject(new Error('the media buffer was replaced'))
      }
      buffer.addEventListener('updateend', done)
      buffer.addEventListener('error', failed)
      signal.addEventListener('abort', aborted, { once: true })
      try {
        buffer.appendBuffer(bytes as unknown as BufferSource)
      } catch (error) {
        cleanup()
        reject(error)
      }
    })
  }

  // One removal every second of playback at most; `SourceBuffer.remove` is asynchronous and a call
  // per frame would queue work the decoder cannot use. A removal the browser refuses costs a rebuild.
  private onTime = () => {
    const stretch = this.loadedWindow
    if (!stretch || !this.sourceBuffer || this.video.paused) return
    const now = stretch.startMs + this.video.currentTime * 1000
    // A gap that already ended this window is not asked about again until a chunk past it appears.
    const blocked = this.gapAfterMs > 0 && !!this.session && !this.session.chunks.some((chunk) => chunk.startMs > this.gapAfterMs)
    if (!blocked && !this.appendFailed && !this.liveFile && !this.extending && this.session && stretch.endMs - now < 15000) {
      this.extending = true
      void this.append(this.session.chunks).finally(() => { this.extending = false })
    }
    if (now - this.lastEvictCheck < 1000) return
    this.lastEvictCheck = now
    const before = evictBeforeMs(now, KEEP_BEHIND_MS)
    if (before <= this.evictedToMs + 1000) return
    const target = Math.min(before, now - 1000)
    const from = (target - stretch.startMs) / 1000
    if (!Number.isFinite(from) || from <= 0.5) return
    this.evictedToMs = target
    const buffer = this.sourceBuffer
    void this.enqueue(async () => {
      if (this.sourceBuffer !== buffer || !this.sourceBuffer) return
      try {
        await this.remove(0, from)
        this.log(`evicted up to ${Math.round(target)} ms, ${Math.round(now - target)} ms behind the playhead`)
      } catch (error) {
        this.log(`eviction failed (${String(error)}); rebuilding`)
        await this.rebuild(now)
      }
    })
  }

  private remove(from: number, to: number): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      const buffer = this.sourceBuffer
      if (!buffer) return reject(new Error('no media buffer'))
      const signal = this.controller.signal
      if (signal.aborted) return reject(new Error('the media buffer was replaced'))
      const cleanup = () => {
        buffer.removeEventListener('updateend', done)
        buffer.removeEventListener('error', failed)
        signal.removeEventListener('abort', aborted)
      }
      const done = () => {
        cleanup()
        resolve()
      }
      const failed = () => {
        cleanup()
        reject(new Error('the buffer refused to remove media'))
      }
      const aborted = () => {
        cleanup()
        reject(new Error('the media buffer was replaced'))
      }
      buffer.addEventListener('updateend', done)
      buffer.addEventListener('error', failed)
      signal.addEventListener('abort', aborted, { once: true })
      try {
        buffer.remove(from, to)
      } catch (error) {
        cleanup()
        reject(error)
      }
    })
  }

  private onFirstFrame = () => {
    if (!this.loadStartedAt) return
    this.log(`first frame in ${Math.round(performance.now() - this.loadStartedAt)} ms`)
    this.loadStartedAt = 0
    this.options.onState?.('ready')
  }

  private onSeeked = () => {
    const stretch = this.loadedWindow
    if (!stretch) return
    const at = stretch.startMs + this.video.currentTime * 1000
    this.log(
      `seeked: asked ${Math.round(this.askedMs)} ms, at ${Math.round(at)} ms, readyState ${this.video.readyState}`,
    )
  }

  private release(): void {
    this.controller.abort()
    this.sourceBuffer = undefined
    if (this.objectUrl) URL.revokeObjectURL(this.objectUrl)
    this.objectUrl = undefined
    this.video.pause()
    this.video.removeAttribute('src')
    this.video.load()
    this.loadedChunks = 0
    this.lastEvictCheck = 0
    this.liveFile = ''
    this.liveJumpRequested = false
    this.liveBytes = new Uint8Array(0)
    this.liveFetched = 0
    this.liveAppended = 0
  }

  private enqueue(work: () => Promise<void>): Promise<void> {
    const next = this.queue.then(work, work)
    this.queue = next.catch(() => {})
    return next
  }

  /** One report per failure episode: true when this error is the one that ended a working append. */
  private appendFailure(error: unknown): boolean {
    const first = !this.appendFailed
    this.appendFailed = true
    this.appendError = String(error)
    if (!first) return false
    this.log(`append failed: ${this.appendError}`)
    this.options.onState?.(
      'append-failed',
      `This Timeline footage could not be added to the preview (${this.appendError}). The footage can still be saved.`,
    )
    return true
  }

  /** Data landed: whatever failed before is over, and the page stops being told about it. */
  private appendWorked(): void {
    if (!this.appendFailed) return
    this.appendFailed = false
    this.appendError = ''
    this.log('append works again')
  }

  private report(generation: number, error: unknown): void {
    if (generation !== this.generation) return
    // A 404 or a 410 is footage that was deleted under the player: the page can offer a retry or a move,
    // and it is not the same message as a stream the preview cannot read.
    const text = String(error)
    if (/\b(404|410)\b/.test(text)) {
      this.fail(generation, 'missing', 'This part has been deleted. Pick another moment on the bar, or retry.')
      return
    }
    this.fail(generation, 'error', `This Timeline footage could not be read by the preview (${text}). The footage can still be saved.`)
  }

  private fail(generation: number, state: PlayerState, message: string): void {
    if (generation !== this.generation) return
    this.log(message)
    this.options.onState?.(state, message)
  }

  private log(message: string): void {
    this.options.onLog?.(message)
  }
}
