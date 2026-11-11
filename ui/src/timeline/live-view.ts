import { clampView, type View } from '../lib/timeline/model.ts'

export class LiveViewClock {
  private edgeMs?: number
  private mediaMs?: number

  reset(): void {
    this.edgeMs = undefined
    this.mediaMs = undefined
  }

  /** Advance the visible edge with the picture, never beyond footage already available. */
  update(mediaMs: number, availableMs: number): number {
    if (this.edgeMs === undefined || this.mediaMs === undefined || mediaMs < this.mediaMs - 250 || availableMs < this.edgeMs) {
      this.edgeMs = availableMs
      this.mediaMs = mediaMs
    } else {
      const at = Math.max(mediaMs, this.mediaMs)
      this.edgeMs = Math.min(availableMs, Math.max(at, this.edgeMs + at - this.mediaMs))
      this.mediaMs = at
    }
    return this.edgeMs
  }
}

/** Follow the picture gradually once it reaches the last tenth of a zoomed view. */
export function followLiveView(view: View, mediaMs: number, lo: number, edge: number): View {
  const span = view.toMs - view.fromMs
  const from = Math.max(view.fromMs, mediaMs - span * 0.9)
  return from > view.fromMs ? clampView({ fromMs: from, toMs: from + span }, lo, edge) : view
}
