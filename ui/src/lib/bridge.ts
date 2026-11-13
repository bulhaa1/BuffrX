// The one road to the engine and the shell.
//
// Three transports, picked once at load:
// - tauri: the packaged shell (`engine_request` over the named pipe, engine frames as `engine_event`,
//   shell commands through invoke, shell events through listen);
// - dev:   tools/dev-bridge.mjs in a browser (HTTP + server-sent events, same frames);
// - mock:  a plain browser with nothing behind it (fixtures, so the interface can be looked at);
// - webview: the panel's clip page, inside the engine's window (WebView2 messages: a request is
//   `{id, cmd, args}`, its answer `{id, ok, data, error}`; the engine's events and shell events arrive as
//   `{event, data}` and `{shell, data}`).
//
// Frames stay exactly as the engine sends them here; lib/api.ts is the single place their field
// names are translated.

export type Transport = 'tauri' | 'dev' | 'mock' | 'webview'
export type Json = Record<string, unknown>

type WebViewMessages = {
  postMessage(message: string): void
  addEventListener(type: 'message', listener: (event: { data: unknown }) => void): void
}

declare global {
  interface Window {
    __TAURI_INTERNALS__?: unknown
    __buffrxDevBridge?: boolean

    __BUFFRX_VIEW?: string
    __BUFFRX_EMBEDDED?: boolean
    chrome?: { webview?: WebViewMessages }
  }
}

const hasWindow = typeof window !== 'undefined'
export const transport: Transport = hasWindow && window.__TAURI_INTERNALS__
  ? 'tauri'
  : hasWindow && window.__buffrxDevBridge
    ? 'dev'
    : hasWindow && window.chrome?.webview
      ? 'webview'
      : 'mock'

type Handler = (data: unknown) => void
const engineListeners = new Map<string, Set<Handler>>()
const shellListeners = new Map<string, Set<Handler>>()
let sequence = 0

// The webview's requests waiting for their answer, by id.
type Reply = { id?: string; ok?: boolean; data?: unknown; error?: string; event?: string; shell?: string }
const awaiting = new Map<string, (reply: Reply) => void>()

function webviewFrame(data: unknown) {
  const frame = (typeof data === 'string' ? JSON.parse(data) : data) as Reply
  if (frame.id !== undefined) {
    const answer = awaiting.get(frame.id)
    awaiting.delete(frame.id)
    answer?.(frame)
    return
  }
  if (frame.event) dispatch(engineListeners, frame.event, frame.data)
  if (frame.shell) dispatch(shellListeners, frame.shell, frame.data)
}

function webviewRequest<T>(cmd: string, args: Json): Promise<T> {
  const id = `ui-${++sequence}`
  return new Promise<T>((resolve, reject) => {
    awaiting.set(id, (reply) => (reply.ok ? resolve(reply.data as T) : reject(new EngineError(reply.error || `${cmd} failed`))))
    window.chrome!.webview!.postMessage(JSON.stringify({ id, cmd, args }))
  })
}

function dispatch(map: Map<string, Set<Handler>>, name: string, data: unknown) {
  for (const handler of map.get(name) ?? []) {
    try {
      handler(data)
    } catch (error) {
      console.error(`handler for ${name} failed`, error)
    }
  }
  for (const handler of map.get('*') ?? []) handler({ name, data })
}

function engineFrame(text: string) {
  try {
    const frame = JSON.parse(text) as { event?: string; data?: unknown }
    if (frame.event) dispatch(engineListeners, frame.event, frame.data)
  } catch {
    /* a frame that is not JSON is not ours */
  }
}

let started = false
async function start() {
  if (started) return
  started = true
  if (transport === 'tauri') {
    const { listen } = await import('@tauri-apps/api/event')
    await listen<string>('engine_event', (event) => engineFrame(event.payload))
    await listen<string>('engine_status', (event) => dispatch(engineListeners, 'connection', event.payload))
    for (const name of ['media-progress', 'media-done', 'editor-files', 'preview-files']) {
      await listen(name, (event) => dispatch(shellListeners, name, event.payload))
    }
  } else if (transport === 'dev') {
    const source = new EventSource('/__engine/events')
    source.onmessage = (message) => engineFrame(message.data)
    source.addEventListener('shell', (message) => {
      const { name, data } = JSON.parse((message as MessageEvent).data) as { name: string; data: unknown }
      dispatch(shellListeners, name, data)
    })
    source.onerror = () => dispatch(engineListeners, 'connection', 'disconnected')
    source.onopen = () => dispatch(engineListeners, 'connection', 'connected')
  } else if (transport === 'webview') {
    window.chrome!.webview!.addEventListener('message', (event) => webviewFrame(event.data))
    dispatch(engineListeners, 'connection', 'connected')
  }
}
void start()

export class EngineError extends Error {}

/** One engine command. Resolves with `data`, rejects with the engine's own message. */
export async function call<T = unknown>(cmd: string, args: Json = {}): Promise<T> {
  if (transport === 'webview') return webviewRequest<T>(cmd, args)
  const id = `ui-${++sequence}`
  const frame = JSON.stringify({ id, cmd, args })
  let text: string
  if (transport === 'tauri') {
    const { invoke } = await import('@tauri-apps/api/core')
    text = await invoke<string>('engine_request', { frame })
  } else if (transport === 'dev') {
    const response = await fetch('/__engine/request', { method: 'POST', body: frame })
    text = await response.text()
  } else {
    const { mockRequest } = await import('./mock')
    return mockRequest(cmd, args) as Promise<T>
  }
  const reply = JSON.parse(text) as { ok?: boolean; data?: T; error?: string }
  if (!reply.ok) throw new EngineError(reply.error || `${cmd} failed`)
  return reply.data as T
}

/** Engine events: status, clip.saved, library.changed, error, … and 'connection'. */
export function onEngine(name: string, handler: Handler): () => void {
  if (!engineListeners.has(name)) engineListeners.set(name, new Set())
  engineListeners.get(name)!.add(handler)
  return () => engineListeners.get(name)?.delete(handler)
}

/** A shell command (window, media, files). */
export async function shell<T = unknown>(cmd: string, args: Json = {}): Promise<T> {
  if (transport === 'webview') return webviewRequest<T>(cmd, args)
  if (transport === 'tauri') {
    const { invoke } = await import('@tauri-apps/api/core')
    return invoke<T>(cmd, args)
  }
  if (transport === 'dev') {
    const response = await fetch('/__shell/invoke', { method: 'POST', body: JSON.stringify({ cmd, args }) })
    const reply = (await response.json()) as { ok: boolean; data?: T; error?: string }
    if (!reply.ok) throw new Error(reply.error || `${cmd} failed`)
    return reply.data as T
  }
  const { mockShell } = await import('./mock')
  return mockShell(cmd, args) as Promise<T>
}

/** Shell events: media-progress, media-done, editor-files, preview-files. */
export function onShell(name: string, handler: Handler): () => void {
  if (!shellListeners.has(name)) shellListeners.set(name, new Set())
  shellListeners.get(name)!.add(handler)
  return () => shellListeners.get(name)?.delete(handler)
}

// The library folders the panel's page may read, as the engine maps them into the page (web_clip.cpp): a host per folder.
export type MediaRoot = { host: string; folder: string }
let mediaRoots: MediaRoot[] = []
export function setMediaRoots(roots: MediaRoot[]) {
  mediaRoots = roots
}

/** A file under one of the mapped folders, as the page's own URL for it (the browser serves the byte ranges). */
function webviewMediaUrl(path: string): string {
  const same = (text: string) => text.replace(/\//g, '\\').replace(/\\+$/, '').toLowerCase()
  for (const root of mediaRoots) {
    const folder = root.folder.replace(/[\\/]+$/, '')
    if (!same(path).startsWith(same(folder) + '\\')) continue
    const relative = path.slice(folder.length + 1).split(/[\\/]/).map(encodeURIComponent).join('/')
    return `https://${root.host}/${relative}`
  }
  return ''
}

/** A clip or screenshot as a URL a <video>/<img> can load (streams with byte ranges). */
// Tauri serves custom schemes as http://<name>.localhost on Windows/Android and as
// <name>://localhost elsewhere; WebKitGTK will not resolve the Windows form.
const customScheme = (name: string, path: string) =>
  /Windows|Android/i.test(navigator.userAgent) ? `http://${name}.localhost/${path}` : `${name}://localhost/${path}`

// The Linux editor serves media from a loopback HTTP server (see src-tauri/src/media.rs): its
// WebKitGTK build does not play a custom scheme reliably, and every WebView agrees on HTTP.
const mediaPort = hasWindow ? new URLSearchParams(window.location.search).get('media_port') : null
const loopbackMedia = mediaPort ? `http://127.0.0.1:${mediaPort}` : ''

export function mediaUrl(path: string): string {
  if (!path) return ''
  if (transport === 'tauri') {
    if (loopbackMedia) return `${loopbackMedia}/media/${encodeURIComponent(path)}`
    return customScheme('media', encodeURIComponent(path))
  }
  if (transport === 'webview') return webviewMediaUrl(path)
  if (transport === 'dev') return `/media/${encodeURIComponent(path)}`
  return ''
}

/** A cached thumbnail by file name (the engine reports thumbnails by name). */
export function thumbnailUrl(name: string): string {
  if (!name) return ''
  if (transport === 'tauri') {
    if (loopbackMedia) return `${loopbackMedia}/thumbnails/${encodeURIComponent(name)}`
    return customScheme('thumbnails', encodeURIComponent(name))
  }
  if (transport === 'dev') return `/thumbnails/${encodeURIComponent(name)}`
  return ''
}

/** How this window opened: the editor, or a clip preview (which can become the editor). */
export function launchView(): { view: 'editor' | 'preview' | 'panel' } {
  const params = hasWindow ? new URLSearchParams(window.location.search) : new URLSearchParams()
  const view = (hasWindow && window.__BUFFRX_VIEW) || params.get('view') || 'editor'
  return { view: view === 'preview' ? 'preview' : view === 'panel' ? 'panel' : 'editor' }
}
