// The sequence's timeline: the clips end to end as separate blocks, and under them the active clip's
// own cut lane. The blocks are the order; the lane is the trim, the split and the sound of one clip.

import clsx from 'clsx'
import { Fragment, useEffect, useMemo, useRef, useState, type Dispatch, type SetStateAction } from 'react'
import { Icon } from '../components/icons'
import { mediaUrl } from '../lib/bridge'
import { simpleTitle } from '../lib/clip_label'
import { duration } from '../lib/format'
import { columnsIn, columnsOf, peakRange, sharedScale } from '../lib/peaks'
import { heardPeaks, trackName, type AudioTrack } from '../lib/audio'
import { clipOutputLength, clipStart, reorder, sequenceLength, type SequenceClip } from '../lib/sequence'
import { ratioAtTime, rulerSteps, snapToKeyframe, timeAtRatio, type TimelineView } from '../lib/timeline_view'
import { moveAll, moveEdge, movePart, type Part } from '../lib/segments'
import { CUT_STYLE } from './editor-parts'

/** The stretches between kept parts: what the cut throws away, drawn dimmed on every lane. */
function cutAway(parts: Part[], total: number): Part[] {
  const gaps: Part[] = []
  let cursor = 0
  for (const [from, to] of parts) {
    if (from - cursor > 0.001) gaps.push([cursor, from])
    cursor = to
  }
  if (total - cursor > 0.001) gaps.push([cursor, total])
  return gaps
}

/** A ruler's marks: a tall line and a label at each step, and small lines dividing the step. Steps come from
 *  the scale, so a long sequence shows few labels and a close zoom shows tenths, never a crowd. */
function RulerMarks({ start, span, width }: { start: number; span: number; width: number }) {
  if (width <= 0) return null
  const perSecond = width / Math.max(0.001, span)
  const { label, minor } = rulerSteps(perSecond)
  const marks: number[] = []
  for (let k = Math.ceil(start / label - 1e-9); k * label <= start + span + 1e-9; k++) marks.push(k * label)
  const name = (t: number) => {
    if (label >= 1) return duration(t)
    const whole = Math.floor(t + 1e-9)
    return `${duration(whole)}.${Math.round((t - whole) * 10) % 10}`
  }
  const shift = -start * perSecond
  return (
    <div aria-hidden="true" className="pointer-events-none absolute inset-0">
      <div
        className="absolute inset-0"
        style={{
          backgroundImage: 'linear-gradient(90deg, var(--bx-line-strong) 1px, transparent 1px), linear-gradient(90deg, var(--bx-line) 1px, transparent 1px)',
          backgroundRepeat: 'repeat-x',
          backgroundSize: `${label * perSecond}px 100%, ${minor * perSecond}px 6px`,
          backgroundPosition: `${shift}px 0, ${shift}px 100%`,
        }}
      />
      {marks
        // A label the ruler would cut in half at its right end is left out.
        .filter((t) => (t - start) * perSecond < width - 40)
        .map((t) => (
          <span key={t} className="num absolute top-[8px] text-[12px] leading-none text-muted" style={{ left: (t - start) * perSecond + 4 }}>
            {name(t)}
          </span>
        ))}
    </div>
  )
}

/** What a drag on the timeline is doing. `base` and `pointer` are where the drag started. */
type CutDrag =
  | { mode: 'seek' }
  | { mode: 'edge'; index: number; edge: 'in' | 'out'; moved: boolean; snapshot?: Part[]; last?: number }
  | { mode: 'part'; index: number; moved: boolean; base: number; pointer: number; snapshot?: Part[]; last?: number }
  | { mode: 'bar'; moved: boolean; base: number; pointer: number; snapshot?: Part[]; last?: number }

/** The active clip's cut: one row a track, then the kept parts with a grip on each edge. */
export function ClipTimeline({ duration: total, time, parts: partsProp, active, tracks, trackIndexes, gains, muted = [], solo = [], sound = 'ready', view, onView, onSelect, onPartMenu, onEdge, onPart, onBar, onSeek, onDrag, flash, peaks, undecoded, keyframes, onFocusTrack, lockReason = '', expanded, onExpanded }: {
  duration: number
  time: number
  parts: Part[]
  active: number
  /** One row a track whose sound is heard: its name in the gutter, its own sound to the right. */
  tracks: AudioTrack[]
  /** Each row's ordinal in the file, so a hidden row still keeps its level and its waveform. */
  trackIndexes: number[]
  /** Each track's level as it is heard, indexed by the file's own ordinal. */
  gains: number[]
  /** Per track, by ordinal: the row is drawn muted, and a solo row carries the accent. */
  muted?: boolean[]
  solo?: boolean[]
  /** Whether the waveform read is still out, done, or failed: the three look different. */
  sound?: 'loading' | 'ready' | 'failed'
  /** The part of the clip on screen; the toolbar's zoom and the wheel both write it. */
  view: TimelineView
  onView: Dispatch<SetStateAction<TimelineView>>
  onSelect: (index: number) => void
  /** A right click on a part (or on one of its grips), at the pointer's place. */
  onPartMenu: (index: number, x: number, y: number) => void
  /** `first` is true for the first step of a drag, so the whole drag is one Undo; `live` while the pointer is down. */
  onEdge: (index: number, edge: 'in' | 'out', t: number, first: boolean, live: boolean) => void
  /** The part's new start, in source time. */
  onPart: (index: number, from: number, first: boolean) => void
  /** The kept range's new start, in source time. */
  onBar: (from: number, first: boolean) => void
  onSeek: (t: number) => void
  /** A cut drag (an edge, a part or the whole kept range) starts or ends: the picture does not chase the handle meanwhile. */
  onDrag?: (on: boolean) => void
  /** G7: a split just happened here (source seconds): a line flashes at the cut for 180 ms. `id` makes each split its own flash. */
  flash?: { at: number; id: number } | null
  peaks?: number[][]
  /** File ordinals whose waveform the engine could not decode: their zeros are unreadable, not silent. */
  undecoded?: number[]
  /** Video keyframes, in source seconds: ticks on the ruler, and where Alt snaps an edge. */
  keyframes?: number[]
  /** Show one track in the mixer, so a lane and its fader are one thing. */
  onFocusTrack?: (index: number) => void
  /** Why this file's levels cannot be edited, or '' when they can. A locked mix has no fader to focus,
   *  so its lane is plain text instead of a button that would do nothing. */
  lockReason?: string
  /** The one audio fold, shared with the inspector: closed draws one combined lane, open one lane per source. */
  expanded: boolean
  onExpanded: (open: boolean) => void
}) {
  // The time axis is the ruler's own cell: its left edge is 0 s, so the gutter beside it is not time.
  const axis = useRef<HTMLDivElement>(null)
  const [width, setWidth] = useState(0)
  useEffect(() => {
    const element = axis.current
    if (!element) return
    setWidth(element.clientWidth)
    const observer = new ResizeObserver(([entry]) => setWidth(entry.contentRect.width))
    observer.observe(element)
    return () => observer.disconnect()
  }, [])
  // While a cut handle is dragged the lane works on its own copy of the parts: each pointer move rewrites this small state and nothing
  // else (the editor around it is not rendered at all), and the one real edit is made on release. Writing the editor's state on every
  // move re-rendered the whole editor per frame, which is what made the trim bar feel heavy.
  const [dragParts, setDragParts] = useState<Part[] | null>(null)
  const parts = dragParts ?? partsProp
  // Sound starts as one combined lane; expanding is a look at the same mix, never a change to it. The
  // fold belongs to the editor, so the inspector and this lane always show the same state.
  const drag = useRef<CutDrag | null>(null)
  const [dragging, setDragging] = useState<string | null>(null)
  // Alt held while an edge is dragged: the edge lands on the nearest keyframe.
  const snapping = useRef(false)
  const at = (clientX: number) => {
    const rect = axis.current!.getBoundingClientRect()
    return timeAtRatio((clientX - rect.left) / Math.max(1, rect.width), view)
  }
  const snappedAt = (t: number) => (snapping.current && width > 0 ? snapToKeyframe(t, keyframes ?? [], (6 / width) * view.span) : t)
  const step = (d: CutDrag, t: number) => {
    if (d.mode === 'seek') { onSeek(t); return }
    d.moved = true
    d.last = t
    const base = d.snapshot ?? partsProp
    if (d.mode === 'edge') setDragParts(moveEdge(base, d.index, d.edge, snappedAt(t), total, 0))
    else if (d.mode === 'part') setDragParts(movePart(base, d.index, d.base + (t - d.pointer) - (base[d.index]?.[0] ?? 0), total))
    else setDragParts(moveAll(base, d.base + (t - d.pointer) - (base[0]?.[0] ?? 0), total))
  }
  // Each pointer move is applied as it comes (the browser already delivers one per frame): no frame of waiting on top of the lane's own render.
  const schedule = (t: number) => {
    const d = drag.current
    if (d) step(d, t)
  }
  const begin = (event: React.PointerEvent, next: CutDrag) => {
    event.stopPropagation()
    ;(event.target as HTMLElement).setPointerCapture(event.pointerId)
    drag.current = next.mode === 'seek' ? next : { ...next, snapshot: partsProp }
    if (next.mode !== 'seek') onDrag?.(true)
  }
  // A focused handle takes the arrows (a second, ten with Shift), Home and End, so a cut can be made
  // without a pointer. Each press is its own Undo step, as one click of a drag would be.
  const nudge = (event: React.KeyboardEvent, current: number, apply: (t: number) => void) => {
    if (event.altKey || event.ctrlKey || event.metaKey) return
    const step = event.shiftKey ? 10 : 1
    let target: number | null = null
    if (event.key === 'ArrowLeft') target = current - step
    else if (event.key === 'ArrowRight') target = current + step
    else if (event.key === 'Home') target = 0
    else if (event.key === 'End') target = total
    if (target === null) return
    event.preventDefault()
    event.stopPropagation()
    apply(Math.max(0, Math.min(total, target)))
  }
  const onMove = (event: React.PointerEvent) => {
    if (!drag.current) return
    snapping.current = event.altKey
    schedule(at(event.clientX))
  }
  const onUp = (event: React.PointerEvent) => {
    snapping.current = event.altKey
    const d = drag.current
    drag.current = null
    setDragging(null)
    if (!d) return
    if (d.mode !== 'seek') onDrag?.(false)
    const t = d.mode === 'seek' ? at(event.clientX) : d.last ?? at(event.clientX)
    if (d.mode === 'seek') { onSeek(t); return }
    if (!d.moved) { setDragParts(null); return }
    // Released: the one real edit, one Undo step, and the shortest-part clamp comes back so the cut never keeps a sliver. The copy the
    // lane drew from is dropped in the same batch.
    if (d.mode === 'edge') onEdge(d.index, d.edge, snappedAt(t), true, false)
    else if (d.mode === 'part') onPart(d.index, d.base + (t - d.pointer), true)
    else onBar(d.base + (t - d.pointer), true)
    setDragParts(null)
  }
  const pct = (t: number) => `${ratioAtTime(t, view) * 100}%`
  const last = parts.length - 1
  const keptFrom = parts[0]?.[0] ?? 0
  const keptTo = parts[last]?.[1] ?? 0
  const activeFrom = parts[active]?.[0] ?? 0
  const activeTo = parts[active]?.[1] ?? total
  const count = columnsIn(width)
  // Every lane drawn together is read against one loudness, so a quiet microphone is not drawn as
  // tall as a loud desktop. The scale covers the whole file, so a zoom does not renormalise it.
  const lanes = trackIndexes.join(',')
  const gainsKey = gains.join(',')
  const scale = useMemo(
    () => sharedScale(trackIndexes
      .filter((index) => !undecoded?.includes(index))
      .map((index) => peaks?.[index])
      .filter((lane): lane is number[] => !!lane)),
    [lanes, peaks, undecoded], // eslint-disable-line react-hooks/exhaustive-deps -- the lanes key names the ordinals read
  )
  // The one lane drawn while folded: every heard row's cached peaks, weighted by the gain it is heard
  // at. The rows already exclude a derived mix, so the mix and its stems are never counted twice.
  const combined = useMemo(
    () => heardPeaks(peaks ?? [], gains, trackIndexes),
    [peaks, gainsKey, lanes], // eslint-disable-line react-hooks/exhaustive-deps -- the keys name the ordinals and levels read
  )
  const combinedScale = useMemo(() => sharedScale(combined ? [combined] : []), [combined])
  const combinedUndecoded = trackIndexes.some((index) => undecoded?.includes(index))
  const separate = expanded && tracks.length > 1
  return (
    <div className="relative select-none" onPointerMove={onMove} onPointerUp={onUp} onPointerCancel={onUp}>
      <div
        className="grid cursor-pointer border border-line bg-canvas"
        style={{ gridTemplateColumns: `${GUTTER}px 1fr` }}
        onPointerDown={(event) => {
          begin(event, { mode: 'seek' })
          onSeek(at(event.clientX))
        }}
        onDoubleClick={() => onView({ start: 0, span: total })}
      >
        {/* The ruler: the scale, and the kept stretch as a handle that slides the whole cut. */}
        <div className="flex h-[28px] cursor-default items-center border-r border-line px-2" onPointerDown={(event) => event.stopPropagation()} onWheel={(event) => event.stopPropagation()}>
          <span className="label leading-none" title="The clip's own time, before its cuts and speed">clip time</span>
        </div>
        <div ref={axis} className="relative h-[28px] overflow-hidden">
          {parts.length > 0 && (
            <div
              role="slider"
              tabIndex={0}
              aria-label="Kept range, all parts together"
              aria-valuemin={0}
              aria-valuemax={total}
              aria-valuenow={keptFrom}
              aria-valuetext={`${keptFrom.toFixed(2)} s`}
              className={clsx('absolute inset-y-0 cursor-grab bg-raised', dragging === 'bar' && 'bg-line')}
              style={{ left: pct(keptFrom), width: `calc(${pct(keptTo)} - ${pct(keptFrom)})` }}
              title="Drag to slide the whole cut"
              onKeyDown={(event) => nudge(event, keptFrom, (t) => onBar(t, true))}
              onPointerDown={(event) => {
                begin(event, { mode: 'bar', moved: false, base: keptFrom, pointer: at(event.clientX) })
                setDragging('bar')
              }}
            />
          )}
          <RulerMarks start={view.start} span={view.span} width={width} />
          {(keyframes ?? [])
            .filter((t) => t >= view.start && t <= view.start + view.span)
            .map((t) => (
              <div key={`key-${t}`} className="pointer-events-none absolute top-0 h-[6px] w-px bg-accent/70" style={{ left: pct(t) }} />
            ))}
        </div>

        {/* Sound starts folded: one waveform of everything heard, with Expand beside its name. Expanding
          *  draws one lane a heard track, each kept at its file ordinal so the mixer and the lane agree.
          *  Alternate rows sit on the pane surface, so two adjacent sources do not read as one tall lane. */}
        {tracks.length > 0 && (separate ? (
          <>
            <div
              className="col-span-2 flex h-[26px] cursor-default items-center gap-2 border-t border-line bg-surface px-2"
              onPointerDown={(event) => event.stopPropagation()}
              onWheel={(event) => event.stopPropagation()}
            >
              <span className="label leading-none" title="One lane for each track you hear">audio · {tracks.length} sources</span>
              <button
                type="button"
                aria-expanded={true}
                aria-label="Collapse audio into one combined waveform"
                title={`Hide audio sources (${tracks.length})`}
                className="btn btn-ghost !h-6 !w-6 shrink-0 !px-0"
                onClick={() => onExpanded(false)}
              >
                <Icon name="compact" size={13} />
              </button>
            </div>
            {tracks.map((track, row) => {
              const index = trackIndexes[row] ?? row
              const name = trackName(track, index)
              const isMuted = !!muted[index]
              const isSolo = !!solo[index]
              // The banding follows the row on screen, never the file's ordinal: hiding the mix must not
              // re-colour the lanes the person is looking at.
              const band = row % 2 === 1 && !isSolo ? 'bg-surface' : ''
              return (
                <Fragment key={track.index}>
                  <div
                    data-lane={index + 1}
                    className={clsx(
                      'flex h-[40px] cursor-default items-center gap-1 border-r border-t border-line px-2',
                      band,
                      isSolo && 'bg-accent-soft',
                    )}
                    onPointerDown={(event) => event.stopPropagation()}
                    onWheel={(event) => event.stopPropagation()}
                  >
                    <button
                      type="button"
                      onClick={() => onFocusTrack?.(index)}
                      className="flex min-w-0 flex-1 items-center text-left"
                      aria-label={`${name}. Show it in the mixer`}
                      title={`${name} · show in the mixer`}
                    >
                      <span className={clsx('label truncate leading-none', isMuted && 'text-warn', isSolo && 'text-accent')} title={name}>
                        {name}
                      </span>
                    </button>
                    {isMuted && <span className="shrink-0 text-warn" title="Muted in the preview and the saved file"><Icon name="speakerOff" size={13} /></span>}
                    {isSolo && (
                      <span className="shrink-0 text-accent" title="Heard alone in the preview only">
                        <Icon name="eye" size={13} />
                      </span>
                    )}
                  </div>
                  <div className={clsx('relative h-[40px] overflow-hidden border-t border-line', band, isSolo && 'bg-accent-soft')}>
                    <Sound
                      values={peaks?.[index]}
                      decoded={!undecoded?.includes(index)}
                      count={count}
                      gain={gains[index] ?? 1}
                      scale={scale}
                      sound={sound}
                      from={Math.max(0, activeFrom - view.start)}
                      to={Math.min(view.span, activeTo - view.start)}
                      total={view.span}
                      start={view.start}
                      span={view.span}
                      fileSeconds={total}
                    />
                  </div>
                </Fragment>
              )
            })}
          </>
        ) : (() => {
          const single = tracks.length === 1
          const index = trackIndexes[0] ?? 0
          // Every heard track under one waveform is the output a person hears, never a guessed mix.
          const name = single ? trackName(tracks[0], index) : 'Combined'
          const isMuted = single && !!muted[index]
          const isSolo = single && !!solo[index]
          return (
            <>
              <div
                data-lane={single ? index + 1 : undefined}
                className={clsx(
                  'flex h-[40px] cursor-default items-center gap-1 border-r border-t border-line px-2',
                  isSolo && 'bg-accent-soft',
                )}
                onPointerDown={(event) => event.stopPropagation()}
                onWheel={(event) => event.stopPropagation()}
              >
                {single && !lockReason ? (
                  <button
                    type="button"
                    onClick={() => onFocusTrack?.(index)}
                    className="flex min-w-0 flex-1 items-center text-left"
                    aria-label={`${name}. Show it in the mixer`}
                    title={`${name} · show in the mixer`}
                  >
                    <span className={clsx('label truncate leading-none', isMuted && 'text-warn', isSolo && 'text-accent')} title={name}>
                      {name}
                    </span>
                  </button>
                ) : (
                  <span
                    className="label min-w-0 flex-1 truncate leading-none"
                    title={single ? `${name} · ${lockReason}` : 'Every track you hear, drawn as one waveform. Expand for each track.'}
                  >
                    {name}
                  </span>
                )}
                {isMuted && <span className="shrink-0 text-warn" title="Muted in the preview and the saved file"><Icon name="speakerOff" size={13} /></span>}
                {isSolo && (
                  <span className="shrink-0 text-accent" title="Heard alone in the preview only">
                    <Icon name="eye" size={13} />
                  </span>
                )}
                {!single && (
                  <button
                    type="button"
                    aria-expanded={false}
                    aria-label="Expand audio into one lane per track"
                    title={`Show audio sources (${tracks.length})`}
                    className="btn btn-ghost !h-6 !w-6 shrink-0 !px-0"
                    onClick={() => onExpanded(true)}
                  >
                    <Icon name="expand" size={13} />
                  </button>
                )}
              </div>
              <div className={clsx('relative h-[40px] overflow-hidden border-t border-line', isSolo && 'bg-accent-soft')}>
                <Sound
                  values={single ? peaks?.[index] : combined}
                  decoded={single ? !undecoded?.includes(index) : !combinedUndecoded}
                  count={count}
                  gain={single ? gains[index] ?? 1 : 1}
                  scale={single ? scale : combinedScale}
                  sound={sound}
                  from={Math.max(0, activeFrom - view.start)}
                  to={Math.min(view.span, activeTo - view.start)}
                  total={view.span}
                  start={view.start}
                  span={view.span}
                  fileSeconds={total}
                />
              </div>
            </>
          )
        })())}

        {/* The cut: what is kept as blocks, what is dropped left as a flat tint in its place. */}
        <div className="flex h-[40px] cursor-default items-center border-r border-t border-line px-2" onPointerDown={(event) => event.stopPropagation()}>
          <span className="label leading-none" title="Drag the ends to trim, split to cut a part out">trim</span>
        </div>
        <div className="relative h-[40px] overflow-hidden border-t border-line">
          {cutAway(parts, total).map(([from, to], index) => (
            <div key={`cut-${index}`} className="pointer-events-none absolute inset-0" style={{ left: pct(from), width: `calc(${pct(to)} - ${pct(from)})`, ...CUT_STYLE }} />
          ))}
          {parts.map(([from, to], index) => (
            <div
              key={`part-${index}`}
              className={clsx(
                'absolute inset-y-0 cursor-grab',
                // The selected part is a brighter block and the accent sits on its two edges, where the cut
                // is, rather than framing the whole span: a full orange rectangle is more accent than the
                // row can carry and it swamped the grips it was meant to point at.
                index === active ? 'border-x-2 border-accent bg-line' : 'border border-line-strong bg-raised hover:bg-line',
              )}
              style={{ left: pct(from), width: `calc(${pct(to)} - ${pct(from)})` }}
              onContextMenu={(event) => {
                event.preventDefault()
                onSelect(index)
                onPartMenu(index, event.clientX, event.clientY)
              }}
              onPointerDown={(event) => {
                // Only the left button drags; a right click must not start a seek on the row beneath.
                if (event.button !== 0) return event.stopPropagation()
                onSelect(index)
                begin(event, { mode: 'part', index, moved: false, base: from, pointer: at(event.clientX) })
                setDragging(`part-${index}`)
                onSeek(at(event.clientX))
              }}
            />
          ))}
          {parts.map(([from, to], index) =>
            (['in', 'out'] as const).map((edge) => (
              <div
                key={`${edge}-${index}`}
                role="slider"
                tabIndex={0}
                aria-label={`${edge === 'in' ? 'Start' : 'End'} of part ${index + 1}`}
                aria-valuemin={0}
                aria-valuemax={total}
                aria-valuenow={edge === 'in' ? from : to}
                aria-valuetext={`${(edge === 'in' ? from : to).toFixed(2)} s`}
                className={clsx(
                  'absolute inset-y-0 w-[12px] cursor-ew-resize',
                  dragging === `edge-${index}-${edge}` ? 'bg-ink' : index === active ? 'bg-accent' : 'bg-line-strong hover:bg-accent',
                )}
                // Each handle sits on its own side of the edge, so where a split left two parts touching,
                // the end of one and the start of the next can both be taken hold of.
                style={{ left: edge === 'in' ? pct(from) : `calc(${pct(to)} - 12px)` }}
                onKeyDown={(event) => nudge(event, edge === 'in' ? from : to, (t) => {
                  onSelect(index)
                  onEdge(index, edge, t, true, false)
                })}
                onContextMenu={(event) => {
                  event.preventDefault()
                  onSelect(index)
                  onPartMenu(index, event.clientX, event.clientY)
                }}
                onPointerDown={(event) => {
                  if (event.button !== 0) return event.stopPropagation()
                  onSelect(index)
                  begin(event, { mode: 'edge', index, edge, moved: false })
                  setDragging(`edge-${index}-${edge}`)
                }}
              />
            )),
          )}
        </div>
      </div>
      {/* The playhead crosses every row at once - the ruler, each track, the cut - as the cursor of a ledger.
       * No head: the column is already the brightest mark, and a square head covered the first ruler label. */}
      {/* G8: the playhead moves by transform (the measured width of the axis), not by left: it follows the picture without layout. */}
      <div className="pointer-events-none absolute bottom-0 left-0 top-0 w-[2px] bg-ink will-change-transform" style={{ transform: `translate3d(${GUTTER + width * ratioAtTime(time, view)}px, 0, 0)` }} />
      {/* G7: the cut flashes where a split was made. */}
      {flash && flash.at >= view.start && flash.at <= view.start + view.span && (
        <div key={flash.id} aria-hidden="true" className="bx-split-flash pointer-events-none absolute bottom-0 left-0 top-0 w-[2px] bg-accent" style={{ transform: `translate3d(${GUTTER + width * ratioAtTime(flash.at, view)}px, 0, 0)` }} />
      )}
    </div>
  )
}

/** The sequence's own axis: its ruler, and the clips end to end under it as one lane (V1), each block as wide
 *  as its own output, so the order and the lengths are both the picture. A block is dragged to another slot;
 *  the playhead is the sequence's own second. The clip's cut lane below has its own ruler: source time. */
export function SequenceLanes({ clips, active, time, onSelect, onMenu, onSeek, onReorder }: {
  clips: SequenceClip[]
  active: number
  /** Seconds into the sequence. */
  time: number
  onSelect: (index: number) => void
  /** A right click on a clip's block, at the pointer's place. */
  onMenu: (index: number, x: number, y: number) => void
  onSeek: (t: number) => void
  onReorder: (from: number, to: number) => void
}) {
  const total = Math.max(0.001, sequenceLength(clips))
  const row = useRef<HTMLDivElement>(null)
  const [width, setWidth] = useState(0)
  const drag = useRef<{ from: number; to: number; x: number; moved: boolean } | null>(null)
  const [carrying, setCarrying] = useState<number | null>(null)
  // While a clip is carried: the slot it would land in, and how far the pointer has pulled it from where it was picked up.
  const [slot, setSlot] = useState(0)
  const [pull, setPull] = useState(0)
  // G5: the block that was dropped settles (a small scale pulse, fast token). G4: a clip that left the sequence dissolves where it was for
  // 90 ms; the others close the gap at once (no layout animation).
  const [settled, setSettled] = useState<string | null>(null)
  const [ghosts, setGhosts] = useState<{ id: string; left: number; width: number }[]>([])
  const previous = useRef<{ id: string; left: number; width: number }[]>([])
  useEffect(() => {
    const whole = Math.max(0.001, sequenceLength(clips))
    let at = 0
    const now = clips.map((clip) => {
      const length = clipOutputLength(clip)
      const entry = { id: clip.id, left: (at / whole) * 100, width: (length / whole) * 100 }
      at += length
      return entry
    })
    const ids = new Set(now.map((entry) => entry.id))
    const gone = previous.current.filter((entry) => !ids.has(entry.id))
    previous.current = now
    if (gone.length === 0) return
    setGhosts(gone)
    const timer = window.setTimeout(() => setGhosts([]), 90)
    return () => window.clearTimeout(timer)
  }, [clips])
  useEffect(() => {
    const element = row.current
    if (!element) return
    setWidth(element.clientWidth)
    const observer = new ResizeObserver(([entry]) => setWidth(entry.contentRect.width))
    observer.observe(element)
    return () => observer.disconnect()
  }, [])
  const slotAt = (clientX: number) => {
    const rect = row.current!.getBoundingClientRect()
    let x = 0
    for (let i = 0; i < clips.length; i++) {
      const w = (clipOutputLength(clips[i]) / total) * rect.width
      if (clientX < rect.left + x + w / 2) return i
      x += w
    }
    return clips.length - 1
  }
  const settleTimer = useRef(0)
  const up = () => {
    const d = drag.current
    drag.current = null
    setCarrying(null)
    setPull(0)
    if (d?.moved && d.from !== d.to) {
      const dropped = clips[d.from]?.id ?? null
      onReorder(d.from, d.to)
      setSettled(dropped)
      window.clearTimeout(settleTimer.current)
      settleTimer.current = window.setTimeout(() => setSettled(null), 140)
    }
  }
  const upRef = useRef(up)
  upRef.current = up
  const slotAtRef = useRef(slotAt)
  slotAtRef.current = slotAt
  useEffect(() => {
    const move = (event: PointerEvent) => {
      const d = drag.current
      if (!d) return
      if (!d.moved && Math.abs(event.clientX - d.x) < 5) return
      d.moved = true
      d.to = slotAtRef.current(event.clientX)
      setSlot(d.to)
      setPull(event.clientX - d.x)
      setCarrying(d.from)
    }
    const finish = () => upRef.current()
    window.addEventListener('pointermove', move)
    window.addEventListener('pointerup', finish)
    window.addEventListener('pointercancel', finish)
    return () => {
      window.removeEventListener('pointermove', move)
      window.removeEventListener('pointerup', finish)
      window.removeEventListener('pointercancel', finish)
      window.clearTimeout(settleTimer.current)
    }
  }, [])
  // Where every block sits, in the order a drop would make while one is carried (else the sequence's own).
  const order = carrying !== null ? reorder(clips, carrying, slot) : clips
  const lefts = new Map<string, number>()
  const natural = new Map<string, number>()
  let cursor = 0
  for (const clip of order) {
    lefts.set(clip.id, (cursor / total) * 100)
    cursor += clipOutputLength(clip)
  }
  cursor = 0
  for (const clip of clips) {
    natural.set(clip.id, (cursor / total) * 100)
    cursor += clipOutputLength(clip)
  }
  const ratio = Math.min(1, Math.max(0, time / total))
  const seekAt = (clientX: number) => {
    const rect = row.current!.getBoundingClientRect()
    onSeek(Math.min(total, Math.max(0, ((clientX - rect.left) / Math.max(1, rect.width)) * total)))
  }
  return (
    <div className="relative select-none border border-line bg-canvas">
      <div className="grid" style={{ gridTemplateColumns: `${GUTTER}px 1fr` }}>
        <div className="flex h-[28px] items-center border-r border-line px-2">
          <span className="label leading-none">time</span>
        </div>
        <div
          className="relative h-[28px] cursor-pointer overflow-hidden"
          onPointerDown={(event) => {
            event.currentTarget.setPointerCapture(event.pointerId)
            seekAt(event.clientX)
          }}
          onPointerMove={(event) => event.buttons === 1 && seekAt(event.clientX)}
        >
          <RulerMarks start={0} span={total} width={width} />
        </div>
        <div className="flex h-[64px] items-center border-r border-t border-line px-2">
          <span className="label leading-none" title="The clips of the sequence, in play order">clips</span>
        </div>
        <div ref={row} className={clsx('relative h-[64px] overflow-hidden border-t border-line', carrying !== null && 'cursor-grabbing')}>
          {ghosts.map((ghost) => (
            <div key={`ghost-${ghost.id}`} aria-hidden="true" className="bx-ghost-out pointer-events-none absolute inset-y-0 z-10 bg-raised" style={{ left: `${ghost.left}%`, width: `${ghost.width}%` }} />
          ))}
          {/* Carrying a clip: the others slide aside to show the order it would make, and a dashed slot marks where it lands. */}
          {carrying !== null && clips[carrying] && (
            <div
              aria-hidden="true"
              className="pointer-events-none absolute inset-y-[3px] z-0 border-2 border-dashed border-accent bg-accent/10"
              style={{ left: `${lefts.get(clips[carrying].id) ?? 0}%`, width: `${(clipOutputLength(clips[carrying]) / total) * 100}%`, transition: 'left var(--bx-dur-fast) var(--bx-ease-out)' }}
            />
          )}
          {clips.map((clip, index) => {
            const length = clipOutputLength(clip)
            const pixels = (length / total) * width
            const naturalPx = ((natural.get(clip.id) ?? 0) / 100) * width
            const slotShift = ((lefts.get(clip.id) ?? 0) / 100) * width - naturalPx
            return (
              <button
                type="button"
                aria-label={`Sequence clip ${index + 1}: ${clip.name}`}
                key={clip.id}
                title={`${clip.name} · ${duration(length)} · Alt+Left/Right moves it`}
                onKeyDown={(event) => {
                  if (!event.altKey || (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight')) return
                  event.preventDefault()
                  event.stopPropagation()
                  const to = index + (event.key === 'ArrowLeft' ? -1 : 1)
                  if (to >= 0 && to < clips.length) onReorder(index, to)
                }}
                onContextMenu={(event) => {
                  event.preventDefault()
                  onSelect(index)
                  onSeek(clipStart(clips, index))
                  onMenu(index, event.clientX, event.clientY)
                }}
                onPointerDown={(event) => {
                  if (event.button !== 0) return
                  event.preventDefault()
                  onSelect(index)
                  onSeek(clipStart(clips, index))
                  drag.current = { from: index, to: index, x: event.clientX, moved: false }
                }}
                className={clsx(
                  // G3: a clip that joins the lane scales in (only a newly mounted clip runs it); G6: the highlight moves on the fast token.
                  'bx-pop-in absolute inset-y-0 flex min-w-0 items-center gap-2 overflow-hidden border-r border-line text-left',
                  carrying === index ? 'z-20 cursor-grabbing border border-accent bg-line shadow-[0_6px_16px_rgb(0_0_0/0.5)]' : 'cursor-grab',
                  carrying !== index && (index === active ? 'bg-line shadow-[inset_0_0_0_2px_var(--bx-accent)]' : 'bg-raised hover:bg-line'),
                  settled === clip.id && 'bx-settle',
                  pixels >= 72 ? 'px-2' : 'px-0',
                )}
                style={{
                  left: `${lefts.get(clip.id) ?? 0}%`,
                  width: `${(length / total) * 100}%`,
                  // The carried block follows the pointer (kept inside the lane) with no easing; the rest ease into their slots.
                  // The block's left edge already sits in its drop slot, so the pointer delta is taken back by that shift.
                  transform: carrying === index ? `translateX(${Math.min(Math.max(pull, -naturalPx), width - naturalPx - pixels) - slotShift}px)` : undefined,
                  transition: carrying === index ? 'none' : 'left var(--bx-dur-fast) var(--bx-ease-out), transform var(--bx-dur-fast) var(--bx-ease-out), background-color var(--bx-dur-fast), box-shadow var(--bx-dur-fast)',
                }}
              >
                {pixels >= 140 && (
                  <video
                    src={`${mediaUrl(clip.path)}#t=0.1`}
                    preload="metadata"
                    muted
                    tabIndex={-1}
                    className="pointer-events-none h-[46px] w-[82px] shrink-0 border border-line bg-canvas object-cover"
                  />
                )}
                {pixels >= 72 && (
                  <span className="min-w-0">
                    <span className="block truncate text-[14px] font-bold leading-tight">{simpleTitle(clip.name)}</span>
                    <span className="num mt-0.5 block truncate text-[12px] leading-tight text-muted">
                      {duration(length)}
                      {clip.speed !== 1 && ` · ${clip.speed}x`}
                      {clip.parts.length > 1 && ' · cut'}
                    </span>
                  </span>
                )}
              </button>
            )
          })}
        </div>
      </div>
      {/* The sequence's playhead crosses its ruler and lane, as the clip's does its rows. */}
      <div className="pointer-events-none absolute bottom-0 top-0 w-[2px] bg-ink" style={{ left: `calc(${GUTTER}px + (100% - ${GUTTER}px) * ${ratio})` }} />
    </div>
  )
}

/** The gutter is the same on every row, so every 0 s lines up: the level, then time. It is wide
 *  enough for a source name and its mute mark, with the expand control beside a folded lane. */
export const GUTTER = 148
/** A lit column and the gap beside it. */
const PITCH = 3

/** One track's sound, drawn the way the product draws sound: a column every 3 px, bottom-anchored and square.
 *  The level scales it from the bottom, so a muted track is a flat line and 200 % is clipped by the row: the
 *  picture and the mix move together, at the same value. */
function Sound({ values, decoded = true, count, gain, scale, sound = 'ready', from, to, total, start = 0, span, fileSeconds }: {
  values?: number[]
  /** False when the engine could not decode this lane: its zeros are not silence. */
  decoded?: boolean
  count: number
  gain: number
  /** The loudness every lane on screen is read against. */
  scale?: number
  /** The waveform read itself: still out, done, or failed. */
  sound?: 'loading' | 'ready' | 'failed'
  from: number
  to: number
  total: number
  /** The visible window's start and length in the clip's seconds, so a zoom shows its own sound. */
  start?: number
  span?: number
  /** The whole file's length: the peaks are spread over it, not over the visible window. */
  fileSeconds?: number
}) {
  const visible = useMemo(() => {
    if (!values) return []
    const [first, last] = peakRange(start, span ?? total, fileSeconds ?? total, values.length)
    return values.slice(first, last)
  }, [values, start, span, total, fileSeconds])
  const columns = useMemo(() => columnsOf(visible, count, scale), [visible, count, scale])
  const silent = values !== undefined && visible.every((value) => value === 0)
  const quiet = !silent && columns.every((value) => value < 0.002)
  // Reading, failure and a silent track are three different things, and a flat row alone would read
  // as all three. The label is the one thing the picture cannot say. A lane the engine could not decode
  // says so on its own row: its zeros must never be drawn as a silent track.
  const status = !decoded ? 'SOUND COULD NOT BE READ'
    : sound === 'failed' ? 'SOUND COULD NOT BE READ'
      : sound === 'loading' && values === undefined ? 'READING SOUND'
        : values === undefined ? 'NO SOUND'
          : silent ? 'SILENT' : quiet ? 'QUIET' : ''
  // The columns are built once per width, part and level-independent: a drag of the level only rewrites the
  // svg's transform below, so it stays smooth however many columns the lane holds.
  const layer = useMemo(
    () =>
      columns.map((v, i) => {
        // 92 rather than 100 leaves a pixel of headroom, so a loud clip does not touch the row's border and
        // read as clipped. A level above 100 % still runs into it, which is the point of the level.
        // The kept part carries the sound at full strength and a cut-away span is left quiet, so the columns
        // agree with the cut instead of contradicting it. The playhead stays the brightest mark on the row,
        // which is why the sound is a step below the ink rather than white.
        return <rect key={i} x={i * PITCH} y={100 - v * 92} width={PITCH - 1} height={v * 92} fill="currentColor" />
      }),
    [columns, count],
  )
  // What the cut drops is dimmed by two covers over the columns instead of a per-column opacity: a trim drag moves from and to every
  // frame, and rebuilding a few hundred rects for it was most of the cost of a frame of the drag.
  const wide = count * PITCH - 1
  const left = Math.max(0, Math.min(1, from / total)) * wide
  const right = Math.max(0, Math.min(1, to / total)) * wide
  return (
    <>
      <svg
        viewBox={`0 0 ${count * PITCH - 1} 100`}
        preserveAspectRatio="none"
        className="absolute inset-0 h-full w-full text-muted"
        style={{ transform: `scaleY(${gain})`, transformOrigin: 'bottom' }}
        aria-hidden="true"
      >
        {layer}
        <rect x={0} y={0} width={left} height={100} fill="var(--bx-canvas)" opacity={0.55} />
        <rect x={right} y={0} width={Math.max(0, wide - right)} height={100} fill="var(--bx-canvas)" opacity={0.55} />
      </svg>
      {status && (
        <span className={clsx(
          'pointer-events-none absolute right-2 top-1/2 -translate-y-1/2 text-[12px] font-bold leading-none',
          !decoded || sound === 'failed' ? 'text-warn' : 'text-faint',
        )}>
          {status}
        </span>
      )}
    </>
  )
}
