// What each tab of the editor's inspector holds. Edit, Color and Speed change the active clip and write
// through `update`; Export is the one place the whole sequence is saved from.

import clsx from 'clsx'
import { useEffect, useRef } from 'react'
import { Button, Segmented, Select, Slider, StepSlider } from '../components/ui'
import { Icon } from '../components/icons'
import { audioDefaults, audioIsEdited, audioLayout, audioRows, isUnknownMultitrack, mixLockReason, trackName, wheelLevel, type AudioRow } from '../lib/audio'
import { duration, fileName } from '../lib/format'
import { clipOutputLength, FLAT, type Crop, type Grade, type SequenceClip } from '../lib/sequence'
import { keptLength } from '../lib/segments'
import { ASPECTS, QUALITY, SPEEDS } from './editor-parts'

/** A change to the look every clip shares: the colour grade, or the crop (a shape for each clip to centre, or a frame placed on the active one). */
export interface LookPatch {
  grade?: Grade
  aspect?: string
  ratio?: number | null
  crop?: Crop
}

/** Shown in every tab while there is no clip to change. */
export function NothingToEdit() {
  return <p className="p-4 text-[13px] leading-relaxed text-faint">Add a clip to edit it.</p>
}

export function Section({ title, aside, children }: { title: string; aside?: React.ReactNode; children: React.ReactNode }) {
  return (
    <div className="inspector-section">
      <div className="mb-2 flex h-6 items-center justify-between">
        <span className="label">{title}</span>
        {aside}
      </div>
      {children}
    </div>
  )
}

/** A channel's level: a native range laid flat across its row, so the arrows, Home/End and Page keys reach it
 *  without a second keyboard path. The track fills to the level in the accent (index.css, .fader-h), so the
 *  value reads at a glance; the percentage sits beside it.
 *
 *  The wheel belongs to the fader and its value, never to the pane: one notch is five points, and a
 *  trackpad's small deltas add up before the level moves (lib/audio.ts `wheelLevel`). The listener is
 *  attached to the element itself because React's wheel handler runs in the root's passive listener,
 *  where `preventDefault` is refused - and the pane must not scroll with the same gesture. */
function Fader({ name, track, value, muted, onChange }: {
  name: string
  /** The track's ordinal in the file: the timeline's lane points at this control. Absent for the combined level. */
  track?: number
  /** 0..200, as the percentage the person sees. */
  value: number
  /** Drawn dim: the level is kept, but nothing of this channel is heard. */
  muted: boolean
  onChange: (value: number) => void
}) {
  const area = useRef<HTMLInputElement>(null)
  const carried = useRef(0)
  const shown = useRef(value)
  shown.current = value
  const write = useRef(onChange)
  write.current = onChange
  useEffect(() => {
    const element = area.current
    if (!element) return
    const onWheel = (event: WheelEvent) => {
      // A sideways or empty gesture belongs to the pane; so does a zoom gesture. Only a plain vertical
      // wheel over the level is the level's.
      if (!event.deltaY || event.ctrlKey || event.metaKey || Math.abs(event.deltaX) > Math.abs(event.deltaY)) return
      const next = wheelLevel(shown.current, carried.current, event.deltaY, event.deltaMode)
      carried.current = next.carried
      event.preventDefault()
      event.stopPropagation()
      // A gesture that did not cross a notch writes nothing: the edit is not touched by a scroll that
      // did not move the level, so no undo step and no gain is written for it.
      if (next.value !== shown.current) {
        // Taken here as well as on the next render, so two notches within one frame both count.
        shown.current = next.value
        write.current(next.value)
      }
    }
    element.addEventListener('wheel', onWheel, { passive: false })
    return () => element.removeEventListener('wheel', onWheel)
  }, [])
  return (
    <input
      ref={area}
      type="range"
      data-audio-track={track}
      className={clsx('fader fader-h block h-4 w-full min-w-0', muted && 'opacity-45')}
      style={{ '--fill': `${value / 2}%` } as React.CSSProperties}
      aria-label={`Volume of ${name}`}
      aria-valuetext={value === 0 ? 'Mute' : `${value}%`}
      title={`Volume of ${name}: drag, or scroll over it`}
      min={0}
      max={200}
      step={5}
      value={value}
      onChange={(event) => onChange(Number(event.target.value))}
    />
  )
}

/** One channel of the clip's mixer: its name, the percentage and the toggles on one line, and the level across the
 *  whole width under them, so the slider is as long as the pane allows. Every channel has the same two lines, so
 *  names, values, buttons and levels line up down the list. */
function ChannelRow({ name, track, value, muted, soloed, onChange, children, strong }: {
  name: string
  track?: number
  value: number
  muted: boolean
  soloed?: boolean
  onChange: (value: number) => void
  /** The row's toggles, at its right end. */
  children?: React.ReactNode
  /** The combined row: its name in ink, the sources under it a step quieter. */
  strong?: boolean
}) {
  return (
    <div data-muted={muted || undefined} className={clsx('flex flex-col gap-1 px-2.5 pb-2 pt-1.5', soloed && 'bg-accent-soft')}>
      <div className="flex h-7 items-center gap-2">
        <span
          className={clsx('min-w-0 flex-1 truncate text-[12px] font-bold leading-none', muted ? 'text-warn' : soloed ? 'text-accent' : strong ? 'text-ink' : 'text-muted')}
          title={name}
        >
          {name}
        </span>
        <span className={clsx('num shrink-0 text-right text-[12px] leading-none', muted ? 'text-warn' : 'text-ink')} title="Scroll over the level to set it">
          {muted ? 'MUTED' : `${value}%`}
        </span>
        <div className="flex shrink-0 items-center gap-0.5">{children}</div>
      </div>
      <Fader name={name} track={track} value={value} muted={muted} onChange={onChange} />
    </div>
  )
}

/** A square toggle on a channel row: pressed draws it in its colour, at rest it is a quiet icon. */
function RowToggle({ icon, pressed, tone, label, title, onPress }: {
  icon: 'speaker' | 'speakerOff' | 'eye' | 'chevronDown' | 'chevronRight'
  pressed: boolean
  tone: 'warn' | 'accent' | 'plain'
  label: string
  title: string
  onPress: () => void
}) {
  return (
    <button
      type="button"
      aria-pressed={tone === 'plain' ? undefined : pressed}
      aria-expanded={tone === 'plain' ? pressed : undefined}
      aria-label={label}
      title={title}
      onClick={onPress}
      className={clsx(
        'grid h-7 w-7 place-items-center border transition-colors duration-[var(--bx-dur-fast)]',
        pressed && tone === 'warn' ? 'border-warn bg-warn-soft text-warn'
          : pressed && tone === 'accent' ? 'border-accent bg-accent-soft text-accent'
            : 'border-transparent text-muted hover:bg-raised hover:text-ink',
      )}
    >
      <Icon name={icon} size={14} />
    </button>
  )
}

export function EditTab({ clip, update, look, audioExpanded, onAudioExpanded }: {
  clip: SequenceClip
  update: (patch: Partial<SequenceClip>) => void
  look: (patch: LookPatch) => void
  /** The one audio fold, shared with the timeline: closed is one combined level, open is one card per source. */
  audioExpanded: boolean
  onAudioExpanded: (open: boolean) => void
}) {
  const cropOn = clip.aspect !== 'off'
  const cropping = clip.crop.width > 0 && (clip.crop.width < clip.width || clip.crop.height < clip.height)
  return (
    <div className="editor-sections">
      <Section title="Crop · all clips" aside={<span className="num text-[13px] text-muted">{cropping ? `${clip.crop.width}×${clip.crop.height}` : `${clip.width}×${clip.height}`}</span>}>
        <Segmented
          label="Crop shape"
          size="sm"
          className="flex w-full [&>button]:flex-1 [&>button]:px-0"
          value={clip.aspect}
          onChange={(value) => look({ aspect: value, ratio: ASPECTS.find((a) => a.value === value)?.ratio ?? null })}
          options={ASPECTS.map((a) => ({ value: a.value, label: a.label }))}
        />
        {cropOn && <div className="mt-2 text-[13px] leading-relaxed text-muted">Drag the frame on the picture to place it.</div>}
        {cropping && (
          <svg viewBox={`0 0 ${clip.width} ${clip.height}`} preserveAspectRatio="xMidYMid meet" style={{ aspectRatio: `${clip.width} / ${clip.height}` }} className="mt-3 w-full border border-line bg-canvas text-line-strong" aria-label="The frame this clip keeps">
            <rect x="1" y="1" width={Math.max(1, clip.width - 2)} height={Math.max(1, clip.height - 2)} className="fill-raised" stroke="currentColor" strokeWidth={1} vectorEffect="non-scaling-stroke" />
            <rect x={clip.crop.x} y={clip.crop.y} width={clip.crop.width} height={clip.crop.height} className="text-accent" fill="none" stroke="currentColor" strokeWidth={2} vectorEffect="non-scaling-stroke" />
          </svg>
        )}
      </Section>
      {clip.audio.length > 0 && (() => {
        const layout = audioLayout(clip.audio)
        const rows = audioRows(clip.audio, layout)
        const strips = rows.filter((row) => row.kind === 'source')
        const locked = rows.filter((row) => row.kind === 'locked')
        const reason = mixLockReason(layout)
        const defaults = audioDefaults(clip.audio)
        const changed = audioIsEdited(clip.audio, clip.volumes, clip.muted, clip.master) || clip.solo.some(Boolean)
        const toggle = (list: boolean[], index: number) => list.map((on, i) => (i === index ? !on : on))
        // The overall level is one control for what the clip plays now, never a guess at a hidden mix.
        // A locked mix keeps its truthful restriction: no master control is offered that the export refuses.
        const combined = Math.round((clip.master ?? 1) * 100)
        const setCombined = (value: number) => update({ master: value / 100 })
        const hasSources = strips.length > 0
        const expands = strips.length > 1
        const showSources = expands && audioExpanded
        const listening = strips.filter(({ index }) => clip.solo[index])
        const listeningNames = listening.map(({ track, index }) => trackName(track, index)).join(', ')
        // One row per source, all in one bordered list: the combined level first when there is more than one
        // source, with the fold that shows the sources under it. The eye is offered only when more than one plays.
        const renderStrip = ({ track, index }: AudioRow) => {
          const name = trackName(track, index)
          const muted = !!clip.muted[index]
          const soloed = !!clip.solo[index]
          const percent = Math.round((clip.volumes[index] ?? 1) * 100)
          const set = (value: number) => update({ volumes: clip.volumes.map((x, i) => (i === index ? value / 100 : x)) })
          return (
            <ChannelRow key={track.index} name={name} track={index} value={percent} muted={muted} soloed={soloed} onChange={set} strong={!expands}>
              <RowToggle
                icon={muted ? 'speakerOff' : 'speaker'}
                pressed={muted}
                tone="warn"
                label={`Mute ${name}`}
                title={`${muted ? 'Unmute' : 'Mute'} ${name}: applies to the preview and saved file`}
                onPress={() => update({ muted: toggle(clip.muted, index) })}
              />
              {expands && (
                <RowToggle
                  icon="eye"
                  pressed={soloed}
                  tone="accent"
                  label={`Listen alone to ${name}`}
                  title={`${soloed ? 'Stop listening alone to' : 'Listen alone to'} ${name} (preview only)`}
                  onPress={() => update({ solo: toggle(clip.solo, index) })}
                />
              )}
            </ChannelRow>
          )
        }
        return (
          <Section
            title="Audio"
            aside={<Button variant="ghost" className="!h-6" isDisabled={!changed} onPress={() => update(defaults)}>Reset</Button>}
          >
            {rows.length === 0 && <p className="text-[13px] leading-relaxed text-muted">No sound on this file's tracks.</p>}
            {isUnknownMultitrack(clip.audio) && (
              <p className="mb-2 text-[12px] leading-snug text-muted" title="These tracks are independent and have no labels. Unmute another track to mix it in.">
                Unlabelled tracks. Track 1 plays first.
              </p>
            )}
            {hasSources && (
              <div
                className="divide-y divide-line border border-line bg-canvas"
                title={`Levels and mutes are saved${rows.some((row) => row.kind === 'derived') ? ', and the saved file combines these tracks' : ''}.${expands ? ' The eye plays one source alone, in the preview only.' : ''}`}
              >
                {expands && (
                  <ChannelRow name="Combined" value={combined} muted={false} onChange={setCombined} strong>
                    <RowToggle
                      icon={audioExpanded ? 'chevronDown' : 'chevronRight'}
                      pressed={audioExpanded}
                      tone="plain"
                      label={`${audioExpanded ? 'Hide' : 'Show'} the ${strips.length} audio sources`}
                      title={`${audioExpanded ? 'Hide' : 'Show'} each source (${strips.length})`}
                      onPress={() => onAudioExpanded(!audioExpanded)}
                    />
                    {/* The eye's width, so the combined percentage lines up with each source's. */}
                    <span className="w-7" aria-hidden="true" />
                  </ChannelRow>
                )}
                {(!expands || showSources) && strips.map(renderStrip)}
              </div>
            )}
            {listening.length > 0 && (
              <div className="mt-2 flex items-center justify-between gap-2 border border-accent bg-accent-soft px-2 py-1 text-[12px] text-accent" role="status">
                <span className="min-w-0 truncate" title={`Preview only: ${listeningNames}`}>Preview only: {listeningNames}</span>
                <button type="button" className="btn btn-ghost !h-6 shrink-0 !px-1" onClick={() => update({ solo: clip.solo.map(() => false) })}>Clear</button>
              </div>
            )}
            {locked.length > 0 && (
              <div className="mt-2 flex flex-col gap-1">
                {locked.map((row) => (
                  <div key={row.index} className="flex items-center justify-between gap-2 text-[13px] leading-snug text-muted">
                    <span className="min-w-0 break-words" title={trackName(row.track, row.index)}>{trackName(row.track, row.index)}</span>
                    <span className="shrink-0 text-faint">Levels locked</span>
                  </div>
                ))}
              </div>
            )}
            {reason && <p className="mt-2 text-[13px] leading-snug text-muted">{reason}</p>}
          </Section>
        )
      })()}
    </div>
  )
}

export function ColorTab({ clip, look }: { clip: SequenceClip; look: (patch: LookPatch) => void }) {
  const graded = clip.grade.brightness !== 100 || clip.grade.contrast !== 100 || clip.grade.saturation !== 100
  return (
    <div className="editor-sections">
      <Section title="Color · all clips" aside={<Button variant="ghost" icon="refresh" className="h-6" isDisabled={!graded} onPress={() => look({ grade: FLAT })}>Reset</Button>}>
        <div className="flex flex-col gap-1.5">
          {(['brightness', 'contrast', 'saturation'] as const).map((key) => (
            <div key={key} className="flex items-center gap-3">
              <span className="label w-[92px] shrink-0">{key}</span>
              <Slider
                label={key}
                value={clip.grade[key]}
                min={0}
                max={200}
                step={5}
                onChange={(value) => look({ grade: { ...clip.grade, [key]: value } })}
                onCommit={(value) => look({ grade: { ...clip.grade, [key]: value } })}
                format={(value) => `${value}%`}
                className="flex-1"
              />
            </div>
          ))}
        </div>
      </Section>
    </div>
  )
}

export function SpeedTab({ clip, update }: { clip: SequenceClip; update: (patch: Partial<SequenceClip>) => void }) {
  const retimed = clip.speed !== 1
  return (
    <div className="editor-sections">
      <Section title="Speed" aside={<span className="num text-[13px] text-muted">{duration(keptLength(clip.parts))}{retimed && ` → ${duration(clipOutputLength(clip))}`}</span>}>
        <StepSlider label="Speed" value={String(clip.speed)} onChange={(value) => update({ speed: Number(value) })} options={SPEEDS.map((s) => ({ value: String(s), label: `${s}x` }))} />
        <p className="mt-3 text-[13px] leading-relaxed text-muted">One speed for the whole clip. Split it first to give each part its own.</p>
      </Section>
    </div>
  )
}

export interface ExportPanel {
  clips: number
  canReplace: boolean
  /** The name the saved copy is given, as typed in this tab. */
  name: string
  onName: (value: string) => void
  /** Why the typed name cannot be saved; empty when it can. */
  nameError: string
  quality: number
  onQuality: (value: number) => void
  resolution: string
  onResolution: (value: string) => void
  /** The picture that sets the output shape, for the size choices. */
  frameHeight: number
  frameWidth: number
  exportable: boolean
  busy: boolean
  clipsDir: string
  /** Why the sequence cannot be joined, when it cannot. */
  warning: string | null
  replacing: boolean
  progress: { id: number; value: number }[]
  onCancel: (id: number) => void
  onSave: (share: boolean) => void
  onReplace: () => void
  done: string | null
  doneStale: boolean
  doneShareable: boolean
  shareable: boolean
  onShare: () => void
  onShow: () => void
  onCopy: () => void
}

export function ExportTab(p: ExportPanel) {
  return (
    <div className="editor-sections flex min-h-full flex-col">
      <div inert={p.replacing} className="flex-1">
        <Section title="Quality">
          <Select label="Quality" value={p.quality} onChange={p.onQuality} options={QUALITY.map((q) => ({ value: q.value, label: q.label, hint: q.hint }))} className="w-full" />
        </Section>
        <Section title="Size">
          <Select
            label="Output size"
            value={p.resolution}
            onChange={(value) => p.onResolution(String(value))}
            options={[
              { value: 'original', label: `Original (${p.frameWidth}×${p.frameHeight})` },
              ...[1440, 1080, 720].filter((h) => h < p.frameHeight).map((h) => ({ value: String(h), label: `${h}p` })),
            ]}
            className="w-full"
          />
          {p.clips > 1 && (
            <p
              className="mt-2 text-[12px] leading-snug text-muted"
              title="Every clip is re-encoded to the first one's shape, with its own crop, colour, speed and levels."
            >
              {p.clips} clips, joined in order.
            </p>
          )}
        </Section>
        <Section title="File name">
          <input
            id="export-name"
            aria-label="File name of the saved copy"
            className="field w-full"
            value={p.name}
            onChange={(event) => p.onName(event.target.value)}
            aria-invalid={!!p.nameError}
            aria-describedby={p.nameError ? 'export-name-error' : 'export-name-hint'}
            spellCheck={false}
            autoComplete="off"
          />
          {p.nameError
            ? <p id="export-name-error" className="mt-2 text-[13px] leading-snug text-warn">{p.nameError}</p>
            : <p id="export-name-hint" className="mt-2 text-[12px] leading-snug text-faint" title=".mp4 is added when it is left off.">Saved in Edits as .mp4</p>}
        </Section>
        {p.warning && <p className="px-3 py-3 text-[13px] leading-snug text-warn">{p.warning}</p>}
      </div>
      <div className="editor-actions flex shrink-0 flex-col gap-2 border-t border-line p-4">
        {p.progress.length > 0 ? (
          <div className="border-2 border-accent bg-canvas p-3">
            {p.progress.map((entry) => (
              <div key={entry.id} className="mb-2 last:mb-0">
                <div className="flex items-center justify-between text-[14px] font-bold text-accent">
                  <span>Saving… {Math.round(entry.value * 100)}%</span>
                  <button type="button" onClick={() => p.onCancel(entry.id)} className="text-[13px] text-muted hover:text-ink">Cancel</button>
                </div>
                <div className="cells mt-2 h-[8px] overflow-hidden">
                  {/* G11: the bar moves by scaleX, eased on the base token (a transform, not a width). */}
                  <div className="cells-on h-full w-full origin-left transition-transform [transition-duration:var(--bx-dur-base)] [transition-timing-function:var(--bx-ease-out)]" style={{ transform: `scaleX(${entry.value})` }} />
                </div>
              </div>
            ))}
          </div>
        ) : (
          <>
            <Button variant="primary" icon="save" isDisabled={!p.exportable || p.busy || !!p.nameError} onPress={() => p.onSave(false)}>Save a copy</Button>
            <Button icon="share" isDisabled={!p.exportable || p.busy || !!p.nameError} onPress={() => p.onSave(true)}>Save & share</Button>
            {p.canReplace && <Button variant="ghost" isDisabled={!p.exportable || p.busy} onPress={p.onReplace}>Replace original…</Button>}
            {!p.clipsDir && <div className="text-[13px] text-warn">The clips folder is unavailable. Check the engine connection.</div>}
          </>
        )}
        {p.done && (
          <div className="border border-line bg-canvas p-3 text-[13px]">
            <div className="flex items-center gap-2">
              {/* G11: completion pops a check from 0.6 to 1 (bx-check-pop). */}
              <span className="bx-check-pop inline-flex items-center gap-1 bg-accent px-1.5 text-[13px] font-bold leading-[18px] text-on-accent"><Icon name="check" size={12} />saved</span>
              <span className="truncate font-bold text-ink" title={p.done}>{fileName(p.done)}</span>
            </div>
            {p.doneStale && <div className="mt-2 text-muted">Your edit has changed since this save.</div>}
            <div className="mt-2 flex flex-wrap gap-1">
              {p.doneShareable && <Button icon="share" variant="ghost" isDisabled={!p.shareable} onPress={p.onShare}>Share</Button>}
              <Button icon="folder" variant="ghost" onPress={p.onShow}>Show</Button>
              <Button icon="copy" variant="ghost" onPress={p.onCopy}>Copy</Button>
            </div>
          </div>
        )}
      </div>
    </div>
  )
}
