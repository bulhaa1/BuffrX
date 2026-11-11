// The engine's protocol as typed calls. This file is the one place the engine's snake_case meets the
// interface's camelCase (AGENTS.md: convert once, in the bridge layer); components only ever see the
// types below.

import { call, onEngine, type Json } from './bridge'
import type { TimelineChunk } from './timeline/model'

export { fromEngine, toEngine } from './convert'
import { fromEngine, toEngine } from './convert'

// ---------------------------------------------------------------- types

export type BufferState = 'off' | 'starting' | 'on' | 'stopping'

export interface Status {
  buffering: boolean
  bufferState: BufferState
  recording: boolean
  recordingSeconds: number
  captureReady: boolean
  capture: { mode: string; target: string; width: number; height: number; fps: number }
  stats: {
    captureFps: number
    totalFrames: number
    dropped: number
    videoFrames: number
    videoLagged: number
    renderTimeMs: number
    restarts: number
    savedClips: number
  }
  encoder: string
  lastError: string
  game: { appId: string; profile: string; detectedExe: string; detectedTitle: string; capturingGame: boolean; autoCapture: boolean }
}

export interface Config {
  captureMode: 'display' | 'window' | 'game'
  displayId: number
  displayName: string
  windowId: string
  windowName: string
  gameTitle: string
  captureFps: number
  outputWidth: number
  outputHeight: number
  captureCursor: boolean
  stretchMode: 'auto' | 'stretch' | 'fit'
  codec: 'h264' | 'hevc' | 'av1'
  encoder: string
  quality: 'low' | 'standard' | 'high' | 'highest' | 'custom'
  customKbps: number
  keyframeSeconds: number
  encoderOptions: Record<string, unknown>
  muxerSettings: string
  bufferSeconds: number
  quickClipSeconds: number
  desktopEnabled: boolean
  desktopVolume: number
  desktopMuted: boolean
  desktopDevice: string
  micEnabled: boolean
  micVolume: number
  micGainDb: number
  micMuted: boolean
  micDevice: string
  micNoiseSuppress: boolean
  micNoiseGate: boolean
  audioTracks: number
  separateTracks: boolean
  audioBitrateKbps: number
  hotkeySave: string
  hotkeyQuick: string
  hotkeyScreenshot: string
  hotkeyRecord: string
  hotkeyToggleUi: string
  clipsDir: string
  screenshotsDir: string
  recordingsDir: string
  theme: string  // a themes.ts id; anything else paints the main theme
  container: 'mp4' | 'mkv'
  notifications: boolean
  notificationSound: boolean
  launchOnStartup: boolean
  startMinimized: boolean
  pauseOnLock: boolean
  pauseOnBattery: boolean
  chunkedRecording: boolean
  chunkSeconds: number
  recordingsBudgetGb: number
  minFreeGb: number
  autoGameCapture: boolean
  gameExclusions: string[]
  activeProfile: number
}

export interface Hello {
  protocol: number
  engine: string
  obs: string
  dataRoot: string
  portable: boolean
  gpu: { vendor: string; name: string }
  capabilities: string[]
}

export interface MediaItem {
  file: string
  name: string
  size: number
  mtime: number
  duration: number
  thumbnail: string // file name in the thumbnails folder, '' when none yet
  folder: string
  kind: 'clip' | 'screenshot'
}

/**
 * One Timeline session as the preview reads it (`timeline.get`): temporary footage with its own store,
 * not a saved file. `title` is the game the session started on, '' for a desktop-only one.
 */
export interface TimelineFootage {
  state: string
  minutes: number
  session: string
  title: string
  availableStartMs: number
  availableEndMs: number
  bytes: number
  chunks: TimelineChunk[]
}

export interface Profile {
  index: number
  bufferSeconds: number
  captureFps: number
  quality: string
}

export interface Hotkeys {
  saveReplay: string
  quickClip: string
  screenshot: string
  toggleRecord: string
  toggleUi: string
  problems: Record<string, string>
}

export interface Storage {
  clipsBytes: number
  clipsCount: number
  screenshotsBytes: number
  screenshotsCount: number
  recordingsBytes: number
  recordingsCount: number
  freeBytes: number
  clipsDir: string
  screenshotsDir: string
}

export interface EncoderProperty {
  name: string
  label: string
  kind: 'bool' | 'int' | 'float' | 'list' | 'text'
  min?: number
  max?: number
  step?: number
  options?: unknown[]
  optionLabels?: string[]
  value: unknown
}

export interface Display {
  id: number
  name: string
  device: string
  width: number
  height: number
  primary: boolean
}

export interface WindowTarget {
  id: string
  name: string
  exe: string
}

// ---------------------------------------------------------------- calls

const fileName = (path: string) => path.split(/[\\/]/).pop() ?? path

function media(items: unknown, kind: 'clip' | 'screenshot'): MediaItem[] {
  return (Array.isArray(items) ? items : []).map((raw) => {
    const item = raw as Json
    return {
      file: String(item.file ?? ''),
      name: String(item.name ?? ''),
      size: Number(item.size ?? 0),
      mtime: Number(item.mtime ?? 0),
      duration: Number(item.duration ?? 0),
      thumbnail: item.thumbnail ? fileName(String(item.thumbnail)) : '',
      folder: String(item.folder ?? ''),
      kind,
    }
  })
}

export const engine = {
  hello: (client: 'ui' | 'editor') => call('hello', { client }).then((data) => fromEngine<Hello>(data)),
  status: () => call('status.get').then((data) => fromEngine<Status>(data)),
  config: () => call('config.get').then((data) => fromEngine<Config>(data)),
  setConfig: (patch: Partial<Config>) => call('config.set', { patch: toEngine(patch) }).then((data) => fromEngine<Config>(data)),
  resetConfig: () => call('config.reset').then((data) => fromEngine<Config>(data)),

  startBuffer: () => call('buffer.start').then((data) => fromEngine<Status>(data)),
  stopBuffer: () => call('buffer.stop').then((data) => fromEngine<Status>(data)),
  save: (seconds?: number) => call('buffer.save', seconds ? { seconds } : {}),
  startRecording: () => call('recording.start').then((data) => fromEngine<Status>(data)),
  stopRecording: () => call('recording.stop').then((data) => fromEngine<Status>(data)),
  screenshot: (format: 'png' | 'jpg' = 'png') => call<{ file: string }>('screenshot.take', { format }),

  clips: () => call('clips.list').then((data) => media(data, 'clip')),
  screenshots: () => call('screenshots.list').then((data) => media(data, 'screenshot')),
  rename: (item: MediaItem, name: string) =>
    call<{ file: string }>(item.kind === 'clip' ? 'clips.rename' : 'screenshots.rename', { file: item.file, name }),
  trash: (item: MediaItem) => call(item.kind === 'clip' ? 'clips.trash' : 'screenshots.trash', { file: item.file }),
  reveal: (item: MediaItem) => call(item.kind === 'clip' ? 'clips.reveal' : 'screenshots.reveal', { file: item.file }),
  open: (item: MediaItem) => call('clips.open', { file: item.file }),
  copy: (files: string[]) => call('clips.copy', { files }),
  // The Share sheet is drawn by the engine's panel: this brings the panel forward with it open.
  share: (file: string) => call('ui.share', { file }),
  openFolder: (kind: 'clips' | 'screenshots' | 'data' | 'timeline', session?: string) =>
    call('app.open_folder', session ? { kind, session } : { kind }),
  pickFolder: (title: string, start: string) => call<{ path: string }>('app.pick_folder', { title, start }).then((data) => data.path),
  openEditor: (files: string[] = []) => call('editor.open', { files }),
  editorTimelineSource: (session: string) =>
    call<Json>('timeline.edit_source', { session }).then((data) => fromEngine<TimelineFootage>(data)),
  pinEditorTimeline: (session: string) => call('timeline.pin', {
    session, owner: `editor:${session}`, start_ms: 0, end_ms: 315360000000,
  }),
  unpinEditorTimeline: (session: string) => call('timeline.unpin', { session, owner: `editor:${session}` }),
  // The ordinary preview plays a session as it was recorded. Its Trim button opens the Timeline over the game.
  timeline: (session: string) =>
    call<Json>('timeline.get', { session, list: true }).then((data) => {
      const reply = fromEngine<Omit<TimelineFootage, 'title'> & { sessions?: { id: string; title: string }[] }>(data)
      return {
        ...reply,
        title: (reply.sessions ?? []).find((entry) => entry.id === reply.session)?.title ?? '',
      } satisfies TimelineFootage
    }),
  openTimeline: (session: string) => call('timeline.open', { session }),
  timelineFolder: (session: string) => call('app.open_folder', { kind: 'timeline', session }),
  notifyLibrary: () => call('library.notify'),
  storage: () => call('storage.usage').then((data) => fromEngine<Storage>(data)),

  displays: () => call('sources.displays').then((data) => fromEngine<Display[]>(data)),
  windows: () => call('sources.windows').then((data) => fromEngine<WindowTarget[]>(data)),
  devices: () =>
    call<{ outputs: Json[]; inputs: Json[] }>('devices.list').then((data) => ({
      outputs: data.outputs.map((d) => ({ id: String(d.id), name: String(d.name), isDefault: Boolean(d.default) })),
      inputs: data.inputs.map((d) => ({ id: String(d.id), name: String(d.name), isDefault: Boolean(d.default) })),
    })),
  setCapture: (args: { mode: string; displayId?: number; windowId?: string; windowName?: string; gameTitle?: string }) =>
    call('capture.set', toEngine(args)).then((data) => fromEngine<Status>(data)),

  hotkeys: () => call('hotkeys.get').then((data) => fromEngine<Hotkeys>(data)),
  setHotkey: (
    action: 'save_replay' | 'quick_clip' | 'screenshot' | 'toggle_record' | 'toggle_ui' | 'view_clip' | 'add_marker',
    chord: string,
  ) => call('hotkeys.set', { action, chord }).then((data) => fromEngine<Hotkeys>(data)),
  suspendHotkeys: (suspended: boolean) => call('hotkeys.suspend', { suspended }),

  profiles: () =>
    call<Json[]>('profiles.list').then((list) =>
      list.map((raw, index) => {
        const patch = (raw.patch ?? {}) as Json
        return {
          index,
          bufferSeconds: Number(patch.buffer_seconds ?? 60),
          captureFps: Number(patch.capture_fps ?? 60),
          // A profile saved before quality levels carries a tier; the engine reads it as the level it became.
          quality: String(patch.quality ?? ({ performance: 'standard', quality: 'highest' } as Record<string, string>)[String(patch.quality_tier)] ?? 'high'),
        } satisfies Profile
      }),
    ),
  addProfile: () => call('profiles.add', {}),
  applyProfile: (index: number) => call('profiles.apply', { index }),
  saveProfile: (index: number) => call('profiles.save', { index }),
  removeProfile: (index: number) => call('profiles.remove', { index }),
  updateProfile: (index: number, patch: Partial<Pick<Config, 'bufferSeconds' | 'captureFps' | 'quality'>>) =>
    call('profiles.update', { index, patch: toEngine(patch) }),

  encoderProperties: () =>
    call('encoder.properties').then((data) => fromEngine<{ id: string; family: string; properties: EncoderProperty[] }>(data)),
  setEncoderOption: (name: string, value: unknown) =>
    call('encoder.set', { name, value }).then((data) => fromEngine<{ id: string; family: string; properties: EncoderProperty[] }>(data)),

  restart: () => call('engine.restart'),
}

export type EngineEvent =
  | 'status'
  | 'clip.saved'
  | 'screenshot.saved'
  | 'recording.changed'
  | 'library.changed'
  | 'config.changed'
  | 'error'
  | 'game.changed'
  | 'connection'

export function onEvent<T = unknown>(name: EngineEvent, handler: (data: T) => void): () => void {
  return onEngine(name, (data) => handler(fromEngine<T>(data)))
}
