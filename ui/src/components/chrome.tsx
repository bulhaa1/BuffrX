// The window's own furniture: the titlebar (the drag handle of a frameless window) and the toasts. The
// titlebar is a status bar, as the native panel's is: the logo's place, the window's name as a framed
// module, the caller's buttons and the window's own.

import clsx from 'clsx'
import type { ReactNode } from 'react'
import { Button as AriaButton } from 'react-aria-components'
import { Icon, Mark } from './icons'
import { IconButton, Tip } from './ui'
import { close, minimise, titlebarMouseDown, toggleMaximise } from '../lib/window'
import { dismiss, useApp } from '../lib/store'

export function Titlebar({ children, title, maximise = true, onClose = close }: {
  children?: ReactNode
  title?: ReactNode
  maximise?: boolean
  onClose?: () => void
}) {
  return (
    <header
      className="flex h-9 shrink-0 items-center gap-1 bg-canvas pl-2.5 pr-1.5"
      onMouseDown={(event) => void titlebarMouseDown(event, maximise)}
    >
      <Mark size={18} />
      {title && (
        <span className="ml-2 flex h-[22px] items-center truncate border border-line bg-surface px-2 text-[14px] font-bold uppercase text-ink">
          {title}
        </span>
      )}
      <div className="flex-1" />
      {children}
      <WindowKeys maximise={maximise} onClose={onClose} />
    </header>
  )
}

/** The window's own keys, shared by the titlebar and the preview's floating chrome. */
export function WindowKeys({ maximise = true, onClose = close }: { maximise?: boolean; onClose?: () => void }) {
  return (
    <>
      <div className="mx-1.5 h-4 w-px bg-line" />
      <IconButton icon="minus" label="Minimise" className={window.__BUFFRX_EMBEDDED ? '!h-8 !w-9' : undefined} onPress={() => void minimise()} />
      {maximise && <IconButton icon="square" label="Maximise" size={13} onPress={() => void toggleMaximise()} />}
      {window.__BUFFRX_EMBEDDED ? (
        <Tip label="Back">
          <AriaButton aria-label="Back" onPress={onClose} className="flex h-8 min-w-16 items-center justify-center px-3 text-[14px] font-bold text-muted outline-none data-[hovered]:bg-raised data-[hovered]:text-ink data-[focus-visible]:outline data-[focus-visible]:outline-2 data-[focus-visible]:outline-accent">
            BACK
          </AriaButton>
        </Tip>
      ) : (
        <Tip label="Close">
          <AriaButton aria-label="Close" onPress={onClose} className="icon-btn data-[hovered]:!bg-danger data-[hovered]:!text-on-accent">
            <Icon name="close" size={15} />
          </AriaButton>
        </Tip>
      )}
    </>
  )
}

export function Toasts() {
  const toasts = useApp((s) => s.toasts)
  return (
    <div className="pointer-events-none fixed bottom-12 right-4 z-[60] flex w-[340px] flex-col gap-2">
      {toasts.map((toast) => (
        // A notice as a notification daemon draws one: square, a 2 px frame in its tone, a filled tag.
        <div
          key={toast.id}
          role="status"
          // F6: enters 12 px from the side and fades (the base token); leaving fades on the fast token (store.ts dismiss).
          className={clsx('bx-toast pointer-events-auto flex items-start gap-3 border-2 bg-surface p-3 transition-opacity [transition-duration:var(--bx-dur-fast)] [transition-timing-function:var(--bx-ease-in)]', toast.kind === 'error' ? 'border-warn' : 'border-accent')}
          style={toast.leaving ? { opacity: 0 } : undefined}
        >
          <span className={clsx('mt-px shrink-0 px-1.5 text-[13px] font-bold leading-[18px]', toast.kind === 'error' ? 'bg-warn text-canvas' : 'bg-accent text-on-accent')}>
            {toast.kind === 'error' ? 'alert' : toast.kind === 'success' ? 'done' : 'info'}
          </span>
          <div className="min-w-0 flex-1">
            <div className="text-[14px] font-bold text-ink">{toast.title}</div>
            {toast.detail && <div className="mt-0.5 break-words text-[13px] text-muted">{toast.detail}</div>}
            {toast.action && (
              <AriaButton onPress={toast.action.run} className="mt-1.5 text-[14px] font-bold text-accent data-[hovered]:underline">{toast.action.label}</AriaButton>
            )}
          </div>
          <AriaButton aria-label="Dismiss" onPress={() => dismiss(toast.id)} className="text-faint data-[hovered]:text-ink">
            <Icon name="close" size={13} />
          </AriaButton>
        </div>
      ))}
    </div>
  )
}
