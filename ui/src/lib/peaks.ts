// The editor's sound graph draws in the product's square column language (the Alt+4 timeline's `#loud`):
// fixed-pitch columns, bottom-anchored, each one the loudest peak it covers. This is the maths that
// turns the engine's 800 even slices into however many columns the lane has room for.

/** The loudness the columns are read against: the 95th percentile, so one explosion cannot flatten
 *  the rest, with a floor so noise is not scaled up to full height. */
function loudness(values: number[]): number {
  if (!values.length) return 0.08
  const sorted = [...values].sort((a, b) => a - b)
  return Math.max(0.08, sorted[Math.floor(sorted.length * 0.95)] ?? 0)
}

/** Max-pool `values` into `count` columns, each the loudest peak it covers.
 *
 *  Single-lane callers read pooled peaks against the clip's own loudness before squaring them: a game
 *  track sits well under full scale, so squaring raw peaks drew every clip as a flat strip. Lanes drawn
 *  together pass a common scale from `sharedScale` and keep their amplitude ratios, so quiet sources
 *  remain visible without making every lane fill its own row. */
export function columnsOf(values: number[], count: number, scale?: number): number[] {
  const n = Math.max(1, Math.floor(count))
  const out = new Array<number>(n).fill(0)
  if (!values.length) return out
  for (let i = 0; i < n; i++) {
    const from = Math.floor((i * values.length) / n)
    const to = Math.max(from + 1, Math.floor(((i + 1) * values.length) / n))
    let peak = 0
    for (let k = from; k < to && k < values.length; k++) if (values[k] > peak) peak = values[k]
    out[i] = Math.min(1, peak)
  }
  const against = Math.max(0.01, scale ?? loudness(out))
  // Side-by-side source lanes use amplitude, so a microphone ten times quieter is ten times shorter,
  // rather than a hundred times shorter and almost invisible. Single-lane callers keep their curve.
  return out.map((peak) => {
    const level = Math.min(1, peak / against)
    return scale === undefined ? level ** 2 : level
  })
}

/** One loudness for every lane drawn together, so the lanes' heights compare honestly: a microphone
 *  at a tenth of the desktop's level is drawn a tenth as tall, not normalised to its own row. */
export function sharedScale(lanes: number[][]): number {
  return loudness(lanes.flat().filter((value) => value > 0))
}

/** The slice of the engine's 800 even peaks that a view covers. The peaks span the whole file, so a
 *  time in seconds becomes a bucket index by the file's own length, never by the bucket count. */
export function peakRange(start: number, span: number, fileSeconds: number, buckets: number): [number, number] {
  const n = Math.max(1, Math.floor(buckets))
  if (!(fileSeconds > 0)) return [0, n]
  const first = Math.max(0, Math.min(n - 1, Math.floor((Math.max(0, start) / fileSeconds) * n)))
  const last = Math.max(first + 1, Math.min(n, Math.ceil((Math.max(0, start) + Math.max(0, span)) / fileSeconds * n)))
  return [first, last]
}

/** How many columns fit: a lit column and its 1 px gap. */
export function columnsIn(width: number, pitch = 3): number {
  return Math.max(1, Math.floor(width / pitch))
}
