import assert from 'node:assert/strict'
import { test } from 'node:test'

import { sessionIdOf } from './model.ts'

test('a session media folder names its session', () => {
  assert.equal(sessionIdOf('C:\\data\\sessions\\BuffrX 2026-10-18 14-03-22\\media'), 'BuffrX 2026-10-18 14-03-22')
  assert.equal(sessionIdOf('C:/data/sessions/BuffrX 2026-10-18 14-03-22/media'), 'BuffrX 2026-10-18 14-03-22')
})

test('an ordinary clip is not a session', () => {
  assert.equal(sessionIdOf('C:\\data\\clips\\CS2\\Replay 2026-10-18 14-03-22.mp4'), '')
  assert.equal(sessionIdOf('C:\\data\\clips\\sessions.mp4'), '')
  assert.equal(sessionIdOf(''), '')
})

test('the folder name is read case-insensitively, and only when an id follows it', () => {
  assert.equal(sessionIdOf('C:\\Data\\Sessions\\abc\\media\\chunk 0.mp4'), 'abc')
  assert.equal(sessionIdOf('C:\\data\\sessions'), '')
  assert.equal(sessionIdOf('C:\\data\\sessions\\'), '')
})
