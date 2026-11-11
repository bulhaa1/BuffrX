import assert from 'node:assert/strict'
import { test } from 'node:test'

// Small fragmented MP4 with one video track; the player still runs the production MP4 parser.
const bytes = (...parts) => Uint8Array.from(parts.flatMap((part) => [...part]))
const u32 = (n) => Uint8Array.of(n >>> 24, n >>> 16, n >>> 8, n)
const str = (s) => Uint8Array.from([...s].map((c) => c.charCodeAt(0)))
const box = (type, ...body) => { const data = bytes(...body); return bytes(u32(data.length + 8), str(type), data) }
const full = (type, flags, ...body) => box(type, u32(flags), ...body)
const videoEntry = box('avc1', new Uint8Array(78), box('avcC', Uint8Array.of(1, 100, 0, 31)))
const tkhd = full('tkhd', 0, u32(0), u32(0), u32(1))
const mdhd = full('mdhd', 0, u32(0), u32(0), u32(1000))
const hdlr = full('hdlr', 0, u32(0), str('vide'))
const stsd = full('stsd', 0, u32(1), videoEntry)
const moov = box('moov', box('trak', tkhd, box('mdia', mdhd, hdlr, box('minf', box('stbl', stsd)))))
const init = bytes(box('ftyp', str('isom')), moov)
const fragment = (time) => bytes(
  box('moof', full('mfhd', 0, u32(0)), box('traf', full('tfhd', 0x20008, u32(1), u32(500)),
    full('tfdt', 0, u32(time)), full('trun', 0, u32(1)))),
  box('mdat', Uint8Array.of(1, 2, 3)),
)

class FakeBuffer extends EventTarget {
  end = 0
  appended = 0
  refuse = false
  stall = false
  quota = false
  static quotaAll = false
  get buffered() { return { length: this.end ? 1 : 0, start: () => 0, end: () => this.end } }
  appendBuffer(data) {
    const type = String.fromCharCode(...new Uint8Array(data).subarray(4, 8))
    if ((this.quota || FakeBuffer.quotaAll) && type === 'moof') throw new DOMException('quota', 'QuotaExceededError')
    if (this.refuse && type === 'moof') {
      queueMicrotask(() => this.dispatchEvent(new Event('error')))
      return
    }
    if (type === 'moof') {
      this.appended++
      this.end += 0.5
    }
    if (this.stall) return
    queueMicrotask(() => this.dispatchEvent(new Event('updateend')))
  }
}
let media
class FakeMedia extends EventTarget {
  static isTypeSupported() { return true }
  constructor() { super(); media = this; this.buffer = new FakeBuffer() }
  addSourceBuffer() { return this.buffer }
}
class FakeVideo extends EventTarget {
  paused = true
  currentTime = 0
  duration = Infinity
  set src(_value) { queueMicrotask(() => media.dispatchEvent(new Event('sourceopen'))) }
  play() { this.paused = false; return Promise.resolve() }
  pause() { this.paused = true }
  removeAttribute() {}
  load() {}
}

test('Live follows the edge: normal speed near it, faster behind it, a jump when far behind', async () => {
  globalThis.window ??= {}
  const { liveCatchUp } = await import('./player.ts')
  assert.deepEqual(liveCatchUp(1.5, 1), { rate: 1 })
  assert.deepEqual(liveCatchUp(3.5, 1), { rate: 1 }, 'not yet behind enough to speed up')
  assert.equal(liveCatchUp(4.2, 1).rate, 1.08)
  assert.equal(liveCatchUp(4.2, 1).jumpTo, undefined)
  assert.equal(liveCatchUp(3.5, 1.08).rate, 1.08, 'keeps catching up until 3 s behind')
  assert.equal(liveCatchUp(2.9, 1.08).rate, 1, 'back to normal speed at 3 s')
  assert.equal(liveCatchUp(11, 1).rate, 1)
  assert.equal(liveCatchUp(11, 1).jumpTo, 3)
})

test('Live appends each fragment once, follows chunk rotation, and restarts after pause', async () => {
  globalThis.window = {}
  globalThis.MediaSource = FakeMedia
  const oldFetch = globalThis.fetch
  const oldCreate = URL.createObjectURL
  const oldRevoke = URL.revokeObjectURL
  let file = bytes(init, fragment(0))
  const files = new Map([['first', file]])
  globalThis.fetch = async (address) => {
    const url = new URL(address)
    const data = files.get(decodeURIComponent(url.pathname.slice(1)))
    const from = Number(url.searchParams.get('piece'))
    if (from >= data.length) return new Response(null, { status: 416 })
    return new Response(data.subarray(from), {
      status: 206, headers: { 'Content-Range': `bytes ${from}-${data.length - 1}/${data.length}` },
    })
  }
  URL.createObjectURL = () => 'blob:test'
  URL.revokeObjectURL = () => {}
  try {
    const { TimelinePlayer } = await import('./player.ts')
    const video = new FakeVideo()
    const player = new TimelinePlayer(video)
    const session = { id: 's', title: 's', availableStartMs: 0, availableEndMs: 0, chunks: [] }
    await player.load(session, 0)
    await player.startLive()
    await player.live('first', 0, file.length)
    assert.equal(media.buffer.appended, 1)
    assert.equal(video.paused, true, 'wait for enough fragments to absorb delivery jitter')
    file = bytes(file, fragment(500))
    files.set('first', file)
    await player.live('first', 0, file.length)
    assert.equal(media.buffer.appended, 2)
    video.currentTime = 0
    file = bytes(file, fragment(1000), fragment(1500), fragment(2000))
    files.set('first', file)
    await player.live('first', 0, file.length)
    assert.equal(video.currentTime, 1, 'initial playback starts behind the buffered edge')
    assert.equal(video.paused, false, 'playback starts once buffered')
    await player.live('first', 0, file.length)
    assert.equal(media.buffer.appended, 5)
    assert.equal(video.currentTime, 1, 'new fragments never seek the playhead')
    assert.equal(player.liveBytes.length, init.length, 'only the init is held once every fragment is appended')
    file = bytes(init, fragment(0))
    files.set('second', file)
    await player.live('second', 2500, file.length)
    assert.equal(media.buffer.appended, 6)
    player.stopLive()
    video.pause()
    file = bytes(file, fragment(500))
    files.set('second', file)
    await player.live('second', 2500, file.length)
    assert.equal(media.buffer.appended, 6)
    assert.equal(video.paused, true)
    await player.startLive()
    await player.live('second', 2500, file.length)
    assert.equal(media.buffer.appended, 7, 'returning to Live keeps the buffered stream')
    assert.equal(video.paused, false, 'returning to Live resumes immediately from buffered footage')
    assert.equal(video.currentTime, 2, 'returning to Live jumps to the latest buffered edge')
    player.stopLive()
    video.pause()
    await player.startLive()
    file = bytes(file, fragment(1000))
    files.set('second', file)
    let resolveFetch
    globalThis.fetch = () => new Promise((resolve) => { resolveFetch = resolve })
    const pending = player.live('second', 2500, file.length)
    while (!resolveFetch) await new Promise((resolve) => setImmediate(resolve))
    player.stopLive()
    video.pause()
    resolveFetch(new Response(file, { status: 200 }))
    await pending
    assert.equal(video.paused, true, 'a late fragment cannot undo a pause')
    player.close()
  } finally {
    globalThis.fetch = oldFetch
    URL.createObjectURL = oldCreate
    URL.revokeObjectURL = oldRevoke
  }
})

test('a new seek interrupts a slow earlier load instead of waiting for it', async () => {
  globalThis.window = {}
  globalThis.MediaSource = FakeMedia
  const oldFetch = globalThis.fetch
  const oldCreate = URL.createObjectURL
  const oldRevoke = URL.revokeObjectURL
  let started
  const fetching = new Promise((resolve) => { started = resolve })
  let release
  globalThis.fetch = (address, options) => {
    if (decodeURIComponent(new URL(address).pathname).includes('slow')) {
      started()
      return new Promise((resolve, reject) => {
        release = () => resolve(new Response(bytes(init, fragment(0))))
        options.signal.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')), { once: true })
      })
    }
    return Promise.resolve(new Response(bytes(init, fragment(0))))
  }
  URL.createObjectURL = () => 'blob:test'
  URL.revokeObjectURL = () => {}
  const { TimelinePlayer } = await import('./player.ts')
  const player = new TimelinePlayer(new FakeVideo())
  const session = { id: 's', title: 's', availableStartMs: 0, availableEndMs: 2500, chunks: [
    { file: 'slow', startMs: 0, endMs: 500, published: true, epoch: 0 },
    { file: 'fast', startMs: 2000, endMs: 2500, published: true, epoch: 0 },
  ] }
  try {
    const old = player.load(session, 0)
    await fetching
    const latest = player.seek(2000)
    const result = await Promise.race([latest.then(() => 'done'), new Promise((resolve) => setTimeout(() => resolve('blocked'), 100))])
    assert.equal(result, 'done', 'the latest seek must not wait for an obsolete fetch')
    assert.equal(player.positionMs(), 2000)
    await old
  } finally {
    release?.()
    player.close()
    globalThis.fetch = oldFetch
    URL.createObjectURL = oldCreate
    URL.revokeObjectURL = oldRevoke
  }
})

test('a seek can replace an append whose buffer never answers', async () => {
  globalThis.window = {}
  globalThis.MediaSource = FakeMedia
  const oldFetch = globalThis.fetch
  const oldCreate = URL.createObjectURL
  const oldRevoke = URL.revokeObjectURL
  const first = bytes(init, fragment(0))
  const second = bytes(init, fragment(500))
  const files = new Map([['first', first], ['second', second]])
  globalThis.fetch = async (address) => {
    const url = new URL(address)
    const data = files.get(decodeURIComponent(url.pathname.slice(1)))
    return new Response(data, { status: 200 })
  }
  URL.createObjectURL = () => 'blob:test'
  URL.revokeObjectURL = () => {}
  try {
    const { TimelinePlayer } = await import('./player.ts')
    const player = new TimelinePlayer(new FakeVideo())
    const chunk = (file, startMs) => ({ file, startMs, endMs: startMs + 500, published: true, epoch: 0 })
    await player.load({ id: 's', title: 's', availableStartMs: 0, availableEndMs: 500, chunks: [chunk('first', 0)] }, 0)
    // The buffer takes the next segment and never sends updateend: the append is parked.
    media.buffer.stall = true
    const appending = player.append([chunk('first', 0), chunk('second', 500)])
    await new Promise((resolve) => setImmediate(resolve))
    const seek = player.seek(1000)
    const result = await Promise.race([seek.then(() => 'done'), new Promise((resolve) => setTimeout(() => resolve('blocked'), 100))])
    assert.equal(result, 'done', 'the seek must not wait for a stale append forever')
    await appending
  } finally {
    globalThis.fetch = oldFetch
    URL.createObjectURL = oldCreate
    URL.revokeObjectURL = oldRevoke
  }
})


test('a clip larger than the media buffer fails instead of rebuilding for ever', async () => {
  globalThis.window = {}
  globalThis.MediaSource = FakeMedia
  const oldFetch = globalThis.fetch
  const oldCreate = URL.createObjectURL
  const oldRevoke = URL.revokeObjectURL
  const file = bytes(init, fragment(0))
  globalThis.fetch = async () => new Response(file, { status: 200 })
  URL.createObjectURL = () => 'blob:test'
  URL.revokeObjectURL = () => {}
  try {
    const { TimelinePlayer } = await import('./player.ts')
    const states = []
    const player = new TimelinePlayer(new FakeVideo(), { onState: (state) => states.push(state) })
    FakeBuffer.quotaAll = true
    const chunks = [
      { file: 'first', startMs: 0, endMs: 500, published: true, epoch: 0 },
      { file: 'second', startMs: 500, endMs: 1000, published: true, epoch: 0 },
    ]
    await player.load({ id: 's', title: 's', availableStartMs: 0, availableEndMs: 1000, chunks }, 0)
    assert.equal(states.at(-1), 'error', 'the page is told the footage is too large')
  } finally {
    FakeBuffer.quotaAll = false
    globalThis.fetch = oldFetch
    URL.createObjectURL = oldCreate
    URL.revokeObjectURL = oldRevoke
  }
})


test('switching sessions aborts old media and opens only the new session', async () => {
  globalThis.window = {}
  globalThis.MediaSource = FakeMedia
  const oldFetch = globalThis.fetch
  const oldCreate = URL.createObjectURL
  const oldRevoke = URL.revokeObjectURL
  let started
  const fetching = new Promise((resolve) => { started = resolve })
  const requests = []
  globalThis.fetch = (address, options) => {
    const path = decodeURIComponent(new URL(address).pathname)
    requests.push(path)
    if (path.includes('old')) {
      started()
      return new Promise((_resolve, reject) => options.signal.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')), { once: true }))
    }
    return Promise.resolve(new Response(bytes(init, fragment(0))))
  }
  URL.createObjectURL = () => 'blob:test'
  URL.revokeObjectURL = () => {}
  const { TimelinePlayer } = await import('./player.ts')
  const player = new TimelinePlayer(new FakeVideo())
  const make = (id, file) => ({ id, title: id, availableStartMs: 0, availableEndMs: 500, chunks: [{ file, startMs: 0, endMs: 500, published: true, epoch: 0 }] })
  try {
    const previous = player.load(make('old', 'old'), 0)
    await fetching
    const current = player.load(make('new', 'new'), 0)
    await current
    await previous
    assert.equal(player.positionMs(), 0)
    assert.ok(requests.some((path) => path.includes('new')))
    assert.equal(player.loaded().endMs, 500)
  } finally {
    player.close()
    globalThis.fetch = oldFetch
    URL.createObjectURL = oldCreate
    URL.revokeObjectURL = oldRevoke
  }
})

test('a seek within buffered footage does not wait for later chunks', async () => {
  globalThis.window = {}
  globalThis.MediaSource = FakeMedia
  const oldFetch = globalThis.fetch
  const oldCreate = URL.createObjectURL
  const oldRevoke = URL.revokeObjectURL
  let started
  const fetching = new Promise((resolve) => { started = resolve })
  let release
  globalThis.fetch = (address) => {
    if (decodeURIComponent(new URL(address).pathname).includes('slow')) {
      started()
      return new Promise((resolve) => { release = () => resolve(new Response(bytes(init, fragment(0)))) })
    }
    return Promise.resolve(new Response(bytes(init, fragment(0))))
  }
  URL.createObjectURL = () => 'blob:test'
  URL.revokeObjectURL = () => {}
  const { TimelinePlayer } = await import('./player.ts')
  const video = new FakeVideo()
  const player = new TimelinePlayer(video)
  const session = { id: 's', title: 's', availableStartMs: 0, availableEndMs: 1000, chunks: [
    { file: 'first', startMs: 0, endMs: 500, published: true, epoch: 0 },
    { file: 'slow', startMs: 500, endMs: 1000, published: true, epoch: 0 },
  ] }
  try {
    const load = player.load(session, 0)
    await fetching
    assert.equal(player.loaded().endMs, 500, 'a chunk is not advertised as loaded while its fetch is pending')
    const seek = player.seek(250)
    const result = await Promise.race([seek.then(() => 'done'), new Promise((resolve) => setTimeout(() => resolve('blocked'), 100))])
    assert.equal(result, 'done', 'already buffered footage must be seekable immediately')
    assert.equal(video.currentTime, 0.25)
    release()
    await load
  } finally {
    release?.()
    player.close()
    globalThis.fetch = oldFetch
    URL.createObjectURL = oldCreate
    URL.revokeObjectURL = oldRevoke
  }
})

test('a failed chunk append does not mark missing footage as loaded', async () => {
  globalThis.window = {}
  globalThis.MediaSource = FakeMedia
  const oldFetch = globalThis.fetch
  const oldCreate = URL.createObjectURL
  const oldRevoke = URL.revokeObjectURL
  let missing = true
  globalThis.fetch = (address) => Promise.resolve(
    decodeURIComponent(new URL(address).pathname).includes('next') && missing
      ? new Response(null, { status: 404 })
      : new Response(bytes(init, fragment(0))),
  )
  URL.createObjectURL = () => 'blob:test'
  URL.revokeObjectURL = () => {}
  const { TimelinePlayer } = await import('./player.ts')
  const player = new TimelinePlayer(new FakeVideo())
  const first = { file: 'first', startMs: 0, endMs: 500, published: true, epoch: 0 }
  const next = { file: 'next', startMs: 500, endMs: 1000, published: true, epoch: 0 }
  try {
    await player.load({ id: 's', title: 's', availableStartMs: 0, availableEndMs: 500, chunks: [first] }, 0)
    await player.append([first, next])
    assert.equal(player.loaded().endMs, 500, 'missing media cannot extend the playable window')
    missing = false
    await player.append([first, next])
    assert.equal(player.loaded().endMs, 1000, 'the chunk can be retried when it becomes available')
  } finally {
    player.close()
    globalThis.fetch = oldFetch
    URL.createObjectURL = oldCreate
    URL.revokeObjectURL = oldRevoke
  }
})

test('a playing seek resumes as soon as the new window is ready', async () => {
  globalThis.window = {}
  globalThis.MediaSource = FakeMedia
  const oldFetch = globalThis.fetch
  const oldCreate = URL.createObjectURL
  const oldRevoke = URL.revokeObjectURL
  globalThis.fetch = () => Promise.resolve(new Response(bytes(init, fragment(0))))
  URL.createObjectURL = () => 'blob:test'
  URL.revokeObjectURL = () => {}
  const { TimelinePlayer } = await import('./player.ts')
  const video = new FakeVideo()
  const player = new TimelinePlayer(video)
  const session = { id: 's', title: 's', availableStartMs: 0, availableEndMs: 2500, chunks: [
    { file: 'first', startMs: 0, endMs: 500, published: true, epoch: 0 },
    { file: 'next', startMs: 2000, endMs: 2500, published: true, epoch: 0 },
  ] }
  try {
    await player.load(session, 0)
    await video.play()
    await player.seek(2000, () => true)
    assert.equal(video.paused, false, 'a rebuild must not turn a playing seek into a pause')
    await player.seek(0, () => false)
    assert.equal(video.paused, true, 'a user pause during the rebuild must not be undone')
  } finally {
    player.close()
    globalThis.fetch = oldFetch
    URL.createObjectURL = oldCreate
    URL.revokeObjectURL = oldRevoke
  }
})

test('two seeks in a row rebuild the window once', async () => {
  globalThis.window = {}
  globalThis.MediaSource = FakeMedia
  const oldFetch = globalThis.fetch
  const oldCreate = URL.createObjectURL
  const oldRevoke = URL.revokeObjectURL
  globalThis.fetch = () => Promise.resolve(new Response(bytes(init, fragment(0))))
  URL.createObjectURL = () => 'blob:test'
  URL.revokeObjectURL = () => {}
  const { TimelinePlayer } = await import('./player.ts')
  const video = new FakeVideo()
  const logs = []
  const player = new TimelinePlayer(video, { onLog: (message) => logs.push(message) })
  const session = { id: 's', title: 's', availableStartMs: 0, availableEndMs: 12500, chunks: [
    { file: 'first', startMs: 0, endMs: 500, published: true, epoch: 0 },
    { file: 'early', startMs: 8000, endMs: 8500, published: true, epoch: 0 },
    { file: 'late', startMs: 12000, endMs: 12500, published: true, epoch: 0 },
  ] }
  try {
    await player.load(session, 0)
    const opens = () => logs.filter((message) => message.startsWith('open:')).length
    assert.equal(opens(), 1, 'the load opens one window')
    // A wheel settles on a target, then the reader adds a notch to it: only the newest target rebuilds.
    await Promise.all([player.seek(8000), player.seek(12000)])
    assert.equal(opens(), 2, 'a seek that a newer seek replaced never opens a window of its own')
    assert.equal(player.loaded().startMs, 12000, 'the window is the one the newest seek asked for')
    assert.equal(player.positionMs(), 12000)
  } finally {
    player.close()
    globalThis.fetch = oldFetch
    URL.createObjectURL = oldCreate
    URL.revokeObjectURL = oldRevoke
  }
})

test('an append started as a seek begins never reports the seek as a failure', async () => {
  globalThis.window = {}
  globalThis.MediaSource = FakeMedia
  const oldFetch = globalThis.fetch
  const oldCreate = URL.createObjectURL
  const oldRevoke = URL.revokeObjectURL
  globalThis.fetch = () => Promise.resolve(new Response(bytes(init, fragment(0))))
  URL.createObjectURL = () => 'blob:test'
  URL.revokeObjectURL = () => {}
  const { TimelinePlayer } = await import('./player.ts')
  const video = new FakeVideo()
  const states = []
  const player = new TimelinePlayer(video, { onState: (state) => states.push(state) })
  const session = { id: 's', title: 's', availableStartMs: 0, availableEndMs: 12500, chunks: [
    { file: 'first', startMs: 0, endMs: 500, published: true, epoch: 0 },
    { file: 'early', startMs: 8000, endMs: 8500, published: true, epoch: 0 },
    { file: 'late', startMs: 12000, endMs: 12500, published: true, epoch: 0 },
  ] }
  try {
    await player.load(session, 0)
    const seeking = player.seek(12000)
    const appending = player.append(session.chunks)
    await Promise.all([seeking, appending])
    assert.ok(!states.includes('append-failed'), 'the seek abort is not a failed append')
    assert.ok(!states.includes('error'), 'the picture was not dropped')
    assert.equal(player.appendFailed, false)
  } finally {
    player.close()
    globalThis.fetch = oldFetch
    URL.createObjectURL = oldCreate
    URL.revokeObjectURL = oldRevoke
  }
})

test('a seek asked for during a seek never paints the moment it replaced', async () => {
  globalThis.window = {}
  globalThis.MediaSource = FakeMedia
  const oldFetch = globalThis.fetch
  const oldCreate = URL.createObjectURL
  const oldRevoke = URL.revokeObjectURL
  let started
  const fetching = new Promise((resolve) => { started = resolve })
  let release
  globalThis.fetch = (address, options) => {
    if (decodeURIComponent(new URL(address).pathname).includes('slow')) {
      started()
      return new Promise((resolve, reject) => {
        release = () => resolve(new Response(bytes(init, fragment(0))))
        options.signal.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')), { once: true })
      })
    }
    return Promise.resolve(new Response(bytes(init, fragment(0))))
  }
  URL.createObjectURL = () => 'blob:test'
  URL.revokeObjectURL = () => {}
  const { TimelinePlayer } = await import('./player.ts')
  const video = new FakeVideo()
  let player
  // Every move of the picture, as a session moment: what the page paints its needle from.
  const painted = []
  let raw = 0
  Object.defineProperty(video, 'currentTime', {
    get: () => raw,
    set: (value) => {
      raw = value
      painted.push((player.loaded()?.startMs ?? 0) + value * 1000)
    },
  })
  player = new TimelinePlayer(video)
  const session = { id: 's', title: 's', availableStartMs: 0, availableEndMs: 12500, chunks: [
    { file: 'first', startMs: 0, endMs: 500, published: true, epoch: 0 },
    { file: 'slow', startMs: 8000, endMs: 8500, published: true, epoch: 0 },
    { file: 'late', startMs: 12000, endMs: 12500, published: true, epoch: 0 },
  ] }
  try {
    await player.load(session, 0)
    const replaced = player.seek(8000)
    await fetching
    const asked = painted.length
    const newest = player.seek(12000)
    release()
    await Promise.all([replaced, newest])
    assert.equal(player.positionMs(), 12000, 'the picture lands on the moment last asked for')
    const after = painted.slice(asked)
    assert.ok(after.length > 0, 'the newer seek moved the picture')
    assert.ok(after.every((at) => at >= 11999), `no paint of the replaced moment, saw ${after.join(', ')}`)
  } finally {
    release?.()
    player.close()
    globalThis.fetch = oldFetch
    URL.createObjectURL = oldCreate
    URL.revokeObjectURL = oldRevoke
  }
})

test('scrub previews avoid busy decoders and never seek into evicted footage', async () => {
  globalThis.window = {}
  globalThis.MediaSource = FakeMedia
  const oldFetch = globalThis.fetch
  const oldCreate = URL.createObjectURL
  const oldRevoke = URL.revokeObjectURL
  globalThis.fetch = () => Promise.resolve(new Response(bytes(init, fragment(0))))
  URL.createObjectURL = () => 'blob:test'
  URL.revokeObjectURL = () => {}
  const { TimelinePlayer } = await import('./player.ts')
  const video = new FakeVideo()
  let fastSeeks = 0
  video.fastSeek = () => { fastSeeks++ }
  const player = new TimelinePlayer(video)
  const chunk = { file: 'first', startMs: 0, endMs: 500, published: true, epoch: 0 }
  try {
    await player.load({ id: 's', title: 's', availableStartMs: 0, availableEndMs: 500, chunks: [chunk] }, 0)
    video.seeking = true
    assert.equal(player.seekPreview(250, true), true)
    assert.equal(fastSeeks, 0, 'do not queue a second preview seek while decoding')
    video.seeking = false
    assert.equal(player.seekPreview(250, true), true)
    assert.equal(fastSeeks, 1)
    player.evictedToMs = 300
    assert.equal(player.seekPreview(250, true), false, 'an evicted target needs a rebuild')
    assert.equal(fastSeeks, 1)
  } finally {
    player.close()
    globalThis.fetch = oldFetch
    URL.createObjectURL = oldCreate
    URL.revokeObjectURL = oldRevoke
  }
})

test('a rejected append is reported once, timeupdate does not retry it, and a growth append does', async () => {
  globalThis.window = {}
  globalThis.MediaSource = FakeMedia
  const oldFetch = globalThis.fetch
  const oldCreate = URL.createObjectURL
  const oldRevoke = URL.revokeObjectURL
  let offline = true
  const asked = []
  globalThis.fetch = (address) => {
    const name = decodeURIComponent(new URL(address).pathname).slice(1)
    asked.push(name)
    if (name === 'next' && offline) return Promise.reject(new Error('offline'))
    return Promise.resolve(new Response(bytes(init, fragment(0))))
  }
  URL.createObjectURL = () => 'blob:test'
  URL.revokeObjectURL = () => {}
  const { TimelinePlayer } = await import('./player.ts')
  const video = new FakeVideo()
  const states = []
  const player = new TimelinePlayer(video, { onState: (state, detail) => states.push([state, detail]) })
  const failed = () => states.filter(([state]) => state === 'append-failed')
  const first = { file: 'first', startMs: 0, endMs: 500, published: true, epoch: 0 }
  const next = { file: 'next', startMs: 500, endMs: 1000, published: true, epoch: 0 }
  try {
    await player.load({ id: 's', title: 's', availableStartMs: 0, availableEndMs: 500, chunks: [first] }, 0)
    await player.append([first, next])
    assert.equal(failed().length, 1, 'one failure episode is one report')
    assert.match(failed()[0][1], /offline/, 'the card says what failed')
    assert.equal(
      states.filter(([state]) => state === 'error' || state === 'missing').length,
      0,
      'a failed chunk is a soft problem, not a dead preview',
    )
    assert.equal(player.appendFailed, true)
    assert.match(player.appendError, /offline/, 'the last error is kept for the page')
    assert.equal(player.loaded().endMs, 500, 'footage that failed is not marked loaded')
    // A second call in the same episode fails again but is not a new episode.
    await player.append([first, next])
    assert.equal(failed().length, 1, 'the same episode is not reported again')
    // A growth append does try the file again: it may have become readable since.
    const askedBefore = asked.length
    await player.append([first, next])
    assert.equal(asked.length, askedBefore + 1, 'a growth append retries the failed chunk')
    assert.equal(failed().length, 1, 'the retry in the same episode is not reported again')
    // The tick asks four times a second; it must not ask again while the failure stands.
    const before = asked.length
    video.paused = false
    video.dispatchEvent(new Event('timeupdate'))
    await new Promise((resolve) => setImmediate(resolve))
    assert.equal(failed().length, 1, 'timeupdate reported the same failure again')
    assert.equal(asked.length, before, 'timeupdate fetched again while failed')
    // A growth append that works clears the flag and the message.
    offline = false
    await player.append([first, next])
    assert.equal(player.appendFailed, false)
    assert.equal(player.appendError, '')
    assert.equal(player.loaded().endMs, 1000, 'the recovered append extends the window')
  } finally {
    player.close()
    globalThis.fetch = oldFetch
    URL.createObjectURL = oldCreate
    URL.revokeObjectURL = oldRevoke
  }
})

test('a Live switch cancels an append in flight without calling it a failure', async () => {
  globalThis.window = {}
  globalThis.MediaSource = FakeMedia
  const oldFetch = globalThis.fetch
  const oldCreate = URL.createObjectURL
  const oldRevoke = URL.revokeObjectURL
  let requested
  const fetching = new Promise((resolve) => { requested = resolve })
  const file = bytes(init, fragment(0))
  globalThis.fetch = (address, options) => {
    if (decodeURIComponent(new URL(address).pathname).includes('slow')) {
      requested()
      return new Promise((resolve, reject) => {
        options.signal.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')), { once: true })
      })
    }
    return Promise.resolve(new Response(file))
  }
  URL.createObjectURL = () => 'blob:test'
  URL.revokeObjectURL = () => {}
  const { TimelinePlayer } = await import('./player.ts')
  const video = new FakeVideo()
  const states = []
  const player = new TimelinePlayer(video, { onState: (state, detail) => states.push([state, detail]) })
  const first = { file: 'first', startMs: 0, endMs: 500, published: true, epoch: 0 }
  const slow = { file: 'slow', startMs: 500, endMs: 1000, published: true, epoch: 0 }
  try {
    await player.load({ id: 's', title: 's', availableStartMs: 0, availableEndMs: 1000, chunks: [first] }, 0)
    await player.startLive()
    await player.live('first', 0, file.length)
    player.stopLive()
    video.pause()
    const appending = player.append([first, slow])
    await fetching
    await player.startLive()
    await appending
    assert.deepEqual(states.filter(([state]) => state === 'append-failed'), [], 'an intentional abort is not a read failure')
    assert.equal(player.appendFailed, false)
    assert.equal(player.appendError, '')
  } finally {
    player.close()
    globalThis.fetch = oldFetch
    URL.createObjectURL = oldCreate
    URL.revokeObjectURL = oldRevoke
  }
})

test('an append the media buffer refuses resolves and is reported once', async () => {
  globalThis.window = {}
  globalThis.MediaSource = FakeMedia
  const oldFetch = globalThis.fetch
  const oldCreate = URL.createObjectURL
  const oldRevoke = URL.revokeObjectURL
  globalThis.fetch = () => Promise.resolve(new Response(bytes(init, fragment(0))))
  URL.createObjectURL = () => 'blob:test'
  URL.revokeObjectURL = () => {}
  const { TimelinePlayer } = await import('./player.ts')
  const video = new FakeVideo()
  const states = []
  const rejections = []
  const onRejection = (reason) => rejections.push(reason)
  process.on('unhandledRejection', onRejection)
  const player = new TimelinePlayer(video, { onState: (state, detail) => states.push([state, detail]) })
  const first = { file: 'first', startMs: 0, endMs: 500, published: true, epoch: 0 }
  const next = { file: 'next', startMs: 500, endMs: 1000, published: true, epoch: 0 }
  try {
    await player.load({ id: 's', title: 's', availableStartMs: 0, availableEndMs: 500, chunks: [first] }, 0)
    media.buffer.refuse = true
    await player.append([first, next])
    await new Promise((resolve) => setImmediate(resolve))
    assert.deepEqual(rejections, [], 'a refused append leaves no unhandled rejection')
    assert.equal(states.filter(([state]) => state === 'append-failed').length, 1)
    assert.equal(player.appendFailed, true)
    assert.match(player.appendError, /refused/)
    assert.equal(player.loaded().endMs, 500, 'a refused segment does not extend the window')
  } finally {
    process.off('unhandledRejection', onRejection)
    player.close()
    globalThis.fetch = oldFetch
    URL.createObjectURL = oldCreate
    URL.revokeObjectURL = oldRevoke
  }
})
