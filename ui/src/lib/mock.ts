// Fixtures for a plain browser with no engine behind it: enough for every screen to render, nothing
// more. Never evidence that a screen works (tools/dev-bridge.mjs drives the real engine for that).

type Json = Record<string, unknown>

const config: Json = {
  capture_mode: 'game', display_id: 0, display_name: '', window_id: '', window_name: '', game_title: '',
  capture_fps: 60, output_width: 0, output_height: 0, capture_cursor: true, stretch_mode: 'auto',
  codec: 'h264', encoder: 'auto', quality: 'high', custom_kbps: 0,
  keyframe_seconds: 2, encoder_options: {}, muxer_settings: '', buffer_seconds: 60,
  quick_clip_seconds: 20, desktop_enabled: true, desktop_volume: 1, desktop_muted: false,
  desktop_device: 'default', mic_enabled: true, mic_volume: 0.8, mic_gain_db: 0, mic_muted: false,
  mic_device: 'default', mic_noise_suppress: true, mic_noise_gate: false, audio_tracks: 1, audio_bitrate_kbps: 192,
  hotkey_save: 'Alt+1', hotkey_quick: 'Ctrl+Alt+1', hotkey_screenshot: 'Alt+2', hotkey_record: 'Alt+3',
  hotkey_toggle_ui: 'Alt+X', clips_dir: 'C:\\Users\\you\\Videos\\BuffrX', screenshots_dir: 'C:\\Users\\you\\Videos\\BuffrX\\Screenshots',
  recordings_dir: '', theme: 'auto', container: 'mp4', notifications: true, notification_sound: false,
  launch_on_startup: false, start_minimized: false, pause_on_lock: false, pause_on_battery: false,
  chunk_seconds: 30, recordings_budget_gb: 0, min_free_gb: 2,
  auto_game_capture: true, game_exclusions: [], active_profile: 0,
}
let buffering = true
let recording = false
const started = Date.now()

const clip = (i: number, folder: string, seconds: number) => ({
  file: `C:\\Users\\you\\Videos\\BuffrX\\${folder}\\Replay 2026-10-1${i} 2${i}-14-0${i}.mp4`,
  name: `Replay 2026-10-1${i} 2${i}-14-0${i}`,
  size: 18_000_000 + i * 4_100_000,
  mtime: Math.floor(started / 1000) - i * 5400,
  duration: seconds,
  folder,
})

export async function mockRequest(cmd: string, args: Json): Promise<unknown> {
  await new Promise((resolve) => setTimeout(resolve, 20))
  switch (cmd) {
    case 'hello':
      return { protocol: 1, engine: 'BuffrX preview', obs: '32.2.2', data_root: 'C:\\BuffrX', portable: false, gpu: { vendor: 'nvidia', name: 'RTX (preview)' }, capabilities: [] }
    case 'status.get':
    case 'buffer.start':
    case 'buffer.stop':
    case 'recording.start':
    case 'recording.stop': {
      if (cmd === 'buffer.start') buffering = true
      if (cmd === 'buffer.stop') buffering = false
      if (cmd === 'recording.start') recording = true
      if (cmd === 'recording.stop') recording = false
      return {
        buffering, buffer_state: buffering ? 'on' : 'off', recording, capture_ready: true,
        capture: { mode: 'game', target: 'Counter-Strike 2', width: 2560, height: 1440, fps: 60 },
        stats: { capture_fps: 60, total_frames: 9000, dropped: 0, video_frames: 9000, video_lagged: 3, render_time_ms: 0.21, restarts: 0, saved_clips: 4 },
        encoder: 'obs_nvenc_h264_tex', last_error: '',
        game: { detected_exe: 'cs2.exe', detected_title: 'Counter-Strike 2', capturing_game: true, auto_capture: true },
      }
    }
    case 'config.get':
      return config
    case 'config.set':
      Object.assign(config, args.patch as Json)
      return config
    case 'clips.list':
      return [clip(1, 'Counter-Strike 2', 60), clip(2, 'Counter-Strike 2', 20), clip(3, 'Display Capture', 45), clip(4, 'THE FINALS', 60)]
    case 'screenshots.list':
      return [1, 2, 3].map((i) => ({ ...clip(i, 'Screenshots', 0), file: `C:\\Users\\you\\Videos\\BuffrX\\Screenshots\\Screenshot 2026-10-1${i}.png`, name: `Screenshot 2026-10-1${i}` }))
    case 'hotkeys.get':
      return { save_replay: config.hotkey_save, quick_clip: config.hotkey_quick, screenshot: config.hotkey_screenshot, toggle_record: config.hotkey_record, toggle_ui: config.hotkey_toggle_ui, problems: {} }
    case 'profiles.list':
      return [
        { patch: { buffer_seconds: 60, capture_fps: 60, quality: 'high' } },
        { patch: { buffer_seconds: 180, capture_fps: 60, quality: 'high' } },
        { patch: { buffer_seconds: 60, capture_fps: 120, quality: 'high' } },
        { patch: { buffer_seconds: 60, capture_fps: 30, quality: 'standard' } },
      ]
    case 'devices.list':
      return { outputs: [{ id: 'default', name: 'Default', default: true }], inputs: [{ id: 'default', name: 'Default', default: true }] }
    case 'sources.displays':
      return [{ id: 0, name: 'Display 1', device: '\\\\.\\DISPLAY1', width: 2560, height: 1440, primary: true }]
    case 'sources.windows':
      return [{ id: 'Counter-Strike 2:SDL_app:cs2.exe', name: 'Counter-Strike 2', exe: 'cs2.exe' }]
    case 'storage.usage':
      return { clips_bytes: 1_200_000_000, clips_count: 41, screenshots_bytes: 90_000_000, screenshots_count: 22, recordings_bytes: 4_000_000_000, recordings_count: 3, free_bytes: 412_000_000_000, clips_dir: config.clips_dir, screenshots_dir: config.screenshots_dir }
    case 'encoder.properties':
      return { id: 'obs_nvenc_h264_tex', family: 'nvenc', properties: [
        { name: 'preset', label: 'Preset', kind: 'list', options: ['p1', 'p3', 'p5', 'p7'], option_labels: ['P1: fastest', 'P3', 'P5', 'P7: slowest'], value: 'p5' },
        { name: 'multipass', label: 'Multipass', kind: 'list', options: ['disabled', 'qres', 'fullres'], option_labels: ['Off', 'Quarter resolution', 'Full resolution'], value: 'qres' },
        { name: 'lookahead', label: 'Look-ahead', kind: 'bool', value: false },
        { name: 'bf', label: 'B-frames', kind: 'int', min: 0, max: 4, step: 1, value: 2 } ] }
    default:
      return {}
  }
}

export async function mockShell(cmd: string, _args: Json): Promise<unknown> {
  if (cmd === 'get_launch') return { role: 'ui', view: 'main', files: [] }
  if (cmd === 'media_run') return '{"ok":false,"error":"no engine in the preview"}'
  return null
}
