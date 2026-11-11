// The editor page's state: what the engine reports, kept in one place and read through useApp(). No
// state library (AGENTS.md): a tiny external store over useSyncExternalStore. The panel's own state
// (library, hotkeys, profiles) lives in the engine's native interface, not here.

import { useSyncExternalStore } from 'react'
import { engine, onEvent, type Config, type Hello, type Status } from './api'
import { themeId, type ThemeId } from './themes'

export interface Toast {
  id: number
  kind: 'success' | 'info' | 'error'
  title: string
  detail?: string
  action?: { label: string; run: () => void }
  /** F6: set when dismissed; the notice fades out for a moment before it is removed. */
  leaving?: boolean
}

export interface AppState {
  connected: boolean
  hello: Hello | null
  status: Status | null
  config: Config | null
  toasts: Toast[]
}

let state: AppState = {
  connected: false,
  hello: null,
  status: null,
  config: null,
  toasts: [],
}
const subscribers = new Set<() => void>()

function set(patch: Partial<AppState>) {
  state = { ...state, ...patch }
  for (const notify of subscribers) notify()
}

export function useApp<T>(select: (s: AppState) => T): T {
  return useSyncExternalStore(
    (notify) => {
      subscribers.add(notify)
      return () => subscribers.delete(notify)
    },
    () => select(state),
  )
}

// ---------------------------------------------------------------- toasts

let toastId = 0
export function toast(kind: Toast['kind'], title: string, detail?: string, action?: Toast['action']) {
  const id = ++toastId
  set({ toasts: [...state.toasts.slice(-3), { id, kind, title, detail, action }] })
  setTimeout(() => dismiss(id), kind === 'error' ? 7000 : 4000)
}
export function dismiss(id: number) {
  const found = state.toasts.find((t) => t.id === id)
  if (!found || found.leaving) return
  // F6: the exit is the fast token (90 ms): the notice is marked, fades in the view, then leaves the list.
  set({ toasts: state.toasts.map((t) => (t.id === id ? { ...t, leaving: true } : t)) })
  setTimeout(() => set({ toasts: state.toasts.filter((t) => t.id !== id) }), 90)
}

export function errorText(error: unknown) {
  return error instanceof Error ? error.message : String(error)
}

/** Runs an engine action; a failure becomes an error toast instead of an unhandled rejection. */
export async function attempt<T>(title: string, work: () => Promise<T>): Promise<T | undefined> {
  try {
    return await work()
  } catch (error) {
    toast('error', title, errorText(error))
    return undefined
  }
}

// ---------------------------------------------------------------- engine data

export async function refreshConfig() {
  const config = await engine.config()
  set({ config })
  applyTheme()
  return config
}

/** The theme the engine stores; an older cockpit or an unknown id paints the main theme. */
export function theme(): ThemeId {
  return themeId(state.config?.theme)
}
function applyTheme() {
  if (typeof document !== 'undefined') document.documentElement.dataset.theme = theme()
}

let initialised = false
/** Connects as the editor, loads the settings it reads, and follows the engine's status. */
export async function initialise() {
  if (initialised) return
  initialised = true

  const load = async () => {
    try {
      const hello = await engine.hello('editor')
      set({ hello, connected: true })
      await refreshConfig()
      set({ status: await engine.status() })
    } catch (error) {
      set({ connected: false })
      console.warn('engine not reachable yet', error)
    }
  }

  onEvent<Status>('status', (status) => {
    // Nothing is re-rendered for a hidden window (AGENTS.md: nothing drawn while no window is open).
    if (typeof document !== 'undefined' && document.hidden) {
      state = { ...state, status }
      return
    }
    set({ status, connected: true })
  })
  onEvent<string>('connection', (value) => {
    if (value === 'connected') void load()
    else set({ connected: false })
  })
  onEvent('config.changed', () => {
    void refreshConfig().catch((error) => console.warn('config refresh failed', error))
  })
  await load()
  // The engine may still be starting: try again until hello answers.
  const retry = setInterval(() => {
    if (state.connected) clearInterval(retry)
    else void load()
  }, 2000)
}
