// The export tab's file name: one Windows name, always .mp4, never a path or a device. Run with `npm test`.
import assert from 'node:assert/strict'
import { test } from 'node:test'
import { exportFileName } from './filename.ts'

test('a plain name is saved as typed, with the extension added', () => {
  assert.deepEqual(exportFileName('Replay-edit'), { name: 'Replay-edit.mp4', error: '' })
  assert.deepEqual(exportFileName('The Long Take'), { name: 'The Long Take.mp4', error: '' })
})

test('the extension is kept, not doubled, and its case is left alone', () => {
  assert.deepEqual(exportFileName('clip.mp4'), { name: 'clip.mp4', error: '' })
  assert.deepEqual(exportFileName('clip.MP4'), { name: 'clip.MP4', error: '' })
  assert.deepEqual(exportFileName('clip.mkv'), { name: 'clip.mkv.mp4', error: '' })
})

test('a name that is empty or only spaces is refused', () => {
  assert.equal(exportFileName('').error, 'Give the copy a name first.')
  assert.equal(exportFileName('   ').error, 'Give the copy a name first.')
  assert.equal(exportFileName('.mp4').error, 'Give the copy a name first.')
})

test('separators, traversal and the characters Windows refuses are refused with a reason', () => {
  for (const value of ['a/b', 'a\\b', '..\\..\\clip', '../clip', 'clip?.mp4', 'a:b', 'a|b', 'a*b', 'a"b', 'a<b', 'a>b']) {
    assert.ok(exportFileName(value).error, `${value} is refused`)
    assert.equal(exportFileName(value).name, '', `${value} gives no file name`)
  }
  assert.equal(exportFileName('..').error, 'A file name cannot be . or ..')
})

test('a Windows device name cannot be a clip', () => {
  for (const value of ['CON', 'con.mp4', 'CON.txt.mp4', 'NUL .clip', 'COM¹.mp4', 'PRN', 'aux.mp4', 'NUL', 'COM1', 'com9.mp4', 'LPT1', 'lpt9.mp4', 'CON ']) {
    assert.match(exportFileName(value).error, /device name/, `${value} is a device`)
  }
  // Not the device: only an exact name (before the extension) is.
  assert.equal(exportFileName('console.mp4').error, '')
  assert.equal(exportFileName('COM10.mp4').error, '')
})

test('a trailing dot is refused; a dot inside the name is not', () => {
  assert.equal(exportFileName('clip.').error, 'A file name cannot end with a dot or a space.')
  assert.deepEqual(exportFileName('clip.1'), { name: 'clip.1.mp4', error: '' })
  assert.deepEqual(exportFileName('clip..mp4'), { name: 'clip..mp4', error: '' })
})

test('a name too long for Windows says so', () => {
  assert.ok(exportFileName('a'.repeat(201)).error)
  assert.equal(exportFileName('a'.repeat(200)).error, '')
})
