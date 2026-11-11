// The clip preview's rules that do not need a browser: which key does what, where a seek lands,
// when the floating chrome steps out of the picture, and how often the ambient copy is redrawn.
// Times are milliseconds.

/** The keys the preview answers. A modifier that the rule does not name means the browser's own key. */
export interface PreviewKey {
  key: string
  altKey?: boolean
  shiftKey?: boolean
  ctrlKey?: boolean
  metaKey?: boolean
}

export type PreviewCommand =
  | { kind: 'escape' }
  | { kind: 'toggle' }
  | { kind: 'seek'; deltaMs: number }
  | { kind: 'seekTo'; edge: 'start' | 'end' }
  | { kind: 'step'; delta: number }
  | { kind: 'mute' }
  | { kind: 'fullscreen' }

export const SEEK_STEP_MS = 5000
export const SEEK_FINE_MS = 1000
export const CHROME_IDLE_MS = 2500
export const AMBIENT_INTERVAL_MS = 2000

/**
 * One key, one command. Escape is the way out even with nothing playing. Without media only Escape
 * and fullscreen mean anything: the other keys belong to the picture.
 */
export function previewCommand(event: PreviewKey, hasMedia: boolean): PreviewCommand | null {
  if (event.ctrlKey || event.metaKey) return null
  if (event.key === 'Escape') return { kind: 'escape' }
  if (!hasMedia) return null
  if (event.key === ' ' || event.key === 'Spacebar') return { kind: 'toggle' }
  if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
    const direction = event.key === 'ArrowRight' ? 1 : -1
    if (event.altKey) return { kind: 'step', delta: direction }
    return { kind: 'seek', deltaMs: direction * (event.shiftKey ? SEEK_FINE_MS : SEEK_STEP_MS) }
  }
  if (event.key === 'Home') return { kind: 'seekTo', edge: 'start' }
  if (event.key === 'End') return { kind: 'seekTo', edge: 'end' }
  if (event.key === 'm' || event.key === 'M') return { kind: 'mute' }
  if (event.key === 'f' || event.key === 'F') return { kind: 'fullscreen' }
  return null
}

/** A position inside the footage. A span that is not there yet parks at the start, not at NaN. */
export function clampSeek(value: number, span: number): number {
  if (!Number.isFinite(value) || !Number.isFinite(span) || span <= 0) return 0
  return Math.min(Math.max(value, 0), span)
}

/**
 * A wheel notch over the seek bar: up walks forward, down walks back. Shift takes the fine step.
 * The result never leaves the footage, and a notch that moves nothing reports the position it was given.
 */
export function wheelSeek(at: number, deltaY: number, span: number, fine = false): number {
  if (!Number.isFinite(deltaY) || deltaY === 0) return clampSeek(at, span)
  const step = fine ? SEEK_FINE_MS : SEEK_STEP_MS
  return clampSeek(at - Math.sign(deltaY) * step, span)
}

/**
 * The chrome leaves only a picture that is playing, still, and not being looked at. Paused,
 * loading, a pointer over a cluster and a focused control all keep it where it is.
 */
export function chromeShouldHide(state: { playing: boolean; idleMs: number; hovering: boolean; focused: boolean }): boolean {
  return state.playing && !state.hovering && !state.focused && state.idleMs >= CHROME_IDLE_MS
}

/** The ambient copy follows the picture at a walk, not a run: one redraw every two seconds is enough. */
export function ambientDue(lastMs: number, nowMs: number): boolean {
  return nowMs - lastMs >= AMBIENT_INTERVAL_MS
}

/**
 * What to do after the decoder refused a clip. Only the editor's process can make the H.264 copy;
 * the panel's page has no bridge for it and must say so rather than wait for a copy that never comes.
 * A probe that is still loading is waited for, a failed one is final.
 */
export function proxyStep(needsProxy: boolean, canProxy: boolean, details: 'loading' | 'failed' | 'ready'): 'idle' | 'wait' | 'prepare' | 'fail' {
  if (!needsProxy) return 'idle'
  if (!canProxy || details === 'failed') return 'fail'
  return details === 'loading' ? 'wait' : 'prepare'
}
