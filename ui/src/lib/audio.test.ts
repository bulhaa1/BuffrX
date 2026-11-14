// A clip's audio layout and the gains the preview and export use. Run with `npm test`.
import assert from 'node:assert/strict'
import { test } from 'node:test'
import {
  audioDefaults,
  audioIsEdited,
  audioLayout,
  audioRows,
  audibleRows,
  elementGain,
  exportGains,
  extraGain,
  extraTracks,
  heardPeaks,
  isUnknownMultitrack,
  joinSoundProblem,
  mixIsDerived,
  mixLockReason,
  monitorGains,
  monitorMode,
  trackIsLocked,
  trackName,
  wheelLevel,
  whenPlayable,
  type AudioTrack,
  type PlayableSource,
} from './audio.ts'

const track = (index: number, role: string, title: string, sources: string[]): AudioTrack => ({ index, channels: 2, title, role, sources })

/** Peaks are floats: compare the mixed lane to six places, so a sum is a value, not a rounding bit. */
const rounded = (values: number[] | undefined) => values?.map((value) => Math.round(value * 1e6) / 1e6)

/** What the capture plan writes for desktop + microphone + Discord on their own tracks. */
const buffrx: AudioTrack[] = [
  track(1, 'mix', 'BuffrX Mix', ['desktop', 'microphone', 'app:Discord.exe']),
  track(2, 'desktop', 'BuffrX Desktop', ['desktop']),
  track(3, 'microphone', 'BuffrX Microphone', ['microphone']),
  track(4, 'application', 'BuffrX Discord', ['app:Discord.exe']),
]

test('an unlabelled multitrack import starts with its first track alone', () => {
  const imported = [1, 2, 3].map((i) => track(i, 'track', '', []))
  assert.equal(isUnknownMultitrack(imported), true)
  assert.deepEqual(audioDefaults(imported), {
    volumes: [1, 1, 1],
    muted: [false, false, false],
    solo: [true, false, false],
    master: 1,
  })
  // Auditioning one track never changes the streams saved in an export.
  const start = audioDefaults(imported)
  assert.deepEqual(monitorGains(start.volumes, start.muted, start.solo, imported.length), [1, 0, 0])
  assert.deepEqual(exportGains(start.volumes, start.muted, imported.length), [1, 1, 1])
  assert.equal(audioIsEdited(imported, [1, 1, 1], [false, false, false]), false)
  assert.equal(audioIsEdited(imported, [1, 1, 1], [false, true, false]), true)
  assert.equal(audioIsEdited(imported, [1, 0.5, 1], [false, false, false]), true)
  // A single track and every labelled BuffrX layout keep every track audible from the start.
  assert.equal(isUnknownMultitrack([imported[0]]), false)
  assert.equal(isUnknownMultitrack(buffrx), false)
  assert.deepEqual(audioDefaults(buffrx).muted, [false, false, false, false])
  assert.deepEqual(audioDefaults([buffrx[0], buffrx[1], buffrx[2]]).muted, [false, false, false])
})

test('a BuffrX sidecar with every source on its own track is complete', () => {
  const layout = audioLayout(buffrx)
  assert.equal(layout.buffrx, true)
  assert.equal(layout.aggregate, true)
  assert.equal(layout.complete, true)
  assert.equal(mixIsDerived(layout), true)
})

test('a mix with a source no track holds is not recomputable, and nothing is inferred from counts', () => {
  const missing = [buffrx[0], buffrx[1], buffrx[2]]
  const layout = audioLayout(missing)
  assert.equal(layout.complete, false)
  assert.equal(mixIsDerived(layout), false)
  assert.equal(trackIsLocked(layout), true)
})

test('an imported multi-track file is never assumed to have an aggregate', () => {
  const imported = [1, 2, 3].map((i) => track(i, 'track', '', []))
  const layout = audioLayout(imported)
  assert.deepEqual(layout, { buffrx: false, aggregate: false, complete: false })
  assert.equal(monitorMode(layout, imported.length), 'streams')
  assert.equal(trackIsLocked(layout), false)
})

test('the same source on two tracks is ambiguous, so the mix is not rebuilt', () => {
  const duplicate = [buffrx[0], track(2, 'desktop', 'Desktop', ['desktop']), track(3, 'desktop', 'Desktop again', ['desktop']), buffrx[2]]
  assert.equal(audioLayout(duplicate).complete, false)
  // Listed twice on one track counts twice, exactly as the engine counts it.
  const twice = [buffrx[0], track(2, 'desktop', 'Desktop', ['desktop', 'desktop']), buffrx[2], buffrx[3]]
  assert.equal(audioLayout(twice).complete, false)
})

test('a complete aggregate monitors its sources only, never the mix as well', () => {
  const layout = audioLayout(buffrx)
  const mode = monitorMode(layout, buffrx.length)
  assert.equal(mode, 'isolated')
  assert.deepEqual(extraTracks(mode, buffrx.length), [1, 2, 3])
  // The element's own track is the mix, and it stays silent while the sources are ready.
  assert.equal(elementGain(mode, [1, 1, 1, 1], 'ready'), 0)
  // If the copies cannot be made, the mix is heard rather than silence.
  assert.equal(elementGain(mode, [1, 1, 1, 1], 'failed'), 1)
  assert.equal(elementGain(mode, [1, 1, 1, 1], 'loading'), 1)
  assert.equal(extraGain([1, 0.5], 1, 'loading'), 0)
  assert.equal(extraGain([1, 0.5], 1, 'failed'), 0)
  assert.equal(extraGain([1, 0.5], 1, 'ready'), 0.5)
})

test('an aggregate that cannot be rebuilt plays the mix alone, so nothing is dropped', () => {
  const layout = audioLayout([buffrx[0], buffrx[1]])
  const mode = monitorMode(layout, 2)
  assert.equal(mode, 'mix')
  assert.deepEqual(extraTracks(mode, 2), [])
  assert.equal(elementGain(mode, [0.5, 1], 'ready'), 0.5)
})

test('independent tracks are all monitored', () => {
  const imported = [track(1, 'track', '', []), track(2, 'track', '', [])]
  const mode = monitorMode(audioLayout(imported), 2)
  assert.equal(mode, 'streams')
  assert.deepEqual(extraTracks(mode, 2), [1])
  assert.equal(elementGain(mode, [0.25, 2], 'ready'), 0.25)
})

test('a join is refused when equal track counts still disagree on what stream 1 means', () => {
  const imported = [track(1, 'track', '', []), track(2, 'track', '', [])]
  const desktop = [track(1, 'mix', 'BuffrX Mix', ['desktop']), track(2, 'desktop', 'BuffrX Desktop', ['desktop'])]
  const microphone = [track(1, 'mix', 'BuffrX Mix', ['microphone']), track(2, 'microphone', 'BuffrX Microphone', ['microphone'])]
  // Both are aggregates with the same width: the engine can describe the joined lanes positionally.
  assert.equal(joinSoundProblem([{ audio: desktop }, { audio: microphone }]), '')
  // Equal width, but one starts with a mix and the other with an independent track: no shared meaning.
  assert.equal(joinSoundProblem([{ audio: desktop }, { audio: imported }]), 'kinds')
  // Imports with imports are still fine: neither claims a mix, and the join carries no layout at all.
  assert.equal(joinSoundProblem([{ audio: imported }, { audio: imported }]), '')
  // Unlike widths are refused first, whatever the kinds are.
  assert.equal(joinSoundProblem([{ audio: buffrx }, { audio: imported.slice(0, 1) }]), 'tracks')
  // One clip is not a join, so nothing is refused.
  assert.equal(joinSoundProblem([{ audio: buffrx }]), '')
  assert.equal(joinSoundProblem([]), '')
})

test('one track is only the element', () => {
  const mode = monitorMode(audioLayout([track(1, 'track', '', [])]), 1)
  assert.equal(mode, 'element')
  assert.deepEqual(extraTracks(mode, 1), [])
})

test('a mute silences its track, and a mute wins over a solo of the same track', () => {
  assert.deepEqual(monitorGains([1, 1, 1], [false, true, false], [], 3), [1, 0, 1])
  assert.deepEqual(monitorGains([1, 1, 1], [], [true, false, false], 3), [1, 0, 0])
  assert.deepEqual(monitorGains([0.5, 1, 2], [true, true, true], [], 3), [0, 0, 0])
  // Soloing a muted track auditions silence: the mute is a saved edit and stays the final word.
  assert.deepEqual(monitorGains([1, 0.5], [true, false], [true, true], 2), [0, 0.5])
  // Exiting solo restores the stored mutes and levels untouched.
  assert.deepEqual(monitorGains([1, 0.5], [true, false], [false, false], 2), [0, 0.5])
  // A gain is clamped to the export's own 0..4 range, and a missing value is unity.
  assert.deepEqual(monitorGains([9], [], [], 2), [4, 1])
  assert.deepEqual(monitorGains([1, 1, 1], [], [false, true, false], 3, true), [1, 1, 0])
})

test('the export carries mutes and levels and never a solo', () => {
  assert.deepEqual(exportGains([1, 0.5, 2], [true, false, false], 3), [0, 0.5, 2])
  assert.deepEqual(exportGains([1, 0.5, 2], [], 3), [1, 0.5, 2], 'nothing changes without a mute')
  // A derived mix stays at unity so the export rebuilds it from the edited sources; a source's mute stays.
  assert.deepEqual(exportGains([0.2, 1, 1], [false, true, false], 3, true), [1, 0, 1])
})

test('the overall level reaches the preview and the export exactly once, never the rebuilt mix twice', () => {
  // The engine multiplies a rebuilt mix's sources by track 1's gain (media_export.cpp), so track 1 must
  // stay at unity while the sources carry the overall level: once in the mix, once in each stem.
  assert.deepEqual(exportGains([1, 1, 1], [false, false, false], 3, true, 0.5), [1, 0.5, 0.5])
  assert.deepEqual(exportGains([1, 0.5, 2], [false, true, false], 3, false, 0.5), [0.5, 0, 1])
  // A derived mix's own row carries it for the fallback that plays before the copies are ready, and the
  // sources carry it once more when they take over.
  assert.deepEqual(monitorGains([1, 1, 1], [false, false, false], [false, false, false], 3, true, 0.5), [0.5, 0.5, 0.5])
  assert.deepEqual(monitorGains([1, 0.5, 2], [false, true, false], [false, false, false], 3, false, 0.5), [0.5, 0, 1])
  // A mute and a solo still win, and the combined level is clamped with the per-track level.
  assert.deepEqual(monitorGains([1, 1, 1], [false, true, false], [false, false, true], 3, false, 0.5), [0, 0, 0.5])
  assert.deepEqual(exportGains([4, 4], [], 2, false, 4), [4, 4])
  // The original mix, heard as the fallback while the copies load or after one failed, carries the level.
  assert.equal(elementGain('isolated', [0.5, 1], 'loading'), 0.5)
  assert.equal(elementGain('isolated', [0.5, 1], 'failed'), 0.5)
  assert.equal(elementGain('isolated', [0.5, 1], 'ready'), 0)
  // The overall level is a saved edit; unity is not.
  assert.equal(audioIsEdited(buffrx, [1, 1, 1, 1], [false, false, false, false], 1), false)
  assert.equal(audioIsEdited(buffrx, [1, 1, 1, 1], [false, false, false, false], 0.5), true)
})

test('a track is named by its sidecar title without the product prefix, else by its role', () => {
  assert.equal(trackName(buffrx[1], 1), 'Desktop')
  assert.equal(trackName(buffrx[0], 0), 'Combined')
  assert.equal(trackName(buffrx[3], 3), 'Discord')
  assert.equal(trackName({ ...buffrx[1], title: '' }, 1), 'Desktop')
  assert.equal(trackName({ ...buffrx[2], title: '' }, 2), 'Microphone')
  // A known combined role with only the writer's ordinal placeholder is the combined output.
  assert.equal(trackName(track(3, 'combined', 'BuffrX Track 3', []), 2), 'Combined')
  assert.equal(trackName(track(3, 'combined', '', []), 2), 'Combined')
  // A real title on a combined track wins; an unknown stream keeps its fallback and is never called a mix.
  assert.equal(trackName(track(3, 'combined', 'BuffrX Game', []), 2), 'Game')
  assert.equal(trackName(track(1, 'track', '', []), 0), 'Track 1')
  assert.equal(trackName(undefined, 2), 'Track 3')
})

test('the mixer hides a derived mix and empty roles but every shown row keeps its file ordinal', () => {
  const rows = audioRows(buffrx, audioLayout(buffrx))
  assert.deepEqual(rows.map((row) => [row.kind, row.index]), [['derived', 0], ['source', 1], ['source', 2], ['source', 3]])
  assert.deepEqual(audibleRows(buffrx, audioLayout(buffrx)).map((row) => row.index), [1, 2, 3])

  const withEmpty = [...buffrx, track(5, 'empty', 'BuffrX Empty', [])]
  assert.deepEqual(audioRows(withEmpty, audioLayout(withEmpty)).map((row) => row.index), [0, 1, 2, 3])

  // A file whose mix cannot be rebuilt locks every row: the engine refuses a gain change on any of its
  // tracks, the mix included, so no control is offered that the save would reject.
  const incomplete = [buffrx[0], buffrx[1], buffrx[2]]
  const fixed = audioRows(incomplete, audioLayout(incomplete))
  assert.deepEqual(fixed.map((row) => [row.kind, row.index]), [['locked', 0], ['locked', 1], ['locked', 2]])
  assert.deepEqual(audibleRows(incomplete, audioLayout(incomplete)).map((row) => row.index), [0])
})

test('an incomplete aggregate locks every level and mute, and says the way out', () => {
  const incomplete = [buffrx[0], buffrx[1], buffrx[2]]
  const layout = audioLayout(incomplete)
  assert.equal(trackIsLocked(layout), true)
  const reason = mixLockReason(layout)
  assert.match(reason, /separate tracks/)
  assert.match(reason, /saving still work/)
  // Untouched controls send unity, so playing and saving keep working: the lock is not an error state.
  assert.deepEqual(exportGains([1, 1, 1], [false, false, false], 3), [1, 1, 1])
  // A complete layout keeps its editable source rows and only the mix is derived.
  assert.equal(trackIsLocked(audioLayout(buffrx)), false)
  assert.equal(mixLockReason(audioLayout(buffrx)), '')
})

test('an imported file has no hidden mix and one track is one row', () => {
  const imported = [1, 2, 3].map((i) => track(i, 'track', '', []))
  assert.deepEqual(audioRows(imported, audioLayout(imported)).map((row) => row.index), [0, 1, 2])
  assert.deepEqual(audibleRows(imported, audioLayout(imported)).map((row) => row.index), [0, 1, 2])
  const single = [track(1, 'desktop', 'BuffrX Desktop', ['desktop'])]
  assert.deepEqual(audioRows(single, audioLayout(single)).map((row) => [row.kind, row.index]), [['source', 0]])
  assert.deepEqual(audibleRows(single, audioLayout(single)).map((row) => row.index), [0])
})

test('the combined lane follows the heard rows and never adds a mix to its stems', () => {
  const peaks = [[0.9, 0.9], [0.2, 0.1], [0.3, 0.2], [0.1, 0.05]]
  const layout = audioLayout(buffrx)
  const indexes = audibleRows(buffrx, layout).map((row) => row.index)
  assert.deepEqual(indexes, [1, 2, 3], 'the derived mix is not one of the heard rows')
  // Unity gains: the combined waveform is the three sources added, not the mix as well.
  const heard = heardPeaks(peaks, [1, 1, 1, 1], indexes)
  assert.deepEqual(rounded(heard), [0.6, 0.35])
  assert.notDeepEqual(rounded(heard), [1.5, 1.25], 'the mix track was not counted a second time')
  // A mute is silence in the preview, so it is absent from the combined waveform too.
  assert.deepEqual(rounded(heardPeaks(peaks, [1, 0, 1, 1], indexes)), [0.4, 0.25])
  // Hearing one row alone leaves only that row, at its own level.
  assert.deepEqual(rounded(heardPeaks(peaks, [1, 0, 1.5, 0], indexes)), [0.45, 0.3])
})

test('the combined lane keeps file ordinals, and a locked mix is drawn from the mix alone', () => {
  const peaks = [[0.8, 0.4], [0.2, 0.1], [0.3, 0.2]]
  const incomplete = [buffrx[0], buffrx[1], buffrx[2]]
  const indexes = audibleRows(incomplete, audioLayout(incomplete)).map((row) => row.index)
  assert.deepEqual(indexes, [0])
  // The mix is the file's first stream; its own peaks are drawn, not a sum of nearby lanes.
  assert.deepEqual(rounded(heardPeaks(peaks, [0.5, 1, 1], indexes)), [0.4, 0.2])

  const imported = [1, 2, 3].map((i) => track(i, 'track', '', []))
  const defaults = audioDefaults(imported)
  const unknownIndexes = audibleRows(imported, audioLayout(imported)).map((row) => row.index)
  assert.deepEqual(unknownIndexes, [0, 1, 2], 'an import keeps every useful track available')
  // Its preview-only solo makes the combined lane track 1 alone.
  assert.deepEqual(rounded(heardPeaks(peaks, monitorGains(defaults.volumes, defaults.muted, defaults.solo, imported.length), unknownIndexes)), [0.8, 0.4])
})

test('a combined lane says it is still reading until a row has peaks', () => {
  assert.equal(heardPeaks([], [1], [0]), undefined)
  assert.equal(heardPeaks([undefined, undefined], [1, 1], [0, 1]), undefined)
  // One readable row is enough to draw: an unread row does not blank the lane.
  assert.deepEqual(heardPeaks([undefined, [0.4]], [1, 1], [0, 1]), [0.4])
})

test('opening the fold shows each source without touching its level, mute or solo', () => {
  const volumes = [1, 0.25, 2, 1]
  const muted = [false, true, false, false]
  const solo = [false, false, true, false]
  const before = { volumes: [...volumes], muted: [...muted], solo: [...solo] }
  const layout = audioLayout(buffrx)
  const collapsed = monitorGains(volumes, muted, solo, buffrx.length, mixIsDerived(layout), 0.5)
  // The expanded lane reads one row per source; it is a view of the same stored arrays, never a rewrite.
  const rows = audioRows(buffrx, layout).filter((row) => row.kind === 'source')
  assert.deepEqual(rows.map((row) => row.index), [1, 2, 3])
  assert.deepEqual({ volumes, muted, solo }, before, 'the stored source state is untouched')
  assert.deepEqual(monitorGains(volumes, muted, solo, buffrx.length, true, 0.5), collapsed, 'the sound is the same folded or open')
  assert.deepEqual(exportGains(volumes, muted, buffrx.length, true, 0.5), [1, 0, 1, 0.5], 'the export keeps the source balance')
})

test('a file with nothing but empty tracks shows no controls', () => {
  const empty = [1, 2].map((i) => track(i, 'empty', '', []))
  assert.deepEqual(audioRows(empty, audioLayout(empty)), [])
  assert.deepEqual(audibleRows(empty, audioLayout(empty)), [])
})

test('reset audio puts every track back to unity, unmuted and unsoloed', () => {
  assert.deepEqual(audioDefaults(buffrx), {
    volumes: [1, 1, 1, 1],
    muted: [false, false, false, false],
    solo: [false, false, false, false],
    master: 1,
  })
})

test('one wheel notch moves a level five points, and the end of the fader spends the notch', () => {
  // Wheel up (a negative delta) raises the level by one notch.
  assert.deepEqual(wheelLevel(100, 0, -100), { value: 105, carried: 0 })
  assert.deepEqual(wheelLevel(100, 0, 100), { value: 95, carried: 0 })
  // Lines and pages read as notches too, so a browser that reports them still answers one notch at a time.
  assert.equal(wheelLevel(100, 0, -3, 1).value, 105)
  assert.equal(wheelLevel(100, 0, -1, 2).value, 105)
  // At either end the value stays put, and the notch is not banked: the first notch back answers at once.
  assert.deepEqual(wheelLevel(200, 0, -100), { value: 200, carried: 0 })
  assert.deepEqual(wheelLevel(200, 0, -100), { value: 200, carried: 0 })
  assert.equal(wheelLevel(200, 0, 100).value, 195)
  assert.equal(wheelLevel(0, 0, 100).value, 0)
  assert.equal(wheelLevel(0, 0, -100).value, 5)
  // The limits are the fader's own.
  assert.equal(wheelLevel(198, 0, -100).value, 200)
  assert.equal(wheelLevel(3, 0, 100).value, 0)
})

test('a trackpad’s small wheel deltas add up to a notch instead of moving the level by jitter', () => {
  let state = { value: 100, carried: 0 }
  for (let i = 0; i < 24; i++) {
    state = wheelLevel(state.value, state.carried, -4)
    assert.equal(state.value, 100, 'below a notch the level is untouched, so nothing is written')
  }
  state = wheelLevel(state.value, state.carried, -4)
  assert.equal(state.value, 105, 'the twenty-fifth pixel of a notch moves the level once')
  assert.deepEqual(state, { value: 105, carried: 0 })
  // A fling is bounded: one event never moves more than two notches, and an empty delta moves nothing.
  assert.equal(wheelLevel(100, 0, -1000).value, 110)
  // A mouse's own notch size (about 120 px, as Chrome reports one) is one step, not one plus a remainder.
  assert.deepEqual(wheelLevel(100, 0, -120), { value: 105, carried: 0 })
  assert.deepEqual(wheelLevel(105, 0, 120), { value: 100, carried: 0 })
  assert.deepEqual(wheelLevel(100, 0, 0), { value: 100, carried: 0 })
})

test('a copied track is ready only when it can play, an error fails it, and listeners are removed', async () => {
  const source = new FakeSource()
  const waiting = whenPlayable(source, 200)
  source.emit('canplay')
  await waiting
  assert.equal(source.count('canplay'), 0, 'the canplay listener was removed')
  assert.equal(source.count('error'), 0, 'the error listener was removed')

  const broken = new FakeSource()
  broken.error = { message: 'unsupported codec' }
  const failing = whenPlayable(broken, 200)
  broken.emit('error')
  await assert.rejects(failing, /unsupported codec/)
  assert.equal(broken.count('canplay'), 0)

  const buffered = new FakeSource()
  buffered.readyState = 3
  await whenPlayable(buffered, 200)
  assert.equal(buffered.count('canplay'), 0, 'a buffered copy resolves without leaving listeners')
})

test('a copy that never becomes playable fails within the bound instead of counting as ready', async () => {
  const source = new FakeSource()
  const started = Date.now()
  await assert.rejects(whenPlayable(source, 25), /did not become playable in time/)
  assert.ok(Date.now() - started < 1000, 'the wait is bounded, not open-ended')
  assert.equal(source.count('canplay'), 0, 'the timed-out wait leaves no listener behind')
})

test('a readiness wait can be cancelled and takes its listeners and timer with it', async () => {
  const source = new FakeSource()
  const controller = new AbortController()
  // The long bound is the check: a timer left running would hold the test process open for 5 s.
  const waiting = whenPlayable(source, 5000, controller.signal)
  controller.abort()
  await assert.rejects(waiting, /cancelled/)
  assert.equal(source.count('canplay'), 0, 'the cancelled wait removed its canplay listener')
  assert.equal(source.count('error'), 0, 'the cancelled wait removed its error listener')
  // A late event reaches nothing: the wait already ended.
  source.emit('canplay')

  const buffered = new FakeSource()
  buffered.readyState = 3
  const gone = new AbortController()
  gone.abort()
  await assert.rejects(whenPlayable(buffered, 5000, gone.signal), /cancelled/)
  assert.equal(buffered.count('canplay'), 0, 'an already-cancelled wait adds no listener')
})

/** The readiness wait's own surface, without a browser: events are delivered by hand. */
class FakeSource implements PlayableSource {
  readyState = 0
  error: { message?: string } | null = null
  private listeners = new Map<string, Set<() => void>>()

  addEventListener(type: 'canplay' | 'error', listener: () => void) {
    const set = this.listeners.get(type) ?? new Set()
    set.add(listener)
    this.listeners.set(type, set)
  }

  removeEventListener(type: 'canplay' | 'error', listener: () => void) {
    this.listeners.get(type)?.delete(listener)
  }

  count(type: 'canplay' | 'error') {
    return this.listeners.get(type)?.size ?? 0
  }

  emit(type: 'canplay' | 'error') {
    for (const listener of [...(this.listeners.get(type) ?? [])]) listener()
  }
}
