// The editor's reads of one clip: its keyframes (`--media-keyframes`) and its sound graph
// (`--media-peaks`). Each is a whole engine process, and peaks decode the file's audio from end to end,
// so a walk through the library must not start one per visit while the earlier ones are still running.
// Work here is shared per file, capped per kind, newest first, and dropped when the view has left the
// file before its turn came. A read the view has left, and nobody else holds, is ended the moment a
// newer read needs its place: a peaks walk can take minutes, and the clip on screen must not wait
// behind work nobody is looking at. A finished read stays cached, so coming back to a clip draws at once.

export type MediaProbe = { keyframes?: number[]; peaks?: number[][] }

type Kind = 'keyframes' | 'peaks'
type Value = number[] | number[][]

interface Job {
  kind: Kind
  key: string
  path: string
  args: string[]
  serial: number
  /** Bumped by `forget`: a read of a file that was rewritten must not cache its old answer. */
  generation: number
  /** The name this read's process is known by, so it can be ended before it finishes. */
  runId: string
  /** A view is still on this file: only a read nobody holds may be ended to free a place. */
  held: boolean
  started: boolean
  cancelled: boolean
  answer: (value: Value | null) => void
  promise: Promise<Value | null>
}

export interface MediaWork {
  /** What is already known about the file, so a view draws it on its first frame. */
  cached(path: string, buckets: number): MediaProbe
  keyframes(path: string): Promise<number[] | null>
  peaks(path: string, buckets: number): Promise<number[][] | null>
  /** The view has left the file: a read that has not started yet is dropped, and one in flight loses
   * its claim on its place, so the next file does not wait behind it. */
  release(path: string): void
  /** The file changed on disk (an export replaced it): its answers are no longer true. */
  forget(path: string): void
  stats(): { running: number; waiting: number }
}

/** Keyframes are a header parse; a peaks read decodes the whole track, so only one runs at a time. */
const LIMITS = { keyframes: 2, peaks: 1 }
/** Beyond this nobody is waiting on the queue any more: the oldest read loses its place rather than run. */
const MAX_WAITING = 8

export function createMediaWork(
  run: (args: string[], id: string) => Promise<unknown>,
  limits: { keyframes?: number; peaks?: number } = {},
  cancel?: (id: string) => void,
): MediaWork {
  const cap = { keyframes: limits.keyframes ?? LIMITS.keyframes, peaks: limits.peaks ?? LIMITS.peaks }
  const done = new Map<string, Value>()
  const inflight = new Map<string, Job>()
  const waiting: Job[] = []
  const running = new Set<Job>()
  const count: Record<Kind, number> = { keyframes: 0, peaks: 0 }
  const generations = new Map<string, number>()
  let serial = 0
  let runSerial = 0

  const keyOf = (kind: Kind, path: string, buckets: number) =>
    kind === 'keyframes' ? `keyframes:${path}` : `peaks:${path}:${buckets}`

  /** The engine's answer, or null when it failed: a failure is not cached, so a later visit tries again. */
  function read(job: Job, raw: unknown): Value | null {
    const report = raw as { ok?: boolean; keyframes?: unknown; tracks?: unknown } | null
    if (!report || report.ok !== true) return null
    const value = job.kind === 'keyframes' ? report.keyframes : report.tracks
    return Array.isArray(value) ? (value as Value) : null
  }

  /** A read that never left the queue: nothing to end, and the view gets no answer. */
  function drop(job: Job) {
    const at = waiting.indexOf(job)
    if (at >= 0) waiting.splice(at, 1)
    if (inflight.get(job.key) === job) inflight.delete(job.key)
    job.cancelled = true
    job.answer(null)
  }

  /** Nobody wants this read any more: end its process, or its place in the queue, now. */
  function abandon(job: Job) {
    if (job.cancelled) return
    if (!job.started) {
      drop(job)
      return
    }
    job.cancelled = true
    if (inflight.get(job.key) === job) inflight.delete(job.key)
    // Ending the process is what frees the place; the promise settling is what starts the next read,
    // so a killed child is never counted as gone before it is.
    cancel?.(job.runId)
  }

  function start(job: Job) {
    job.started = true
    count[job.kind]++
    running.add(job)
    let answer: Promise<unknown>
    try {
      answer = Promise.resolve(run(job.args, job.runId))
    } catch (error) {
      answer = Promise.reject(error)
    }
    void answer
      .then((raw) => read(job, raw))
      .catch(() => null)
      .then((value) => {
        running.delete(job)
        count[job.kind]--
        // A file rewritten or left while its read was out gives no answer: the bytes it describes
        // are not the ones on screen, and caching it would hand them to the next visit.
        const stale = job.cancelled || (generations.get(job.path) ?? 0) !== job.generation
        if (value && !stale) done.set(job.key, value)
        if (inflight.get(job.key) === job) inflight.delete(job.key)
        job.answer(stale ? null : value)
        pump()
      })
  }

  function pump() {
    for (let started = true; started; ) {
      started = false
      // The file on screen goes first: its read was asked for last.
      waiting.sort((a, b) => b.serial - a.serial)
      for (const job of [...waiting]) {
        if (count[job.kind] >= cap[job.kind]) {
          // The place is held by a read the view has left: end it rather than make the file on screen
          // wait behind work nobody is looking at. Its place comes free when its process is gone.
          const idle = [...running]
            .filter((entry) => entry.kind === job.kind && !entry.held && !entry.cancelled)
            .sort((a, b) => a.serial - b.serial)[0]
          if (idle) abandon(idle)
          continue
        }
        waiting.splice(waiting.indexOf(job), 1)
        start(job)
        started = true
      }
    }
  }

  function request(kind: Kind, path: string, buckets: number): Promise<Value | null> {
    const key = keyOf(kind, path, buckets)
    const known = done.get(key)
    if (known) return Promise.resolve(known)
    const live = inflight.get(key)
    if (live) {
      // The same file again while its read is out (a revisit, or a view asking twice): one process, and
      // the newest ask comes first when a slot frees.
      live.serial = ++serial
      live.held = true
      return live.promise
    }
    const args = kind === 'keyframes' ? ['--media-keyframes', path] : ['--media-peaks', path, String(buckets)]
    let answer: (value: Value | null) => void = () => undefined
    const promise = new Promise<Value | null>((resolve) => { answer = resolve })
    const job: Job = {
      kind,
      key,
      path,
      args,
      serial: ++serial,
      generation: generations.get(path) ?? 0,
      runId: `media-${++runSerial}`,
      held: true,
      started: false,
      cancelled: false,
      answer,
      promise,
    }
    inflight.set(key, job)
    waiting.push(job)
    if (waiting.length > MAX_WAITING) {
      drop(waiting.reduce((oldest, entry) => (entry.serial < oldest.serial ? entry : oldest)))
    }
    pump()
    return promise
  }

  return {
    cached(path, buckets) {
      const out: MediaProbe = {}
      const keyframes = done.get(keyOf('keyframes', path, buckets))
      if (keyframes) out.keyframes = keyframes as number[]
      const peaks = done.get(keyOf('peaks', path, buckets))
      if (peaks) out.peaks = peaks as number[][]
      return out
    },
    keyframes: (path) => request('keyframes', path, 0) as Promise<number[] | null>,
    peaks: (path, buckets) => request('peaks', path, buckets) as Promise<number[][] | null>,
    release(path) {
      for (const job of inflight.values()) if (job.path === path) job.held = false
      for (const job of [...waiting]) if (job.path === path) drop(job)
      pump()
    },
    forget(path) {
      generations.set(path, (generations.get(path) ?? 0) + 1)
      for (const key of [...done.keys()]) {
        if (key === `keyframes:${path}` || key.startsWith(`peaks:${path}:`)) done.delete(key)
      }
      // A read that is out describes the bytes that were there before: it must not show, must not be
      // cached, and must not hold the place the rewritten file needs.
      for (const job of [...inflight.values()]) if (job.path === path) abandon(job)
      pump()
    },
    stats: () => ({ running: count.keyframes + count.peaks, waiting: waiting.length }),
  }
}
