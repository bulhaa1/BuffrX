import test from 'node:test'
import assert from 'node:assert/strict'

// lib/api.ts is the one place the engine's snake_case meets the interface's camelCase.
import { fromEngine, toEngine } from '../src/lib/convert.ts'

test('engine fields become camelCase, nested ones too', () => {
  const status = fromEngine({ buffer_state: 'on', stats: { ring: { bitrate_kbps: 18000, filled: 12 }, video_lagged: 3 } })
  assert.equal(status.bufferState, 'on')
  assert.equal(status.stats.ring.bitrateKbps, 18000)
  assert.equal(status.stats.videoLagged, 3)
})

test('a patch goes back as the engine spells it', () => {
  assert.deepEqual(toEngine({ micGainDb: 6, hotkeyToggleUi: 'Alt+X', captureFps: 120 }), {
    mic_gain_db: 6,
    hotkey_toggle_ui: 'Alt+X',
    capture_fps: 120,
  })
})

test("maps keyed by someone else's names pass through untouched", () => {
  const config = fromEngine({ encoder_options: { keyint_sec: 2, bf: 0 } })
  assert.deepEqual(config.encoderOptions, { keyint_sec: 2, bf: 0 })
  assert.deepEqual(toEngine({ encoderOptions: { keyint_sec: 2 } }), { encoder_options: { keyint_sec: 2 } })
})

test('lists convert element by element', () => {
  assert.deepEqual(fromEngine([{ is_default: true }, { is_default: false }]), [{ isDefault: true }, { isDefault: false }])
})
