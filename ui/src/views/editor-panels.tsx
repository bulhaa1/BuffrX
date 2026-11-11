// The editor's left and middle panes: the BuffrX library on the left (clips, and a box of screenshots under it),
// the empty stage and the transport under the picture. They hold no editing state; the editor owns that and
// hands each one what it draws.

import clsx from 'clsx'
import { useEffect, useRef, useState } from 'react'
import { Icon } from '../components/icons'
import { Thumb } from '../components/thumb'
import { Button, ContextMenu, DialogBox, IconButton, Spinner } from '../components/ui'
import { engine, type MediaItem } from '../lib/api'
import { onEngine } from '../lib/bridge'
import { simpleTitle, stampOf, stampText } from '../lib/clip_label'
import { bytes, duration, stem } from '../lib/format'
import { type SequenceClip } from '../lib/sequence'
import { attempt, toast } from '../lib/store'

/** What BuffrX has saved of one kind, newest first, kept current while the editor is open. */
function useLibrary(kind: 'clip' | 'screenshot'): MediaItem[] | null {
  const [items, setItems] = useState<MediaItem[] | null>(null)
  useEffect(() => {
    let live = true
    const load = () =>
      void (kind === 'clip' ? engine.clips() : engine.screenshots())
        .then((list) => live && setItems(list))
        .catch(() => live && setItems((current) => current ?? []))
    load()
    const offChanged = onEngine('library.changed', load)
    const offConnection = onEngine('connection', (state) => state === 'connected' && load())
    return () => {
      live = false
      offChanged()
      offConnection()
    }
  }, [kind])
  return items
}

/** A row pressed and dragged out of the library: onto the picture or the timeline (anything marked `data-drop`) it
 *  joins the edit, and a plain press does the same. Pointer events rather than the browser's own drag and drop: the
 *  packaged window hands file drops to the shell, which turns that off. */
function useDragSource(onAdd: (paths: string[]) => void, onDragging?: (dragging: boolean) => void) {
  const add = useRef(onAdd)
  add.current = onAdd
  const report = useRef(onDragging)
  report.current = onDragging
  const press = useRef<{ file: string; name: string; x: number; y: number; moved: boolean } | null>(null)
  const justDragged = useRef(false)
  const [ghost, setGhost] = useState<{ x: number; y: number; name: string } | null>(null)
  /** The drop zone under the pointer, so the whole area answers the drag and not only its frame. */
  const armed = useRef<Element | null>(null)
  useEffect(() => {
    const arm = (zone: Element | null) => {
      if (armed.current === zone) return
      armed.current?.removeAttribute('data-drop-over')
      zone?.setAttribute('data-drop-over', '')
      armed.current = zone
    }
    const end = () => {
      document.body.classList.remove('clip-dragging')
      setGhost(null)
      arm(null)
    }
    const move = (event: PointerEvent) => {
      const p = press.current
      if (!p) return
      if (!p.moved && Math.hypot(event.clientX - p.x, event.clientY - p.y) < 6) return
      if (!p.moved) {
        p.moved = true
        document.body.classList.add('clip-dragging')
        report.current?.(true)
      }
      setGhost({ x: event.clientX, y: event.clientY, name: p.name })
      arm(document.elementFromPoint(event.clientX, event.clientY)?.closest('[data-drop]') ?? null)
    }
    const up = (event: PointerEvent) => {
      const p = press.current
      press.current = null
      if (!p) return
      end()
      if (!p.moved) return
      report.current?.(false)
      // The click that follows a drag is not a press.
      justDragged.current = true
      window.setTimeout(() => (justDragged.current = false), 0)
      if (document.elementFromPoint(event.clientX, event.clientY)?.closest('[data-drop]')) add.current([p.file])
    }
    // Leaving the window mid-drag never says where the press ended: end it here, or the lit areas stay lit.
    const blur = () => {
      const wasDragging = press.current?.moved
      press.current = null
      end()
      if (wasDragging) report.current?.(false)
    }
    window.addEventListener('pointermove', move)
    window.addEventListener('pointerup', up)
    window.addEventListener('pointercancel', up)
    window.addEventListener('blur', blur)
    return () => {
      window.removeEventListener('pointermove', move)
      window.removeEventListener('pointerup', up)
      window.removeEventListener('pointercancel', up)
      window.removeEventListener('blur', blur)
      document.body.classList.remove('clip-dragging')
      arm(null)
    }
  }, [])
  return {
    ghost,
    down: (event: React.PointerEvent, item: MediaItem) => {
      if (event.button !== 0) return
      press.current = { file: item.file, name: simpleTitle(item.name), x: event.clientX, y: event.clientY, moved: false }
    },
    pressed: () => !justDragged.current,
  }
}

type RowState = 'idle' | 'added' | 'reading' | 'open'

/** One file of the library: a picture, a short title, when it was made, and how long and how big it is. */
function LibraryRow({ item, state, returnable = false, fresh = false, onPress, onDown, onMenu }: {
  /** G2: a file that arrived while the rail was open rises in (a file that was there on the first list does not). */
  fresh?: boolean
  item: MediaItem
  state: RowState
  /** A right click on the row, at the pointer's place. */
  onMenu: (x: number, y: number) => void
  /** A clip already in the sequence can be pressed to go back to the sequence (while a screenshot hides it). */
  returnable?: boolean
  onPress: () => void
  onDown: (event: React.PointerEvent) => void
}) {
  const made = stampOf(item.name) ?? (item.mtime ? new Date(item.mtime * 1000) : null)
  const figures = [item.kind === 'clip' && item.duration > 0 ? duration(item.duration) : '', item.size > 0 ? bytes(item.size) : ''].filter(Boolean).join(' · ')
  const idle = state === 'idle'
  const quiet = state === 'open' ? 'text-on-accent/80' : 'text-muted'
  return (
    <button
      type="button"
      disabled={(state === 'added' && !returnable) || state === 'reading'}
      title={`${item.name}${item.folder ? ` · ${item.folder}` : ''}`}
      onPointerDown={idle ? onDown : undefined}
      onClick={onPress}
      onContextMenu={(event) => {
        event.preventDefault()
        onMenu(event.clientX, event.clientY)
      }}
      onDragStart={(event) => event.preventDefault()}
      className={clsx(
        fresh && 'bx-tab-enter',
        'group mb-px flex w-full items-center gap-2.5 p-1.5 text-left transition-[background-color,transform,opacity] [transition-duration:var(--bx-dur-fast)]',
        // G1: a row being picked up lifts on the frame of the press (scale 1.02, opacity 0.9).
        state === 'open' ? 'bg-accent text-on-accent' : idle ? 'cursor-grab hover:bg-raised active:scale-[1.02] active:opacity-90' : state === 'added' && returnable ? 'cursor-pointer hover:bg-raised' : 'cursor-default opacity-50',
      )}
    >
      <Thumb item={item} className="h-[42px] w-[74px] shrink-0 border border-line" />
      <span className="min-w-0 flex-1">
        <span className="block truncate text-[13px] font-bold leading-tight">{simpleTitle(item.name)}</span>
        <span className={clsx('num block truncate text-[12px] leading-snug', quiet)}>{state === 'reading' ? 'Reading…' : made ? stampText(made) : ''}</span>
        <span className={clsx('num block truncate text-[12px] leading-snug', quiet)}>{state === 'added' ? (returnable ? 'Open the sequence' : 'In the sequence') : state === 'open' ? 'Editing' : figures}</span>
      </span>
      {/* G1: the reading spinner and the check swap with a cross-fade (the wrapper is keyed by the state). */}
      <span key={state} className="bx-swap inline-grid shrink-0">
        {state === 'reading' ? <Spinner size={14} /> : state === 'added' ? <Icon name="check" size={14} className="shrink-0 text-accent" /> : idle ? <Icon name="plus" size={14} className="shrink-0 text-faint opacity-0 transition-opacity [transition-duration:var(--bx-dur-fast)] group-hover:opacity-100 group-focus-visible:opacity-100" /> : null}
      </span>
    </button>
  )
}

/** The BuffrX library as two bins: the clips, and under them the screenshots. A clip joins the sequence, a screenshot
 *  opens in the screenshot editor. */
export function LibraryRail({ taken, loading, warning, openShot, onAdd, onReturn, onRemove, onBrowse, onDragChange }: {
  /** Paths already in the sequence, lower case: shown, not draggable. */
  taken: Set<string>
  /** Files being read: a spinner on their row until they are clips. */
  loading: string[]
  /** Said under the clips when they cannot be joined as they are. */
  warning: string | null
  /** The screenshot in the screenshot editor, if one is open. */
  openShot: string | null
  onAdd: (paths: string[]) => void
  /** A clip of the sequence was pressed while a screenshot is open: back to the sequence. */
  onReturn: () => void
  /** Take a clip out of the sequence (its file is not touched). */
  onRemove: (file: string) => void
  onBrowse: () => void
  /** A library row is being dragged: the drop areas light up for it. */
  onDragChange?: (dragging: boolean) => void
}) {
  const clips = useLibrary('clip')
  const shots = useLibrary('screenshot')
  const drag = useDragSource(onAdd, onDragChange)
  const reading = new Set(loading.map((path) => path.toLowerCase()))
  const open = openShot?.toLowerCase()
  // A right click on a row: what can be done with that file without opening anything.
  const [menu, setMenu] = useState<{ item: MediaItem; x: number; y: number } | null>(null)
  const [renaming, setRenaming] = useState<{ item: MediaItem; value: string } | null>(null)
  const [deleting, setDeleting] = useState<MediaItem | null>(null)
  const inUse = (item: MediaItem) => taken.has(item.file.toLowerCase()) || item.file.toLowerCase() === open
  // G2: the files the first lists held are known; any file after that is new until the rail has drawn it once.
  const known = useRef<Set<string> | null>(null)
  const isFresh = (file: string) => known.current !== null && !known.current.has(file)
  useEffect(() => {
    const all = [...(clips ?? []), ...(shots ?? [])].map((item) => item.file)
    if (known.current === null) {
      if (clips !== null && shots !== null) known.current = new Set(all)
      return
    }
    for (const file of all) known.current.add(file)
  }, [clips, shots])
  const menuItems = (() => {
    if (!menu) return []
    const { item } = menu
    const locked = inUse(item)
    const added = taken.has(item.file.toLowerCase())
    return [
      { id: 'use', label: item.kind === 'clip' ? (added ? 'Open the sequence' : 'Add to the sequence') : 'Open in the editor', onAction: () => (added ? onReturn() : onAdd([item.file])) },
      ...(added ? [{ id: 'unuse', label: 'Remove from the sequence', onAction: () => onRemove(item.file) }] : []),
      { id: 'reveal', label: 'Show in folder', onAction: () => void attempt('The folder did not open', () => engine.reveal(item)) },
      { id: 'copy', label: 'Copy', onAction: () => void attempt('Not copied', () => engine.copy([item.file])).then((r) => r !== undefined && toast('success', item.kind === 'clip' ? 'Clip copied' : 'Image copied', 'Paste it into Discord, a chat or a folder')) },
      { id: 'share', label: 'Share', onAction: () => void attempt('The share sheet did not open', () => engine.share(item.file)) },
      { id: 'rename', label: 'Rename', isDisabled: locked, onAction: () => setRenaming({ item, value: stem(item.file) }) },
      { id: 'delete', label: 'Delete', isDisabled: locked, onAction: () => setDeleting(item) },
    ]
  })()
  const rename = async () => {
    const entry = renaming
    setRenaming(null)
    if (!entry || !entry.value.trim() || entry.value.trim() === stem(entry.item.file)) return
    const done = await attempt('The file was not renamed', () => engine.rename(entry.item, entry.value.trim()))
    if (done !== undefined) void engine.notifyLibrary().catch(() => undefined)
  }
  const remove = async () => {
    const item = deleting
    setDeleting(null)
    if (!item) return
    const done = await attempt('The file was not deleted', () => engine.trash(item))
    if (done !== undefined) {
      toast('success', 'Moved to the Recycle Bin', simpleTitle(item.name))
      void engine.notifyLibrary().catch(() => undefined)
    }
  }
  return (
    <div className="area-rail flex min-h-0 flex-col gap-2">
      <aside className="pane flex min-h-0 flex-[3] flex-col">
        <div className="flex h-9 shrink-0 items-center justify-between px-2">
          <div className="flex items-center gap-2"><h3 className="text-[13px] text-muted">Clips</h3><span className="num bg-raised px-1.5 text-[13px] font-bold leading-[20px] text-accent">{clips?.length ?? 0}</span></div>
          <Button variant="ghost" icon="folder" className="h-6" title="Choose video files or pictures from your computer" onPress={onBrowse}>Browse</Button>
        </div>
        <div className="min-h-0 flex-1 select-none overflow-y-auto px-2 pb-2">
          {clips === null && <div className="grid h-[80px] place-items-center"><Spinner size={16} /></div>}
          {clips?.length === 0 && <p className="px-1 pt-2 text-[13px] leading-relaxed text-faint">No clips yet.</p>}
          {clips?.map((item) => {
            const key = item.file.toLowerCase()
            const state: RowState = taken.has(key) ? 'added' : reading.has(key) ? 'reading' : 'idle'
            return <LibraryRow key={item.file} item={item} state={state} fresh={isFresh(item.file)} returnable={openShot !== null} onDown={(event) => drag.down(event, item)} onMenu={(x, y) => setMenu({ item, x, y })} onPress={() => (state === 'added' ? onReturn() : drag.pressed() && onAdd([item.file]))} />
          })}
        </div>
        {warning && <p className="shrink-0 border-t border-line px-3 py-2 text-[13px] leading-snug text-warn">{warning}</p>}
      </aside>

      <aside className="pane flex min-h-0 flex-[2] flex-col">
        <div className="flex h-9 shrink-0 items-center gap-2 px-2">
          <h3 className="text-[13px] text-muted">Screenshots</h3>
          <span className="num bg-raised px-1.5 text-[13px] font-bold leading-[20px] text-accent">{shots?.length ?? 0}</span>
        </div>
        <div className="min-h-0 flex-1 select-none overflow-y-auto px-2 pb-2">
          {shots === null && <div className="grid h-[60px] place-items-center"><Spinner size={16} /></div>}
          {shots?.length === 0 && <p className="px-1 text-[13px] leading-relaxed text-faint">No screenshots yet.</p>}
          {shots?.map((item) => {
            const key = item.file.toLowerCase()
            const state: RowState = key === open ? 'open' : reading.has(key) ? 'reading' : 'idle'
            return <LibraryRow key={item.file} item={item} state={state} fresh={isFresh(item.file)} onDown={(event) => drag.down(event, item)} onMenu={(x, y) => setMenu({ item, x, y })} onPress={() => drag.pressed() && onAdd([item.file])} />
          })}
        </div>
      </aside>

      <ContextMenu at={menu ? { x: menu.x, y: menu.y } : null} label={menu ? `Actions for ${simpleTitle(menu.item.name)}` : 'File'} items={menuItems} onClose={() => setMenu(null)} />
      <DialogBox
        isOpen={renaming !== null}
        onClose={() => setRenaming(null)}
        title="Rename"
        width={380}
        actions={
          <>
            <Button variant="ghost" onPress={() => setRenaming(null)}>Cancel</Button>
            <Button variant="primary" onPress={() => void rename()}>Rename</Button>
          </>
        }
      >
        <input
          autoFocus
          aria-label="New name"
          value={renaming?.value ?? ''}
          onChange={(event) => renaming && setRenaming({ ...renaming, value: event.target.value })}
          onKeyDown={(event) => event.key === 'Enter' && void rename()}
          className="field w-full"
        />
      </DialogBox>
      <DialogBox
        isOpen={deleting !== null}
        onClose={() => setDeleting(null)}
        title="Delete this file?"
        width={380}
        actions={
          <>
            <Button variant="ghost" onPress={() => setDeleting(null)}>Keep it</Button>
            <Button variant="danger" onPress={() => void remove()}>Delete</Button>
          </>
        }
      >
        {deleting ? `${deleting.name} goes to the Recycle Bin.` : ''}
      </DialogBox>
      {drag.ghost && (
        <div className="pointer-events-none fixed z-[70] max-w-[240px] truncate border-2 border-accent bg-surface px-2 py-1 text-[13px] font-bold" style={{ left: drag.ghost.x + 12, top: drag.ghost.y + 12 }}>
          {drag.ghost.name}
        </div>
      )}
    </div>
  )
}

/** What the picture area says while no clip is in the edit: the drop target, and the picker's door. */
export function EmptyStage({ hover, onBrowse }: { hover: boolean; onBrowse: () => void }) {
  return (
    <div className={clsx('grid h-full w-full place-items-center border border-dashed transition-colors [transition-duration:var(--bx-dur-fast)]', hover ? 'border-accent bg-accent-soft' : 'border-line')}>
      <div className="flex max-w-[460px] flex-col items-center px-4 text-center">
        <button
          type="button"
          onClick={onBrowse}
          title="Choose video files or pictures from your computer"
          aria-label="Browse for a clip"
          className={clsx(
            'mb-4 grid h-14 w-14 cursor-pointer place-items-center transition-[transform,background-color,color] [transition-duration:var(--bx-dur-fast)] [transition-timing-function:var(--bx-ease-out)] hover:scale-105 focus-visible:scale-105',
            hover ? 'scale-105 bg-accent text-on-accent' : 'bg-raised text-accent hover:bg-accent hover:text-on-accent',
          )}
        >
          <Icon name="plus" size={26} />
        </button>
        <div className="text-[20px] font-extrabold">{hover ? 'Let go to add' : 'Drop clips to edit'}</div>
        <p className="mt-2 text-[14px] leading-relaxed text-muted">From the library, or click + to browse.</p>
      </div>
    </div>
  )
}

/** The row under the picture: play, the clip before and after, where the playhead is, and the clip's figures. */
export function Transport({ playing, fullscreen, time, total, hasNext, clip, onToggle, onPrev, onNext, onFullscreen }: {
  playing: boolean
  fullscreen: boolean
  time: number
  total: number
  hasNext: boolean
  clip: SequenceClip
  onToggle: () => void
  onPrev: () => void
  onNext: () => void
  onFullscreen: () => void
}) {
  // Three columns on one centre line: where the playhead is, the three playback buttons, and the clip figures.
  return (
    <div className="grid h-[38px] shrink-0 grid-cols-[1fr_auto_1fr] items-center gap-2 border-t border-line px-2 text-[13px] leading-none">
      <div className="num flex items-baseline gap-1 whitespace-nowrap">
        <span className="font-bold text-ink">{duration(time)}</span>
        <span className="text-muted">/ {duration(total)}</span>
      </div>
      <div className="flex items-center gap-1">
        <IconButton icon="skipBack" label="Previous clip (Up)" onPress={onPrev} />
        <IconButton icon={playing ? 'pause' : 'play'} label={playing ? 'Pause (Space)' : 'Play (Space)'} onPress={onToggle} instant />
        <IconButton icon="skipForward" label="Next clip (Down)" isDisabled={!hasNext} onPress={onNext} />
      </div>
      <div className="num flex items-center justify-end gap-2 whitespace-nowrap">
        <span className="text-ink">{clip.width}×{clip.height}</span>
        <span className="h-3.5 w-px bg-line" aria-hidden="true" />
        <span className="text-muted">{Math.round(clip.fps)} FPS</span>
        <span className="h-3.5 w-px bg-line" aria-hidden="true" />
        <span className="text-muted">{bytes(clip.bytes)}</span>
        <span className="h-3.5 w-px bg-line" aria-hidden="true" />
        <IconButton icon={fullscreen ? 'compact' : 'expand'} label={fullscreen ? 'Leave full screen (Esc)' : 'Full screen (F)'} onPress={onFullscreen} />
      </div>
    </div>
  )
}
