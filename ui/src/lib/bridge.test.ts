import assert from 'node:assert/strict'
import { test } from 'node:test'
import { onShell, shellEvent } from './bridge.ts'

const tick = () => new Promise((resolve) => setTimeout(resolve, 0))

test('files sent before a view listens reach the first listener once; progress is never held', async () => {
  // The panel's Edit reached a preview whose code was still loading: it said hello, but nothing listened yet.
  shellEvent('editor-files', ['a.mp4'])
  shellEvent('media-progress', 0.5)
  const edits: unknown[] = []
  const progress: unknown[] = []
  const off = onShell('editor-files', (files) => edits.push(files))
  const offProgress = onShell('media-progress', (value) => progress.push(value))
  await tick()
  assert.deepEqual(edits, [['a.mp4']], 'the held files are handed over')
  assert.deepEqual(progress, [], 'a late progress figure is stale, not held')
  const second: unknown[] = []
  const offSecond = onShell('editor-files', (files) => second.push(files))
  await tick()
  assert.deepEqual(second, [], 'held files are delivered once, not to every later listener')
  shellEvent('editor-files', ['b.mp4'])
  assert.deepEqual(edits, [['a.mp4'], ['b.mp4']], 'with a listener, files go straight through')
  off(); offProgress(); offSecond()
})

test('held files survive a listener that leaves and comes back in one commit', async () => {
  shellEvent('preview-files', ['c.mp4'])
  const first: unknown[] = []
  const got: unknown[] = []
  const off = onShell('preview-files', (files) => first.push(files))
  off()
  const offAgain = onShell('preview-files', (files) => got.push(files))
  await tick()
  assert.deepEqual(first, [], 'the listener that left gets nothing')
  assert.deepEqual(got, [['c.mp4']], 'the one that is there now gets the files')
  offAgain()
  shellEvent('preview-files', ['d.mp4'])
  const gone = onShell('preview-files', () => {})
  gone()
  await tick()
  const late: unknown[] = []
  const offLate = onShell('preview-files', (files) => late.push(files))
  await tick()
  assert.deepEqual(late, [['d.mp4']], 'with nobody left, they are held again for the next')
  offLate()
})
