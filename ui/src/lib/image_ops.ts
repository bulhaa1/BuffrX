// The screenshot editor's drawing: a stack of small operations (turn, flip, pen, arrow, box, text, obscure) replayed over
// the picture. The stack is the undo history and the picture is never altered in place, so a turn after a drawing turns the
// drawing with it, as the pixels would. Colour and crop are applied last, on top of the replay, by the editor.

export interface Point {
  x: number
  y: number
}

/** How a hidden area looks: a mosaic of blocks, a soft blur, frosted glass (blur and a pale veil), or a solid colour. */
export type Effect = 'mosaic' | 'blur' | 'frost' | 'solid'

/** What an effect covers: a rectangle, or whatever a brush was painted over. */
export type Area = { type: 'box'; from: Point; to: Point } | { type: 'brush'; points: Point[]; width: number }

export type Op =
  | { kind: 'rotate'; direction: 'cw' | 'ccw' }
  | { kind: 'flip'; axis: 'h' | 'v' }
  | { kind: 'pen'; color: string; width: number; points: Point[] }
  | { kind: 'arrow'; color: string; width: number; from: Point; to: Point }
  | { kind: 'box'; color: string; width: number; from: Point; to: Point }
  | { kind: 'text'; color: string; size: number; at: Point; text: string }
  /** `strength` is the block size of a mosaic or the radius of a blur, in picture pixels; `color` fills a solid. */
  | { kind: 'obscure'; effect: Effect; strength: number; color: string; area: Area }

export type DrawOp = Exclude<Op, { kind: 'rotate' } | { kind: 'flip' }>

/** The rectangle two corners make, whichever way the pointer went, kept inside a w by h picture. */
export function boxOf(from: Point, to: Point, w: number, h: number): { x: number; y: number; width: number; height: number } {
  const x0 = Math.min(Math.max(0, Math.min(from.x, to.x)), w)
  const y0 = Math.min(Math.max(0, Math.min(from.y, to.y)), h)
  const x1 = Math.min(Math.max(0, Math.max(from.x, to.x)), w)
  const y1 = Math.min(Math.max(0, Math.max(from.y, to.y)), h)
  return { x: x0, y: y0, width: x1 - x0, height: y1 - y0 }
}

/** The whole-pixel rectangle an area occupies in a w by h picture, `pad` pixels wider on every side. */
export function areaBounds(area: Area, w: number, h: number, pad = 0): { x: number; y: number; width: number; height: number } {
  let x0: number
  let y0: number
  let x1: number
  let y1: number
  if (area.type === 'box') {
    x0 = Math.min(area.from.x, area.to.x)
    y0 = Math.min(area.from.y, area.to.y)
    x1 = Math.max(area.from.x, area.to.x)
    y1 = Math.max(area.from.y, area.to.y)
  } else {
    const r = area.width / 2
    x0 = Math.min(...area.points.map((p) => p.x)) - r
    y0 = Math.min(...area.points.map((p) => p.y)) - r
    x1 = Math.max(...area.points.map((p) => p.x)) + r
    y1 = Math.max(...area.points.map((p) => p.y)) + r
  }
  const left = Math.max(0, Math.floor(x0 - pad))
  const top = Math.max(0, Math.floor(y0 - pad))
  const right = Math.min(w, Math.ceil(x1 + pad))
  const bottom = Math.min(h, Math.ceil(y1 + pad))
  return { x: left, y: top, width: Math.max(0, right - left), height: Math.max(0, bottom - top) }
}

/** Black on a light colour, white on a dark one: what a line of text is outlined with so it reads on any picture. */
export function outlineFor(color: string): string {
  const value = Number.parseInt(color.replace('#', '').padEnd(6, '0').slice(0, 6), 16)
  const r = (value >> 16) & 255
  const g = (value >> 8) & 255
  const b = value & 255
  return 0.299 * r + 0.587 * g + 0.114 * b > 140 ? '#000000' : '#ffffff'
}

/** The two barbs of an arrow's head, at the tip `to`, for a line from `from`. */
export function arrowHead(from: Point, to: Point, width: number): [Point, Point] {
  const angle = Math.atan2(to.y - from.y, to.x - from.x)
  const length = Math.max(width * 4.5, 18)
  const spread = Math.PI / 7
  return [
    { x: to.x - length * Math.cos(angle - spread), y: to.y - length * Math.sin(angle - spread) },
    { x: to.x - length * Math.cos(angle + spread), y: to.y - length * Math.sin(angle + spread) },
  ]
}

const sheet = (width: number, height: number) => {
  const canvas = document.createElement('canvas')
  canvas.width = Math.max(1, width)
  canvas.height = Math.max(1, height)
  return canvas
}

/** The shape of an area as a path on `ctx`, in picture pixels (translated by the caller). */
export function traceArea(ctx: CanvasRenderingContext2D, area: Area) {
  if (area.type === 'box') {
    const box = boxOf(area.from, area.to, Infinity, Infinity)
    ctx.fillRect(box.x, box.y, box.width, box.height)
    return
  }
  ctx.lineCap = 'round'
  ctx.lineJoin = 'round'
  ctx.lineWidth = area.width
  if (area.points.length === 1) {
    ctx.beginPath()
    ctx.arc(area.points[0].x, area.points[0].y, area.width / 2, 0, Math.PI * 2)
    ctx.fill()
    return
  }
  ctx.beginPath()
  area.points.forEach((point, index) => (index === 0 ? ctx.moveTo(point.x, point.y) : ctx.lineTo(point.x, point.y)))
  ctx.stroke()
}

/** An effect over an area of `source`, drawn onto `target` (the same canvas, when the picture is being edited): the area is
 *  cut out, given the effect, trimmed to the shape the pointer made, and laid back. */
export function drawObscure(target: CanvasRenderingContext2D, source: HTMLCanvasElement, op: Extract<Op, { kind: 'obscure' }>) {
  const w = source.width
  const h = source.height
  const inner = areaBounds(op.area, w, h)
  if (inner.width < 2 || inner.height < 2) return
  const strength = Math.max(1, op.strength)
  const result = sheet(inner.width, inner.height)
  const rctx = result.getContext('2d')!
  if (op.effect === 'solid') {
    rctx.fillStyle = op.color
    rctx.fillRect(0, 0, result.width, result.height)
  } else if (op.effect === 'mosaic') {
    const block = Math.max(2, Math.round(strength))
    const small = sheet(Math.ceil(inner.width / block), Math.ceil(inner.height / block))
    small.getContext('2d')!.drawImage(source, inner.x, inner.y, inner.width, inner.height, 0, 0, small.width, small.height)
    rctx.imageSmoothingEnabled = false
    rctx.drawImage(small, 0, 0, small.width, small.height, 0, 0, result.width, result.height)
  } else {
    // A blur needs what lies around the area, or its edges fade into nothing: the area is blurred with a margin and trimmed.
    const outer = areaBounds(op.area, w, h, Math.ceil(strength * 3))
    const blurred = sheet(outer.width, outer.height)
    const bctx = blurred.getContext('2d')!
    bctx.filter = `blur(${strength}px)`
    bctx.drawImage(source, outer.x, outer.y, outer.width, outer.height, 0, 0, outer.width, outer.height)
    rctx.drawImage(blurred, inner.x - outer.x, inner.y - outer.y, inner.width, inner.height, 0, 0, inner.width, inner.height)
    if (op.effect === 'frost') {
      rctx.fillStyle = 'rgba(255,255,255,0.3)'
      rctx.fillRect(0, 0, result.width, result.height)
    }
  }
  const mask = sheet(inner.width, inner.height)
  const mctx = mask.getContext('2d')!
  mctx.translate(-inner.x, -inner.y)
  mctx.fillStyle = '#ffffff'
  mctx.strokeStyle = '#ffffff'
  traceArea(mctx, op.area)
  rctx.globalCompositeOperation = 'destination-in'
  rctx.drawImage(mask, 0, 0)
  target.drawImage(result, inner.x, inner.y)
}

/** One drawing operation, in the picture's own pixels. */
export function drawOp(ctx: CanvasRenderingContext2D, op: DrawOp) {
  ctx.save()
  ctx.lineCap = 'round'
  ctx.lineJoin = 'round'
  if (op.kind === 'pen') {
    ctx.strokeStyle = op.color
    ctx.fillStyle = op.color
    ctx.lineWidth = op.width
    if (op.points.length === 1) {
      ctx.beginPath()
      ctx.arc(op.points[0].x, op.points[0].y, op.width / 2, 0, Math.PI * 2)
      ctx.fill()
    } else {
      ctx.beginPath()
      op.points.forEach((point, index) => (index === 0 ? ctx.moveTo(point.x, point.y) : ctx.lineTo(point.x, point.y)))
      ctx.stroke()
    }
  } else if (op.kind === 'arrow') {
    ctx.strokeStyle = op.color
    ctx.fillStyle = op.color
    ctx.lineWidth = op.width
    const [left, right] = arrowHead(op.from, op.to, op.width)
    // The shaft stops inside the head, so its round end does not poke past the tip.
    const base = { x: (left.x + right.x) / 2, y: (left.y + right.y) / 2 }
    ctx.beginPath()
    ctx.moveTo(op.from.x, op.from.y)
    ctx.lineTo(base.x, base.y)
    ctx.stroke()
    ctx.beginPath()
    ctx.moveTo(op.to.x, op.to.y)
    ctx.lineTo(left.x, left.y)
    ctx.lineTo(right.x, right.y)
    ctx.closePath()
    ctx.fill()
  } else if (op.kind === 'box') {
    const { x, y, width, height } = boxOf(op.from, op.to, Infinity, Infinity)
    ctx.strokeStyle = op.color
    ctx.lineWidth = op.width
    ctx.lineJoin = 'miter'
    ctx.strokeRect(x, y, width, height)
  } else if (op.kind === 'text') {
    ctx.font = `700 ${op.size}px "JetBrains Mono", monospace`
    ctx.textBaseline = 'top'
    ctx.lineWidth = Math.max(2, op.size / 8)
    ctx.strokeStyle = outlineFor(op.color)
    ctx.fillStyle = op.color
    const lines = op.text.split('\n')
    lines.forEach((line, index) => {
      const y = op.at.y + index * op.size * 1.2
      ctx.strokeText(line, op.at.x, y)
      ctx.fillText(line, op.at.x, y)
    })
  } else if (op.kind === 'obscure') {
    drawObscure(ctx, ctx.canvas, op)
  }
  ctx.restore()
}

/** One more operation on a picture: a drawing changes `canvas` in place, a turn or a flip hands back a new one. */
export function applyOp(canvas: HTMLCanvasElement, op: Op): HTMLCanvasElement {
  if (op.kind === 'rotate') {
    const next = sheet(canvas.height, canvas.width)
    const ctx = next.getContext('2d')!
    if (op.direction === 'cw') {
      ctx.translate(next.width, 0)
      ctx.rotate(Math.PI / 2)
    } else {
      ctx.translate(0, next.height)
      ctx.rotate(-Math.PI / 2)
    }
    ctx.drawImage(canvas, 0, 0)
    return next
  }
  if (op.kind === 'flip') {
    const next = sheet(canvas.width, canvas.height)
    const ctx = next.getContext('2d')!
    if (op.axis === 'h') {
      ctx.translate(next.width, 0)
      ctx.scale(-1, 1)
    } else {
      ctx.translate(0, next.height)
      ctx.scale(1, -1)
    }
    ctx.drawImage(canvas, 0, 0)
    return next
  }
  drawOp(canvas.getContext('2d')!, op)
  return canvas
}

/** A copy of a picture, to draw one more operation on without touching the one the undo history may still need. */
export function copyOf(canvas: HTMLCanvasElement): HTMLCanvasElement {
  const next = sheet(canvas.width, canvas.height)
  next.getContext('2d')!.drawImage(canvas, 0, 0)
  return next
}

/** The picture with every operation applied, in order. */
export function replay(image: CanvasImageSource & { width: number; height: number }, ops: Op[]): HTMLCanvasElement {
  let canvas = sheet(image.width, image.height)
  canvas.getContext('2d')!.drawImage(image, 0, 0)
  for (const op of ops) canvas = applyOp(canvas, op)
  return canvas
}
