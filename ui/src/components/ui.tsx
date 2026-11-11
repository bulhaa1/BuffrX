// The primitives every screen is built from, on React Aria Components (AGENTS.md: no Radix, no Base
// UI), drawn the way the native panel draws them: flat, square, 1 px seams, the accent for what is on.

import clsx from 'clsx'
import { useEffect, useLayoutEffect, useRef, useState, type ReactNode, type RefObject } from 'react'
import {
  Button as AriaButton,
  Dialog,
  Heading,
  ListBox,
  ListBoxItem,
  Menu,
  MenuItem,
  Modal,
  ModalOverlay,
  Popover,
  Select as AriaSelect,
  SelectValue,
  Slider as AriaSlider,
  SliderThumb,
  SliderTrack,
  Tab,
  TabList,
  TabPanel,
  Tabs as AriaTabs,
  Tooltip,
  TooltipTrigger,
  type Key,
} from 'react-aria-components'
import { Icon, type IconName } from './icons'

export function Button({ children, variant = 'default', className, onPress, isDisabled, icon, title, type }: {
  children?: ReactNode
  variant?: 'default' | 'primary' | 'ghost' | 'danger'
  className?: string
  onPress?: () => void
  isDisabled?: boolean
  icon?: IconName
  title?: string
  type?: 'button' | 'submit'
}) {
  return (
    <AriaButton
      type={type}
      aria-label={title}
      className={clsx('btn', variant !== 'default' && `btn-${variant}`, className)}
      onPress={onPress}
      isDisabled={isDisabled}
    >
      {icon && <Icon name={icon} size={15} />}
      {children}
    </AriaButton>
  )
}

/** An icon-only button with a tooltip. */
export function IconButton({ icon, label, onPress, className, active, isDisabled, size = 16, instant = false }: {
  icon: IconName
  label: string
  onPress?: () => void
  className?: string
  active?: boolean
  isDisabled?: boolean
  size?: number
  /** Transport controls can act on the press, without waiting for its release. */
  instant?: boolean
}) {
  return (
    <Tip label={label}>
      <AriaButton
        aria-label={label}
        className={clsx('icon-btn', active && '!text-accent', className)}
        onPress={instant ? undefined : onPress}
        onPressStart={instant ? onPress : undefined}
        isDisabled={isDisabled}
      >
        {/* G9, H6, I8: the icon cross-fades when its name changes (the wrapper is keyed by it). */}
        <span key={icon} className="bx-swap inline-grid">
          <Icon name={icon} size={size} />
        </span>
      </AriaButton>
    </Tip>
  )
}

export function Tip({ label, children, placement = 'bottom' }: {
  label: ReactNode
  children: ReactNode
  placement?: 'top' | 'bottom' | 'left' | 'right'
}) {
  return (
    <TooltipTrigger delay={450} closeDelay={60}>
      {children}
      <Tooltip
        placement={placement}
        offset={6}
        className="bx-tooltip border border-line-strong bg-raised px-2 py-1 text-[13px] text-ink"
      >
        {label}
      </Tooltip>
    </TooltipTrigger>
  )
}

export interface Option<T extends string | number = string> {
  value: T
  label: string
  hint?: string
}

export function Select<T extends string | number>({ value, options, onChange, label, className, isDisabled }: {
  value: T
  options: Option<T>[]
  onChange: (value: T) => void
  label: string
  className?: string
  isDisabled?: boolean
}) {
  return (
    <AriaSelect
      aria-label={label}
      selectedKey={String(value)}
      onSelectionChange={(key: Key | null) => {
        const option = options.find((o) => String(o.value) === String(key))
        if (option) onChange(option.value)
      }}
      isDisabled={isDisabled}
      className={clsx('min-w-[150px]', className)}
    >
      <AriaButton className="field flex w-full items-center justify-between gap-2 text-left data-[disabled]:opacity-45">
        <SelectValue className="truncate">{({ selectedText }) => selectedText ?? options.find((o) => String(o.value) === String(value))?.label}</SelectValue>
        <Icon name="chevronDown" size={14} className="bx-chevron shrink-0 text-faint" />
      </AriaButton>
      <Popover className="popover min-w-[--trigger-width] max-h-[320px] overflow-auto">
        <ListBox className="outline-none">
          {options.map((option) => (
            <ListBoxItem key={String(option.value)} id={String(option.value)} textValue={option.label} className="menu-item">
              <span className="flex flex-col">
                <span>{option.label}</span>
                {option.hint && <span className="text-[13px] text-faint">{option.hint}</span>}
              </span>
            </ListBoxItem>
          ))}
        </ListBox>
      </Popover>
    </AriaSelect>
  )
}

/** A slider that commits on release: a write per step would restart work in the engine. `onChange` hears
 *  every step while the thumb is held, for a preview that should follow it (the editor's sound graph).
 *  `instant` is a playback bar: its fill and thumb take each value at once, with no easing behind the picture. */
export function Slider({ value, min, max, step = 1, onCommit, onChange, label, format, className, instant }: {
  value: number
  min: number
  max: number
  step?: number
  onCommit: (value: number) => void
  onChange?: (value: number) => void
  label: string
  format?: (value: number) => string
  className?: string
  instant?: boolean
}) {
  const [live, setLive] = useState(value)
  useEffect(() => setLive(value), [value])
  return (
    <div className={clsx('bx-slider flex items-center gap-3', className, instant && 'bx-slider-instant')}>
      <AriaSlider
        aria-label={label}
        value={live}
        minValue={min}
        maxValue={max}
        step={step}
        onChange={(v) => {
          setLive(v as number)
          onChange?.(v as number)
        }}
        onChangeEnd={(v) => onCommit(v as number)}
        className="bx-slider-main flex-1"
      >
        <SliderTrack className="bx-slider-track relative h-5 w-full">
          {({ state }) => (
            <>
              <div className="bx-slider-rail absolute top-1/2 h-[4px] w-full -translate-y-1/2 bg-raised" />
              <div
                className={clsx('bx-slider-fill absolute left-0 top-1/2 h-[4px] w-full -translate-y-1/2 origin-left bg-accent', !instant && !state.isThumbDragging(0) && 'transition-transform [transition-duration:var(--bx-dur-fast)] [transition-timing-function:var(--bx-ease-out)]')}
                style={{ transform: `scaleX(${state.getThumbPercent(0)})` }}
              />
              <SliderThumb className="bx-slider-thumb top-1/2 h-[14px] w-[8px] bg-accent data-[dragging]:bg-ink data-[focus-visible]:outline data-[focus-visible]:outline-2 data-[focus-visible]:outline-offset-2 data-[focus-visible]:outline-accent" />
            </>
          )}
        </SliderTrack>
      </AriaSlider>
      {format && <span className="num w-[52px] text-right text-[14px] text-muted">{format(live)}</span>}
    </div>
  )
}

/** A slider that only rests on its marks: one tick per choice with its name beneath, the thumb snapping tick to tick.
 *  For a short list of named values (the editor's speed), where a free slider would invite values that mean nothing. */
export function StepSlider<T extends string | number>({ value, options, onChange, label, className }: {
  value: T
  options: Option<T>[]
  onChange: (value: T) => void
  label: string
  className?: string
}) {
  const last = Math.max(1, options.length - 1)
  const index = Math.max(0, options.findIndex((o) => o.value === value))
  return (
    <div className={clsx('px-3', className)}>
      <AriaSlider
        aria-label={label}
        value={index}
        minValue={0}
        maxValue={options.length - 1}
        step={1}
        onChange={(v) => {
          const next = options[v as number]
          if (next && next.value !== value) onChange(next.value)
        }}
      >
        <SliderTrack className="relative h-6 w-full cursor-pointer">
          {({ state }) => (
            <>
              <div className="absolute top-1/2 h-[2px] w-full -translate-y-1/2 bg-line-strong" />
              <div className="absolute top-1/2 h-[2px] -translate-y-1/2 bg-accent" style={{ width: `${state.getThumbPercent(0) * 100}%` }} />
              {options.map((option, i) => (
                <div
                  key={String(option.value)}
                  className={clsx('absolute top-1/2 h-[9px] w-px -translate-x-1/2 -translate-y-1/2', i <= index ? 'bg-accent' : 'bg-line-strong')}
                  style={{ left: `${(i / last) * 100}%` }}
                />
              ))}
              <SliderThumb className="top-1/2 h-[18px] w-[10px] border border-on-accent bg-accent data-[dragging]:bg-ink data-[focus-visible]:outline data-[focus-visible]:outline-2 data-[focus-visible]:outline-offset-2 data-[focus-visible]:outline-accent" />
            </>
          )}
        </SliderTrack>
      </AriaSlider>
      <div className="relative mt-1 h-4">
        {options.map((option, i) => (
          <button
            type="button"
            key={String(option.value)}
            tabIndex={-1}
            onClick={() => onChange(option.value)}
            className={clsx('num absolute -translate-x-1/2 text-[13px] leading-4', i === index ? 'font-bold text-accent' : 'text-muted hover:text-ink')}
            style={{ left: `${(i / last) * 100}%` }}
          >
            {option.label}
          </button>
        ))}
      </div>
    </div>
  )
}

/** A row of mutually exclusive choices: cells in one frame, split by seams, the chosen one filled. */
/** The measured box of the selected child of `box`: a sliding indicator moves between the choices (F5, F7) by transform and width. */
function useSelectedBox(box: RefObject<HTMLElement | null>, selector: string, key: string) {
  const [at, setAt] = useState({ x: 0, w: 0 })
  useLayoutEffect(() => {
    const parent = box.current
    if (!parent) return
    const measure = () => {
      const el = parent.querySelector<HTMLElement>(selector)
      if (!el) return
      setAt((prev) => (prev.x === el.offsetLeft && prev.w === el.offsetWidth ? prev : { x: el.offsetLeft, w: el.offsetWidth }))
    }
    measure()
    // A resize of the list (a window or panel width) moves the chosen cell: measure again, no animation frame loop.
    const observer = new ResizeObserver(measure)
    observer.observe(parent)
    return () => observer.disconnect()
  }, [box, selector, key])
  return at
}

export function Segmented<T extends string | number>({ value, options, onChange, label, className, size = 'md' }: {
  value: T
  options: Option<T>[]
  onChange: (value: T) => void
  label: string
  className?: string
  size?: 'sm' | 'md'
}) {
  const group = useRef<HTMLDivElement>(null)
  // F7: the chosen fill is one block that moves between the choices (a transform and a width, measured once per change).
  const chosen = useSelectedBox(group, '[aria-checked="true"]', String(value))
  return (
    <div ref={group} role="radiogroup" aria-label={label} className={clsx('relative inline-flex divide-x divide-line border border-line bg-canvas', className)}>
      <span
        aria-hidden
        className="pointer-events-none absolute bottom-0 top-0 left-0 bg-ink transition-[transform,width] [transition-duration:var(--bx-dur-fast)] [transition-timing-function:var(--bx-ease-out)]"
        style={{ transform: `translateX(${chosen.x}px)`, width: chosen.w }}
      />
      {options.map((option) => {
        const selected = option.value === value
        return (
          <button
            type="button"
            key={String(option.value)}
            role="radio"
            aria-checked={selected}
            onClick={() => onChange(option.value)}
            className={clsx(
              'relative font-bold transition-colors [transition-duration:var(--bx-dur-fast)]',
              size === 'sm' ? 'h-[24px] px-2.5 text-[13px]' : 'h-[28px] px-3 text-[14px]',
              selected ? 'text-canvas' : 'text-muted hover:bg-raised hover:text-ink',
            )}
          >
            {option.label}
          </button>
        )
      })}
    </div>
  )
}

/** Eight cells round a square, one lit and its trail fading: the native panel's spinner. */
export function Spinner({ size = 14 }: { size?: number }) {
  const cell = Math.max(2, Math.round(size / 4))
  const at = [[0, 0], [1, 0], [2, 0], [2, 1], [2, 2], [1, 2], [0, 2], [0, 1]]
  return (
    <span className="relative inline-block" style={{ width: cell * 3 + 2, height: cell * 3 + 2 }} role="status" aria-label="Working">
      {at.map(([x, y], i) => (
        <span
          key={i}
          className="bx-cell absolute bg-accent"
          style={{ width: cell, height: cell, left: x * (cell + 1), top: y * (cell + 1), animationDelay: `${(i * 90) - 720}ms` }}
        />
      ))}
    </span>
  )
}

/** A confirm / form dialog. */
export function DialogBox({ isOpen, onClose, title, children, actions, width = 400 }: {
  isOpen: boolean
  onClose: () => void
  title: string
  children: ReactNode
  actions?: ReactNode
  width?: number
}) {
  return (
    <ModalOverlay
      isOpen={isOpen}
      onOpenChange={(open) => !open && onClose()}
      isDismissable
      className="bx-overlay fixed inset-0 z-50 grid place-items-center bg-[color-mix(in_srgb,var(--bx-canvas)_72%,transparent)]"
    >
      {/* A dialog is the focused pane: the 2 px accent frame, its title set into the top border. It never
          grows past the window: at 1024x600 a fixed 400 px card with a long body would run off it. Its arrival and
          exit are the motion tokens (index.css, bx-dialog), read from react-aria's data-entering and data-exiting. */}
      <Modal className="bx-dialog pane is-focused max-w-[calc(100dvw-2rem)] p-5 outline-none" style={{ width }}>
        <Dialog className="outline-none">
          <Heading slot="title" className="pane-title !text-[14px] font-bold !normal-case">{title}</Heading>
          {/* Only the body scrolls: the title sits half outside the frame, so the frame itself must not clip. */}
          <div className="max-h-[calc(100dvh-9rem)] overflow-y-auto pt-2 text-[14px] text-muted">{children}</div>
          {actions && <div className="mt-5 flex justify-end gap-2">{actions}</div>}
        </Dialog>
      </Modal>
    </ModalOverlay>
  )
}

/** Tabs that share one frame: an icon over each name, the chosen one underlined in the accent. The panels
 *  scroll on their own, so a long one never pushes the tab row away. */
export function Tabs<T extends string>({ value, onChange, label, tabs, className }: {
  value: T
  onChange: (value: T) => void
  label: string
  tabs: { id: T; label: string; icon: IconName; content: ReactNode }[]
  className?: string
}) {
  // Tabs divide the row evenly, so the selection can move on the same render without a DOM measurement.
  const selected = Math.max(0, tabs.findIndex((tab) => tab.id === value))
  return (
    <AriaTabs selectedKey={value} onSelectionChange={(key) => onChange(key as T)} className={clsx('flex min-h-0 flex-1 flex-col', className)}>
      <div className="relative shrink-0">
        <TabList aria-label={label} className="grid border-b border-line" style={{ gridTemplateColumns: `repeat(${tabs.length}, minmax(0, 1fr))` }}>
          {tabs.map((tab) => (
            <Tab
              key={tab.id}
              id={tab.id}
              className="flex cursor-default flex-col items-center gap-1 border-b-2 border-transparent px-1 pb-2 pt-2.5 text-[13px] font-bold text-muted outline-none data-[hovered]:text-ink data-[selected]:text-accent data-[focus-visible]:outline data-[focus-visible]:outline-2 data-[focus-visible]:-outline-offset-2 data-[focus-visible]:outline-accent"
            >
              <Icon name={tab.icon} size={18} />
              {tab.label}
            </Tab>
          ))}
        </TabList>
        <span
          aria-hidden
          className="pointer-events-none absolute bottom-[-1px] left-0 h-[2px] bg-accent transition-transform [transition-duration:var(--bx-dur-fast)] [transition-timing-function:var(--bx-ease-out)]"
          style={{ transform: `translateX(${selected * 100}%)`, width: `${100 / tabs.length}%` }}
        />
      </div>
      {/* React Aria waits for animations on the panel before unmounting it. Animate its child instead,
          and suppress the global scrollbar transition so a rapid selection never waits for the old panel. */}
      {tabs.map((tab) => (
        <TabPanel key={tab.id} id={tab.id} className="min-h-0 flex-1 overflow-y-auto outline-none transition-none">
          <div className="bx-tab-enter">{tab.content}</div>
        </TabPanel>
      ))}
    </AriaTabs>
  )
}

/** A menu that opens at the pointer, for a right click. `at` is where; null keeps it closed. */
export function ContextMenu({ at, label, items, onClose }: {
  at: { x: number; y: number } | null
  label: string
  items: { id: string; label: string; onAction: () => void; isDisabled?: boolean }[]
  onClose: () => void
}) {
  const anchor = useRef<HTMLDivElement>(null)
  return (
    <>
      <div ref={anchor} aria-hidden="true" className="pointer-events-none fixed h-px w-px" style={{ left: at?.x ?? 0, top: at?.y ?? 0 }} />
      <Popover triggerRef={anchor} isOpen={at !== null} onOpenChange={(open) => !open && onClose()} placement="bottom start" offset={0} className="popover menu-compact">
        <Menu
          aria-label={label}
          autoFocus="first"
          className="outline-none"
          onAction={(key) => {
            items.find((item) => item.id === key)?.onAction()
            onClose()
          }}
        >
          {items.map((item) => (
            <MenuItem key={item.id} id={item.id} textValue={item.label} isDisabled={item.isDisabled} className="menu-item data-[disabled]:opacity-40">
              {item.label}
            </MenuItem>
          ))}
        </Menu>
      </Popover>
    </>
  )
}
