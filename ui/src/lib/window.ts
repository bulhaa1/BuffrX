// The window this page lives in. Frameless everywhere, so the interface draws its own titlebar and
// these are its buttons. In a browser (dev bridge, mock) every call is a quiet no-op.

import { shell, transport } from './bridge'

async function current() {
  if (transport !== 'tauri') return null
  const { getCurrentWindow } = await import('@tauri-apps/api/window')
  return getCurrentWindow()
}

const INTERACTIVE = 'button, a, input, select, textarea, [role="button"], [role="slider"], [role="tab"], [data-no-drag]'

/** Mouse-down on the titlebar: move the window unless a control was pressed; double-click maximises. */
export async function titlebarMouseDown(event: React.MouseEvent, maximise = true) {
  if (event.button !== 0) return
  if ((event.target as HTMLElement).closest(INTERACTIVE)) return
  if (globalThis.window.__BUFFRX_EMBEDDED) { await shell('parent_drag'); return }
  const window = await current()
  if (!window) return
  if (event.detail === 2 && maximise) await window.toggleMaximize()
  else await window.startDragging()
}

export async function minimise() {
  if (window.__BUFFRX_EMBEDDED) { await shell('parent_minimise'); return }
  await (await current())?.minimize()
}

export async function toggleMaximise() {
  await (await current())?.toggleMaximize()
}

/** Closes the window: the editor process ends with it and gives back everything it used. */
export async function close() {
  await (await current())?.close()
}

/** Fullscreen is the monitor, not a bigger window: the preview's picture gets the whole screen. The
 *  host lifts the editor's design max on the way in and puts it back on the way out. */
export async function setFullscreen(on: boolean) {
  if (transport !== 'tauri') return false
  try {
    return await shell<boolean>('preview_fullscreen', { on })
  } catch {
    return false
  }
}

export async function isFullscreen() {
  if (transport !== 'tauri') return false
  try {
    return await shell<boolean>('preview_fullscreen_state')
  } catch {
    return false
  }
}
