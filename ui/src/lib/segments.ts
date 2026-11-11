// The editor's cut, as data: the parts of a clip that are kept, in order, each [from, to] in seconds.
// A split divides the part under the playhead, a delete throws one away, and the export joins what is
// left into one file (the engine's ExportJob.segments). Pure functions, so they are tested on their own.

export type Part = [number, number]

/** The shortest part worth keeping: a few frames, and room for both handles on the timeline. */
export const SHORTEST = 0.1

export function keptLength(parts: Part[]): number {
  return parts.reduce((sum, [from, to]) => sum + Math.max(0, to - from), 0)
}

/** The index of the part that holds `t`, or -1 when `t` lies in something that was cut. */
export function partAt(parts: Part[], t: number): number {
  return parts.findIndex(([from, to]) => t >= from - 0.0005 && t < to)
}

/** Splits the part under `t` in two; the new active part is the second. Null when there is nothing to split. */
export function split(parts: Part[], t: number): { parts: Part[]; active: number } | null {
  const index = partAt(parts, t)
  if (index < 0) return null
  const [from, to] = parts[index]
  if (t - from < SHORTEST || to - t < SHORTEST) return null
  const next = [...parts.slice(0, index), [from, t] as Part, [t, to] as Part, ...parts.slice(index + 1)]
  return { parts: next, active: index + 1 }
}

/** Throws one part away. Null for the last one left: an export of nothing is not an edit. */
export function remove(parts: Part[], index: number): { parts: Part[]; active: number } | null {
  if (parts.length <= 1 || index < 0 || index >= parts.length) return null
  const next = parts.filter((_, i) => i !== index)
  return { parts: next, active: Math.min(index, next.length - 1) }
}

/**
 * Moves one edge of one part to `t`, held between its neighbours and the clip's ends.
 * `minLength` is the shortest the part may become: a live drag passes 0 so the edge follows the
 * pointer exactly, and the release passes the default so the cut never keeps a sliver.
 */
export function moveEdge(parts: Part[], index: number, edge: 'in' | 'out', t: number, total: number, minLength = SHORTEST): Part[] {
  const part = parts[index]
  if (!part) return parts
  const [from, to] = part
  const before = index > 0 ? parts[index - 1][1] : 0
  const after = index < parts.length - 1 ? parts[index + 1][0] : total
  const next = parts.slice()
  if (edge === 'in') next[index] = [Math.min(Math.max(t, before), to - minLength), to]
  else next[index] = [from, Math.max(Math.min(t, after), from + minLength)]
  return next
}

/**
 * Moves one part bodily in source time: it keeps its length, never overlaps its neighbours and
 * stays inside the clip. `delta` is in seconds, positive to the right.
 */
export function movePart(parts: Part[], index: number, delta: number, total: number): Part[] {
  const part = parts[index]
  if (!part) return parts
  const [from, to] = part
  const length = to - from
  const before = index > 0 ? parts[index - 1][1] : 0
  const after = index < parts.length - 1 ? parts[index + 1][0] : total
  // The gap between the neighbours is the room the part has; its length rides along inside it.
  const high = Math.max(before, after - length)
  const start = Math.min(Math.max(from + delta, before), high)
  const next = parts.slice()
  next[index] = [start, start + length]
  return next
}

/** Slides every part by the same `delta`: the whole kept material moves in time, gaps and all. */
export function moveAll(parts: Part[], delta: number, total: number): Part[] {
  if (!parts.length) return parts
  const low = -parts[0][0]
  const high = Math.max(low, total - parts[parts.length - 1][1])
  const shift = Math.min(Math.max(delta, low), high)
  return parts.map(([from, to]) => [from + shift, to + shift] as Part)
}

/**
 * Where playback should jump from `t`: the next kept part when `t` is in a cut, the first part when it
 * has run past the last one, or null to carry on.
 */
export function playbackJump(parts: Part[], t: number): number | null {
  if (!parts.length || partAt(parts, t) >= 0) return null
  const next = parts.find(([from]) => from > t)
  return next ? next[0] : parts[0][0]
}
