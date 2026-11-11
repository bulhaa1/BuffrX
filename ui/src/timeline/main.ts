// The Timeline preview's page (buffrx-ui.exe --timeline <session>): the footage the engine keeps on disk,
// in a window over the game. Plain DOM on purpose - no React and no editor bundle - so it opens the
// moment Alt+4 is pressed. The panel on top holds the session, the ruler, the bar with its chunks, gaps
// and markers, the clip track, the overview of a zoomed bar and the transport; the picture sits under it.
// No import of @tauri-apps/api, for the reason viewer.ts gives: with several entry pages the bundler puts
// the shared helper in the editor's chunk, which would drag in the whole editor. Tauri's protocol is
// two calls, below.
import './timeline.css'
import { fromEngine } from '../lib/convert'
import { themeId } from '../lib/themes'
import {
  clampView,
  CLIP_DEFAULT_MS,
  clipAround,
  clipLabel,
  clipSaveDisposition,
  clock,
  clipLoopTarget,
  viewHoldingClip,
  defaultView,
  dragMoved,
  escapeAction,
  followView,
  footageAge,
  rulerAge,
  rulerMinors,
  glideView,
  isRecorded,
  isZoomed,
  loudColumns,
  nextWindow,
  pickSession,
  loudHighlights,
  loudRise,
  loudScale,
  markEdge,
  moveSelection,
  frozenBarEnd,
  gapLabel,
  gapSuccessor,
  seekableMoment,
  retentionNotice,
  rulerStep,
  saveRefusal,
  scrubBy,
  selectionClamp,
  selectionOnSpan,
  snapMs,
  timelineReadout,
  transportState,
  trimRange,
  viewFor,
  wheelSeconds,
  windowLabel,
  windowOf,
  WINDOW_DEFAULT_MS,
  zoomView,
  type LoudChunk,
  type Selection,
  type TimelineChunk,
  type TimelineMarker,
  type TimelineSession,
  type TimelineSessionSummary,
  type View,
} from '../lib/timeline/model'
import { nextSession } from '../lib/timeline/session-follow'
import { GAP_MS } from '../lib/timeline/window'
import { TimelinePlayer } from './player'
import { followLiveView, LiveViewClock } from './live-view'
import { mediaUrl } from './url'

interface TauriInternals {
  invoke(command: string, args?: Record<string, unknown>): Promise<unknown>
  transformCallback(callback: (event: { payload: string }) => void): number
}

// The page speaks to Tauri directly, so the dev server cannot drive it in a browser: there is no bridge
// to intercept. This is the one command it needs, and only when that server says it is serving.
const browser = window as unknown as { __TAURI_INTERNALS__?: unknown; __buffrxDevBridge?: boolean }
if (!browser.__TAURI_INTERNALS__ && browser.__buffrxDevBridge) {
  browser.__TAURI_INTERNALS__ = {
    transformCallback: () => 0,
    invoke: async (command: string, args: Record<string, unknown> = {}) => {
      if (command === 'engine_request') {
        const response = await fetch('/__engine/request', { method: 'POST', body: String(args.frame ?? '') })
        return await response.text()
      }
      if (command === 'plugin:event|listen') return 0
      // A browser has no launch line: ?session=<id> stands in for `--timeline <id>`.
      if (command === 'get_launch') return { session: new URLSearchParams(location.search).get('session') ?? '' }
      throw new Error(`${command} is not available in the browser`)
    },
  }
}

const internals = (window as unknown as { __TAURI_INTERNALS__: TauriInternals }).__TAURI_INTERNALS__
const invoke = <T = unknown>(command: string, args: Record<string, unknown> = {}) => internals.invoke(command, args) as Promise<T>
async function listen(event: string, handler: (event: { payload: string }) => void): Promise<void> {
  await invoke('plugin:event|listen', { event, target: { kind: 'Any' }, handler: internals.transformCallback(handler) })
}

/** One `timeline.get` reply, in the page's own spelling. */
interface TimelineGet {
  enabled: boolean
  state: string
  minutes: number
  /** Absent when the page asked with `list: false`: it keeps the list it has. */
  sessions?: TimelineSessionSummary[]
  session: string
  availableStartMs: number
  availableEndMs: number
  bytes: number
  chunks: TimelineChunk[]
  chunkSeconds?: number
  currentSession?: string
  /** The chunk the writer holds right now; its flushed bytes are readable before it is published. */
  live?: { file: string; startMs: number; bytes: number }
}

/** One `session.get` reply: the markers are on the index's tick clock, `startedAt` moves them onto it. */
interface SessionGet {
  startedMs: number
  markers: { atMs: number; kind: string; label: string }[]
}

let sequence = 0
/** One engine command: its data, or why the engine said no. Nothing here throws at the page. */
async function attempt<T>(cmd: string, args: Record<string, unknown> = {}): Promise<{ data?: T; error?: string }> {
  try {
    const reply = await invoke<string>('engine_request', { frame: JSON.stringify({ id: sequence++, cmd, args }) })
    const frame = JSON.parse(reply) as { ok?: boolean; data?: unknown; error?: string }
    if (frame.ok === false) {
      const error = frame.error ?? 'no reason given'
      note(`${cmd} refused: ${error}`)
      return { error }
    }
    // The engine's snake_case becomes the page's camelCase once, here (AGENTS.md: translate in the bridge).
    return { data: fromEngine<T>(frame.data) }
  } catch (error) {
    note(`${cmd} failed: ${String(error)}`)
    return { error: String(error) }
  }
}
/** One engine command, its data or nothing: the page still closes when the engine is gone. */
async function engine<T>(cmd: string, args: Record<string, unknown> = {}): Promise<T | undefined> {
  return (await attempt<T>(cmd, args)).data
}

/** Where a probe reads how far the page got: logs/ui.log, through the shell. */
function note(message: string) {
  void invoke('timeline_log', { message }).catch(() => {})
}

/** The clip's name as the library words it: the file without its folders or extension. */
const clipName = (path: string) => (path.split(/[\\/]/).pop() ?? path).replace(/\.[^.]+$/, '')

/** A per-viewer convenience (volume, mute): may be unavailable, and the page works without it. */
const remembered = (key: string): string | null => {
  try {
    return localStorage.getItem(key)
  } catch {
    return null
  }
}
const remember = (key: string, value: string) => {
  try {
    localStorage.setItem(key, value)
  } catch {
    /* a private or locked store: the setting lasts this window only */
  }
}

const PLAY = 'M8 5v14l11-7L8 5Z'
const PAUSE = 'M8 5h3v14H8zM13 5h3v14h-3z'
const WAITING = 'M12 2a10 10 0 1 0 0 20 10 10 0 0 0 0-20Zm1 5h-2v6l5 3 1-1.7-4-2.3V7Z'

/** What each marker kind is called on its pin. */
const MARKER_NAMES: Record<string, string> = {
  clip: 'Clip saved',
  screenshot: 'Screenshot',
  recording: 'Recording',
  profile: 'Profile',
  game: 'Game',
  marker: 'Marker',
  gap: 'Gap',
}

/** The lease a preview holds on the footage it shows, renewed while the window is open. */
const PIN_OWNER = 'timeline-preview'
// Live reads half-second fragments; ordinary playback refreshes the session less often.
const LIVE_POLL_MS = 500
const POLL_MS = 2000
/** The full session list and markers are refreshed independently of the Live poll rate. */
const LIST_INTERVAL_MS = 10000
/** Where the window opens: this far behind the newest saved footage, the moment that just happened. */
const OPEN_BEHIND_MS = 1000
// The stored theme, read once after the page is up; until it answers (or if it never does) the page
// wears the main theme. Same ids as themes.css, whose blocks are keyed by data-theme.
async function applyTheme(): Promise<void> {
  const config = await engine<{ theme?: string }>('config.get')
  document.documentElement.dataset.theme = themeId(config?.theme)
}

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T

async function main() {
  note('page loaded')
  void applyTheme()
  window.addEventListener('error', (event) => note(`error: ${event.message}`))
  window.addEventListener('unhandledrejection', (event) => note(`rejected: ${String(event.reason)}`))
  document.addEventListener('contextmenu', (event) => event.preventDefault())

  const panel = $('timeline-panel')
  // One arrival, started as soon as the page can paint: the shell shows the window at once (it is invisible, alpha 0) so the page
  // has frames to paint with, and when two of them are out the arrival begins and the shell fades the whole window in
  // (timeline_fade: the game and the screen behind it dim and blur in together with the panel dropping and the picture rising).
  // Nothing waits for the copy of the screen: it arrives under the fade.
  const startArrival = () => {
    panel.classList.remove('settled')
    document.body.classList.remove('arrival-started')  // the panel and the picture wait clear until the arrival begins
    requestAnimationFrame(() => {
      panel.classList.add('arriving')
      document.body.classList.add('arriving', 'arrival-started')
      backdrop.classList.add('on')
      void invoke('timeline_fade').catch(() => {})
      // Reduced motion plays no animation, so no animationend: the frosting still comes on.
      window.setTimeout(() => panel.classList.add('settled'), 600)
    })
  }
  panel.addEventListener('animationend', (event) => {
    // The frosted blur is switched on once the panel has stopped moving: a live blur under a moving layer is what made it stutter.
    if (event.target === panel) panel.classList.add('settled')
  })
  let readyAt = performance.now()
  void invoke('timeline_ready').catch(() => {}).then(() => { readyAt = performance.now(); startArrival() })
  const video = $<HTMLVideoElement>('v')
  const still = $<HTMLImageElement>('still')
  const fail = $('fail')
  const failText = $('failtext')
  const retryButton = $<HTMLButtonElement>('retry')
  const stage = $('stage')
  const frame = $('frame')
  const range = $('range')
  const strip = $('strip')
  const bar = $('bar')
  const cells = $('cells')
  const gaps = $('gaps')
  const fill = $('fill')
  const head = $('head')
  const markers = $('markers')
  const ruler = $('ruler')
  const rulerTicks = $('ruler-ticks')
  const cut = $('cut')
  const overview = $('overview')
  const ovCells = $('ovcells')
  const ovGaps = $('ovgaps')
  const ovView = $('ovview')
  const trimRead = $('trimread')
  const clipGuide = $('clip-guide')
  const clipDetails = $('clip-details')
  const clipLength = $('clip-length')
  const clipRange = $('clip-range')
  const bubble = $('bubble')
  const bubbleImg = $<HTMLImageElement>('bubbleimg')
  const bubbleTime = $('bubbletime')
  const stateText = $('state')
  const spanText = $('span')
  const clockText = $('current-clock')
  // 3.7: the clock is written only when the minute changes (the one-second tick is then a comparison, not a repaint).
  const paintClock = () => {
    const text = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })
    if (clockText.textContent !== text) clockText.textContent = text
  }
  paintClock()
  const clockTimer = window.setInterval(paintClock, 1000)
  let rulerTimer = 0
  const timeText = $('time')
  const hereText = $('here')
  const playButton = $<HTMLButtonElement>('play')
  const muteButton = $<HTMLButtonElement>('mute')
  const volumeInput = $<HTMLInputElement>('volume')
  const liveButton = $<HTMLButtonElement>('live')
  const clipButton = $<HTMLButtonElement>('clip')
  const previewClipButton = $<HTMLButtonElement>('previewclip')
  const clipBar = $('clipbar')
  const saveButton = $<HTMLButtonElement>('saveclip')
  const menuButton = $<HTMLButtonElement>('savemenu')
  const menu = $('menu')
  const hideButton = $<HTMLButtonElement>('hidevideo')
  const emptyState = $('empty')

  // The blurred screen behind the window: the shell hands over a small copy of it (it never contains this window), asked
  // for again every three seconds while the page is up (a full-screen read, a 300 KB picture over the bridge and a blur each time:
  // the picture behind is a mood, not a live view, and the game is what is running). Where there is no such command (a browser) the ground colour stays.
  const backdrop = $('backdrop')
  let backdropBusy = false
  let lastBackdrop = ''
  let backdropFailures = 0
  const refreshBackdrop = async () => {
    // Not while hidden, except the first copy: the window is shown only after it, so the page is hidden until then.
    if (backdropBusy || (document.hidden && lastBackdrop)) return
    backdropBusy = true
    try {
      const url = await invoke<string | null>('timeline_backdrop')
      backdropFailures = 0
      if (url && url !== lastBackdrop) {
        lastBackdrop = url
        backdrop.style.backgroundImage = `url(${url})`
      }
    } catch {
      // A shell that cannot take a screen grab (a browser, a gone window) still leaves the ground colour;
      // one transient failure is retried on the next beat instead of ending the backdrop for good.
      if (++backdropFailures >= 5) clearInterval(backdropTimer)
    } finally {
      backdropBusy = false
    }
  }
  // 3.7: the backdrop timer stops while the page is hidden (it used to wake and return) and starts again when it is shown.
  let backdropTimer = window.setInterval(() => void refreshBackdrop(), 3000)
  void refreshBackdrop()
  // The window can be hidden (save and back to game) and shown again by the same page: the arrival plays again then.
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
      clearInterval(backdropTimer)
      return
    }
    backdropTimer = window.setInterval(() => void refreshBackdrop(), 3000)
    void refreshBackdrop()
    // The first show is the one the arrival above already started: it is not played a second time.
    if (performance.now() - readyAt < 800) return
    panel.classList.remove('arriving')
    document.body.classList.remove('arriving')
    backdrop.classList.remove('on')
    void panel.offsetWidth // a reflow, so the animation starts over instead of finding itself finished
    startArrival()
  })

  // Live: the newest footage, played as the writer flushes it. Any other movement leaves it, so the
  // control never claims to be live while the playhead sits behind.
  let liveMode = false
  let livePaused = false
  let writerState = ''
  let liveMissingSince = 0
  let currentSession = ''
  /** The owner opened the stored session on screen by its id: it stays when a new one starts. A session the page took
   *  from the writer is never that, and gives way when the writer starts the next one. */
  let shownChosen = false
  /** The new session already announced to a reviewing owner, so the pill is said once. */
  let announcedSession = ''
  /** The session a buffer stop closed, waiting for its index summary to say it really ended. */
  let endedCheckId = ''
  let pendingLive = false
  let transportIntent = 0
  let libraryOpen = false
  let disposeLibrary: (() => void) | undefined
  let disposeShare: (() => Promise<void>) | undefined
  let shareGeneration = 0
  let retentionMinutes = 0
  const liveViewClock = new LiveViewClock()
  let displayedLiveEdge: number | undefined
  const setLive = (on: boolean) => {
    // LIVE asks for the newest footage: the review view is given back, or the bar would stay behind it.
    if (on) releaseView()
    liveMode = on
    if (on) livePaused = false
    liveViewClock.reset()
    displayedLiveEdge = undefined
    liveButton.setAttribute('aria-pressed', String(on))
    paintTime()
  }
  // `cancelIntent` is false when the page itself moves to another session: the caller's transport
  // intent must survive that move, or a LIVE press would be cancelled by its own session switch.
  const leaveLive = (cancelIntent = true) => {
    if (cancelIntent) transportIntent += 1
    pendingLive = false
    livePaused = false
    if (liveMode) {
      player.stopLive()
      setLive(false)
      if (chip.textContent === 'BUFFERING') say('')
      paintAll()
    }
  }
  // Review remains in the selected session until Live is pressed.

  /** Why nothing can be played, and whether trying again could help (`Retry` is drawn only then). */
  const showFail = (message: string, retryable = false) => {
    failText.textContent = message
    retryButton.hidden = !retryable
    fail.classList.add('on')
    if (chip.textContent === 'BUFFERING') say('')
    else refreshPlaybackStatus()
  }
  const hideFail = () => fail.classList.remove('on')

  /**
   * Waiting for footage is a state of the preview, not a failure of it: the empty card carries the
   * headline and the writer's own reason under it, and no fail card is drawn. Off, waiting for a game,
   * stopped, paused and waiting for the first chunk each say which they are.
   */
  const showWaiting = (reason = '') => {
    hideFail()
    stage.classList.add('empty')
    emptyState.hidden = false
    refreshPlaybackStatus()
    const off = reason.startsWith('Timeline is off')
    const game = reason.startsWith('Waiting for a game')
    const stopped = reason.startsWith('Timeline is stopped')
    const paused = reason.startsWith('Timeline is paused')
    $('emptyline').textContent = off
      ? 'Timeline is off.'
      : game
        ? 'Waiting for a game.'
        : stopped
          ? 'Timeline is stopped.'
          : paused
            ? 'Timeline is paused.'
            : 'Waiting for the first chunk.'
    $('emptysub').textContent = reason || 'The buffer fills while it runs. Edited footage stays in the panel.'
  }

  // The frame the video element blanks over, kept sharp: a seek shows nothing for a frame or two and the
  // theme ground would flash through. Repainted when a frame is about to go and when a new one lands.
  const hold = $<HTMLCanvasElement>('hold')
  const holdCtx = hold.getContext('2d')
  let holdValid = false
  let holdSeq = 0
  const paintHold = () => {
    if (!holdCtx || !video.videoWidth || video.readyState < 2) return
    hold.width = video.videoWidth
    hold.height = video.videoHeight
    holdCtx.drawImage(video, 0, 0)
    holdValid = true
  }
  // Kept until a frame is actually presented: `loadeddata` and `seeked` both fire while the element still
  // has nothing on screen, and hiding on either let the empty screen flash through.
  const holdFrame = () => {
    paintHold()
    if (!holdValid) return
    hold.hidden = false
    const seq = ++holdSeq
    const done = () => {
      if (seq === holdSeq) hold.hidden = true
    }
    if (typeof video.requestVideoFrameCallback === 'function') video.requestVideoFrameCallback(done)
    else video.addEventListener('seeked', () => requestAnimationFrame(done), { once: true })
  }
  video.addEventListener('seeking', holdFrame)
  video.addEventListener('emptied', holdFrame)
  video.addEventListener('timeupdate', paintHold)

  // I5: a word that changes (LIVE to REPLAY, a new state) fades in over the fast token instead of switching. Reduced motion skips it.
  const swapFade = (el: HTMLElement) => {
    if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) return
    el.animate([{ opacity: 0.25 }, { opacity: 1 }], { duration: 90, easing: 'cubic-bezier(0.22, 0.9, 0.3, 1)' })
  }
  // One status line over the picture: buffering, a gap, a save in progress, why a save was refused.
  const chip = document.createElement('span')
  chip.className = 'chip playback'
  stage.append(chip)
  let chipTimer = 0
  /**
   * One line, one tone: `problem` stays until something else is said, `new-session` holds a session
   * that started under a review until LIVE takes it back, and `ended` marks the session a buffer
   * stop closed. A gap playback steps over says nothing: its band already says why the hole is there.
   */
  const say = (text: string, kind: 'state' | 'problem' | 'new-session' | 'ended' = 'state') => {
    window.clearTimeout(chipTimer)
    if (!text) {
      chip.className = 'chip playback'
      refreshPlaybackStatus()
      return
    }
    if (chip.textContent !== text) swapFade(chip)
    chip.textContent = text
    chip.className = `chip on ${kind}`
    if (text && kind === 'state' && text !== 'BUFFERING') chipTimer = window.setTimeout(() => say(''), 4000)
  }
  function refreshPlaybackStatus() {
    if (!chip.classList.contains('playback')) return
    if (!session || stage.classList.contains('empty') || fail.classList.contains('on')) {
      chip.className = 'chip playback'
      return
    }
    const live = liveMode && writerState === 'writing' && player.isLive() && !video.paused && !livePaused && !playerBuffering
    const text = live ? 'LIVE' : `REPLAY${video.paused ? ' · PAUSED' : ''} · ${ageAt(wantMs)}`
    if (chip.textContent !== text) {
      // The age ticks every second and is not a change of state: only the word before it (LIVE, REPLAY, PAUSED) fades.
      if (chip.textContent?.split(' · ')[0] !== text.split(' · ')[0] || chip.textContent?.includes('PAUSED') !== text.includes('PAUSED')) swapFade(chip)
      chip.textContent = text
    }
    chip.className = 'chip on playback'
  }
  /** The new-session pill holds until LIVE or another session: dropping the announcement drops the pill. */
  const dropAnnouncement = () => {
    if (!announcedSession) return
    announcedSession = ''
    if (chip.classList.contains('new-session')) say('')
  }

  const selectionBand = document.createElement('span')
  selectionBand.id = 'sel'
  const barSelection = document.createElement('span')
  barSelection.id = 'bar-selection'
  bar.insertBefore(barSelection, strip)
  const inEdge = document.createElement('span')
  inEdge.className = 'edge'
  inEdge.title = inEdge.ariaLabel = 'Clip start'
  const outEdge = document.createElement('span')
  outEdge.className = 'edge'
  outEdge.title = outEdge.ariaLabel = 'Clip end'
  cut.append(selectionBand, inEdge, outEdge)

  // The card that names a saved clip, with what can be done with it next.
  const card = document.createElement('div')
  card.id = 'card'
  const cardName = document.createElement('span')
  cardName.id = 'cardname'
  const cardButton = (text: string, title: string, action: () => void) => {
    const button = document.createElement('button')
    button.textContent = text
    button.title = button.ariaLabel = title
    button.addEventListener('click', action)
    return button
  }
  card.append(
    cardName,
    cardButton('Share', 'Share this clip (Shift+S)', () => void after('share', true)),
  )
  frame.append(card)

  let session: TimelineSession | undefined
  /** The clip being made; `undefined` is no clip (the Clip button is up, Save is not offered). */
  let selection: Selection | undefined
  let previewingClip = false
  let markersList: TimelineMarker[] = []
  let selectedMarker: TimelineMarker | undefined
  /** The clip saved last and the range it came from, so Edit/Share/Copy after a save never save twice. */
  let saved: { file: string; inMs: number; outMs: number } | undefined
  let saving = false
  /** The moment the user is looking at: the last seek asked for, so a burst of wheel notches adds up. */
  let wantMs = 0
  /** Bumped by every session load, so a slow reply for the previous one is dropped. */
  let generation = 0
  /** Changes only when a different session view is installed, including a reload of the same session. */
  let sessionRevision = 0
  let closing = false
  /** The sessions a reply last named, so a vanished one can fall back without another command. */
  let knownSessions: TimelineSessionSummary[] = []
  /** A stretch of the bar the owner moved or zoomed to; `undefined` is the default window, which follows the newest footage. */
  let zoom: View | undefined
  /** How much footage the default window holds (Infinity is the whole session); the window button and +/- change it. */
  let windowMs = WINDOW_DEFAULT_MS
  /** The owner scrolled the bar back while Live plays: the picture's own follow must not pull the bar forward again. */
  let scrolledAway = false
  /** The review view, kept as it stood when the owner left live: new footage must not move it. */
  let frozenView: View | undefined
  /** The edge a clip was opened against (see liveEdge). */
  let clipEdge = 0
  /** Whether clip mode zoomed the bar, and the view it should restore. */
  let clipZoomed = false
  let clipPreviousZoom: View | undefined
  let pollTimer = 0
  let pinTimer = 0
  let playbackFrame = 0
  let playbackWaiting = false
  /** The player is buffering a new window, separate from the element's own waiting event. */
  let playerBuffering = false
  let lastListAt = 0
  let polling = false
  let pollAgain = false

  // The player: one MediaSource over a bounded window, seeks inside it with `currentTime` and
  // rebuilds around a moment outside it. Its own logs land in the shell's log beside the page's, so
  // a probe can read where a seek landed.
  /** Whether the player's last append failed and the chip is carrying that word. */
  let appendProblem = false
  const player = new TimelinePlayer(video, {
    onState: (state, detail) => {
      // The first frame of a rebuilt window is the seek landing: the needle may follow the picture again.
      if (state === 'ready' && seekInFlight) settleSeek(seekSerial)
      if (state === 'ready' || state === 'buffering') {
        playerBuffering = state === 'buffering'
        syncPlay()
        hideFail()
        // A chunk that could not be added is said once and does not stop the preview: the next
        // buffering or ready clears the word, and the view and the clip stay where they are.
        if (appendProblem) {
          appendProblem = false
          if (state === 'ready' && chip.textContent !== 'BUFFERING') say('')
        }
        if (state === 'ready' && liveMode && player.isLive()) {
          stage.classList.remove('empty')
          emptyState.hidden = true
        }
      }
      else if (state === 'empty') {
        // Nothing to play is a state of the preview, not a failure of it: the empty card already says so.
        stage.classList.add('empty')
        emptyState.hidden = false
        hideFail()
      }
      else if (state === 'append-failed') {
        // The stream is intact and the footage can still be saved: one chunk did not reach the buffer.
        appendProblem = true
        say(detail ?? 'This Timeline footage could not be added to the preview.', 'problem')
      }
      else if (state === 'missing') {
        leaveLive()
        showFail(detail ?? 'This footage is no longer on disk.', true)
      } else if (detail) {
        leaveLive()
        showFail(detail, true)
      }
      if (state === 'buffering') say('BUFFERING')
      else if (state === 'ready' && chip.textContent === 'BUFFERING') say(player.loaded()?.startsInGap ? 'GAP · THE PICTURE JUMPS' : '')
      note(`player: ${state}${detail ? `: ${detail}` : ''}`)
    },
    onLog: (message) => note(`player: ${message}`),
  })

  // ---- clocks: the footage, the stretch the bar shows, and where a clip may end ----
  const lo = () => session && session.chunks.length === 0 && liveMode && player.isLive()
    ? player.loaded()?.startMs ?? session.availableStartMs
    : session?.availableStartMs ?? 0
  const hi = () => session?.availableEndMs ?? 0
  /**
   * The newest moment on screen and in reach of a clip: the published edge, the writer's own chunk while
   * Live plays it, and - while a clip exists - the edge it was opened against. Leaving Live (any seek, an
   * edge drag) must not pull a clip that reached the live moment back to the published edge.
   */
  const liveEdge = () =>
    Math.max(hi(), liveMode && player.isLive() ? player.loaded()?.endMs ?? 0 : 0, selection ? clipEdge : 0)
  // The available edge arrives in fragments. The displayed edge follows the media clock instead, while
  // commands and selection bounds continue to use the real available edge.
  const visualEdge = () => liveMode && displayedLiveEdge !== undefined
    ? Math.max(displayedLiveEdge, selection ? clipEdge : 0)
    : liveEdge()
  const ageAt = (at: number) => {
    const summary = knownSessions.find((entry) => entry.id === session?.id)
    return footageAge(Date.now(), summary?.startedAt ?? 0, at, Math.max(hi(), player.loaded()?.endMs ?? 0))
  }
  // Show the available span only; true gaps between recorded chunks remain visible.
  const view = (): View => viewFor(zoom, frozenView, defaultView(lo(), visualEdge(), windowMs))
  // A frozen review view keeps its scale while newer footage arrives.
  // The first action that leaves live freezes the stretch on screen; taking it twice keeps the first, so
  // a seek settling after a scrub does not re-freeze a view the writer has already grown.
  const freezeView = () => {
    if (frozenView) return
    const shown = view()
    // Live plays the writer's open chunk, so the picture can sit past the published edge the bar ends
    // at: the frozen bar holds that moment, or the playhead has nowhere to be drawn until the second is
    // published. Only the end grows, and only to the moment the picture is already on.
    frozenView = { fromMs: shown.fromMs, toMs: frozenBarEnd(shown, wantMs) }
    // The bar drew its ruler and its chunks against the published edge; the end it now holds is redrawn
    // once, so a pause just after Live does not leave the labels and the cells at the old scale, and the window button
    // is offered from here - the only way out of a review view without asking for Live.
    paintAll()
  }
  // LIVE, the window button, an explicit zoom or a session opened fresh are the ways back to a following bar.
  const releaseView = () => {
    frozenView = undefined
    scrolledAway = false
  }
  const viewSpan = () => Math.max(1, view().toMs - view().fromMs)
  /** Where a moment sits on the bar, as a CSS percentage of its width; outside the view it is pinned to an edge. */
  const pct = (ms: number) => Math.min(100, Math.max(0, ((ms - view().fromMs) / viewSpan()) * 100))
  const position = (ms: number) => `${pct(ms)}%`
  const placeSpan = (element: HTMLElement, fromMs: number, toMs: number, minPct = 0.2) => {
    const v = view()
    const visible = toMs > v.fromMs && fromMs < v.toMs
    element.style.display = visible ? '' : 'none'
    if (!visible) return
    element.style.left = position(fromMs)
    element.style.width = `${Math.max(minPct, pct(toMs) - pct(fromMs))}%`
  }
  /** The moment under a pointer on any track the width of the bar. */
  const momentAt = (element: HTMLElement, event: PointerEvent | WheelEvent | MouseEvent) => {
    const box = element.getBoundingClientRect()
    const fraction = Math.min(1, Math.max(0, (event.clientX - box.left) / Math.max(1, box.width)))
    return view().fromMs + fraction * viewSpan()
  }

  /** The clip, clamped to what can be saved: the published footage, or up to the live edge while Live plays. */
  const currentSelection = (): Selection | undefined =>
    selection ? selectionClamp(selection.inMs, selection.outMs, lo(), liveEdge()) : undefined

  const paintTime = () => {
    if (!session) {
      if (timeText.textContent !== '0:00 / 0:00') timeText.textContent = '0:00 / 0:00'
      return
    }
    // In Live the pressed Live button says it: a second LIVE beside the speed was read as two controls.
    if (timeText.hidden !== liveMode) timeText.hidden = liveMode
    // Footage published behind a frozen bar is not played by itself: the readout says how much waits.
    const text = liveMode ? '' : ageAt(wantMs)
    if (timeText.textContent !== text) timeText.textContent = text
    refreshPlaybackStatus()
  }

  const paintHead = (at = wantMs) => {
    if (!session) return
    // The playhead the page acts on is the one it last asked for: `player.seek` is queued, so reading the
    // element between two notches would hand every notch the same stale moment.
    wantMs = at
    const shown = view()
    // A frozen bar holds the stretch the owner is reviewing: the picture can sit a frame or two past its
    // end (Live hands over mid-chunk, and the element pauses a frame later), so the head is held at the
    // edge instead of dropped. Under a zoom the head may be off the crop, and there it is not drawn: the
    // bar would otherwise claim a moment outside it is inside.
    const inside = at >= shown.fromMs && (at <= shown.toMs || Boolean(frozenView && !zoom))
    head.style.display = inside ? '' : 'none'
    head.style.left = position(at)
    fill.style.width = position(at)
    const time = timelineReadout(at, session.availableStartMs, session.availableEndMs)
    hereText.textContent = `HERE ${time.position}`
    bar.setAttribute('aria-valuemin', '0')
    // The picture can be a moment past the published edge (Live plays the writer's open chunk): the bar's
    // own end must never read as less than where its playhead is.
    bar.setAttribute('aria-valuemax', String(Math.round(Math.max(visualEdge(), at) - session.availableStartMs)))
    bar.setAttribute('aria-valuenow', String(Math.round(at - session.availableStartMs)))
    bar.setAttribute('aria-valuetext', ageAt(at))
    paintTime()
  }

  /** Clip mode is the clip existing: the Clip button gives way to Cancel and Save. */
  const paintClipMode = () => {
    const on = Boolean(selection)
    clipButton.hidden = on
    clipBar.hidden = !on
    clipGuide.hidden = !on
    document.body.classList.toggle('clipping', on)
    previewClipButton.setAttribute('aria-pressed', String(on && previewingClip))
    previewClipButton.textContent = previewingClip ? 'Stop clip' : 'Play clip'
    if (!on) closeMenu()
  }

  let cutLeft = 0
  let cutWidth = 0
  let cutTop = 0
  const placeSelection = (sel: Selection) => {
    placeSpan(selectionBand, sel.inMs, sel.outMs, 0)
    placeSpan(barSelection, sel.inMs, sel.outMs, 0)
    // The stylesheet hides the band until a clip exists, so an empty inline display must say `block`.
    if (selectionBand.style.display === '') selectionBand.style.display = 'block'
    if (barSelection.style.display === '') barSelection.style.display = 'block'
    // An end outside a zoomed stretch is not drawn pinned to the edge, where it would read as the clip's end.
    const v = view()
    inEdge.style.display = sel.inMs >= v.fromMs && sel.inMs <= v.toMs ? 'block' : 'none'
    outEdge.style.display = sel.outMs >= v.fromMs && sel.outMs <= v.toMs ? 'block' : 'none'
    inEdge.style.left = position(sel.inMs)
    outEdge.style.left = position(sel.outMs)
    inEdge.title = `Clip begins at ${clock(sel.inMs)} (drag)`
    outEdge.title = `Clip ends at ${clock(sel.outMs)} (drag)`
    // The length floats over the range it describes, so a drag says what it will save without moving the eye.
    const text = clipLabel(sel)
    if (trimRead.textContent !== text) trimRead.textContent = text
    clipDetails.textContent = `${text} · ${ageAt(sel.inMs)} – ${ageAt(sel.outMs)}`
    // The strip under the controls: the length first, then where the clip sits, in the ruler's own words.
    const nowMs = Date.now()
    const startedAt = knownSessions.find((entry) => entry.id === session?.id)?.startedAt ?? 0
    const end = Math.max(hi(), player.loaded()?.endMs ?? 0)
    const length = clock(sel.outMs - sel.inMs)
    if (clipLength.textContent !== length) clipLength.textContent = length
    const range = `${rulerAge(nowMs, startedAt, sel.inMs, end)} \u2192 ${rulerAge(nowMs, startedAt, sel.outMs, end)}`
    if (clipRange.textContent !== range) clipRange.textContent = range
    const middle = pct(sel.inMs + (sel.outMs - sel.inMs) / 2)
    trimRead.style.left = `clamp(100px, ${cutLeft + (cutWidth * middle) / 100}px, calc(100% - 100px))`
    trimRead.style.top = `${cutTop - 1}px`
  }
  const paintSelection = () => {
    paintClipMode()
    const sel = currentSelection()
    const shown = Boolean(session && sel && sel.outMs > sel.inMs)
    selectionBand.style.display = inEdge.style.display = outEdge.style.display = shown ? 'block' : 'none'
    barSelection.style.display = shown ? 'block' : 'none'
    trimRead.hidden = !shown
    saveButton.disabled = menuButton.disabled = saving || !shown
    if (shown && sel) placeSelection(sel)
  }

  // The writer's markers: a pin where it left one (a game change, a saved clip, a screenshot, a mark of
  // the user's own). Pressing one moves the playhead there.
  const paintMarkers = () => {
    if (!session) return
    for (const old of markers.querySelectorAll('.mk')) old.remove()
    const v = view()
    for (const marker of markersList) {
      const pin = document.createElement('button')
      const kind = MARKER_NAMES[marker.kind] ? marker.kind : 'marker'
      const name = `${MARKER_NAMES[kind]}${marker.label ? `: ${marker.label}` : ''} at ${clock(marker.atMs)}`
      pin.className = `mk ${kind}`
      const at = marker.atMs
      pin.dataset.atMs = String(at)
      pin.style.display = at >= v.fromMs && at <= v.toMs ? '' : 'none'
      pin.style.left = position(at)
      pin.title = pin.ariaLabel = `${name}. Right-click to remove, or select and use Remove marker.`
      pin.setAttribute('aria-pressed', String(marker === selectedMarker))
      pin.addEventListener('click', (event) => {
        event.stopPropagation()
        selectedMarker = marker
        paintMarkerAction()
        for (const other of markers.querySelectorAll('.mk')) other.setAttribute('aria-pressed', String(other === pin))
        seekTo(marker.atMs, false)
      })
      pin.addEventListener('contextmenu', (event) => {
        event.preventDefault()
        event.stopPropagation()
        void removeMarker(marker)
      })
      markers.append(pin)
    }
  }

  /** A few labels show time before the latest available moment, rather than an arbitrary session clock. */
  let rulerWidth = 480
  const paintRuler = () => {
    if (!session) {
      ruler.replaceChildren()
      return
    }
    const v = view()
    // Ticks stay anchored to session time; their labels age without moving the reviewed footage.
    const step = rulerStep(v.toMs - v.fromMs, rulerWidth, 135)
    const first = Math.ceil(v.fromMs / step) * step
    const summary = knownSessions.find((entry) => entry.id === session?.id)
    let index = 0
    for (let at = first; at <= v.toMs; at += step) {
      let label = ruler.children[index] as HTMLElement | undefined
      if (!label) {
        label = document.createElement('span')
        label.className = 'rl'
        ruler.append(label)
      }
      label.style.left = position(at)
      const text = rulerAge(Date.now(), summary?.startedAt ?? 0, at, Math.max(hi(), player.loaded()?.endMs ?? 0))
      if (label.textContent !== text) label.textContent = text
      label.dataset.now = text === 'NOW' ? 'true' : 'false'
      label.title = summary?.startedAt ? new Date(summary.startedAt * 1000 + at).toLocaleString() : 'Time before the end of this session'
      // A label at the right edge hangs to the left of its tick, so its tick is on the side the text ends on: the
      // text and the line it names stay one thing, never a line at one end of a box and the box at the other.
      const hangs = pct(at) > 95
      const wanted = hangs ? 'rl end' : 'rl'
      if (label.className !== wanted) label.className = wanted
      label.style.transform = hangs ? 'translateX(-100%)' : ''
      index++
    }
    while (ruler.children.length > index) ruler.lastElementChild?.remove()
    paintRulerTicks(v.fromMs, v.toMs, step)
  }
  /** The small ticks between the labelled ones, rebuilt only when the stretch or the step moved. */
  let rulerTicksKey = ''
  const paintRulerTicks = (from: number, to: number, step: number) => {
    const key = `${step}|${Math.round(from)}|${Math.round(to)}`
    if (key === rulerTicksKey) return
    rulerTicksKey = key
    const minors = rulerMinors(step)
    const fragment = document.createDocumentFragment()
    if (minors > 1 && to > from) {
      const part = step / minors
      let count = 0
      for (let base = Math.floor(from / step) * step; base <= to && count < 400; base += step) {
        for (let k = 1; k < minors; k++) {
          const at = base + k * part
          if (at <= from || at >= to) continue
          const tick = document.createElement('span')
          tick.className = 'rt'
          tick.style.left = position(at)
          fragment.append(tick)
          count++
        }
      }
    }
    rulerTicks.replaceChildren(fragment)
  }

  const paintRetention = () => {
    // A short chip; the sentence it stands for is its tooltip.
    const text = retentionMinutes > 0 ? `LAST ${retentionMinutes} MIN` : 'TIMELINE OFF'
    if (spanText.textContent !== text) spanText.textContent = text
    spanText.title = retentionMinutes > 0
      ? `Temporary Timeline footage is automatically removed after ${retentionMinutes} minutes. Saved clips remain.`
      : 'Turn Timeline on to keep temporary footage.'
  }

  const placeOverviewSpan = (element: HTMLElement, from: number, to: number, start: number, end: number) => {
    const visible = to > start && from < end
    element.style.display = visible ? '' : 'none'
    if (!visible) return
    const span = Math.max(1, end - start)
    const left = Math.max(start, from)
    const right = Math.min(end, to)
    const leftPct = ((left - start) / span) * 100
    element.style.left = `${leftPct}%`
    element.style.width = `${Math.min(100 - leftPct, Math.max(0.2, ((right - left) / span) * 100))}%`
  }

  /** Recorded footage as bands, including the readable part of the live chunk. */
  const paintChunks = (next: TimelineSession) => {
    cells.replaceChildren()
    ovCells.replaceChildren()
    for (const old of markers.querySelectorAll('.tick')) old.remove()
    // Neighbouring chunks are one stretch of picture: drawing a cell per chunk drew a seam every few
    // seconds, hundreds of them on a long session, where the picture never stops.
    let runStart = -1
    let runEnd = -1
    const flush = () => {
      if (runStart < 0) return
      const cell = document.createElement('span')
      cell.className = 'cell'
      cell.dataset.fromMs = String(runStart)
      cell.dataset.toMs = String(runEnd)
      placeSpan(cell, runStart, runEnd)
      cells.append(cell)
      const overviewCell = document.createElement('span')
      overviewCell.className = 'cell'
      overviewCell.dataset.fromMs = String(runStart)
      overviewCell.dataset.toMs = String(runEnd)
      placeOverviewSpan(overviewCell, runStart, runEnd, lo(), visualEdge())
      ovCells.append(overviewCell)
    }
    for (let i = 0; i < next.chunks.length; i++) {
      const chunk = next.chunks[i]
      const previous = next.chunks[i - 1]
      const gap = previous && chunk.startMs > previous.endMs + GAP_MS
      if (gap) {
        // A gap: Timeline stopped writing (another window in front, a pause) and the picture jumps here.
        // The hatch and its label are drawn once by paintGaps, in their own layer.
        flush()
        runStart = -1
      } else if (previous && chunk.epoch !== previous.epoch) {
        // The settings changed here: a clip cannot run across it, so the line is drawn.
        const tick = document.createElement('span')
        tick.className = 'tick'
        tick.dataset.atMs = String(chunk.startMs)
        tick.title = 'Settings changed here: a clip cannot cross this line'
        tick.style.left = position(chunk.startMs)
        tick.style.display = chunk.startMs >= view().fromMs && chunk.startMs <= view().toMs ? '' : 'none'
        markers.append(tick)
      }
      if (runStart < 0) runStart = chunk.startMs
      runEnd = chunk.endMs
    }
    const live = player.isLive() ? player.loaded() : undefined
    if (live && live.endMs > runEnd) {
      const start = Math.max(runEnd, live.startMs)
      if (runStart >= 0 && start > runEnd + GAP_MS) {
        flush()
        runStart = -1
      }
      if (runStart < 0) runStart = start
      runEnd = live.endMs
    }
    flush()
    if (runEnd >= 0 && view().toMs > runEnd) {
      const unavailable = document.createElement('span')
      unavailable.className = 'unavailable'
      unavailable.dataset.fromMs = String(runEnd)
      unavailable.dataset.toMs = String(view().toMs)
      unavailable.textContent = 'NOT YET AVAILABLE'
      unavailable.title = 'This part of the view is not available to play yet.'
      placeSpan(unavailable, runEnd, view().toMs, 0)
      cells.append(unavailable)
    }
    const empty = next.chunks.length === 0 && !live
    stage.classList.toggle('empty', empty)
    emptyState.hidden = !empty
  }

  /**
   * The stretches with nothing recorded: a seam between two published chunks longer than GAP_MS. The
   * bands and their labels are rebuilt with the rest of the geometry, never per frame; the label says
   * whether the game was not running or the writer simply was not writing.
   */
  const paintGaps = () => {
    gaps.replaceChildren()
    ovGaps.replaceChildren()
    if (!session) return
    const ordered = session.chunks.filter((chunk) => chunk.published).sort((a, b) => a.startMs - b.startMs)
    for (let i = 0; i < ordered.length - 1; i++) {
      const startMs = ordered[i].endMs
      const endMs = ordered[i + 1].startMs
      if (endMs - startMs <= GAP_MS) continue
      const title = `Nothing recorded ${clock(startMs)} to ${clock(endMs)}`
      const band = document.createElement('span')
      band.className = 'gap'
      band.dataset.fromMs = String(startMs)
      band.dataset.toMs = String(endMs)
      band.title = title
      placeSpan(band, startMs, endMs)
      const label = document.createElement('span')
      label.className = 'gap-label'
      label.textContent = gapLabel(markersList, { startMs, endMs })
      band.append(label)
      gaps.append(band)
      // The overview is 10 px tall: a bar there, never a label.
      const overviewHole = document.createElement('span')
      overviewHole.className = 'gap'
      overviewHole.title = title
      placeOverviewSpan(overviewHole, startMs, endMs, lo(), visualEdge())
      ovGaps.append(overviewHole)
    }
  }

  const placeOverviewWindow = (v: View) => {
    const start = lo()
    const full = Math.max(1, visualEdge() - start)
    const left = Math.min(100, Math.max(0, ((v.fromMs - start) / full) * 100))
    const right = Math.min(100, Math.max(left, ((v.toMs - start) / full) * 100))
    ovView.style.left = `${left}%`
    ovView.style.width = `${Math.min(100 - left, Math.max(0.5, right - left))}%`
  }

  /** The overview is the bar's scrollbar: the whole session, with the stretch on screen as a window to drag. */
  const paintOverview = () => {
    overview.hidden = !session || visualEdge() <= lo()
    paintWindowLabel()
    if (!overview.hidden) placeOverviewWindow(view())
  }
  /** The window button reads the length the bar shows now: ALL, or minutes. */
  const zoomWindow = $('zoomwin')
  const paintWindowLabel = () => {
    const v = view()
    const text = windowLabel(v.toMs - v.fromMs, !isZoomed(v, lo(), visualEdge()))
    if (zoomWindow.textContent !== text) zoomWindow.textContent = text
  }

  // ---- loud moments: the sound of each second, drawn along the bar ----
  const loudCanvas = $<HTMLCanvasElement>('loud')
  let loudPaintedView: View | undefined
  let loudPaintedWidth = 0
  let loudChunks: LoudChunk[] = []
  let loudBucketMs = 1000
  let loudLevel = 255
  const paintLoud = () => {
    const width = loudCanvas.clientWidth
    const height = loudCanvas.clientHeight
    if (!width || !height) return
    const ratio = window.devicePixelRatio || 1
    if (loudCanvas.width !== Math.round(width * ratio) || loudCanvas.height !== Math.round(height * ratio)) {
      loudCanvas.width = Math.round(width * ratio)
      loudCanvas.height = Math.round(height * ratio)
    }
    const context = loudCanvas.getContext('2d')
    if (!context) return
    const v = view()
    loudPaintedView = v
    loudPaintedWidth = width
    loudCanvas.style.transform = ''
    context.setTransform(ratio, 0, 0, ratio, 0, 0)
    context.clearRect(0, 0, width, height)
    if (!session || loudChunks.length === 0) return
    const step = 2
    const columns = Math.floor(width / step)
    const profile = loudColumns(loudChunks, v.fromMs, v.toMs, columns, loudBucketMs, loudLevel)
    const highlights = new Set(loudHighlights(profile))
    const style = getComputedStyle(loudCanvas)
    const ink = style.color
    const accent = getComputedStyle(document.body).getPropertyValue('--bx-acc').trim() || ink
    // Quiet columns first, then the loud regions with a soft glow of their own: the shadow is only paid for
    // on the few accented columns, and only when the bar is painted - never per frame at rest.
    for (const loud of [false, true]) {
      if (loud) {
        context.shadowColor = accent
        context.shadowBlur = 8 * ratio
      }
      profile.forEach((level, column) => {
        if (level <= 0 || highlights.has(column) !== loud) return
        const h = Math.max(1, level * (height - 4) * loudRise(loudGrow, column, columns))
        context.globalAlpha = loud ? 1 : 0.35
        context.fillStyle = loud ? accent : ink
        context.fillRect(column * step, height - h, step - 1, h)
      })
    }
    context.shadowBlur = 0
    context.globalAlpha = 1
  }
  // The profile rises from the left when a session's sound first arrives (~450 ms, then nothing runs).
  let loudGrow = 1
  let loudGrowRaf = 0
  const growLoud = () => {
    cancelAnimationFrame(loudGrowRaf)
    if (matchMedia('(prefers-reduced-motion: reduce)').matches) {
      loudGrow = 1
      return
    }
    const began = performance.now()
    const step = (now: number) => {
      loudGrow = Math.min(1, (now - began) / 450)
      paintLoud()
      loudGrowRaf = loudGrow < 1 ? requestAnimationFrame(step) : 0
    }
    loudGrow = 0
    loudGrowRaf = requestAnimationFrame(step)
  }
  let loudBusy = false
  let loudSeen = ''
  let loudPending = 0
  let loudTimer = 0
  /** The stretch the sound on the bar was asked for. */
  let loudRange: { from: number; to: number } | undefined
  let loudMoveTimer = 0
  /** The bar scrolled near the edge of the sound it has: ask for the stretch it is on now (debounced). */
  const wantLoudRange = () => {
    if (!loudRange || loudPending > 0 || loudMoveTimer) return
    const v = view()
    const half = (v.toMs - v.fromMs) / 2
    const near = (v.fromMs < loudRange.from + half && loudRange.from > lo()) || (v.toMs > loudRange.to - half && loudRange.to < visualEdge())
    if (!near) return
    loudMoveTimer = window.setTimeout(() => {
      loudMoveTimer = 0
      void refreshLoud()
    }, 200)
  }
  /** Asks the engine for the sound of the footage; it reads a few chunks per call, so this repeats until none is left. */
  const refreshLoud = async () => {
    if (!session || loudBusy || document.hidden) return
    const asked = session
    loudBusy = true
    // The stretch on screen and a view of margin either side: a long session's sound is read as it is scrolled to.
    const shown = view()
    const margin = shown.toMs - shown.fromMs
    const range = { from: Math.max(0, Math.round(shown.fromMs - margin)), to: Math.round(shown.toMs + margin) }
    const reply = await engine<{ bucketMs?: number; chunks?: LoudChunk[]; pending?: number }>('timeline.loudness', {
      session: asked.id,
      at_ms: Math.round(wantMs),
      from_ms: range.from,
      to_ms: range.to,
    })
    loudBusy = false
    if (!reply || session?.id !== asked.id) return
    loudRange = range
    const arriving = loudChunks.length === 0 && (reply.chunks?.length ?? 0) > 0
    loudChunks = reply.chunks ?? []
    loudBucketMs = reply.bucketMs || 1000
    loudLevel = loudScale(loudChunks)
    loudPending = reply.pending ?? 0
    loudSeen = asked.chunks.map((chunk) => `${chunk.file}:${chunk.published}`).join('|')
    if (arriving) growLoud()
    else paintLoud()
    if (loudPending > 0) {
      window.clearTimeout(loudTimer)
      loudTimer = window.setTimeout(() => void refreshLoud(), 1000)
    }
  }
  // Retention can replace the oldest chunk as a new one arrives without changing the count.
  const loudPollTimer = window.setInterval(() => {
    if (session && loudPending === 0 && session.chunks.map((chunk) => `${chunk.file}:${chunk.published}`).join('|') !== loudSeen) void refreshLoud()
  }, 4000)

  let geometryView: View | undefined
  /** Everything the footage and the view decide. */
  const paintAll = () => {
    if (!session) return
    rulerWidth = bar.clientWidth || 480
    cutLeft = bar.offsetLeft + cut.offsetLeft
    cutWidth = cut.clientWidth
    cutTop = bar.offsetTop + cut.offsetTop
    // Retention or a shorter session can leave a zoomed stretch past the footage: it is pulled back inside.
    if (zoom) zoom = isZoomed(zoom, lo(), visualEdge()) ? clampView(zoom, lo(), visualEdge()) : undefined
    paintRetention()
    paintChunks(session)
    paintGaps()
    paintRuler()
    paintMarkers()
    paintSelection()
    paintHead()
    paintOverview()
    paintLoud()
    wantLoudRange()
    geometryView = view()
  }

  // Move existing geometry when the live view grows; fragment polls alone rebuild the chunk bands.
  const paintLiveGeometry = () => {
    if (!session) return
    const v = view()
    for (const child of cells.children) {
      const element = child as HTMLElement
      placeSpan(element, Number(element.dataset.fromMs), Number(element.dataset.toMs))
    }
    for (const child of markers.children) {
      const element = child as HTMLElement
      const at = Number(element.dataset.atMs)
      const visible = at >= v.fromMs && at <= v.toMs
      if (!visible && element.style.display === 'none') continue
      element.style.display = visible ? '' : 'none'
      element.style.left = position(at)
    }
    for (const child of ovCells.children) {
      const element = child as HTMLElement
      const from = Number(element.dataset.fromMs)
      const to = Number(element.dataset.toMs)
      placeOverviewSpan(element, from, to, lo(), visualEdge())
    }
    paintRuler()
    const sel = currentSelection()
    if (sel && sel.outMs > sel.inMs) placeSelection(sel)
    paintWindowLabel()
    if (!overview.hidden) placeOverviewWindow(v)
    if (loudPaintedView) {
      const span = Math.max(1, v.toMs - v.fromMs)
      const scale = (loudPaintedView.toMs - loudPaintedView.fromMs) / span
      const offset = ((loudPaintedView.fromMs - v.fromMs) / span) * loudPaintedWidth
      loudCanvas.style.transform = `matrix(${scale}, 0, 0, 1, ${offset}, 0)`
    }
    geometryView = v
  }

  const draw = (next: TimelineSession) => {
    session = next
    range.dataset.session = next.id
    paintAll()
  }

  /** The sessions a reply names; the title shows the open one, there is no picker. */
  const fillChooser = (sessions: TimelineSessionSummary[]) => {
    knownSessions = sessions
  }

  // ---- zoom ----
  // A new view glides in over ~200 ms: each frame is the ordinary repaint at an in-between view, so nothing
  // runs at rest and a person who asks for reduced motion gets the jump.
  const GLIDE_MS = 200
  let glideRaf = 0
  const stopGlide = () => {
    cancelAnimationFrame(glideRaf)
    glideRaf = 0
  }
  const glideTo = (next: View | undefined) => {
    stopGlide()
    const from = view()
    const to = next ?? defaultView(lo(), visualEdge(), windowMs)
    const near = Math.abs(from.fromMs - to.fromMs) + Math.abs(from.toMs - to.toMs) < 1
    if (!session || near || matchMedia('(prefers-reduced-motion: reduce)').matches) {
      zoom = next
      paintAll()
      return
    }
    const began = performance.now()
    const step = (now: number) => {
      const progress = (now - began) / GLIDE_MS
      if (progress >= 1) {
        glideRaf = 0
        zoom = next
        paintAll()
        return
      }
      zoom = glideView(from, to, progress)
      paintAll()
      glideRaf = requestAnimationFrame(step)
    }
    glideRaf = requestAnimationFrame(step)
  }
  /** The bar shows `next`, and its length is the window the owner chose. */
  const setZoom = (next: View, why: string) => {
    clipZoomed = false
    clipPreviousZoom = undefined
    // The window button and +/- are a new view of the bar the owner asked for: the review view it replaces is released.
    releaseView()
    const edge = visualEdge()
    windowMs = windowOf(next, lo(), edge)
    // A window that ends at the newest footage goes back to following it while Live plays; any other stays where it was put.
    const following = liveMode && next.toMs >= edge - 1
    glideTo(following || windowMs === Infinity ? undefined : next)
    note(`zoom: ${why} ${windowMs === Infinity ? 'whole session' : `${Math.round(next.fromMs)}-${Math.round(next.toMs)} ms`}`)
  }
  const zoomBy = (factor: number, anchorMs = wantMs) => {
    if (!session) return
    setZoom(zoomView(view(), factor, anchorMs, lo(), visualEdge()), factor > 1 ? 'in' : 'out')
  }
  /** The window button: the next length (1 / 5 / 15 / 60 min / all), around the playhead, wrapping to the shortest. */
  const cycleWindow = () => {
    if (!session) return
    const edge = visualEdge()
    const v = view()
    const target = nextWindow(windowOf(v, lo(), edge))
    setZoom(target === Infinity ? { fromMs: lo(), toMs: edge } : zoomView(v, (v.toMs - v.fromMs) / target, wantMs, lo(), edge), 'window')
  }
  /** Key 0: the default window again, on the newest footage while Live plays, else around the playhead. */
  const resetWindow = () => {
    if (!session) return
    clipZoomed = false
    clipPreviousZoom = undefined
    releaseView()
    windowMs = WINDOW_DEFAULT_MS
    const edge = visualEdge()
    glideTo(liveMode ? undefined : clampView({ fromMs: wantMs - windowMs / 2, toMs: wantMs + windowMs / 2 }, lo(), edge))
    note('zoom: default window')
  }
  /** A bar holding less than the footage (zoomed, or a review view held) moves so the playhead stays on it; true when it moved. */
  const followPlayhead = (at: number): boolean => {
    const held = zoom ?? frozenView
    if (!held) return false
    const followed = followView(held, at, lo(), Math.max(liveEdge(), held.toMs))
    if (followed === held) return false
    if (zoom) zoom = followed
    else frozenView = followed
    paintAll()
    return true
  }
  $('zoomin').addEventListener('click', () => zoomBy(2))
  $('zoomout').addEventListener('click', () => zoomBy(0.5))
  zoomWindow.addEventListener('click', cycleWindow)

  // The overview: a press centres the view there, a drag slides it.
  let ovDragging = false
  const ovAim = (event: PointerEvent) => {
    const held = view()
    const edge = visualEdge()
    // The window already holds every second: there is nothing to scroll to.
    if (!isZoomed(held, lo(), edge)) return
    clipZoomed = false
    clipPreviousZoom = undefined
    // A dragged overview window is an explicit view too: the frozen stretch is no longer what is drawn.
    releaseView()
    stopGlide()
    const box = overview.getBoundingClientRect()
    const at = lo() + Math.min(1, Math.max(0, (event.clientX - box.left) / Math.max(1, box.width))) * (edge - lo())
    const span = held.toMs - held.fromMs
    windowMs = span
    const next = clampView({ fromMs: at - span / 2, toMs: at + span / 2 }, lo(), edge)
    // Dragged to the newest footage while Live plays, the bar follows it again; dragged back, it stays where it was put.
    const following = liveMode && next.toMs >= edge - 1
    zoom = following ? undefined : next
    scrolledAway = liveMode && !following
    paintAll()
  }
  overview.addEventListener('pointerdown', (event) => {
    if (event.button !== 0) return
    overview.setPointerCapture(event.pointerId)
    ovDragging = true
    ovAim(event)
    event.preventDefault()
  })
  overview.addEventListener('pointermove', (event) => {
    if (ovDragging) ovAim(event)
  })
  for (const end of ['pointerup', 'pointercancel']) overview.addEventListener(end, () => (ovDragging = false))

  // The moment the head lands on can be far from the loaded window, and the rebuild that follows takes
  // a second or more. One still from the engine's thumbnail cache stands in for the picture until the
  // video reports the new frame, so a long rewind never lands on a stale frame or on black.
  let stillTimer = 0
  let stillToken = 0
  const showStill = (atMs: number) => {
    const id = session?.id
    if (!id) return
    if (stillTimer) clearTimeout(stillTimer)
    // Every request takes the token, so a still that arrives after the picture is back is dropped, and a
    // held arrow's older requests never land on top of a newer one.
    const token = ++stillToken
    // A still that is already up has time: 80 ms of debounce coalesces a held arrow. With nothing on screen
    // the request goes at once, or the rebuild's black would show first.
    const wait = still.hidden ? 0 : 80
    stillTimer = window.setTimeout(() => {
      stillTimer = 0
      void attempt<{ thumbs: { atMs: number; file: string }[] }>('timeline.thumbs', {
        session: id,
        times_ms: [Math.round(atMs)],
      }).then((reply) => {
        if (token !== stillToken) return
        const file = reply.data?.thumbs?.[0]?.file
        if (!file) return
        still.src = mediaUrl(file)
        still.hidden = false
      })
    }, wait)
  }
  const hideStill = () => {
    stillToken += 1
    if (stillTimer) {
      clearTimeout(stillTimer)
      stillTimer = 0
    }
    still.hidden = true
  }
  // The wait ends on the frame presented after the seek, never on the one already on screen: `seeked` fires
  // before the new frame is painted, which used to drop the still for a frame and let the old picture show.
  const hideOnFrame = () => {
    const token = stillToken
    let timer = 0
    const done = () => {
      if (timer) {
        clearTimeout(timer)
        timer = 0
      }
      if (token === stillToken) hideStill()
    }
    // A seek to the frame already on screen presents nothing new; the still must not outlive it.
    timer = window.setTimeout(done, 3000)
    if (typeof video.requestVideoFrameCallback === 'function') video.requestVideoFrameCallback(done)
    else video.addEventListener('seeked', () => requestAnimationFrame(done), { once: true })
  }

  // A seek leaves Live. A moment without footage lands on an available edge.
  let seekSerial = 0
  let ageTimer = 0
  let seekInFlight = false
  let resumeAfterSeek = false
  let seekTimer = 0
  /** Ends a seek's hold on the needle; a stale serial is another seek's business. */
  const settleSeek = (serial: number) => {
    if (serial !== seekSerial) return
    seekInFlight = false
    if (seekTimer) {
      window.clearTimeout(seekTimer)
      seekTimer = 0
    }
  }
  const cancelSeekResume = () => {
    seekSerial += 1
    settleSeek(seekSerial)
    resumeAfterSeek = false
  }
  /**
   * `fromMs` is where the movement started: a head already on the newest footage that steps one frame is
   * not asking for Live. `snapLive` is false for a clip handle, whose end is the newest footage on
   * purpose and must not hand the bar back to the growing edge under the owner's hand.
   */
  const seekTo = (targetMs: number, _snapLive = true, _fromMs = wantMs, direction: -1 | 0 | 1 = 0) => {
    if (!session) return
    if (direction < 0 || (_snapLive && targetMs < _fromMs)) {
      const label = $('rewind-age')
      label.textContent = ageAt(targetMs)
      label.hidden = false
      window.clearTimeout(ageTimer)
      ageTimer = window.setTimeout(() => { label.hidden = true }, 1800)
    }
    // The end timestamp is exclusive for the decoder; parking on it can reset playback.
    let target = scrubBy(targetMs, 0, lo(), Math.max(lo(), hi() - 1))
    // The end of the bar remains a review position; Live is an explicit action.
    leaveLive()
    // A seek is a review: the stretch on screen is what the playhead lands in, and it stays put.
    freezeView()
    if (scrubRaf) {
      cancelAnimationFrame(scrubRaf)
      scrubRaf = 0
    }
    const selected = currentSelection()
    if (previewingClip && selected && (target < selected.inMs || target >= selected.outMs)) {
      previewingClip = false
      paintClipMode()
    }
    if (!isRecorded(session.chunks, target)) {
      const landing = seekableMoment(session.chunks, target, direction)
      if (landing === null) {
        say('Nothing recorded here.', 'problem')
        return
      }
      target = landing
    }
    hideFail()
    card.classList.remove('on')
    showStill(target)
    hideOnFrame()
    resumeAfterSeek ||= !video.paused
    const serial = ++seekSerial
    // The target is the position until the seek settles: the element still reports the old one while a
    // rebuild runs, and painting that put the needle back where it came from before it moved forward.
    wantMs = target
    // A seek asked for while the last one is still settling takes its wait over.
    if (seekTimer) window.clearTimeout(seekTimer)
    seekInFlight = true
    syncPlay()
    // A fetch that never settles must not hold the needle, and with it the play/pause toggle, for good:
    // the first frame of the new window or five seconds ends the wait, whichever comes first.
    seekTimer = window.setTimeout(() => settleSeek(serial), 5000)
    void player.seek(target, () => {
      if (serial !== seekSerial || !resumeAfterSeek) return false
      resumeAfterSeek = false
      return true
    }).then(() => {
      if (serial !== seekSerial) return
      settleSeek(serial)
      syncPlay()
      if (resumeAfterSeek && video.paused) void video.play().catch(() => {})
      resumeAfterSeek = false
    }).catch(() => {
      if (serial === seekSerial) cancelSeekResume()
    })
    paintHead(target)
    if (loudPending > 0) {
      window.clearTimeout(loudTimer)
      loudTimer = window.setTimeout(() => void refreshLoud(), 100)
    }
    if (followPlayhead(target)) stopGlide()
    note(`seek to ${Math.round(target)} ms`)
  }

  // A gap between two runs of footage is stepped over while playing: MediaSource stalls at the end of
  // a chunk with the element still unpaused, so the picture would sit on its last frame for the pause.
  const NEAR_CHUNK_END_MS = 300
  const CROSS_GAP_MIN_MS = GAP_MS
  /** The chunk end already stepped over, so one stall cannot ask for the same skip twice. */
  let skippedGapAt = -1
  const crossGap = (atMs: number): boolean => {
    if (!session || liveMode || video.paused) return false
    if (scrubRaf || seeking || seekInFlight) return false
    if (skippedGapAt >= 0 && Math.abs(atMs - skippedGapAt) > NEAR_CHUNK_END_MS) skippedGapAt = -1
    for (const chunk of session.chunks) {
      if (!chunk.published || Math.abs(atMs - chunk.endMs) > NEAR_CHUNK_END_MS) continue
      const start = gapSuccessor(session.chunks, chunk.endMs, CROSS_GAP_MIN_MS)
      if (start === null) continue
      const clip = previewingClip ? currentSelection() : undefined
      if (clip && start >= clip.outMs) {
        seekTo(clip.inMs, false)
        return true
      }
      if (skippedGapAt === chunk.endMs) return true
      skippedGapAt = chunk.endMs
      // The hole is stepped over without a word: its band already says why there is no picture.
      seekTo(start + 1, false)
      return true
    }
    return false
  }

  // A wheel is a direction, not a step: the playhead eases toward the moment the notches add up to, and
  // one real seek runs when the motion stops. The head has a moment of its own, so the ease keeps moving
  // after the target leaves the loaded window instead of stalling on the old picture.
  let scrubTarget = 0
  let scrubNow = 0
  /** Where the wheel gesture started: a wheel that began on the edge is not asking to return to it. */
  let scrubFrom = 0
  let scrubRaf = 0
  let scrubLastFrame = 0
  let scrubLastPreview = 0
  let scrubLastStill = 0
  let scrubOutside = false
  /** Whether the picture was playing when this wheel/drag gesture began; settle resumes it. */
  let scrubResume = false
  const stepScrub = (now: number) => {
    const elapsed = Math.min(100, now - scrubLastFrame)
    scrubLastFrame = now
    scrubNow += (scrubTarget - scrubNow) * (1 - Math.exp(-elapsed / 55))
    if (Math.abs(scrubTarget - scrubNow) < 30) {
      scrubRaf = 0
      if (scrubResume) {
        resumeAfterSeek = true
        scrubResume = false
      }
      seekTo(scrubTarget, false, scrubFrom, scrubTarget < scrubFrom ? -1 : scrubTarget > scrubFrom ? 1 : 0)
      return
    }
    paintHead(scrubNow)
    // A decoder cannot use a fresh seek on every display frame; the final seek remains exact.
    if (now - scrubLastPreview >= 120) {
      scrubLastPreview = now
      if (!player.seekPreview(scrubNow, true)) {
        scrubOutside = true
        if (now - scrubLastStill >= 250) {
          scrubLastStill = now
          showStill(scrubTarget)
        }
      } else if (scrubOutside) {
        scrubOutside = false
        hideOnFrame()
      }
    }
    scrubRaf = requestAnimationFrame(stepScrub)
  }
  const scrubTo = (targetMs: number) => {
    if (!session) return
    const target = scrubBy(targetMs, 0, lo(), hi())
    if (!scrubRaf) {
      scrubNow = wantMs
      // The gesture's own starting moment: a wheel that began on the edge, or an ease still travelling
      // toward it, is not a movement from a review position back to Live.
      scrubFrom = scrubNow
      scrubLastFrame = performance.now()
      scrubLastPreview = 0
      scrubLastStill = 0
      scrubOutside = false
      // A wheel while the picture plays pauses it for the gesture: the decoder then serves one still
      // at a time instead of fighting the playhead, and the settle resumes it.
      scrubResume = !video.paused
      if (scrubResume) video.pause()
      syncPlay()
    }
    // A wheel gesture stays in review even when it reaches the newest available footage.
    leaveLive()
    // A wheel or a scrub is a review: the bar holds the stretch the owner is walking through.
    freezeView()
    scrubTarget = target
    const selected = currentSelection()
    if (previewingClip && selected && (scrubTarget < selected.inMs || scrubTarget >= selected.outMs)) {
      previewingClip = false
      paintClipMode()
    }
    card.classList.remove('on')
    if (!scrubRaf) scrubRaf = requestAnimationFrame(stepScrub)
  }

  // ---- the clip ----
  /** Both ends of the clip are on the bar, whatever scale or place the bar had: a view that misses one is moved or widened. */
  const showWholeClip = () => {
    if (!selection) return
    const next = viewHoldingClip(view(), selection, lo(), liveEdge())
    if (!next) return
    // The zoom this makes for the clip goes with the clip, as the one Clip makes for itself does.
    if (!clipZoomed) {
      clipPreviousZoom = zoom
      clipZoomed = true
    }
    glideTo(next)
  }
  const openClip = () => {
    if (!session || liveEdge() <= lo()) return
    // Choosing a clip is a review: the edge the clip may reach is the moment it was opened at, and the
    // bar leaves the growing edge before it freezes, so the poll's review path is the one that runs from
    // here. A clip made while Live used to leave the frozen view inert, because Live was still on.
    clipEdge = liveEdge()
    leaveLive()
    previewingClip = false
    selection = clipAround(wantMs, lo(), clipEdge, CLIP_DEFAULT_MS)
    freezeView()
    // A 30-second clip on an hour of footage is a sliver nobody can grab: the bar zooms onto it, with
    // the clip's own length of context either side, as Steam's clip mode does.
    const length = selection.outMs - selection.inMs
    if (!zoom && viewSpan() > length * 8) {
      clipPreviousZoom = zoom
      clipZoomed = true
      glideTo(clampView({ fromMs: selection.inMs - length, toMs: selection.outMs + length }, lo(), liveEdge()))
    } else {
      showWholeClip()
    }
    paintSelection()
    say(`Clip ${clock(selection.outMs - selection.inMs)}: drag its ends, then Save (Enter)`)
    note(`clip opened: in ${Math.round(selection.inMs)}, out ${Math.round(selection.outMs)}`)
  }
  const clearClip = () => {
    selection = undefined
    previewingClip = false
    // The zoom Clip made for itself goes with it; one the person chose stays.
    const unzoom = clipZoomed
    const previousZoom = clipPreviousZoom
    clipZoomed = false
    clipPreviousZoom = undefined
    if (unzoom) glideTo(previousZoom)
    else paintAll()
  }
  const cancelClip = () => { clearClip(); say('') }
  /** I and O: the clip's start or end at the playhead; with no clip yet this starts one. */
  const setEdge = (which: 'in' | 'out') => {
    if (!session || liveEdge() <= lo()) return
    // I and O are a review action too: the edge is marked against the moment it is pressed at, then the
    // bar leaves the growing edge and holds it, or the clip would move under the owner at the next poll.
    const edge = liveEdge()
    // The clip reaches the edge it was cut against, and that edge is kept after leaving Live: the frozen
    // bar must still show the range the clip can be saved over.
    clipEdge = edge
    const before = currentSelection()
    leaveLive()
    selection = markEdge(before, which, wantMs, lo(), edge, CLIP_DEFAULT_MS)
    freezeView()
    showWholeClip()
    paintSelection()
    const length = clock(selection.outMs - selection.inMs)
    say(which === 'in'
      ? `Clip ${length}: begins at ${clock(selection.inMs)}`
      : `Clip ${length}: ends at ${clock(selection.outMs)}`)
    note(`mark ${which}: in ${Math.round(selection.inMs)}, out ${Math.round(selection.outMs)}`)
  }
  clipButton.addEventListener('click', openClip)
  previewClipButton.addEventListener('click', () => {
    const sel = currentSelection()
    if (!session || !sel) return
    if (previewingClip) {
      previewingClip = false
      cancelSeekResume()
      video.pause()
      paintClipMode()
      say('Clip preview stopped.')
      return
    }
    if (sel.inMs >= hi()) {
      say('The selected footage is still being written. Try again when it is available.', 'problem')
      return
    }
    if (videoHidden) setVideoHidden(false)
    leaveLive()
    previewingClip = true
    paintClipMode()
    resumeAfterSeek = true
    // Playing a clip is the clip's own moment, even if it ends at the newest footage: it is not a
    // gesture toward the live edge.
    seekTo(sel.inMs, false)
    say('Playing the clip on repeat.')
  })
  $('trimclip').addEventListener('click', () => {
    const sel = currentSelection()
    if (!session || !sel || sel.outMs <= sel.inMs) return
    leaveLive()
    cancelSeekResume()
    video.pause()
    const length = sel.outMs - sel.inMs
    if (!clipZoomed) clipPreviousZoom = zoom
    clipZoomed = true
    glideTo(clampView({ fromMs: sel.inMs - length / 2, toMs: sel.outMs + length / 2 }, lo(), liveEdge()))
    say('Drag either handle to trim, then save the clip.')
  })
  $('cancelclip').addEventListener('click', cancelClip)
  const showClip = (file: string) => {
    cardName.textContent = clipName(file)
    card.classList.add('on')
  }

  type Then = 'stay' | 'game' | 'edit' | 'share' | 'copy'

  async function openShare(file: string) {
    if (document.body.classList.contains('sharing') || closing) return
    const generation = ++shareGeneration
    const previousFocus = document.activeElement instanceof HTMLElement ? document.activeElement : null
    const picture = libraryOpen ? document.querySelector<HTMLVideoElement>('#timeline-library-preview video') : null
    const wasPlaying = Boolean(picture && !picture.paused)
    if (wasPlaying) picture?.pause()
    const wasHidden = videoHidden
    if (wasHidden) setVideoHidden(false)
    document.body.classList.add('sharing')
    const host = document.createElement('div')
    host.id = 'timeline-share-host'
    document.body.append(host)
    const dismiss = () => {
      shareGeneration++
      void disposeShare?.(); disposeShare = undefined
      host.remove()
      document.body.classList.remove('sharing')
      if (wasHidden && !closing) setVideoHidden(true)
      if (wasPlaying && !closing) void picture?.play().catch(() => {})
      previousFocus?.focus()
    }
    try {
      const { mountShare } = await import('./share')
      if (generation !== shareGeneration || closing) { host.remove(); return }
      disposeShare = mountShare(host, file, dismiss)
    } catch {
      dismiss()
      say('Could not open Share. Try again.', 'problem')
    }
  }

  /** Saves the clip (once per range) and gives back its file, or nothing with the reason said. */
  // I6: the Save control reads "Saved" with a small pop for a moment, and the saved range flashes the accent for 400 ms.
  let savedTimer = 0
  const flashSaved = () => {
    saveButton.textContent = 'Saved'
    saveButton.dataset.state = 'saved'
    window.clearTimeout(savedTimer)
    savedTimer = window.setTimeout(() => {
      saveButton.textContent = 'Save clip'
      delete saveButton.dataset.state
    }, 1400)
    if (!reducedMotion.matches && selectionBand.style.display !== 'none') {
      selectionBand.animate([{ outline: '2px solid var(--bx-acc)', outlineOffset: '0px' }, { outline: '2px solid transparent', outlineOffset: '0px' }], { duration: 400, easing: 'ease-out' })
    }
  }
  async function saveClip(notify = false): Promise<string | undefined> {
    if (!session || saving) return undefined
    const sel = currentSelection()
    if (!sel) return saved?.file
    if (saved && saved.inMs === Math.round(sel.inMs) && saved.outMs === Math.round(sel.outMs)) {
      const file = saved.file
      cancelClip()
      return file
    }
    const refusal = saveRefusal(sel, lo(), liveEdge())
    if (refusal) {
      say(refusal, 'problem')
      note(`save: refused before the command: ${refusal}`)
      return undefined
    }
    saving = true
    paintSelection()
    // Past the published edge the engine first closes the writer's open chunk, which takes a moment.
    say(sel.outMs > hi() ? `SAVING ${clock(sel.outMs - sel.inMs)} · FINISHING THE NEWEST SECONDS` : `SAVING ${clock(sel.outMs - sel.inMs)}`)
    note(`save: ${session.id} [${sel.inMs}, ${sel.outMs})`)
    const range = { inMs: Math.round(sel.inMs), outMs: Math.round(sel.outMs) }
    const selectionAtSave = selection
    const revisionAtSave = sessionRevision
    const reply = await attempt<{ file: string; bytes: number; chunks: number; cutHeadMs?: number; gapMs?: number }>('timeline.save', {
      session: session.id,
      start_ms: range.inMs,
      end_ms: range.outMs,
      notify,
    })
    saving = false
    paintSelection()
    const disposition = clipSaveDisposition(Boolean(reply.data?.file), selectionAtSave, selection, revisionAtSave, sessionRevision)
    if (reply.data?.file) {
      if (disposition !== 'stale') {
        saved = { file: reply.data.file, ...range }
        if (disposition === 'clear') cancelClip()
        showClip(reply.data.file)
        const cutHead = reply.data.cutHeadMs ?? 0
        const gap = reply.data.gapMs ?? 0
        const notes: string[] = []
        if (cutHead > 0) notes.push(`${clock(cutHead)} OMITTED FROM THE START AFTER A FORMAT CHANGE`)
        if (gap > 0) notes.push(`${clock(gap)} OF A PAUSE LEFT OUT`)
        if (notes.length) say(`SAVED ${clipName(reply.data.file)} · ${notes.join(' · ')}`, 'problem')
        else say(`SAVED ${clipName(reply.data.file)}`)
        flashSaved()
        if (session) void loadMarkers(session)
      }
      note(`saved: ${reply.data.file}, ${reply.data.bytes} bytes, ${reply.data.chunks} chunk(s)`)
      return reply.data.file
    }
    // The engine's own words: a refusal names what it could not copy, and paraphrasing it would hide that.
    say(`Save failed: ${reply.error ?? 'the engine did not answer'}`, 'problem')
    note(`save: failed: ${reply.error ?? 'no reply'}`)
    return undefined
  }

  /** What follows a save: stay, go back to the game, or hand the clip to the editor, the clipboard or Share. */
  async function after(then: Then, savedCard = false) {
    closeMenu()
    if (then === 'game') {
      const sel = currentSelection()
      if (!sel || saving) return
      // Refused while the window is still up, so the reason is read; once hidden only the notice speaks.
      const refusal = saveRefusal(sel, lo(), liveEdge())
      if (refusal) {
        say(refusal, 'problem')
        return
      }
      // The window goes at once; the engine's own notice says when the clip is in the library.
      await invoke('timeline_hide').catch(() => {})
      note('save and back to game')
      await saveClip(true)
      void close('save and back to game')
      return
    }
    const file = savedCard ? saved?.file : currentSelection() ? await saveClip() : saved?.file
    if (!file) {
      if (!currentSelection()) say('Make a clip first (C), then save it.', 'problem')
      return
    }
    if (then === 'edit') {
      const opened = await engine<{ pid?: number; forwarded?: boolean }>('editor.open', { files: [file], preview: false })
      note(`edit: ${file} (${opened?.forwarded ? 'forwarded' : opened?.pid ? `pid ${opened.pid}` : 'no editor'})`)
      // The preview is topmost over most of the screen; leaving it up would put the editor behind it.
      void close('save and edit')
    } else if (then === 'share') {
      void openShare(file)
      note(`share: ${file}`)
    } else if (then === 'copy') {
      const copied = await attempt('clips.copy', { files: [file] })
      if (!copied.error) say('Copied: paste it into Discord, a chat or a folder.')
    }
  }
  saveButton.addEventListener('click', () => void after('stay'))

  // The save menu: Steam's Save / Share split, on BuffrX's own actions.
  const closeMenu = () => {
    if (menu.hidden) return
    menu.hidden = true
    menuButton.setAttribute('aria-expanded', 'false')
    fitHidden()
  }
  menuButton.addEventListener('click', (event) => {
    event.stopPropagation()
    menu.hidden = !menu.hidden
    menuButton.setAttribute('aria-expanded', String(!menu.hidden))
    fitHidden()
    if (!menu.hidden) menu.querySelector<HTMLButtonElement>('button')?.focus()
  })
  for (const item of menu.querySelectorAll<HTMLButtonElement>('button[data-then]')) {
    item.addEventListener('click', () => void after(item.dataset.then as Then))
  }
  document.addEventListener('pointerdown', (event) => {
    if (!menu.hidden && !(event.target as HTMLElement).closest('#savegroup')) closeMenu()
  })

  // ---- markers of the user's own ----
  const addMarker = async () => {
    if (!session) return
    const at = Math.round(wantMs)
    const reply = await attempt('markers.add', { session: session.id, at_ms: at, kind: 'marker', label: '' })
    if (reply.error) return
    markersList = [...markersList, { atMs: at, kind: 'marker', label: '' }]
    paintMarkers()
    say('MARKED')
    window.clearTimeout(chipTimer)
    chipTimer = window.setTimeout(() => say(''), 1200)
  }
  $('addmark').addEventListener('click', () => void addMarker())
  const removeMarkerButton = $<HTMLButtonElement>('removemark')
  const paintMarkerAction = () => {
    removeMarkerButton.disabled = !selectedMarker
    // Only there while a marker is selected: a greyed button the rest of the time was clutter.
    removeMarkerButton.hidden = !selectedMarker
    removeMarkerButton.title = selectedMarker
      ? `Remove ${MARKER_NAMES[selectedMarker.kind] ?? 'marker'} at ${clock(selectedMarker.atMs)}`
      : 'Select a marker above the timeline to remove it'
  }
  let removingMarker = false
  const removeMarker = async (target: TimelineMarker) => {
    if (!session || removingMarker) return
    const wanted = session
    removingMarker = true
    removeMarkerButton.disabled = true
    try {
      const reply = await attempt('markers.remove', {
        session: wanted.id, at_ms: target.atMs, kind: target.kind, label: target.label,
      })
      if (reply.error) {
        say(`Could not remove marker: ${reply.error}`, 'problem')
        return
      }
      if (selectedMarker === target) selectedMarker = undefined
      if (session?.id === wanted.id) await loadMarkers(wanted)
      say('Marker removed')
    } finally {
      removingMarker = false
      paintMarkerAction()
    }
  }
  removeMarkerButton.addEventListener('click', () => {
    if (selectedMarker) void removeMarker(selectedMarker)
  })

  // ---- sound: the preview's own volume, remembered for the next time it opens ----
  const paintSound = () => {
    muteButton.setAttribute('aria-pressed', String(video.muted))
    muteButton.title = muteButton.ariaLabel = video.muted ? 'Unmute (Shift+M)' : 'Mute (Shift+M)'
    $('wave').style.display = video.muted || video.volume === 0 ? 'none' : ''
    volumeInput.value = String(video.muted ? 0 : video.volume)
    volumeInput.style.setProperty('--volume-percent', `${Number(volumeInput.value) * 100}%`)
  }
  video.volume = Math.min(1, Math.max(0, Number(remembered('bx.timeline.volume') ?? '1') || 0))
  video.muted = remembered('bx.timeline.muted') === '1'
  paintSound()
  const toggleMute = () => {
    video.muted = !video.muted
    if (!video.muted && video.volume === 0) video.volume = 0.5
    remember('bx.timeline.muted', video.muted ? '1' : '0')
    paintSound()
  }
  muteButton.addEventListener('click', toggleMute)
  volumeInput.addEventListener('input', () => {
    video.volume = Number(volumeInput.value)
    video.muted = video.volume === 0
    remember('bx.timeline.volume', String(video.volume))
    remember('bx.timeline.muted', video.muted ? '1' : '0')
    paintSound()
  })

  // ---- Hide video: the window shrinks to the panel, so the game shows under the timeline ----
  let videoHidden = false
  /** While the video is hidden the window is exactly the panel, plus the save menu while it is open. */
  const fitHidden = () => {
    if (!videoHidden) return
    const bottom = Math.max(panel.getBoundingClientRect().bottom, menu.hidden ? 0 : menu.getBoundingClientRect().bottom + 6)
    void invoke('timeline_height', { height: Math.ceil(bottom) }).catch(() => {})
  }
  // The panel grows with a clip's overview row and shrinks without it; the window follows.
  const fitObserver = new ResizeObserver(fitHidden)
  fitObserver.observe(panel)
  // I3: hiding fades the picture and scales it to 0.98 before the stage goes and the shell shrinks the window; showing grows the window
  // at once and fades the picture in. The shell's resize itself stays instant.
  let hideAnimation: Animation | null = null
  const setVideoHidden = (on: boolean) => {
    videoHidden = on
    hideAnimation?.cancel()
    hideAnimation = null
    if (on && !reducedMotion.matches && !document.body.classList.contains('novideo')) {
      const animation = stage.animate(
        [{ opacity: 1, transform: 'scale(1)' }, { opacity: 0, transform: 'scale(0.98)' }],
        { duration: 140, easing: 'cubic-bezier(0.55, 0.055, 0.675, 0.19)', fill: 'forwards' },
      )
      hideAnimation = animation
      void animation.finished.then(() => {
        if (hideAnimation !== animation) return
        hideAnimation = null
        if (videoHidden) {
          document.body.classList.add('novideo')
          fitHidden()
        }
        animation.cancel()
      }, () => {})
    } else {
      document.body.classList.toggle('novideo', on)
      if (!on && !reducedMotion.matches) stage.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 140, easing: 'cubic-bezier(0.22, 0.9, 0.3, 1)' })
    }
    hideButton.setAttribute('aria-pressed', String(on))
    hideButton.textContent = on ? 'Show video' : 'Hide video'
    // Nothing is decoded for a picture nobody sees.
    if (on) {
      leaveLive()
      cancelSeekResume()
      video.pause()
    }
    if (on && document.body.classList.contains('novideo')) fitHidden()
    else if (!on) void invoke('timeline_height', { height: 0 }).catch(() => {})
    note(`video ${on ? 'hidden' : 'shown'}`)
  }
  hideButton.addEventListener('click', () => setVideoHidden(!videoHidden))

  // ---- the window's other ways out ----
  // ---- the library: the saved clips and screenshots, on the picture's own stage ----
  // One button opens it and brings the Timeline back (View clips / Back to Timeline). A saved file plays inside the
  // preview card, so the card is where the page says it is a saved file and not the footage.
  const viewClips = $('viewclips')
  const screen = $('screen')
  let libraryLeaving = false
  let leaveLibrary: (() => void) | undefined
  // The bar and the transport fold away (or come back) at once and the stage glides from where it was to where it is: a transform
  // on the compositor, not a height animated through layout every frame under the panel's blur.
  const reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)')
  const glideStage = (change: () => void) => {
    const before = stage.getBoundingClientRect().top
    change()
    const shift = before - stage.getBoundingClientRect().top
    if (!shift || reducedMotion.matches) return
    stage.animate([{ transform: `translateY(${shift}px)` }, { transform: 'translateY(0)' }], { duration: 420, easing: 'cubic-bezier(0.22, 0.9, 0.3, 1)' })
  }
  // The library's code is fetched before it is asked for: when the pointer reaches the button, and once the page is quiet.
  viewClips.addEventListener('pointerenter', () => void import('./library'), { once: true })
  window.setTimeout(() => void import('./library'), 2500)
  // One toggle: lit while the library is open, and pressing it again is the way back. The way back is also named in
  // the library's own header (Timeline), so the top bar never carries two buttons that both say "back".
  const setViewClips = (open: boolean) => {
    viewClips.setAttribute('aria-pressed', String(open))
    viewClips.title = open ? 'Back to the Timeline' : 'Open the saved clips and screenshots here, in the preview'
  }
  viewClips.addEventListener('click', async () => {
    if (libraryLeaving) return
    if (libraryOpen) {
      leaveLibrary?.()
      return
    }
    libraryOpen = true
    setViewClips(true)
    const previous = { id: session?.id, at: wantMs, live: liveMode, playing: !video.paused, hidden: videoHidden }
    leaveLive()
    cancelSeekResume()
    video.pause()
    if (videoHidden) setVideoHidden(false)
    const host = document.createElement('div')
    host.id = 'timeline-library'
    const previewHost = document.createElement('div')
    previewHost.id = 'timeline-library-preview'
    previewHost.hidden = true
    screen.append(previewHost)
    // In two beats, so nothing snaps: the bar and the transport fade out (and the grid starts to arrive over the stage) first, and only
    // then do they fold away, with the stage gliding into its new place under the grid.
    stage.append(host)
    document.body.classList.add('library-fading')
    window.setTimeout(() => {
      if (!libraryOpen || !host.isConnected) return
      glideStage(() => document.body.classList.add('library-mode'))
    }, 150)
    const abandon = () => {
      host.remove()
      previewHost.remove()
      document.body.classList.remove('library-mode', 'library-leaving', 'library-fading')
      libraryOpen = false
      setViewClips(false)
    }
    try {
      const { mountLibrary } = await import('./library')
      if (!libraryOpen || closing) { abandon(); return }
      let leaving = false
      leaveLibrary = () => {
        if (leaving) return
        leaving = true
        libraryLeaving = true
        // The grid and the saved picture fade out while the Timeline comes back underneath them: the footage is
        // already resuming when they are gone, and the bar and the transport fade back in.
        previewHost.querySelector('video')?.pause()
        glideStage(() => {
          document.body.classList.add('library-leaving')
          document.body.classList.remove('library-mode', 'library-fading')
        })
        delete document.body.dataset.saved
        libraryOpen = false
        setViewClips(false)
        if (previous.hidden) setVideoHidden(true)
        else if (!session || previous.id !== session.id) say('The earlier Timeline is no longer available.', 'problem')
        else if (previous.live && currentSession === previous.id) void goLive()
        else { resumeAfterSeek = previous.playing; seekTo(previous.at, false) }
        window.setTimeout(() => {
          disposeLibrary?.()
          disposeLibrary = undefined
          leaveLibrary = undefined
          host.remove()
          previewHost.remove()
          document.body.classList.remove('library-leaving')
          libraryLeaving = false
        }, 220)
      }
      disposeLibrary = mountLibrary(host, previewHost, $('library-context'), () => leaveLibrary?.(), (item) => void openShare(item.file))
    } catch {
      abandon()
      say('Could not open the library.', 'problem')
    }
  })

  // The lease: retention keeps the footage this window shows while it is open. Renewed halfway through
  // the lease, and dropped on close, so a closed preview never keeps old footage alive.
  const pinNow = async () => {
    if (closing || !session || session.availableEndMs <= session.availableStartMs) return 600000
    const id = session.id
    const start = session.availableStartMs
    const end = session.availableEndMs
    const pinned = await engine<{ id: string; ttl_ms?: number }>('timeline.pin', {
      session: id,
      owner: PIN_OWNER,
      start_ms: start,
      end_ms: end,
    })
    if (closing) {
      // The close's own unpin may already have run while this reply was in flight: release what this
      // call managed to take, or the session stays pinned for the whole lease with no window watching.
      if (pinned?.id) await engine('timeline.unpin', { owner: PIN_OWNER, session: id }).catch(() => undefined)
      return pinned?.ttl_ms ?? 600000
    }
    note(`pin: ${pinned?.id ?? 'not taken'} on ${id} [${start}, ${end})`)
    return pinned?.ttl_ms ?? 600000
  }
  // Renew halfway through the lease the engine granted, never on a period guessed here.
  const leaseMs = await pinNow()
  pinTimer = window.setInterval(() => void pinNow(), Math.max(2000, Math.floor(leaseMs / 2)))

  // Close: stop the picture, drop the media, give the lease back, ask the shell to end the process, and -
  // if the shell is already gone - close the window ourselves. Nothing is left decoding or downloading.
  // The reason is written where a page that shut itself can be read afterwards: a key that reached the
  // window after an overlay had already gone is invisible in a stack, and the log needs to say which way out.
  const close = async (reason: string) => {
    if (closing) return
    closing = true
    generation += 1  // a reply that lands after this may not reinstall a session or take a pin
    void invoke('timeline_leave').catch(() => {})  // I2: the window fades out over 90 ms while the page lets go of its work
    shareGeneration++
    const shareClosed = disposeShare?.()
    disposeShare = undefined
    if (shareClosed) await Promise.race([shareClosed, new Promise<void>((resolve) => window.setTimeout(resolve, 2000))])
    libraryOpen = false
    disposeLibrary?.()
    disposeLibrary = undefined
    if (scrubRaf) {
      cancelAnimationFrame(scrubRaf)
      scrubRaf = 0
    }
    window.clearTimeout(pollTimer)
    window.clearInterval(pinTimer)
    window.clearInterval(clockTimer)
    window.clearInterval(rulerTimer)
    // The screen grab, the sound probe and the panel's measurements end with the window: anything left
    // running asks the engine for work nothing draws.
    window.clearInterval(backdropTimer)
    window.clearInterval(loudPollTimer)
    window.clearTimeout(loudTimer)
    fitObserver.disconnect()
    if (playbackFrame) cancelAnimationFrame(playbackFrame)
    player.close()
    cancelSeekResume()
    video.pause()
    video.srcObject = null
    video.removeAttribute('src')
    video.load()
    note(`closing: ${reason}`)
    // Every lease this page took, not only the session on screen: browsing sessions can leave an
    // older one pinned, and a closed preview must keep nothing alive.
    const released = await engine<{ released: number }>('timeline.unpin', { owner: PIN_OWNER }).catch(() => undefined)
    note(`unpin: ${released?.released ?? 0} lease(s) released`)
    window.setTimeout(() => window.close(), 1000)
    await invoke('timeline_exit').catch(() => {})
  }
  $('close').addEventListener('click', () => {
    void close('back to game')
  })

  // A retry reloads the window the page is looking at; a chunk that expired or a fetch that failed then
  // costs one more attempt, and the picture is never left dead behind a message.
  retryButton.addEventListener('click', () => {
    if (!session) return
    hideFail()
    void player.load(session, wantMs)
    note(`retry at ${Math.round(wantMs)} ms`)
  })

  // ---- the transport ----
  const syncPlay = () => {
    const state = transportState({ paused: video.paused, waiting: playbackWaiting || playerBuffering || Boolean(scrubRaf), seeking: seekInFlight })
    $('stage-play').hidden = state !== 'paused' || videoHidden || !session || !player.loaded() || fail.classList.contains('on')
    stage.classList.toggle('paused', state === 'paused')
    stage.classList.toggle('waiting', state === 'waiting')
    const playPath = playButton.querySelector('path')!
    const nextIcon = state === 'playing' ? PAUSE : state === 'paused' ? PLAY : WAITING
    if (playPath.getAttribute('d') !== nextIcon) {
      playPath.setAttribute('d', nextIcon)
      // I8: the icon pops in when it changes (opacity and scale on the fast token).
      const icon = playButton.querySelector('svg')
      icon?.classList.remove('swap')
      void icon?.getBoundingClientRect()
      icon?.classList.add('swap')
    }
    playButton.title = playButton.ariaLabel = state === 'playing' ? 'Pause (Space)' : state === 'paused' ? 'Play (Space)' : 'Waiting for the picture'
    refreshPlaybackStatus()
  }
  const toggle = () => {
    if (videoHidden) setVideoHidden(false)
    if (seekInFlight) {
      if (resumeAfterSeek || !video.paused) {
        cancelSeekResume()
        leaveLive()
        freezeView()
        video.pause()
      } else {
        resumeAfterSeek = true
      }
      return
    }
    if (!video.paused) {
      leaveLive()
      // Pausing is the first review action: where the bar stands now is where it stays.
      freezeView()
      video.pause()
      return
    }
    if (player.isLive()) {
      const loaded = player.loaded()
      const at = player.positionMs()
      if (loaded && at >= loaded.startMs && at < loaded.endMs) {
        // Paused in the writer's own chunk: its frames are still buffered. Resume them, and let the
        // next poll append the bytes that arrived in the meantime, instead of seeking back to the
        // published edge (which threw away up to the open chunk's worth of picture).
        setLive(true)
        player.resumeLive()
        void video.play().catch(() => undefined)
        requestPoll()
        return
      }
      const edge = Math.min(at, hi())
      void player.seek(edge).then(() => video.play()).catch(() => {})
      return
    }
    const sel = currentSelection()
    const at = player.positionMs()
    if (previewingClip && sel && (at < sel.inMs || at >= sel.outMs)) {
      resumeAfterSeek = true
      seekTo(sel.inMs, false)
      return
    }
    void video.play().catch(() => {})
  }
  video.addEventListener('click', toggle)
  $('stage-play').addEventListener('click', toggle)
  video.addEventListener('waiting', () => {
    if (crossGap(player.positionMs())) return
    if (!liveMode && session && !scrubRaf && !seeking && !seekInFlight && !previewingClip && player.positionMs() >= hi() - 300) {
      video.pause()
      playbackWaiting = false
      syncPlay()
      return
    }
    playbackWaiting = true
    syncPlay()
    if (playbackFrame) cancelAnimationFrame(playbackFrame)
    playbackFrame = 0
    if (liveMode) say('BUFFERING')
  })
  video.addEventListener('playing', () => {
    playbackWaiting = false
    syncPlay()
    if (!document.hidden && !playbackFrame) playbackFrame = requestAnimationFrame(paintPlaybackFrame)
    if (chip.textContent === 'BUFFERING') say('')
  })
  video.addEventListener('pause', () => paintTime())
  playButton.addEventListener('click', toggle)
  // Ten seconds each way, asked for unclamped: `seekTo` clamps the seek and reads the ask for the Live
  // snap, so a frozen bar that ends past the published edge is not read as a jump backwards.
  $('back10').addEventListener('click', () => seekTo(wantMs - 10000))
  $('fwd10').addEventListener('click', () => seekTo(wantMs + 10000))
  // Live starts the writer's own chunk; each poll then hands the flushed bytes to the player.
  const goLive = async () => {
    // A press while Live is already playing is a no-op; a press while the writer is paused is the
    // owner asking to try again, so it falls through to the retry below.
    if (liveMode && !livePaused) return
    // LIVE is the way back to a following bar: the review view goes even if the writer is not readable yet.
    releaseView()
    cancelSeekResume()
    previewingClip = false
    paintClipMode()
    const intent = ++transportIntent
    const latest = await engine<TimelineGet>('timeline.get', {})
    if (closing || intent !== transportIntent) return
    if (!latest) {
      say('Could not reach Timeline. Try Live again.', 'problem')
      return
    }
    currentSession = latest.currentSession ?? ''
    if (currentSession && currentSession !== session?.id) await open(currentSession, 'current')
    if (closing || intent !== transportIntent) return
    if (!currentSession || !session || session.id !== currentSession) {
      pendingLive = true
      say(stateReason(latest) || 'Waiting for the current recording.', 'problem')
      // The ordinary poll cadence retries this: asking again at once spun on a writer whose index is
      // not readable yet, one refusal after another. `nextSession` ranks a pending Live first, so the
      // next poll opens the session and calls back here.
      return
    }
    pendingLive = false
    // LIVE asked for the writer's session: follow it again, and forget the pill that announced it.
    shownChosen = false
    dropAnnouncement()
    if (videoHidden) setVideoHidden(false)
    setSpeed(1)
    setLive(true)
    liveMissingSince = 0
    void player.startLive()
    say('BUFFERING')
    requestPoll()
  }
  liveButton.addEventListener('click', () => void goLive())
  const paintPlaybackFrame = () => {
    playbackFrame = 0
    if (closing || video.paused || playbackWaiting || document.hidden) return
    if (!scrubRaf && !seeking && !seekInFlight) {
      const at = player.positionMs()
      // MSE stalls at a gap with the element still unpaused: the frame loop steps over it as well.
      if (crossGap(at)) {
        playbackFrame = requestAnimationFrame(paintPlaybackFrame)
        return
      }
      if (liveMode && player.isLive()) {
        const before = visualEdge()
        displayedLiveEdge = liveViewClock.update(at, liveEdge())
        let movedView = before !== visualEdge()
        if (zoom && !glideRaf && !scrolledAway) {
          const next = followLiveView(zoom, at, lo(), visualEdge())
          movedView ||= next.fromMs !== zoom.fromMs || next.toMs !== zoom.toMs
          zoom = next
        }
        if (movedView) {
          const v = view()
          const drift = geometryView
            ? Math.max(Math.abs(v.fromMs - geometryView.fromMs), Math.abs(v.toMs - geometryView.toMs))
              * rulerWidth / Math.max(1, v.toMs - v.fromMs)
            : Infinity
          if (drift >= 0.25) paintLiveGeometry()
        }
      }
      paintHead(at)
    }
    playbackFrame = requestAnimationFrame(paintPlaybackFrame)
  }
  video.addEventListener('play', () => {
    syncPlay()
    // A rewind frozen under the owner catches up once with the footage published while it was paused;
    // growth after this appends as it plays, without moving the bar.
    if (frozenView && !liveMode && session && player.loaded()) void player.append(session.chunks)
    if (!playbackWaiting && !playbackFrame) playbackFrame = requestAnimationFrame(paintPlaybackFrame)
  })
  video.addEventListener('pause', () => {
    syncPlay()
    if (playbackFrame) cancelAnimationFrame(playbackFrame)
    playbackFrame = 0
  })
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden && !video.paused && !playbackWaiting && !playbackFrame) playbackFrame = requestAnimationFrame(paintPlaybackFrame)
  })
  video.addEventListener('timeupdate', () => {
    // While a seek is in flight the element still reports where the picture was: reading it here put
    // the needle back on the old moment before it landed on the chosen one.
    if (scrubRaf || seeking || seekInFlight) return
    wantMs = player.positionMs()
    // A playing head that reaches the end of a chunk with a gap after it steps over the pause.
    if (crossGap(wantMs)) return
    if (video.paused) paintHead(wantMs)
    // A playing clip wraps at its end, so Space previews exactly what Save keeps.
    const wrap = !liveMode && previewingClip ? clipLoopTarget(currentSelection(), wantMs, !video.paused) : undefined
    if (wrap !== undefined) {
      // A clip loop is the clip's own moment: wrapping to its start is never a request for Live.
      seekTo(wrap, false)
      return
    }
    // A zoomed or held bar follows the picture as it plays past its edge.
    if (!liveMode && !video.paused && !glideRaf) followPlayhead(wantMs)
  })
  // Both of these fire while a seek settles, when the element's own position is still the old one: the
  // playhead is painted from the moment asked for, never from what the element reports mid-seek.
  video.addEventListener('durationchange', () => { if (video.paused) paintHead(wantMs); syncPlay() })
  video.addEventListener('seeked', () => { if (video.paused || seeking || scrubRaf || seekInFlight) paintHead(wantMs); syncPlay() })

  // The two handles drag the clip's ends with the pointer, and clamp to what can be saved.
  let snapTargets: number[] = []
  const dragEdge = (edge: HTMLElement, which: 'in' | 'out') => {
    let startX = 0
    let moved = false
    const move = (event: PointerEvent) => {
      if (!session) return
      const sel = currentSelection()
      if (!sel) return
      const raw = Math.round(momentAt(bar, event))
      const at = event.shiftKey ? raw : snapMs(raw, snapTargets, viewSpan(), cut.clientWidth)
      const bounded = which === 'in' ? Math.min(at, sel.outMs) : Math.max(at, sel.inMs)
      selection = selectionClamp(which === 'in' ? bounded : sel.inMs, which === 'out' ? bounded : sel.outMs, lo(), liveEdge())
      paintSelection()
      // The picture follows the handle, so the cut is made on what is seen, not on a number.
      player.seekPreview(bounded)
      paintHead(bounded)
    }
    edge.addEventListener('pointerdown', (event) => {
      if (event.button !== 0) return
      leaveLive()
      freezeView()
      cancelSeekResume()
      video.pause()
      edge.setPointerCapture(event.pointerId)
      startX = event.clientX
      moved = false
      event.preventDefault()
      event.stopPropagation()
      // The playhead as it stood and every marker: the end sticks to them (Shift drags freely).
      snapTargets = [wantMs, ...markersList.map((marker) => marker.atMs)]
      note(`drag ${which} handle`)
    })
    edge.addEventListener('pointermove', (event) => {
      if (!edge.hasPointerCapture(event.pointerId) || !session) return
      moved ||= event.clientX !== startX
      if (moved) move(event)
    })
    for (const end of ['pointerup', 'pointercancel']) {
      edge.addEventListener(end, (event) => {
        const pointer = event as PointerEvent
        if (!edge.hasPointerCapture(pointer.pointerId)) return
        if (end === 'pointerup' && (moved || pointer.clientX !== startX)) move(pointer)
        edge.releasePointerCapture(pointer.pointerId)
        // The clip's end may sit on the newest footage on purpose: that is a cut, not a return to Live.
        if (end === 'pointerup') seekTo(wantMs, false)
      })
    }
  }
  dragEdge(inEdge, 'in')
  dragEdge(outEdge, 'out')

  // A wheel notch is a second, against the scroll: rolling down walks back, up walks forward. Shift
  // takes ten seconds, the same as the arrow keys; Ctrl zooms the bar around the pointer. It never scrolls the page.
  const wheel = (event: WheelEvent) => {
    if (!session || libraryOpen) return
    event.preventDefault()
    if (event.ctrlKey) {
      const anchor = range.contains(event.target as Node) ? momentAt(bar, event) : wantMs
      zoomBy(event.deltaY < 0 ? 1.5 : 1 / 1.5, anchor)
      return
    }
    const from = scrubRaf ? scrubTarget : wantMs
    // The ask goes in unclamped: a frozen bar can end inside the writer's open chunk, past the published
    // edge the seek clamps to, and wheeling to that end is asking for Live. `scrubTo` clamps the seek.
    scrubTo(from + wheelSeconds(event.deltaY, event.shiftKey) * 1000)
  }
  for (const target of [range, stage]) target.addEventListener('wheel', wheel, { passive: false })

  // The moment under the pointer, with its picture from the thumbnail cache once the pointer rests, so a
  // person finds the moment by looking instead of by guessing.
  let bubbleTimer = 0
  let bubbleToken = 0
  let bubbleValidAfter = 0
  let bubbleDisplayed = 0
  let bubbleKey = ''
  let bubbleBusy = false
  let bubbleWanted: { id: string; at: number; token: number } | undefined
  const requestBubble = () => {
    bubbleTimer = 0
    if (bubbleBusy || !bubbleWanted) return
    const wanted = bubbleWanted
    bubbleWanted = undefined
    bubbleBusy = true
    void attempt<{ thumbs: { file: string }[] }>('timeline.thumbs', { session: wanted.id, times_ms: [Math.round(wanted.at)] }).then((reply) => {
      const file = reply.data?.thumbs?.[0]?.file
      if (wanted.token < bubbleValidAfter || bubble.hidden || session?.id !== wanted.id) return
      if (!file) {
        if (wanted.token === bubbleToken) { bubbleImg.hidden = true; bubbleTime.textContent = 'PREVIEW UNAVAILABLE' }
        return
      }
      const image = new Image()
      image.onload = () => {
        if (wanted.token < bubbleValidAfter || wanted.token < bubbleDisplayed || bubble.hidden || session?.id !== wanted.id) return
        bubbleDisplayed = wanted.token
        bubbleImg.src = image.src
        bubbleImg.hidden = false
      }
      image.onerror = () => {
        if (wanted.token === bubbleToken && !bubble.hidden) { bubbleImg.hidden = true; bubbleTime.textContent = 'PREVIEW UNAVAILABLE' }
      }
      image.src = mediaUrl(file)
    }).finally(() => {
      bubbleBusy = false
      if (bubbleWanted && !bubbleTimer) bubbleTimer = window.setTimeout(requestBubble, 140)
    })
  }
  // The picture follows the pointer with a short ease instead of jumping: a frame loop that runs only while it
  // has ground to cover, so a resting pointer costs nothing.
  let bubbleTo = 0
  let bubbleX = 0
  let bubbleRaf = 0
  let bubbleLast = 0
  const bubbleMove = () => {
    bubble.style.transform = `translate3d(${bubbleX.toFixed(1)}px, 0, 0) translateX(-50%)`
  }
  const bubbleStep = (now: number) => {
    const dt = Math.min(48, now - bubbleLast)
    bubbleLast = now
    bubbleX += (bubbleTo - bubbleX) * (1 - Math.exp(-dt / 40))
    if (Math.abs(bubbleTo - bubbleX) < 0.4) bubbleX = bubbleTo
    bubbleMove()
    bubbleRaf = bubbleX === bubbleTo ? 0 : requestAnimationFrame(bubbleStep)
  }
  const bubbleAt = (event: PointerEvent) => {
    if (!session) return
    const at = momentAt(bar, event)
    bubbleTo = event.clientX - range.getBoundingClientRect().left
    if (bubble.hidden || matchMedia('(prefers-reduced-motion: reduce)').matches) {
      bubbleX = bubbleTo
      bubbleMove()
    } else if (!bubbleRaf) {
      bubbleLast = performance.now()
      bubbleRaf = requestAnimationFrame(bubbleStep)
    }
    bubble.hidden = false
    const id = session.id
    const recorded = isRecorded(session.chunks, at)
    const key = `${id}:${Math.floor(at / 1000)}:${recorded}`
    if (key === bubbleKey) return
    bubbleKey = key
    // The hover is the picture alone; words appear only when there is no picture to show.
    bubbleTime.textContent = ''
    const token = ++bubbleToken
    bubbleWanted = undefined
    if (!recorded) {
      bubbleValidAfter = token + 1
      bubbleImg.hidden = true
      bubbleTime.textContent = 'NO FOOTAGE'
      return
    }
    bubbleWanted = { id, at, token }
    if (!bubbleTimer && !bubbleBusy) bubbleTimer = window.setTimeout(requestBubble, 140)
  }
  const bubbleOff = () => {
    bubbleToken += 1
    bubbleValidAfter = bubbleToken + 1
    bubbleDisplayed = 0
    bubbleKey = ''
    bubbleWanted = undefined
    window.clearTimeout(bubbleTimer)
    bubbleTimer = 0
    cancelAnimationFrame(bubbleRaf)
    bubbleRaf = 0
    bubble.hidden = true
    bubbleImg.hidden = true
  }
  // Child handles bubble pointer movement to the bar; entering one must not dismiss its preview.
  bar.addEventListener('pointermove', bubbleAt)
  bar.addEventListener('pointerleave', bubbleOff)

  // The upper track is the playhead's alone: a press or a drag moves it, and nothing here trims.
  const aim = (at: number) => {
    wantMs = at
    paintHead(at)
    player.seekPreview(at)
  }
  let seeking = false
  let seekFrame = 0
  let seekTarget = 0
  /** Where the head drag began: dragging from the newest footage to it is a nudge, not a return to Live. */
  let dragFrom = 0
  const queueAim = (at: number) => {
    seekTarget = at
    if (!seekFrame) seekFrame = requestAnimationFrame(() => { seekFrame = 0; aim(seekTarget) })
  }
  strip.addEventListener('pointerdown', (event) => {
    if (!session || event.button !== 0) return
    strip.setPointerCapture(event.pointerId)
    seeking = true
    dragFrom = wantMs
    leaveLive()
    // The drag walks the bar it started on: the stretch is frozen before the pointer maps to it.
    freezeView()
    queueAim(momentAt(bar, event))
    event.preventDefault()
  })
  strip.addEventListener('pointermove', (event) => {
    if (!seeking || !strip.hasPointerCapture(event.pointerId)) return
    queueAim(momentAt(bar, event))
  })
  for (const end of ['pointerup', 'pointercancel']) {
    strip.addEventListener(end, (event) => {
      const pointer = event as PointerEvent
      if (strip.hasPointerCapture(pointer.pointerId)) strip.releasePointerCapture(pointer.pointerId)
      if (!seeking) return
      seeking = false
      if (seekFrame) { cancelAnimationFrame(seekFrame); seekFrame = 0 }
      if (end === 'pointercancel') return
      // The bar's own right end is the newest footage it can show, however long the session is: at a
      // 30-minute span a few pixels are seconds, and a tolerance measured in time would miss the end a
      // person aimed at. The drawn end is what is asked for; `seekTo` decides whether that is Live.
      const at = momentAt(bar, pointer)
      seekTo(pointer.clientX >= bar.getBoundingClientRect().right - 4 ? view().toMs : at, true, dragFrom)
    })
  }

  // The clip has its own track below: a drag inside the clip moves it, a drag outside makes a new one,
  // and neither can move the playhead.
  let trimAnchor = 0
  let trimMoved = false
  let movingSelection: Selection | undefined
  const insideSelection = (at: number) => {
    const sel = currentSelection()
    return Boolean(sel && sel.outMs > sel.inMs && at > sel.inMs && at < sel.outMs)
  }
  cut.addEventListener('pointerdown', (event) => {
    if (!session || event.button !== 0) return
    leaveLive()
    // A range is drawn on the stretch on screen: it must not rescale under the drag.
    freezeView()
    cancelSeekResume()
    video.pause()
    cut.setPointerCapture(event.pointerId)
    trimAnchor = momentAt(cut, event)
    movingSelection = insideSelection(trimAnchor) ? currentSelection() : undefined
    trimMoved = false
    event.preventDefault()
  })
  cut.addEventListener('pointermove', (event) => {
    if (!cut.hasPointerCapture(event.pointerId)) {
      cut.style.cursor = insideSelection(momentAt(cut, event)) ? 'grab' : 'crosshair'
      return
    }
    const at = momentAt(cut, event)
    // A hair of travel is a click, not a range, judged in the track's own pixels.
    if (!trimMoved && !dragMoved(trimAnchor, at, viewSpan(), cut.clientWidth)) return
    if (!trimMoved && !selection) clipEdge = liveEdge()
    trimMoved = true
    cut.style.cursor = movingSelection ? 'grabbing' : 'crosshair'
    selection = movingSelection
      ? moveSelection(movingSelection, at - trimAnchor, lo(), liveEdge())
      : trimRange(trimAnchor, at, lo(), liveEdge())
    paintSelection()
  })
  for (const end of ['pointerup', 'pointercancel']) {
    cut.addEventListener(end, (event) => {
      const pointer = event as PointerEvent
      if (cut.hasPointerCapture(pointer.pointerId)) cut.releasePointerCapture(pointer.pointerId)
      cut.style.cursor = insideSelection(momentAt(cut, pointer)) ? 'grab' : 'crosshair'
      movingSelection = undefined
      if (end === 'pointercancel') return
      if (!trimMoved) { seekTo(momentAt(bar, pointer), false); return }
      const sel = currentSelection()
      if (!sel) return
      say(`Clip ${clock(sel.outMs - sel.inMs)} ready: Save (Enter)`)
      note(`trim dragged: in ${sel.inMs}, out ${sel.outMs}`)
    })
  }

  // ---- frame step and speed ----
  // One frame is what the picture last advanced by, measured while it plays; sixty a second until it has.
  let frameMs = 1000 / 60
  if (typeof video.requestVideoFrameCallback === 'function') {
    let lastMedia = -1
    const onFrame = (_now: number, meta: { mediaTime: number }) => {
      const gap = (meta.mediaTime - lastMedia) * 1000
      if (lastMedia >= 0 && gap > 2 && gap < 80) frameMs = gap
      lastMedia = meta.mediaTime
      video.requestVideoFrameCallback(onFrame)
    }
    video.requestVideoFrameCallback(onFrame)
  }
  const stepFrame = (direction: -1 | 1) => {
    if (!session) return
    cancelSeekResume()
    video.pause()
    // A frame step is a frame step even on the newest one: it never turns into Live.
    seekTo(wantMs + direction * frameMs, false)
  }

  const SPEEDS = [0.5, 1, 1.5, 2]
  const speedButton = $<HTMLButtonElement>('speed')
  const setSpeed = (rate: number) => {
    if (rate !== 1) leaveLive()
    video.defaultPlaybackRate = rate
    video.playbackRate = rate
    speedButton.textContent = `${rate}\u00d7`
    speedButton.dataset.rate = String(rate)
  }
  const speedBy = (direction: -1 | 1) => {
    const at = SPEEDS.indexOf(video.playbackRate)
    const next = SPEEDS[Math.min(SPEEDS.length - 1, Math.max(0, (at < 0 ? 1 : at) + direction))]
    setSpeed(next)
    say(`Speed ${next}\u00d7`)
  }
  speedButton.addEventListener('click', () => {
    const at = SPEEDS.indexOf(video.playbackRate)
    setSpeed(SPEEDS[(at < 0 ? 1 : at + 1) % SPEEDS.length])
  })

  // ---- the keys: one keyboard reaches everything in the window ----
  // Chromium turns :focus-visible on for the focused element as soon as a key is pressed, so the
  // live value cannot tell a mouse focus from a keyboard one. Read it once, when the focus lands
  // (and once more on the next frame, because Chromium settles the flag after `focusin`).
  let keyboardFocus = false
  const readKeyboardFocus = (target: EventTarget | null) => {
    keyboardFocus = target instanceof Element && target.matches(':focus-visible')
  }
  document.addEventListener('focusin', (event) => {
    readKeyboardFocus(event.target)
    requestAnimationFrame(() => { if (event.target === document.activeElement) readKeyboardFocus(event.target) })
  }, true)
  const onKey = (event: KeyboardEvent) => {
    if (libraryOpen || document.body.classList.contains('sharing')) return
    if (event.altKey || event.metaKey) return
    const target = event.target as HTMLElement | null
    const tag = target?.tagName ?? ''
    // A picker or a field has its own keys, Esc included: the page must not close under an open picker.
    if (/^(SELECT|INPUT|TEXTAREA)$/.test(tag)) return
    const key = event.key
    // Holding a key must not repeat an edit: a held M used to add about thirty markers a second, and
    // a held H toggled the window height the same way. The arrows keep repeating, because seeking is
    // what a held arrow is for.
    if (event.repeat && key !== 'ArrowLeft' && key !== 'ArrowRight') return
    if (event.ctrlKey) {
      if (key.toLowerCase() === 'c' && (selection || saved)) {
        event.preventDefault()
        void after('copy')
      }
      return
    }
    if (key === 'Escape') {
      event.preventDefault()
      if (!menu.hidden) return closeMenu()
      if (escapeAction(Boolean(selection)) === 'clear') return cancelClip()
      void close('Escape')
      return
    }
    // A keyboard-focused button keeps native activation. After a mouse click, Space still controls playback.
    if (tag === 'BUTTON' && (key === 'Enter' || (key === ' ' && keyboardFocus))) return
    const actions: Record<string, () => void> = {
      ' ': toggle,
      ArrowLeft: () => seekTo(wantMs - (event.shiftKey ? 10000 : 1000)),
      ArrowRight: () => seekTo(wantMs + (event.shiftKey ? 10000 : 1000)),
      Home: () => seekTo(lo()),
      End: () => seekTo(hi() - OPEN_BEHIND_MS, false),
      Enter: () => (selection ? void after(event.shiftKey ? 'game' : 'stay') : openClip()),
      c: openClip,
      i: () => setEdge('in'),
      o: () => setEdge('out'),
      e: () => void after('edit'),
      S: () => void after('share'),
      m: () => void addMarker(),
      ',': () => stepFrame(-1),
      '.': () => stepFrame(1),
      '[': () => speedBy(-1),
      ']': () => speedBy(1),
      M: toggleMute,
      l: () => void goLive(),
      h: () => setVideoHidden(!videoHidden),
      '+': () => zoomBy(2),
      '=': () => zoomBy(2),
      '-': () => zoomBy(0.5),
      '0': () => resetWindow(),
    }
    const action = actions[key] ?? actions[key.toLowerCase()]
    if (!action || (key === 'S' && !event.shiftKey)) return
    event.preventDefault()
    action()
  }
  document.addEventListener('keydown', onKey)

  /** The session's markers, on its own clock, from the index the engine keeps. */
  const loadMarkers = async (wanted: TimelineSession) => {
    const reply = await engine<SessionGet>('session.get', { id: wanted.id })
    if (!reply || wanted.id !== session?.id) return
    markersList = (reply.markers ?? [])
      .map((marker) => ({
        // The index keeps a 32-bit tick clock: the difference is taken in that clock so a wrap cannot flip it.
        atMs: Math.max(0, ((marker.atMs - reply.startedMs) | 0)),
        kind: marker.kind,
        label: marker.label,
      }))
      .sort((a, b) => a.atMs - b.atMs)
    if (selectedMarker) {
      selectedMarker = markersList.find((marker) => marker.atMs === selectedMarker?.atMs &&
        marker.kind === selectedMarker?.kind && marker.label === selectedMarker?.label)
    }
    paintMarkerAction()
    paintMarkers()
    // A gap's label can name the game, which is one of the markers: the labels follow the pins.
    paintGaps()
    note(`markers: ${markersList.length} in ${wanted.id}`)
  }

  /** Why nothing new is arriving, in the page's words; empty while the writer is doing its job. */
  const stateReason = (reply: TimelineGet): string => {
    if (!reply.enabled) return 'Timeline is off: turn it on in Settings to keep footage.'
    const state = (reply.state ?? '').toLowerCase()
    if (state === 'paused_no_game') return 'Waiting for a game. Earlier footage can still be played.'
    if (state === 'paused_idle') return 'Idle: no keyboard or mouse for a while. It records again when you are back. Earlier footage can still be played.'
    if (/stop|error|fail/.test(state)) return 'Timeline is stopped, so no new footage is arriving. What was kept still plays here.'
    if (/paus/.test(state)) return 'Timeline is paused, so no new footage is arriving. What was kept still plays here.'
    return ''
  }
  const paintState = (state: string) => {
    writerState = state
    const writer = state === 'writing' ? 'Recording' : state === 'paused_no_game' ? 'Waiting for a game' : state === 'paused_idle' ? 'Idle' : state.startsWith('paused') ? 'Paused' : (state || 'unknown').replace(/_/g, ' ')
    const text = session && session.id !== currentSession ? `Earlier session · ${writer}` : writer
    if (stateText.textContent !== text) {
      swapFade(stateText)
      stateText.textContent = text
    }
    stateText.hidden = false
    stateText.dataset.state = state
    refreshPlaybackStatus()
    // A writer state change can turn a hole from NO FOOTAGE into NO GAME (or back) while the bar is
    // frozen: relabel the gaps without touching the view or the picture.
    if (session) paintGaps()
  }

  /**
   * One session: its footage, its markers, its lease, and the player on it - at the newest moment.
   * `explicit` is an id that was asked for (a launch session, the deck's history): it falls back to the
   * newest session with a notice when the footage is gone. `current` is the writer's session, whose index
   * may not be readable yet: it waits, because an older session is not a substitute for it.
   */
  const open = async (id: string, kind: 'explicit' | 'current' = 'explicit', fallback = true) => {
    if (closing) return
    const token = ++generation
    if (session && id && session.id !== id) {
      // The player drops its live tail with the old session, but the page's own live flag would stay
      // set: LIVE then reads as pressed and refuses every press. Leave Live before the session goes.
      leaveLive(false)
      hideStill()
      bubbleOff()
      cancelSeekResume()
      if (scrubRaf) cancelAnimationFrame(scrubRaf)
      scrubRaf = 0
      player.clearSession()
      session = undefined
      delete range.dataset.session
    }
    const reply = await engine<TimelineGet>('timeline.get', id ? { session: id } : {})
    if (token !== generation || closing) return
    if (!reply) {
      if (kind === 'current') {
        // The writer's index is not readable yet: this is waiting, not a reason to show yesterday's session.
        showWaiting('The first chunk is still being written.')
        return
      }
      // The command itself was refused: the session was erased or its footage expired. Fall back to
      // the newest session the page last heard of once, rather than asking for the dead id every poll.
      if (fallback) {
        const pick = pickSession(knownSessions, id)
        if (pick.id && pick.id !== id) {
          say('That footage is gone. Showing the newest session.', 'problem')
          await open(pick.id, 'explicit', false)
          return
        }
      }
      session = undefined
      player.clearSession()
      showFail('That footage is gone.', true)
      say('That footage was deleted: pick another session.', 'problem')
      return
    }
    fillChooser(reply.sessions ?? [])
    currentSession = reply.currentSession ?? ''
    dropAnnouncement()
    retentionMinutes = reply.minutes
    paintRetention()
    paintState(reply.state)
    const reason = stateReason(reply)
    if (!reply.session) {
      showWaiting(reason)
      return
    }
    const next: TimelineSession = {
      id: reply.session,
      title: (reply.sessions ?? []).find((entry) => entry.id === reply.session)?.title ?? reply.session,
      availableStartMs: reply.availableStartMs,
      availableEndMs: reply.availableEndMs,
      chunks: reply.chunks ?? [],
    }
    if (session?.id !== next.id) {
      leaveLive(false)
      hideStill()
      bubbleOff()
      cancelSeekResume()
      if (scrubRaf) cancelAnimationFrame(scrubRaf)
      scrubRaf = 0
    }
    liveViewClock.reset()
    displayedLiveEdge = undefined
    player.updateSession(next)
    sessionRevision++
    selection = undefined
    previewingClip = false
    stopGlide()
    zoom = undefined
    scrolledAway = false
    // A session opened fresh starts at its own newest moment: there is no older stretch to hold.
    frozenView = undefined
    clipZoomed = false
    clipPreviousZoom = undefined
    markersList = []
    selectedMarker = undefined
    paintMarkerAction()
    saved = undefined
    // The moment that just happened: Alt+4, C, Enter saves it. Opening on the oldest footage of a
    // two-hour session sent every person on a two-hour scrub for what they came for.
    wantMs = Math.max(next.availableStartMs, next.availableEndMs - OPEN_BEHIND_MS)
    card.classList.remove('on')
    fillChooser(knownSessions)
    draw(next)
    paintState(reply.state)
    if (reason) say(reason, 'problem')
    // A session with nothing published yet is the normal first moments of a recording, never a failure.
    if (next.chunks.length === 0) showWaiting(reason)
    void player.load(next, wantMs)
    void loadMarkers(next)
    void pinNow()
    loudChunks = []
    loudRange = undefined
    loudSeen = ''
    loudPending = 0
    void refreshLoud()
    note(`open: ${next.id}, ${next.chunks.length} chunk(s), ${next.availableEndMs} ms, at ${wantMs} ms, ${reply.state}`)
  }

  // The footage the writer is still making appears by asking: `timeline.changed` is a writer-state
  // change, never one per chunk, so the page polls its own session and extends the window in place.
  // Nothing here resets the playhead or the clip: growth only adds. With no session at all (Alt+4
  // before anything was recorded) the same poll adopts the first one that appears.
  const poll = async () => {
    if (closing) return
    const wanted = session?.id ?? ''
    const token = generation
    // The list (every index on the disk) is read every few polls; the rest read the one session shown.
    const listed = !wanted || Date.now() - lastListAt >= LIST_INTERVAL_MS
    if (listed) lastListAt = Date.now()
    const reply = await engine<TimelineGet>('timeline.get', listed ? { session: wanted } : { session: wanted, list: false })
    if (!reply || token !== generation) {
      if (!reply && wanted) {
        if (!listed) {
          // The shown session is gone (or its index just closed): ask with the list next time.
          lastListAt = 0
          return
        }
        // open() owns the deleted-footage fallback and its notice; the poll never guesses a replacement.
        await open(wanted, 'explicit')
      }
      return
    }
    paintState(reply.state)
    const previousCurrent = currentSession
    currentSession = reply.currentSession ?? ''
    paintState(reply.state)
    retentionMinutes = reply.minutes
    paintRetention()
    if (reply.sessions) fillChooser(reply.sessions)
    // A manual buffer stop closes the writer's session. The index closes a moment after the stop,
    // so the chip waits for the summary that says the session really ended: an internal pause
    // (battery, lock, rebuild) keeps it open and must not wear the ended word.
    if (previousCurrent && !currentSession && reply.state === 'stopped' && session) {
      endedCheckId = session.id
      lastListAt = 0
    }
    const stoppedSummary = endedCheckId && reply.sessions
      ? reply.sessions.find((entry) => entry.id === endedCheckId)
      : undefined
    if (stoppedSummary?.endedAt) {
      say('TIMELINE ENDED · BUFFER STOPPED', 'ended')
      endedCheckId = ''
    } else if (endedCheckId && currentSession && currentSession === endedCheckId) {
      endedCheckId = ''
    }
    const endedByStop = Boolean(stoppedSummary?.endedAt)
    // The adoption rule is the pure one: `reply.session` is the newest session on disk when no id was
    // asked for, so it is never the source here; only `current_session` - the writer's own - is.
    const action = nextSession({
      shownId: session?.id ?? '',
      currentSession,
      chosen: shownChosen,
      // A clip or a held view is work on the session shown. A paused picture is not: the session that ended is
      // paused too, and a stopped session must never stay up inside the new one.
      reviewing: Boolean(selection) || Boolean(frozenView),
      pendingLive,
    })
    if (action === 'open-current') {
      note(`poll: the writer is on ${currentSession}; opening it`)
      await open(currentSession, 'current')
      // Only a session that actually landed calls LIVE back: a refused open (the writer's index is not
      // readable yet) would otherwise ask again at once, and the ordinary cadence is the retry.
      if (pendingLive && session?.id === currentSession) void goLive()
      return
    }
    if (action === 'announce') {
      // A new session under a review is announced, never adopted: the shown session, its pin and its clip stay.
      if (announcedSession !== currentSession) {
        announcedSession = currentSession
        say('A NEW TIMELINE STARTED · LIVE', 'new-session')
      }
    } else if (announcedSession && announcedSession !== currentSession) {
      dropAnnouncement()
    }
    if (pendingLive && currentSession && currentSession === session?.id) {
      pendingLive = false
      void goLive()
      return
    }
    if (!session) {
      showWaiting(stateReason(reply))
      return
    }
    if (listed) void loadMarkers(session)
    const previous = session
    const next: TimelineSession = {
      ...previous,
      availableStartMs: reply.availableStartMs,
      availableEndMs: reply.availableEndMs,
      chunks: reply.chunks ?? [],
    }
    player.updateSession(next)
    const oldLiveEnd = player.loaded()?.endMs
    // Live: the writer's own chunk grows without a chunk being published, so the feed runs before
    // the growth guard.
    if (liveMode && reply.live) {
      livePaused = false
      liveMissingSince = 0
      await player.live(reply.live.file, reply.live.startMs, reply.live.bytes)
        .catch((error) => {
          note(`live: ${String(error)}`)
          leaveLive()
          showFail('Live playback stopped. Retry, or choose another moment.', true)
        })
      if (liveMode && video.paused) paintHead(player.positionMs())
    } else if (liveMode && !reply.live) {
      // A writer pause is not the end of Live: keep the mode so the next poll that hands back a live
      // chunk continues on its own. Only a stopped buffer ends it, because the session has closed.
      const reason = stateReason(reply)
      if (reply.state === 'stopped') {
        leaveLive()
        say(endedByStop ? 'TIMELINE ENDED · BUFFER STOPPED' : reason, endedByStop ? 'ended' : 'problem')
      } else if (reason) {
        // A passing state word, never a problem: the same bar carries on when the writer returns.
        livePaused = true
        liveMissingSince = 0
        say(reason, 'state')
      } else {
        liveMissingSince ||= Date.now()
        if (Date.now() - liveMissingSince >= 10000) {
          leaveLive()
          say('Live footage is not available for this session.', 'problem')
        } else say('BUFFERING')
      }
    }
    const movedStart = previous.chunks.length > 0 && next.availableStartMs > previous.availableStartMs
    const grown = movedStart || next.availableEndMs !== previous.availableEndMs || next.chunks.length !== previous.chunks.length
    if (!grown) {
      if (liveMode && player.loaded()?.endMs !== oldLiveEnd) paintChunks(session)
      return
    }
    session = next
    if (movedStart) {
      // Retention took the oldest footage: the clip and the playhead are cut to what is left.
      const notice = retentionNotice(previous.availableStartMs, next.availableStartMs)
      if (notice) say(notice, 'problem')
      note(`retention: start ${previous.availableStartMs} -> ${next.availableStartMs} ms`)
      if (selection) {
        const moved = selectionOnSpan(selection, next.availableStartMs, liveEdge())
        if (moved.selection.outMs > moved.selection.inMs) {
          if (moved.trimmed) selection = moved.selection
        } else {
          clearClip()
        }
        if (moved.trimmed) note('selection: trimmed to the footage that is still here')
      }
      // Retention only moves the start: the playhead and a frozen bar's end are not pulled back toward
      // the published edge, because the picture can sit in the writer's open chunk, ahead of it.
      wantMs = scrubBy(wantMs, 0, lo(), Math.max(liveMode ? liveEdge() : hi(), wantMs))
      // The footage under a frozen bar moved forward: the view is pulled inside what is left. Only the
      // range that expired is trimmed; nothing here shifts the clip by itself, and the end never shrinks.
      if (frozenView) frozenView = clampView(frozenView, lo(), Math.max(hi(), frozenView.toMs))
      paintAll()
      if (!liveMode) void player.load(next, wantMs)
    } else if (frozenView && !liveMode) {
      // A frozen review view is never repainted for growth: the bar, the clip and its handles stay where
      // the owner put them. The readout says what is waiting; a picture that is playing may take it now,
      // and a paused one is appended when it plays.
      if (!player.loaded()) void player.load(next, wantMs)
      else if (!video.paused) void player.append(next.chunks)
      paintTime()
      note(`poll: ${next.chunks.length} chunk(s) held under a frozen view`)
      return
    } else {
      // Growth only extends: the playhead stays where the user left it.
      paintAll()
      // Live owns the picture while it runs: a published window would fight it for the same buffer.
      if (!liveMode) {
        // A session that was still waiting for its first chunk has no window yet: this growth is its
        // picture, and appending to a player that never opened would leave the stage empty.
        if (player.loaded()) void player.append(next.chunks)
        else void player.load(next, wantMs)
      }
    }
    note(`poll: ${next.chunks.length} chunk(s), end ${next.availableEndMs} ms`)
  }
  // One poll at a time: slow disk reads or media appends cannot queue an unbounded stream of work.
  const schedule = (delay = liveMode ? LIVE_POLL_MS : POLL_MS) => {
    window.clearTimeout(pollTimer)
    pollTimer = window.setTimeout(async () => {
      polling = true
      try { await poll() } finally {
        polling = false
        if (!closing) {
          const immediate = pollAgain
          pollAgain = false
          schedule(immediate ? 0 : undefined)
        }
      }
    }, delay)
  }
  const requestPoll = () => {
    if (polling) pollAgain = true
    else schedule(0)
  }
  schedule()
  rulerTimer = window.setInterval(() => {
    if (!document.hidden && session && !libraryOpen) { paintRuler(); paintTime() }
  }, 1000)

  /**
   * Launch with no id asked for: the writer's own session, or the waiting state. An empty id is never
   * asked for: it makes the engine answer with its selected session, the newest one on disk when nothing
   * is being written.
   */
  const adoptCurrent = async () => {
    const reply = await engine<TimelineGet>('timeline.get', {})
    if (closing) return
    if (!reply) {
      showWaiting('Timeline is not answering yet.')
      return
    }
    fillChooser(reply.sessions ?? [])
    paintState(reply.state)
    retentionMinutes = reply.minutes
    paintRetention()
    currentSession = reply.currentSession ?? ''
    if (!currentSession) {
      showWaiting(stateReason(reply))
      return
    }
    await open(currentSession, 'current')
    // Alt+4 while the buffer runs still lands live: that is the session the writer is making.
    void goLive()
  }

  // The writer's state and the markers, when they change: the page reads footage on its own poll.
  await listen('engine_event', (event) => {
    try {
      const frame = JSON.parse(event.payload) as { event?: string; data?: { state?: string; id?: string } }
      if (frame.event === 'timeline.changed' && frame.data?.state) paintState(frame.data.state)
      else if (frame.event === 'markers.changed' && session && frame.data?.id === session.id) void loadMarkers(session)
      else if (frame.event === 'timeline.close') void close('timeline.close')
    } catch {
      /* not ours */
    }
  }).catch((error) => note(`events: ${String(error)}`))

  // A resized window (Hide video, a DPI change) moves every percentage: the labels and the clip readout follow.
  window.addEventListener('resize', () => paintAll())
  // The card follows the recording's shape; until a size is known it is 16:9.
  const fitFrame = () => {
    if (video.videoWidth && video.videoHeight) frame.style.setProperty('--ar', String(video.videoWidth / video.videoHeight))
  }
  video.addEventListener('loadedmetadata', fitFrame)
  video.addEventListener('resize', fitFrame)

  // What the window was opened on: a session the engine asked for by id, or the writer's own session.
  // With no id there is no fallback to the newest session on disk: that is yesterday's footage.
  const launch = await invoke<{ session: string }>('get_launch').catch(() => ({ session: '' }))
  shownChosen = Boolean(launch.session)
  if (launch.session) await open(launch.session)
  else await adoptCurrent()
  void engine('hello', { client: 'timeline' })

  // The window is on screen: say so once, so a probe knows the shell got this far.
  note(`page ready: ${bar.clientWidth} px of range bar`)
}

void main()
