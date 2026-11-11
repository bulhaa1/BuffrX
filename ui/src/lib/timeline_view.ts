/** The part of the clip the timeline is showing, in seconds. */
export interface TimelineView {
  start: number
  span: number
}

export function clampView(view: TimelineView, duration: number, minSpan = 0.1): TimelineView {
  const total = Math.max(0.001, duration)
  const span = Math.min(total, Math.max(Math.min(minSpan, total), view.span))
  const start = Math.min(Math.max(0, view.start), total - span)
  return { start, span }
}

/** Zooms around a point in time, so the frame under the pointer stays under it. */
export function zoomView(view: TimelineView, duration: number, anchor: number, factor: number): TimelineView {
  const next = clampView({ start: 0, span: view.span * factor }, duration)
  const ratio = view.span > 0 ? (anchor - view.start) / view.span : 0.5
  return clampView({ start: anchor - ratio * next.span, span: next.span }, duration)
}

export function panView(view: TimelineView, duration: number, delta: number): TimelineView {
  return clampView({ start: view.start + delta, span: view.span }, duration)
}

export function timeAtRatio(ratio: number, view: TimelineView): number {
  return view.start + Math.min(1, Math.max(0, ratio)) * view.span
}

export function ratioAtTime(time: number, view: TimelineView): number {
  return view.span > 0 ? (time - view.start) / view.span : 0
}

/** The keyframe nearest `time` within `tolerance` seconds, or the time itself. */
export function snapToKeyframe(time: number, keyframes: number[], tolerance: number): number {
  let best = time
  let distance = tolerance
  for (const key of keyframes) {
    const gap = Math.abs(key - time)
    if (gap < distance) {
      best = key
      distance = gap
    }
  }
  return best
}

/** The shortest span a view may show, in seconds. */
export const MIN_SPAN = 0.1

/** Where a zoom control sits for a span: 0 shows the whole clip, 1 the tightest view, on a log scale so
 *  each step of the control is the same ratio of zoom. */
export function zoomOfSpan(span: number, duration: number): number {
  if (duration <= MIN_SPAN || span >= duration) return 0
  const level = Math.log(duration / Math.max(MIN_SPAN, span)) / Math.log(duration / MIN_SPAN)
  return Math.min(1, Math.max(0, level))
}

/** The span a zoom control position asks for; the inverse of `zoomOfSpan`. */
export function spanOfZoom(level: number, duration: number): number {
  if (duration <= MIN_SPAN) return Math.max(0.001, duration)
  return duration * (MIN_SPAN / duration) ** Math.min(1, Math.max(0, level))
}

/** The view at a new span, keeping `anchor` where it was on screen. */
export function viewAtSpan(view: TimelineView, duration: number, anchor: number, span: number): TimelineView {
  return zoomView(view, duration, anchor, span / Math.max(0.001, view.span))
}

/** Ruler steps, as the seconds between labelled marks and between the small marks inside them. */
const RULER_STEPS: [label: number, minor: number][] = [
  [0.1, 0.02], [0.2, 0.05], [0.5, 0.1], [1, 0.25], [2, 0.5], [5, 1], [10, 2], [15, 5], [30, 5],
  [60, 10], [120, 30], [300, 60], [600, 120], [1800, 300], [3600, 600],
]

/** The finest labelled step whose labels stay at least `gap` pixels apart, with the small marks that divide it. */
export function rulerSteps(perSecond: number, gap = 84): { label: number; minor: number } {
  const hit = RULER_STEPS.find(([label]) => label * perSecond >= gap) ?? RULER_STEPS[RULER_STEPS.length - 1]
  return { label: hit[0], minor: hit[1] }
}
