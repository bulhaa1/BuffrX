// The screenshot editor: turn and flip, crop, draw (pen, arrow, box, text) and hide (a blur brush or box with four effects),
// colour, zoom, and undo. What is drawn is a stack of operations replayed over the picture (lib/image_ops.ts); the colour and
// the crop sit on top of it and are applied when the picture is saved. Saved as a copy in the Edits folder, or over the
// original after a confirm.

import clsx from 'clsx'
import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { Icon, type IconName } from '../components/icons'
import { Button, DialogBox, IconButton, Segmented, Slider, Tabs } from '../components/ui'
import { engine } from '../lib/api'
import { mediaUrl, shell } from '../lib/bridge'
import { canShareSaved } from '../lib/editor-share'
import { fileName, stem } from '../lib/format'
import { applyOp, boxOf, copyOf, drawOp, replay, traceArea, type Area, type DrawOp, type Effect, type Op, type Point } from '../lib/image_ops'
import type { Crop } from '../lib/sequence'
import { saveScreenshot } from '../lib/shot_save'
import { attempt, toast } from '../lib/store'
import { ASPECTS, CropOverlay, centredCrop, uniquePath, useFitted } from './editor-parts'
import { Section } from './inspector-tabs'

type Tool = 'pen' | 'arrow' | 'box' | 'text' | 'blur'

const TOOLS: { id: Tool; label: string; icon: IconName; key: string }[] = [
  { id: 'pen', label: 'Pen', icon: 'edit', key: 'P' },
  { id: 'arrow', label: 'Arrow', icon: 'arrow', key: 'A' },
  { id: 'box', label: 'Box', icon: 'square', key: 'R' },
  { id: 'text', label: 'Text', icon: 'text', key: 'T' },
  { id: 'blur', label: 'Hide', icon: 'blur', key: 'B' },
]

const EFFECTS: { value: Effect; label: string }[] = [
  { value: 'mosaic', label: 'Mosaic' },
  { value: 'blur', label: 'Blur' },
  { value: 'frost', label: 'Frost' },
  { value: 'solid', label: 'Solid' },
]

/** Marking colours are part of the picture, not of the interface, so they are fixed whatever the theme. */
const COLORS = ['#ff4d5e', '#ffd23f', '#3ddc84', '#4da3ff', '#ffffff', '#111111']

/** Every size is per 1280 px of picture width, so a mark looks the same on a 1080p shot and a 4K one. */
const START_SIZES = { pen: 6, arrow: 6, box: 5, text: 34, brush: 48, strength: 16 }
type Sizes = typeof START_SIZES

const FLAT = { brightness: 100, contrast: 100, saturation: 100 }
const MIN_ZOOM = 0.5
const MAX_ZOOM = 32

const clamp = (value: number, low: number, high: number) => Math.min(high, Math.max(low, value))

/** One size, with a sample of what it draws beside the slider: a line as thick as the pen, "Aa" as big as the text, a ring
 *  as wide as the brush. */
function SizeControl({ label, value, min, max, onChange, preview }: {
  label: string
  value: number
  min: number
  max: number
  onChange: (value: number) => void
  preview: React.ReactNode
}) {
  return (
    <Section title={label} aside={<span className="num text-[13px] text-ink">{value}</span>}>
      <div className="flex items-center gap-3">
        <div className="grid h-10 w-[64px] shrink-0 place-items-center overflow-hidden border border-line bg-canvas">{preview}</div>
        <Slider label={label} value={value} min={min} max={max} step={1} onChange={onChange} onCommit={onChange} className="flex-1" />
      </div>
    </Section>
  )
}

export function ImageEditor({ path, clipsDir, hover, clips, onBack, onDirtyChange }: {
  path: string
  clipsDir: string
  /** A file is over the window. */
  hover: boolean
  /** How many clips wait in the sequence behind this screenshot. */
  clips: number
  onBack: () => void
  onDirtyChange: (dirty: boolean) => void
}) {
  const stage = useRef<HTMLDivElement>(null)
  const base = useRef<HTMLCanvasElement>(null)
  const overlay = useRef<HTMLCanvasElement>(null)
  const typingInput = useRef<HTMLInputElement>(null)
  const [img, setImg] = useState<HTMLImageElement | null>(null)
  const [failed, setFailed] = useState(false)
  const [ops, setOps] = useState<Op[]>([])
  const [undone, setUndone] = useState<Op[]>([])
  /** Bumped by every change to the drawing, so "is this save still current" never compares two different pictures as equal. */
  const [version, setVersion] = useState(0)
  const [tab, setTab] = useState<'edit' | 'draw' | 'color' | 'export'>('edit')
  const [tool, setTool] = useState<Tool>('pen')
  const [color, setColor] = useState(COLORS[0])
  const [sizes, setSizes] = useState<Sizes>(START_SIZES)
  const [area, setArea] = useState<'brush' | 'box'>('brush')
  const [effect, setEffect] = useState<Effect>('mosaic')
  const [typing, setTyping] = useState<{ at: Point; value: string } | null>(null)
  // As in the clip editor: a shape other than Off shows its frame and the save uses it; Off undoes it.
  const [aspect, setAspect] = useState('off')
  const [crop, setCrop] = useState<Crop>({ x: 0, y: 0, width: 0, height: 0 })
  const [adjust, setAdjust] = useState(FLAT)
  const [view, setView] = useState({ zoom: 1, x: 0, y: 0 })
  const [grab, setGrab] = useState<'idle' | 'ready' | 'panning'>('idle')
  const [busy, setBusy] = useState(false)
  const saving = useRef(false)
  const [saved, setSaved] = useState<string | null>(null)
  const [savedSignature, setSavedSignature] = useState('')
  const [savedShareable, setSavedShareable] = useState(false)
  const [confirmReplace, setConfirmReplace] = useState(false)

  useEffect(() => {
    const image = new Image()
    image.crossOrigin = 'anonymous'
    image.onload = () => setImg(image)
    image.onerror = () => setFailed(true)
    image.src = mediaUrl(path)
  }, [path])

  // The picture with every operation applied. One more operation is drawn on a copy of the last result; an undo, a redo or
  // a first look replays the stack. It is rebuilt when the stack changes, never while one is being drawn: that is painted
  // on the overlay.
  const lastBake = useRef<{ ops: Op[]; canvas: HTMLCanvasElement } | null>(null)
  const baked = useMemo(() => {
    if (!img) return null
    const previous = lastBake.current
    let canvas: HTMLCanvasElement
    if (previous && ops.length === previous.ops.length + 1 && previous.ops.every((op, i) => op === ops[i])) {
      const next = ops[ops.length - 1]
      canvas = applyOp(next.kind === 'rotate' || next.kind === 'flip' ? previous.canvas : copyOf(previous.canvas), next)
    } else canvas = replay(img, ops)
    lastBake.current = { ops, canvas }
    return canvas
  }, [img, ops])
  const natural = { width: baked?.width ?? 0, height: baked?.height ?? 0 }
  const fitted = useFitted(stage, natural.width, natural.height)
  useEffect(() => {
    const canvas = base.current
    if (!canvas || !baked) return
    canvas.width = baked.width
    canvas.height = baked.height
    canvas.getContext('2d')!.drawImage(baked, 0, 0)
    if (overlay.current) {
      overlay.current.width = baked.width
      overlay.current.height = baked.height
    }
  }, [baked])

  const ratio = ASPECTS.find((a) => a.value === aspect)?.ratio ?? null
  useEffect(() => {
    if (natural.width) setCrop(centredCrop(natural.width, natural.height, ratio))
  }, [ratio, natural.width, natural.height])
  const cropOn = aspect !== 'off'
  const cropping = cropOn && natural.width > 0 && (crop.width < natural.width || crop.height < natural.height)
  const graded = adjust.brightness !== 100 || adjust.contrast !== 100 || adjust.saturation !== 100
  const filter = `brightness(${adjust.brightness}%) contrast(${adjust.contrast}%) saturate(${adjust.saturation}%)`
  const changed = ops.length > 0 || cropping || graded
  const editSignature = JSON.stringify([version, cropping && crop, adjust])
  const editSignatureRef = useRef(editSignature)
  editSignatureRef.current = editSignature
  useEffect(() => { onDirtyChange(changed && savedSignature !== editSignature) }, [changed, editSignature, savedSignature, onDirtyChange])

  // ---------------------------------------------------------------- zoom

  /** A pan that keeps the picture from leaving the stage altogether. */
  const keep = (next: { zoom: number; x: number; y: number }) => {
    if (next.zoom <= 1) return { zoom: next.zoom, x: 0, y: 0 }
    const reach = { x: (fitted.width * next.zoom) / 2, y: (fitted.height * next.zoom) / 2 }
    return { zoom: next.zoom, x: clamp(next.x, -reach.x, reach.x), y: clamp(next.y, -reach.y, reach.y) }
  }
  /** Zoom by a factor (or to the fit, or to one picture pixel per screen pixel), around a point of the screen. */
  const zoomBy = (how: number | 'fit' | 'actual', around?: { x: number; y: number }) => {
    if (how === 'fit') return setView({ zoom: 1, x: 0, y: 0 })
    setView((current) => {
      const zoom = clamp(how === 'actual' ? natural.width / Math.max(1, fitted.width) : current.zoom * how, MIN_ZOOM, MAX_ZOOM)
      const rect = stage.current?.getBoundingClientRect()
      const point = rect && around ? { x: around.x - (rect.left + rect.width / 2), y: around.y - (rect.top + rect.height / 2) } : { x: 0, y: 0 }
      const ratio = zoom / current.zoom
      return keep({ zoom, x: point.x - ratio * (point.x - current.x), y: point.y - ratio * (point.y - current.y) })
    })
  }
  const zoomRef = useRef(zoomBy)
  zoomRef.current = zoomBy
  // The wheel zooms around the pointer; a native listener, because React's own is passive and cannot stop the page scrolling.
  useEffect(() => {
    const element = stage.current
    if (!element) return
    const wheel = (event: WheelEvent) => {
      event.preventDefault()
      zoomRef.current(Math.exp(-event.deltaY * (event.deltaMode === 1 ? 16 : 1) * 0.0015), { x: event.clientX, y: event.clientY })
    }
    element.addEventListener('wheel', wheel, { passive: false })
    return () => element.removeEventListener('wheel', wheel)
  }, [])
  // Pan: the middle button, or Space and the left one, so neither the crop frame nor a drawing tool is in the way.
  const spaceHeld = useRef(false)
  const panning = useRef<{ x: number; y: number; from: { x: number; y: number } } | null>(null)
  const percent = Math.round((natural.width ? fitted.width / natural.width : 1) * view.zoom * 100)

  // ---------------------------------------------------------------- the drawing

  const change = (nextOps: Op[], nextUndone: Op[]) => {
    setOps(nextOps)
    setUndone(nextUndone)
    setVersion((v) => v + 1)
  }
  const commit = (op: Op) => change([...ops, op], [])
  const undo = () => ops.length > 0 && change(ops.slice(0, -1), [...undone, ops[ops.length - 1]])
  const redo = () => undone.length > 0 && change([...ops, undone[undone.length - 1]], undone.slice(0, -1))
  // A turn or a flip keeps the crop: the crop effect recentres it on the new picture size, so the
  // shape chosen before the transform is still the shape saved after it.
  const transform = (op: Op) => commit(op)

  const scale = Math.max(1, natural.width / 1280)
  const live = useRef<DrawOp | null>(null)
  const textStart = useRef<Point | null>(null)
  /** The overlay: the mark being drawn, the shape being hidden, or the text being typed. */
  const paint = () => {
    const canvas = overlay.current
    const ctx = canvas?.getContext('2d')
    if (!canvas || !ctx) return
    ctx.clearRect(0, 0, canvas.width, canvas.height)
    const op = live.current
    if (op?.kind === 'obscure') {
      // The effect itself is made on release; while the pointer is down the area is shown as a veil with an outline.
      ctx.save()
      ctx.fillStyle = 'rgba(255,255,255,0.22)'
      ctx.strokeStyle = 'rgba(255,255,255,0.22)'
      traceArea(ctx, op.area)
      if (op.area.type === 'box') {
        const { x, y, width, height } = boxOf(op.area.from, op.area.to, canvas.width, canvas.height)
        ctx.setLineDash([10 * scale, 7 * scale])
        ctx.lineWidth = 2 * scale
        ctx.strokeStyle = '#ffffff'
        ctx.strokeRect(x, y, width, height)
      }
      ctx.restore()
    } else if (op) drawOp(ctx, op)
    else if (typing?.value) drawOp(ctx, { kind: 'text', color, size: sizes.text * scale, at: typing.at, text: typing.value })
  }
  useEffect(paint) // eslint-disable-line react-hooks/exhaustive-deps -- repaints whenever the typed text, colour or size changes

  const toPicture = (event: React.PointerEvent): Point => {
    const rect = overlay.current!.getBoundingClientRect()
    return { x: ((event.clientX - rect.left) / rect.width) * natural.width, y: ((event.clientY - rect.top) / rect.height) * natural.height }
  }
  const onDown = (event: React.PointerEvent) => {
    if (event.button !== 0 || !baked) return
    const at = toPicture(event)
    event.currentTarget.setPointerCapture(event.pointerId)
    if (tool === 'text') {
      // The text box opens when the button is released: opening it now would hand focus to the picture, which closes it again.
      textStart.current = at
      return
    }
    if (tool === 'blur') {
      const hidden: Area = area === 'brush' ? { type: 'brush', points: [at], width: sizes.brush * scale } : { type: 'box', from: at, to: at }
      live.current = { kind: 'obscure', effect, strength: sizes.strength * scale, color, area: hidden }
    } else if (tool === 'pen') live.current = { kind: 'pen', color, width: sizes.pen * scale, points: [at] }
    else live.current = { kind: tool, color, width: sizes[tool] * scale, from: at, to: at }
    paint()
  }
  const onMove = (event: React.PointerEvent) => {
    const op = live.current
    if (!op) return
    const at = toPicture(event)
    if (op.kind === 'pen') op.points.push(at)
    else if (op.kind === 'obscure') {
      if (op.area.type === 'brush') op.area.points.push(at)
      else op.area.to = at
    } else if (op.kind === 'arrow' || op.kind === 'box') op.to = at
    paint()
  }
  const onUp = () => {
    if (textStart.current) {
      const at = textStart.current
      textStart.current = null
      if (typing) finishText(true)
      setTyping({ at, value: '' })
      return
    }
    const op = live.current
    live.current = null
    paint()
    if (!op) return
    let keepIt = true
    if (op.kind === 'arrow' || op.kind === 'box') keepIt = Math.hypot(op.to.x - op.from.x, op.to.y - op.from.y) > 6 * scale
    else if (op.kind === 'obscure' && op.area.type === 'box') keepIt = Math.abs(op.area.to.x - op.area.from.x) > 4 * scale && Math.abs(op.area.to.y - op.area.from.y) > 4 * scale
    if (keepIt) commit(op)
  }
  const finishText = (keep: boolean) => {
    const entry = typing
    setTyping(null)
    if (keep && entry && entry.value.trim()) commit({ kind: 'text', color, size: sizes.text * scale, at: entry.at, text: entry.value })
  }
  // The text box takes the keyboard as soon as it exists.
  const typingOpen = typing !== null
  useEffect(() => {
    if (typingOpen) typingInput.current?.focus()
  }, [typingOpen])

  // ---------------------------------------------------------------- saving

  const save = useCallback(async (replace: boolean, share = false) => {
    if (saving.current || !baked || (!replace && !clipsDir)) return
    saving.current = true
    setBusy(true)
    try {
      // The signature is read before the picture is encoded: the file this save writes is that picture, never
      // an edit made while the write is in flight.
      const { written: target, saved: signature, current } = await saveScreenshot({
        signature: () => editSignatureRef.current,
        write: async () => {
          const picture = cropping ? crop : { x: 0, y: 0, width: baked.width, height: baked.height }
          const canvas = document.createElement('canvas')
          canvas.width = picture.width
          canvas.height = picture.height
          const context = canvas.getContext('2d')!
          context.filter = filter
          context.drawImage(baked, picture.x, picture.y, picture.width, picture.height, 0, 0, picture.width, picture.height)
          const jpeg = /\.jpe?g$/i.test(path)
          const blob = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, jpeg ? 'image/jpeg' : 'image/png', 0.95))
          if (!blob) throw new Error('the image could not be encoded')
          const target = replace ? path : await uniquePath(`${clipsDir}\\Edits\\${stem(path)}-edit.${jpeg ? 'jpg' : 'png'}`)
          // Base64, not one JSON number per byte: a 4K picture is millions of numbers and froze the window.
          const bytes_base64 = await new Promise<string>((resolve, reject) => {
            const reader = new FileReader()
            reader.onload = () => resolve(String(reader.result).replace(/^data:[^,]*,/, ''))
            reader.onerror = () => reject(reader.error ?? new Error('the image could not be read'))
            reader.readAsDataURL(blob)
          })
          await shell('write_image', { path: target, bytes_base64 })
          return target
        },
      })
      setSaved(target)
      setSavedSignature(signature)
      setSavedShareable(!replace)
      toast('success', 'Saved', fileName(target))
      void engine.notifyLibrary().catch(() => undefined)
      if (share && canShareSaved(signature, current, !replace)) void attempt('The share sheet did not open', () => engine.share(target))
      else if (share) toast('info', 'Image saved', 'Your edit changed while saving. Save it again to share the latest version.')
    } catch (error) {
      toast('error', 'The image was not saved', error instanceof Error ? error.message : String(error))
    } finally {
      saving.current = false
      setBusy(false)
    }
  }, [baked, clipsDir, cropping, crop, filter, path])

  const saveRef = useRef(save)
  saveRef.current = save
  const actions = useRef({ undo, redo })
  actions.current = { undo, redo }
  useEffect(() => {
    const typingNow = (event: KeyboardEvent) => {
      const target = event.target instanceof Element ? event.target : null
      return !!target?.closest('input, textarea, select, [role="dialog"], [role="slider"], [role="tab"], [role="menu"], [role="listbox"]')
    }
    const onKey = (event: KeyboardEvent) => {
      if (typingNow(event)) return
      // Space and Enter belong to a focused control: a button must still be pressable from the keyboard.
      const focused = event.target instanceof Element ? event.target : null
      if ((event.key === ' ' || event.key === 'Enter') && focused?.closest('button, a, [role="button"], summary')) return
      if (event.key === ' ') {
        event.preventDefault()
        spaceHeld.current = true
        setGrab((now) => (now === 'panning' ? now : 'ready'))
        return
      }
      const key = event.key.toLowerCase()
      if (event.ctrlKey || event.metaKey) {
        if (key === 'z' && !event.shiftKey) actions.current.undo()
        else if (key === 'y' || (key === 'z' && event.shiftKey)) actions.current.redo()
        else if (key === 's') void saveRef.current(false, event.shiftKey)
        else return
        event.preventDefault()
        return
      }
      if (event.altKey) return
      if (event.key === '+' || event.key === '=') zoomRef.current(1.25)
      else if (event.key === '-' || event.key === '_') zoomRef.current(0.8)
      else if (event.key === '0') zoomRef.current('fit')
      else if (event.key === '1') zoomRef.current('actual')
      else {
        const chosen = TOOLS.find((entry) => entry.key.toLowerCase() === key)
        if (!chosen) return
        setTab('draw')
        setTool(chosen.id)
      }
      event.preventDefault()
    }
    const onKeyUp = (event: KeyboardEvent) => {
      if (event.key !== ' ') return
      spaceHeld.current = false
      setGrab((now) => (now === 'panning' ? now : 'idle'))
    }
    window.addEventListener('keydown', onKey)
    window.addEventListener('keyup', onKeyUp)
    return () => {
      window.removeEventListener('keydown', onKey)
      window.removeEventListener('keyup', onKeyUp)
    }
  }, [])

  const drawing = tab === 'draw' && grab === 'idle'
  const set = (key: keyof Sizes) => (value: number) => setSizes((current) => ({ ...current, [key]: value }))
  const lineSample = (value: number) => <span className="block w-[44px]" style={{ height: clamp(value * 0.8, 2, 26), backgroundColor: color }} />
  const markColors = (
    <Section title="Color">
      <div className="flex gap-1.5">
        {COLORS.map((value) => (
          <button
            type="button"
            key={value}
            aria-label={`Color ${value}`}
            aria-pressed={color === value}
            onClick={() => setColor(value)}
            className={clsx('h-7 w-7 border', color === value && 'outline outline-2 outline-offset-2 outline-accent')}
            style={{ backgroundColor: value, borderColor: 'var(--bx-line-strong)' }}
          />
        ))}
      </div>
    </Section>
  )

  const draw = (
    <div className="editor-sections">
      <Section title="Tool">
        <div className="grid grid-cols-5 gap-1">
          {TOOLS.map((entry) => (
            <button
              type="button"
              key={entry.id}
              title={`${entry.label} (${entry.key})`}
              aria-pressed={tool === entry.id}
              onClick={() => setTool(entry.id)}
              className={clsx('flex h-[52px] flex-col items-center justify-center gap-1 border text-[12px] font-bold', tool === entry.id ? 'border-accent bg-accent text-on-accent' : 'border-line text-muted hover:bg-raised hover:text-ink')}
            >
              <Icon name={entry.icon} size={16} />
              {entry.label}
            </button>
          ))}
        </div>
      </Section>
      {/* The options of the chosen tool rise and fade in when the tool changes (keyed: a new tool remounts them). */}
      <div key={tool} className="bx-tab-enter">
      {tool === 'blur' && (
        <>
          <Section title="Hide with">
            <Segmented label="Area" size="sm" className="flex w-full [&>button]:flex-1" value={area} onChange={setArea} options={[{ value: 'brush', label: 'Brush' }, { value: 'box', label: 'Box' }]} />
            <div className="mt-3">
              <Segmented label="Effect" size="sm" className="flex w-full [&>button]:flex-1 [&>button]:px-0" value={effect} onChange={setEffect} options={EFFECTS} />
            </div>
          </Section>
          {area === 'brush' && (
            <SizeControl label="Brush size" value={sizes.brush} min={8} max={160} onChange={set('brush')} preview={<span className="block rounded-full border border-ink" style={{ width: clamp(sizes.brush / 5, 4, 32), height: clamp(sizes.brush / 5, 4, 32) }} />} />
          )}
          {effect !== 'solid' && (
            <SizeControl
              label={effect === 'mosaic' ? 'Block size' : 'Strength'}
              value={sizes.strength}
              min={4}
              max={60}
              onChange={set('strength')}
              preview={effect === 'mosaic'
                ? <span className="block h-[28px] w-[28px]" style={{ backgroundImage: 'conic-gradient(var(--bx-fg) 25%, var(--bx-muted) 0 50%, var(--bx-fg) 0 75%, var(--bx-muted) 0)', backgroundSize: `${clamp(sizes.strength / 2, 3, 14)}px ${clamp(sizes.strength / 2, 3, 14)}px` }} />
                : <span className="block h-[16px] w-[16px] bg-ink" style={{ filter: `blur(${clamp(sizes.strength / 8, 0.5, 6)}px)` }} />}
            />
          )}
          {effect === 'solid' && markColors}
        </>
      )}
      {tool !== 'blur' && markColors}
      {(tool === 'pen' || tool === 'arrow' || tool === 'box') && (
        <SizeControl label="Thickness" value={sizes[tool]} min={1} max={30} onChange={set(tool)} preview={lineSample(sizes[tool])} />
      )}
      {tool === 'text' && (
        <SizeControl label="Text size" value={sizes.text} min={12} max={120} onChange={set('text')} preview={<span className="font-bold leading-none" style={{ color, fontSize: clamp(sizes.text * 0.5, 10, 30) }}>Aa</span>} />
      )}
      <p className="px-3 pb-3 text-[13px] leading-relaxed text-muted">
        {tool === 'text' ? 'Click the picture, type, press Enter.' : tool === 'blur' ? `Drag over what to hide with the ${area}.` : 'Drag on the picture.'}
      </p>
      </div>
    </div>
  )

  const edit = (
    <div className="editor-sections">
      <Section title="Crop" aside={<span className="num text-[13px] text-muted">{cropping ? `${crop.width}×${crop.height}` : `${natural.width}×${natural.height}`}</span>}>
        <Segmented
          label="Crop shape"
          size="sm"
          className="flex w-full [&>button]:flex-1 [&>button]:px-0"
          value={aspect}
          onChange={setAspect}
          options={ASPECTS.map((a) => ({ value: a.value, label: a.label }))}
        />
        {cropOn && <div className="mt-2 text-[13px] leading-relaxed text-muted">Drag the frame on the picture to place it.</div>}
      </Section>
    </div>
  )

  const colorTab = (
    <div className="editor-sections">
      <Section title="Color" aside={<Button variant="ghost" icon="refresh" className="h-6" isDisabled={!graded} onPress={() => setAdjust(FLAT)}>Reset</Button>}>
        <div className="flex flex-col gap-1.5">
          {(['brightness', 'contrast', 'saturation'] as const).map((key) => (
            <div key={key} className="flex items-center gap-3">
              <span className="label w-[92px] shrink-0">{key}</span>
              <Slider label={key} value={adjust[key]} min={0} max={200} step={5} onChange={(value) => setAdjust((a) => ({ ...a, [key]: value }))} onCommit={(value) => setAdjust((a) => ({ ...a, [key]: value }))} format={(value) => `${value}%`} className="flex-1" />
            </div>
          ))}
        </div>
      </Section>
    </div>
  )

  const exporting = (
    <div className="editor-sections">
      <div className="editor-actions flex flex-col gap-2 p-4">
        <Button variant="primary" icon="share" isDisabled={!changed || busy || !clipsDir} onPress={() => void save(false, true)}>{busy ? 'Saving…' : 'Save & share'}</Button>
        <Button icon="save" isDisabled={!changed || busy || !clipsDir} onPress={() => void save(false)}>Save a copy</Button>
        {!clipsDir && <div className="text-[13px] text-warn">The clips folder is unavailable. Check the engine connection.</div>}
        <Button variant="ghost" isDisabled={!changed || busy} onPress={() => setConfirmReplace(true)}>Replace original…</Button>
        {saved && (
          <div className="border-t border-line pt-3">
            <div className="mb-2 truncate text-[13px] font-bold" title={saved}>Saved · {fileName(saved)}</div>
            {savedSignature !== editSignature && <div className="mb-2 text-[13px] text-muted">Your edit has changed since this save.</div>}
            {savedShareable
              ? <Button variant="primary" icon="share" isDisabled={!canShareSaved(savedSignature, editSignature, savedShareable)} onPress={() => void attempt('The share sheet did not open', () => engine.share(saved))}>Share saved image</Button>
              : <div className="text-[13px] text-muted">Save a copy to share this edit.</div>}
          </div>
        )}
      </div>
    </div>
  )

  return (
    <div className="editor-workspace flex h-full min-h-0 gap-2">
      <div data-drop className={clsx('pane flex min-w-0 flex-1 flex-col pt-3', hover && 'is-focused')}>
        <h3 className="pane-title">screenshot</h3>
        <div className="flex h-10 shrink-0 items-center gap-0.5 border-b border-line px-2">
          <IconButton icon="undo" label="Undo (Ctrl+Z)" isDisabled={ops.length === 0} onPress={undo} />
          <IconButton icon="undo" label="Redo (Ctrl+Y)" className="-scale-x-100" isDisabled={undone.length === 0} onPress={redo} />
          <span className="mx-2 h-5 w-px bg-line" aria-hidden="true" />
          <IconButton icon="rotateCcw" label="Turn left" isDisabled={!baked} onPress={() => transform({ kind: 'rotate', direction: 'ccw' })} />
          <IconButton icon="rotateCw" label="Turn right" isDisabled={!baked} onPress={() => transform({ kind: 'rotate', direction: 'cw' })} />
          <IconButton icon="flipH" label="Flip left to right" isDisabled={!baked} onPress={() => transform({ kind: 'flip', axis: 'h' })} />
          <IconButton icon="flipV" label="Flip top to bottom" isDisabled={!baked} onPress={() => transform({ kind: 'flip', axis: 'v' })} />
          <span className="mx-2 h-5 w-px bg-line" aria-hidden="true" />
          <IconButton icon="zoomOut" label="Zoom out (-)" isDisabled={!baked || view.zoom <= MIN_ZOOM} onPress={() => zoomBy(0.8)} />
          <button type="button" title="Fit the picture (0)" onClick={() => zoomBy('fit')} className="num h-7 min-w-[52px] px-1 text-[13px] font-bold text-ink hover:bg-raised">{percent}%</button>
          <IconButton icon="zoomIn" label="Zoom in (+)" isDisabled={!baked || view.zoom >= MAX_ZOOM} onPress={() => zoomBy(1.25)} />
          <button type="button" title="Actual size, one picture pixel to a screen pixel (1)" onClick={() => zoomBy('actual')} className="num h-7 px-1.5 text-[13px] text-muted hover:bg-raised hover:text-ink">1:1</button>
          <div className="flex-1" />
          <Button variant="primary" icon="chevronLeft" className="h-7" title="Close the screenshot and return to the clips" onPress={onBack}>{clips > 0 ? `Back to clips (${clips})` : 'Back'}</Button>
        </div>
        <div
          ref={stage}
          className="relative min-h-0 min-w-0 flex-1 overflow-hidden bg-canvas p-4"
          style={{ cursor: grab === 'panning' ? 'grabbing' : grab === 'ready' ? 'grab' : undefined }}
          onPointerDownCapture={(event) => {
            if (!(event.button === 1 || (event.button === 0 && spaceHeld.current))) return
            event.preventDefault()
            event.stopPropagation()
            try { event.currentTarget.setPointerCapture(event.pointerId) } catch { /* a pointer that is not held */ }
            panning.current = { x: event.clientX, y: event.clientY, from: { x: view.x, y: view.y } }
            setGrab('panning')
          }}
          onPointerMove={(event) => {
            const start = panning.current
            if (start) setView((current) => keep({ zoom: current.zoom, x: start.from.x + event.clientX - start.x, y: start.from.y + event.clientY - start.y }))
          }}
          onPointerUp={() => {
            if (!panning.current) return
            panning.current = null
            setGrab(spaceHeld.current ? 'ready' : 'idle')
          }}
        >
          {failed ? (
            <div className="grid h-full w-full place-items-center text-muted">This image could not be shown.</div>
          ) : (
            <div
              className="absolute left-1/2 top-1/2"
              style={{ width: fitted.width * view.zoom, height: fitted.height * view.zoom, transform: `translate(calc(-50% + ${view.x}px), calc(-50% + ${view.y}px))`, transition: panning.current ? 'none' : 'width var(--bx-dur-fast) var(--bx-ease-out), height var(--bx-dur-fast) var(--bx-ease-out), transform var(--bx-dur-fast) var(--bx-ease-out)' }}
            >
              <canvas ref={base} className="absolute inset-0 h-full w-full" style={{ filter, imageRendering: percent >= 200 ? 'pixelated' : 'auto' }} />
              <canvas
                ref={overlay}
                className={clsx('absolute inset-0 h-full w-full touch-none', drawing && 'cursor-crosshair')}
                style={{ pointerEvents: drawing ? 'auto' : 'none' }}
                onPointerDown={onDown}
                onPointerMove={onMove}
                onPointerUp={onUp}
                onPointerCancel={onUp}
              />
              {cropOn && !drawing && natural.width > 0 && <CropOverlay frame={natural} crop={crop} onCrop={setCrop} ratio={ratio} />}
              {typing && (
                <input
                  ref={typingInput}
                  value={typing.value}
                  aria-label="Text to place"
                  placeholder="Type, then Enter"
                  onChange={(event) => setTyping({ ...typing, value: event.target.value })}
                  onKeyDown={(event) => {
                    if (event.key === 'Enter') finishText(true)
                    else if (event.key === 'Escape') finishText(false)
                    event.stopPropagation()
                  }}
                  onBlur={() => finishText(true)}
                  className="field absolute z-10 min-w-[180px] bx-pop-in"
                  style={{ left: `${(typing.at.x / natural.width) * 100}%`, top: `${(typing.at.y / natural.height) * 100}%` }}
                />
              )}
            </div>
          )}
        </div>
      </div>
      <aside className="pane editor-inspector flex shrink-0 flex-col">
        <h3 className="pane-title">tools</h3>
        <Tabs
          label="Screenshot tools"
          value={tab}
          onChange={setTab}
          className="mt-1"
          tabs={[
            { id: 'edit', label: 'Edit', icon: 'crop', content: edit },
            { id: 'draw', label: 'Draw', icon: 'edit', content: draw },
            { id: 'color', label: 'Color', icon: 'palette', content: colorTab },
            { id: 'export', label: 'Export', icon: 'share', content: exporting },
          ]}
        />
      </aside>
      <DialogBox
        isOpen={confirmReplace}
        onClose={() => setConfirmReplace(false)}
        title="Replace the original?"
        actions={
          <>
            <Button variant="ghost" onPress={() => setConfirmReplace(false)}>Cancel</Button>
            <Button variant="danger" onPress={() => { setConfirmReplace(false); void save(true) }}>Replace</Button>
          </>
        }
      >
        The screenshot is overwritten with this edit. This cannot be undone.
      </DialogBox>
    </div>
  )
}
