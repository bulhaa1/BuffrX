// What each tab of the editor's inspector holds. Edit, Color and Speed change the active clip and write
// through `update`; Export is the one place the whole sequence is saved from.

import { Button, Segmented, Select, Slider, StepSlider } from '../components/ui'
import { Icon } from '../components/icons'
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

export function EditTab({ clip, update, look }: { clip: SequenceClip; update: (patch: Partial<SequenceClip>) => void; look: (patch: LookPatch) => void }) {
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
      {clip.audio.length > 0 && (
        <Section title="Audio">
          <div className="flex flex-col gap-1.5">
            {clip.audio.map((track, index) => {
              const set = (value: number) => update({ volumes: clip.volumes.map((x, i) => (i === index ? value / 100 : x)) })
              return (
                <div key={track.index} className="flex items-center gap-3">
                  <span className="label w-[64px] shrink-0 truncate" title={track.title || `Audio track ${index + 1}`}>
                    {track.title || (clip.audio.length === 1 ? 'Level' : `Track ${index + 1}`)}
                  </span>
                  <Slider
                    label={`Volume of ${track.title || `track ${index + 1}`}`}
                    value={Math.round((clip.volumes[index] ?? 1) * 100)}
                    min={0}
                    max={200}
                    step={5}
                    onChange={set}
                    onCommit={set}
                    format={(v) => (v === 0 ? 'MUTE' : `${v}%`)}
                    className="flex-1"
                  />
                </div>
              )
            })}
          </div>
        </Section>
      )}
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
          <p className="mt-3 text-[13px] leading-relaxed text-muted">
            {p.clips <= 1
              ? 'One clip is exported as it is edited.'
              : `${p.clips} clips are joined in this order. Every clip is re-encoded to the first one's shape, with its own crop, colour, speed and levels.`}
          </p>
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
            <Button variant="primary" icon="save" isDisabled={!p.exportable || p.busy} onPress={() => p.onSave(false)}>Save a copy</Button>
            <Button icon="share" isDisabled={!p.exportable || p.busy} onPress={() => p.onSave(true)}>Save & share</Button>
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
