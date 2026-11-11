// The clip preview: a clip clicked in the engine's panel plays here, in the editor's own process (the
// panel is drawn natively and plays no video). Edit turns this window into the editor with the clip
// loaded; closing it ends the process and gives back everything it used.
//
// It also plays the Timeline's footage: a session clicked in the panel's timeline tab opens its media
// folder, which is not one file but a run of short chunks. SessionView feeds those to the Timeline's own
// player, and Trim opens the Timeline over the game, where a clip is cut and saved.
//
// The picture fills the window and the chrome floats over it in three glass clusters: identity at the
// top left, actions at the top right, transport at the foot. A blurred copy of the picture fills the
// bands beside it, so a 16:9 clip in a taller window never sits between two black fields.

import clsx from 'clsx'
import { useCallback, useEffect, useMemo, useRef, useState, type MouseEvent as ReactMouseEvent, type MutableRefObject, type ReactNode } from 'react'
import { WindowKeys } from '../components/chrome'
import { Icon } from '../components/icons'
import { Button, IconButton, Slider, Spinner } from '../components/ui'
import { engine, type MediaItem, type TimelineFootage } from '../lib/api'
import { mediaUrl, onShell, shell, transport } from '../lib/bridge'
import { duration, stem } from '../lib/format'
import { runEngine } from '../lib/media'
import { ambientDue, chromeShouldHide, clampSeek, previewCommand, proxyStep, wheelSeek, type PreviewCommand } from '../lib/preview'
import { attempt, toast } from '../lib/store'
import { CLIP_DEFAULT_MS, sessionIdOf, type TimelineSession } from '../lib/timeline/model'
import { close, isFullscreen, setFullscreen, titlebarMouseDown } from '../lib/window'
import { TimelinePlayer, type PlayerState } from '../timeline/player'
import './preview.css'

export interface Info {
  duration: number
  bytes: number
  width: number
  height: number
  sample_aspect_ratio: number
  video_codec: string
}

/** What the window's keys drive, whichever picture is on screen. */
export interface Control {
  toggle: () => void
  seekBy: (deltaMs: number) => void
  seekTo: (edge: 'start' | 'end') => void
  mute: () => void
  reveal: () => void
}

function item(path: string): MediaItem {
  const parts = path.split(/[\\/]/)
  return {
    file: path,
    name: stem(path),
    size: 0,
    mtime: 0,
    duration: 0,
    thumbnail: '',
    folder: parts.length > 1 ? parts[parts.length - 2] : '',
    kind: /\.(png|jpe?g)$/i.test(path) ? 'screenshot' : 'clip',
  }
}

/** The chrome steps aside only while a picture is playing and nobody is looking at it. */
function useChrome(playing: boolean, always: boolean) {
  const [hidden, setHidden] = useState(false)
  const [hovering, setHovering] = useState(false)
  const last = useRef(Date.now())
  const reveal = useCallback(() => {
    last.current = Date.now()
    setHidden(false)
  }, [])

  useEffect(() => {
    if (!playing || always) reveal()
  }, [playing, always, reveal])

  useEffect(() => {
    if (always) return
    const timer = window.setInterval(() => {
      const active = document.activeElement
      const focused = active instanceof HTMLElement && active.closest('.preview-glass') !== null
      if (chromeShouldHide({ playing, idleMs: Date.now() - last.current, hovering, focused })) setHidden(true)
    }, 250)
    return () => window.clearInterval(timer)
  }, [playing, hovering, always])

  return { hidden: hidden && !always, reveal, setHovering }
}

/** Each click acts immediately; a double click toggles twice and opens fullscreen. */
function usePictureClicks(toggle: () => void, onDouble: () => void, enabled: boolean) {
  // Pausing adds a play button over the picture, so the second click can land on a different DOM
  // target and the browser's dblclick event would never arrive. Track the gesture across both.
  const last = useRef<{ time: number; x: number; y: number } | null>(null)
  return {
    onClick: (event: ReactMouseEvent) => {
      event.stopPropagation()
      if (enabled) toggle()
      const now = event.timeStamp
      const previous = last.current
      const double = event.detail > 0 && previous !== null && now - previous.time <= 400 &&
        Math.hypot(event.clientX - previous.x, event.clientY - previous.y) <= 8
      last.current = double || event.detail === 0 ? null : { time: now, x: event.clientX, y: event.clientY }
      if (double) onDouble()
    },
    onDoubleClick: (event: ReactMouseEvent) => event.stopPropagation(),
  }
}

function Frame() {
  return <div className="preview-frame" aria-hidden />
}

function TopChrome({ name, temporary, counter, actions, hidden, onHover, onActivity, onDrag, embedded }: {
  name: string
  temporary?: boolean
  counter?: string
  actions?: ReactNode
  hidden: boolean
  onHover: (on: boolean) => void
  onActivity: () => void
  onDrag: (event: React.MouseEvent) => void
  embedded?: boolean
}) {
  return (
    <>
      {!embedded && <div className="preview-drag" onMouseDown={onDrag} />}
      <div
        className="preview-glass preview-cluster preview-identity"
        data-hidden={hidden}
        onPointerEnter={() => onHover(true)}
        onPointerLeave={() => onHover(false)}
        onPointerMove={onActivity}
      >
        {temporary && <span className="preview-chip preview-chip-quiet">temporary</span>}
        <span className="preview-name" title={name}>{name}</span>
        {counter && <span className="preview-count">{counter}</span>}
      </div>
      <div
        className="preview-glass preview-cluster preview-actions"
        data-hidden={hidden}
        onPointerEnter={() => onHover(true)}
        onPointerLeave={() => onHover(false)}
        onPointerMove={onActivity}
      >
        {actions}
        {!embedded && <WindowKeys maximise={false} />}
      </div>
    </>
  )
}

function Transport({ playing, at, span, onToggle, onSeek, onSkip, volume, muted, onVolume, onMute, fullscreen, onFullscreen, hidden, onHover, onActivity, embedded }: {
  playing: boolean
  at: number
  span: number
  onToggle: () => void
  onSeek: (ms: number) => void
  onSkip: (deltaMs: number) => void
  volume: number
  muted: boolean
  onVolume: (value: number) => void
  onMute: () => void
  fullscreen: boolean
  onFullscreen: () => void
  hidden: boolean
  onHover: (on: boolean) => void
  onActivity: () => void
  embedded?: boolean
}) {
  const [scrub, setScrub] = useState<number | null>(null)
  // Where the pointer is over the seek bar, as a share of it: the time under it is named above it before it is pressed.
  const [hover, setHover] = useState<number | null>(null)
  useEffect(() => {
    if (span <= 0) setScrub(null)
  }, [span])
  const position = clampSeek(scrub ?? at, span)

  return (
    <div
      className="preview-glass preview-transport"
      data-hidden={hidden}
      onPointerEnter={() => onHover(true)}
      onPointerLeave={() => onHover(false)}
      onPointerMove={onActivity}
    >
      <IconButton icon="rewind" label="Back 5 seconds" onPress={() => onSkip(-5000)} />
      <IconButton icon={playing ? 'pause' : 'play'} label={playing ? 'Pause' : 'Play'} className="preview-play" onPress={onToggle} instant />
      <IconButton icon="forward" label="Forward 5 seconds" onPress={() => onSkip(5000)} />
      <span className="preview-time">
        <span className="text-ink">{duration(position / 1000)}</span>
        <span className="text-muted"> / {duration(span / 1000)}</span>
      </span>
      <div
        className="preview-seek-wrap"
        onPointerMove={(event) => {
          const box = event.currentTarget.getBoundingClientRect()
          setHover(Math.min(1, Math.max(0, (event.clientX - box.left) / Math.max(1, box.width))))
        }}
        onPointerLeave={() => setHover(null)}
      >
        <Slider
          className="preview-seek"
          instant
          label="Seek"
          value={position}
          min={0}
          max={Math.max(1, span)}
          step={250}
          onChange={(value) => setScrub(clampSeek(value, span))}
          onCommit={(value) => {
            setScrub(null)
            onSeek(clampSeek(value, span))
          }}
        />
        {hover !== null && span > 0 && (
          <span className="preview-seek-hover" style={{ left: `${hover * 100}%` }}>{duration((hover * span) / 1000)}</span>
        )}
      </div>
      <IconButton
        icon={muted || volume <= 0 ? 'speakerOff' : 'speaker'}
        label={muted ? 'Unmute' : 'Mute'}
        onPress={onMute}
      />
      <Slider
        className="preview-volume"
        label="Volume"
        value={muted ? 0 : Math.round(volume * 100)}
        min={0}
        max={100}
        step={1}
        onChange={(value) => onVolume(value / 100)}
        onCommit={(value) => onVolume(value / 100)}
      />
      {!embedded && <IconButton
        icon={fullscreen ? 'compact' : 'expand'}
        label={fullscreen ? 'Exit fullscreen' : 'Fullscreen'}
        onPress={onFullscreen}
      />}
    </div>
  )
}

function Notice({ children, action }: { children: ReactNode; action?: ReactNode }) {
  return (
    <div className="preview-notice preview-glass">
      <Icon name="alert" size={22} className="mx-auto mb-2 text-muted" />
      <div>{children}</div>
      {action && <div className="preview-notice-action">{action}</div>}
    </div>
  )
}

/** A clip or a screenshot: the picture, its ambient copy, and the chrome that fades off it. */
export function ClipView({ path, kind, details, actions, counter, siblings, control, fullscreen, onFullscreen, onStep, embedded, persistent, onState }: {
  path: string
  kind: 'clip' | 'screenshot'
  details?: Info | 'failed'
  actions: ReactNode
  counter?: string
  siblings: boolean
  control: MutableRefObject<Control | null>
  fullscreen: boolean
  onFullscreen: () => void
  onStep: (delta: number) => void
  embedded?: boolean
  /** The controls stay on screen while it plays: a clip inside another window has no idle pointer to read. */
  persistent?: boolean
  /** Reports when the picture can play, or cannot be shown here (the panel's clip page listens). */
  onState?: (state: 'ready' | 'broken') => void
}) {
  const video = useRef<HTMLVideoElement>(null)
  const ambient = useRef<HTMLCanvasElement>(null)
  const lastDraw = useRef(0)
  const [playing, setPlaying] = useState(false)
  const [at, setAt] = useState(0)
  const [span, setSpan] = useState(0)
  const [volume, setVolume] = useState(1)
  const [muted, setMuted] = useState(false)
  const [shown, setShown] = useState(false)
  const [failed, setFailed] = useState(false)
  const [ready, setReady] = useState(false)
  const [proxy, setProxy] = useState<string | null>(null)
  const [needsProxy, setNeedsProxy] = useState(false)
  const [preparing, setPreparing] = useState(false)
  const [progress, setProgress] = useState(0)
  const [proxyError, setProxyError] = useState('')
  const starting = useRef(false)
  const pending = useRef<number | null>(null)
  // Only the editor's process has the bridge that makes a preview copy; the panel's page plays what
  // WebView2 itself can decode, and says so when it cannot.
  const canProxy = transport === 'tauri'
  const probe = details === undefined ? 'loading' : details === 'failed' ? 'failed' : 'ready'
  const step = proxyStep(needsProxy, canProxy, probe)
  const source = mediaUrl(proxy ?? path)
  // A file outside the folders the page may read has no URL at all: say so instead of waiting.
  const noSource = transport !== 'mock' && !source
  const broken = failed || noSource || proxyError !== ''
  const chrome = useChrome(playing, kind !== 'clip' || broken)
  const hiddenChrome = chrome.hidden && !persistent

  useEffect(() => {
    if (ready) onState?.('ready')
  }, [ready, onState])
  useEffect(() => {
    if (broken) onState?.('broken')
  }, [broken, onState])

  const drawAmbient = useCallback(() => {
    const element = video.current
    const canvas = ambient.current
    if (!element || !canvas || !element.videoWidth) return
    const context = canvas.getContext('2d')
    if (!context) return
    try {
      context.drawImage(element, 0, 0, canvas.width, canvas.height)
      lastDraw.current = Date.now()
      setShown(true)
    } catch {
      setShown(false)
    }
  }, [])

  useEffect(() => {
    if (kind !== 'clip' || broken || !playing) return
    const timer = window.setInterval(() => {
      if (ambientDue(lastDraw.current, Date.now())) drawAmbient()
    }, 500)
    return () => window.clearInterval(timer)
  }, [kind, broken, playing, drawAmbient])

  useEffect(() => {
    const element = video.current
    if (!element) return
    element.volume = volume
    element.muted = muted
  }, [volume, muted])

  // Direct playback first. Windows' decoder is allowed to say no (an MKV, an HEVC file without the
  // codec pack, an unusual profile); then one cached H.264 copy is made and the page plays that.
  useEffect(() => {
    if (step === 'fail') setFailed(true)
  }, [step])

  useEffect(() => {
    if (step !== 'prepare' || starting.current || proxy || !details || details === 'failed') return
    starting.current = true
    let live = true
    const offs: (() => void)[] = []
    let expected: number | null = null
    let output = ''
    const earlyProgress = new Map<number, number>()
    const earlyDone = new Map<number, { ok?: boolean; error?: string }>()
    const finish = (done: { id?: number; ok?: boolean; error?: string }) => {
      if (!live || done.id !== expected) return
      if (done.ok) setProxy(output)
      else setProxyError(done.error || 'the preview copy failed')
      setPreparing(false)
      pending.current = null
      for (const off of offs) off()
    }
    // The listeners go on before the command: a short clip can finish its copy before the reply
    // arrives, and its `media-done` would otherwise be lost.
    offs.push(
      onShell('media-progress', (raw) => {
        const update = raw as { id?: number; value?: number }
        if (update?.id === undefined) return
        if (update.id === expected) setProgress(update.value ?? 0)
        else earlyProgress.set(update.id, update.value ?? 0)
      }),
    )
    offs.push(
      onShell('media-done', (raw) => {
        const done = raw as { id?: number; ok?: boolean; error?: string }
        if (done?.id === undefined) return
        if (done.id === expected) finish(done)
        else earlyDone.set(done.id, done)
      }),
    )
    setPreparing(true)
    setProxyError('')
    void shell<{ output: string; id?: number; cached?: boolean }>('preview_proxy', {
      input: path,
      width: details.width,
      height: details.height,
      sar: details.sample_aspect_ratio,
    })
      .then((result) => {
        if (!live) {
          if (result.id) void shell('media_cancel', { id: result.id }).catch(() => undefined)
          return
        }
        if (result.cached) {
          setProxy(result.output)
          setPreparing(false)
          return
        }
        if (result.id === undefined) {
          setProxyError('the preview copy did not start')
          setPreparing(false)
          return
        }
        expected = result.id
        output = result.output
        pending.current = result.id
        const early = earlyProgress.get(result.id)
        if (early !== undefined) setProgress(early)
        const done = earlyDone.get(result.id)
        if (done) finish(done)
      })
      .catch((reason: unknown) => {
        if (!live) return
        setProxyError(String(reason))
        setPreparing(false)
      })
    return () => {
      live = false
      for (const off of offs) off()
      if (pending.current !== null) {
        void shell('media_cancel', { id: pending.current }).catch(() => undefined)
        pending.current = null
      }
      starting.current = false
    }
  }, [step, proxy, details, path])

  useEffect(() => {
    setReady(false)
  }, [proxy])

  useEffect(() => {
    if (kind !== 'clip') {
      control.current = null
      return
    }
    const object: Control = {
      toggle: () => {
        const element = video.current
        if (!element) return
        chrome.reveal()
        if (element.paused) void element.play().catch(() => undefined)
        else element.pause()
      },
      seekBy: (deltaMs) => {
        const element = video.current
        if (!element) return
        chrome.reveal()
        const length = (element.duration || 0) * 1000 || span
        const next = clampSeek(element.currentTime * 1000 + deltaMs, length)
        element.currentTime = next / 1000
        setAt(next)
      },
      seekTo: (edge) => {
        const element = video.current
        if (!element) return
        chrome.reveal()
        const length = (element.duration || 0) * 1000 || span
        const next = edge === 'start' ? 0 : length
        element.currentTime = next / 1000
        setAt(next)
      },
      mute: () => {
        const element = video.current
        if (!element) return
        chrome.reveal()
        element.muted = !element.muted
        setMuted(element.muted)
      },
      reveal: chrome.reveal,
    }
    control.current = object
    return () => {
      if (control.current === object) control.current = null
    }
  }, [kind, span, control, chrome.reveal])

  // A paused picture carries one quiet play mark; nothing pops when it is toggled.
  const toggle = () => control.current?.toggle()
  const seek = (ms: number) => {
    chrome.reveal()
    const element = video.current
    if (!element) return
    element.currentTime = clampSeek(ms, span) / 1000
    setAt(clampSeek(ms, span))
  }
  const setVolumeTo = (value: number) => {
    setVolume(value)
    setMuted(value <= 0)
  }
  const mute = () => {
    const element = video.current
    if (!element) return
    element.muted = !element.muted
    setMuted(element.muted)
  }
  const clicks = usePictureClicks(toggle, onFullscreen, kind === 'clip' && !broken && !preparing)

  return (
    <div
      className="preview-root"
      data-idle={hiddenChrome && playing}
      onPointerMove={chrome.reveal}
      onWheel={(event) => seek(wheelSeek(at, event.deltaY, span, event.shiftKey))}
    >
      <div className={clsx('preview-ambient', !shown && 'preview-fallback')} aria-hidden>
        {kind === 'clip' && !broken && <canvas ref={ambient} width={160} height={90} className="preview-ambient-image" />}
        {kind !== 'clip' && !broken && <img src={mediaUrl(path)} alt="" draggable={false} className="preview-ambient-image" />}
      </div>
      <div className="preview-scrim" aria-hidden />
      <div className="preview-picture" onClick={clicks.onClick} onDoubleClick={clicks.onDoubleClick}>
        {kind === 'clip' ? (
          !broken && (
            <video
              ref={video}
              src={source}
              autoPlay
              playsInline
              className="preview-media"
              style={{ viewTransitionName: 'clip-pane', opacity: ready ? 1 : 0, transition: 'opacity var(--bx-dur-base) var(--bx-ease-out)' }}
              onLoadedMetadata={(event) => {
                setSpan(event.currentTarget.duration * 1000)
                setReady(true)
                drawAmbient()
              }}
              onLoadedData={() => {
                setReady(true)
                drawAmbient()
              }}
              onSeeked={drawAmbient}
              onTimeUpdate={(event) => setAt(event.currentTarget.currentTime * 1000)}
              onPlay={() => {
                setPlaying(true)
                chrome.reveal()
              }}
              onPause={() => {
                setPlaying(false)
                chrome.reveal()
              }}
              onVolumeChange={(event) => {
                setMuted(event.currentTarget.muted)
                setVolume(event.currentTarget.volume)
              }}
              onError={() => {
                if (proxy) setFailed(true)
                else setNeedsProxy(true)
              }}
            />
          )
        ) : (
          !broken && (
            <img
              src={mediaUrl(path)}
              alt={stem(path)}
              draggable={false}
              className="preview-media"
              style={{ viewTransitionName: 'clip-pane' }}
              onError={() => setFailed(true)}
            />
          )
        )}
      </div>
      <Frame />
      {kind === 'clip' && ready && !playing && !broken && !preparing && (
        <button
          type="button"
          className="preview-pause"
          aria-label="Play"
          onClick={(event) => {
            event.stopPropagation()
            toggle()
          }}
          onDoubleClick={(event) => event.stopPropagation()}
        >
          <Icon name="play" size={22} />
        </button>
      )}
      {kind === 'clip' && step === 'prepare' && preparing && !broken && (
        <div className="preview-prepare preview-glass">
          <Spinner />
          <span>Preparing preview… {Math.round(progress * 100)}%</span>
        </div>
      )}
      {kind === 'clip' && step === 'wait' && !broken && (
        <div className="preview-prepare preview-glass">
          <Spinner />
          <span>Reading the file…</span>
        </div>
      )}
      {embedded && kind === 'clip' && !ready && !broken && !preparing && (
        // The panel's picture shows the page from the start (a hidden page does not load its media): until the clip can play,
        // the spinner stands where the picture will be.
        <div className="preview-prepare preview-glass">
          <Spinner />
        </div>
      )}
      {broken && (
        <Notice action={<Button icon="external" onPress={() => void attempt('The file did not open', () => engine.open(item(path)))}>Open in the default app</Button>}>
          {proxyError ? 'This file could not be prepared for the preview.' : 'This file could not be shown here.'}
        </Notice>
      )}
      <TopChrome
        name={stem(path)}
        counter={counter}
        actions={actions}
        embedded={embedded}
        hidden={hiddenChrome}
        onHover={chrome.setHovering}
        onActivity={chrome.reveal}
        onDrag={(event) => {
          if (!fullscreen) void titlebarMouseDown(event, false)
        }}
      />
      {siblings && (
        <>
          <button
            type="button"
            aria-label="Previous"
            data-hidden={hiddenChrome}
            onClick={() => onStep(-1)}
            className="preview-glass preview-nav preview-nav-left"
          >
            <Icon name="chevronLeft" size={18} />
          </button>
          <button
            type="button"
            aria-label="Next"
            data-hidden={hiddenChrome}
            onClick={() => onStep(1)}
            className="preview-glass preview-nav preview-nav-right"
          >
            <Icon name="chevronRight" size={18} />
          </button>
        </>
      )}
      {kind === 'clip' && !broken && (
        <Transport
          playing={playing}
          at={at}
          span={span}
          onToggle={toggle}
          onSeek={seek}
          onSkip={(deltaMs) => seek(clampSeek(at + deltaMs, span))}
          volume={volume}
          muted={muted}
          onVolume={setVolumeTo}
          onMute={mute}
          fullscreen={fullscreen}
          onFullscreen={onFullscreen}
          hidden={hiddenChrome}
          onHover={chrome.setHovering}
          onActivity={chrome.reveal}
          embedded={embedded}
        />
      )}
    </div>
  )
}

/** The Timeline's temporary footage, played from its chunks: temporary by design, so the header says so. */
function SessionView({ id, fullscreen, onFullscreen }: { id: string; fullscreen: boolean; onFullscreen: (want?: boolean) => Promise<void> }) {
  const [footage, setFootage] = useState<TimelineFootage | null>(null)
  const [error, setError] = useState('')
  const [state, setState] = useState<PlayerState>('buffering')
  const [playing, setPlaying] = useState(false)
  const [at, setAt] = useState(0)
  const [volume, setVolume] = useState(1)
  const [muted, setMuted] = useState(false)
  const [shown, setShown] = useState(false)
  const video = useRef<HTMLVideoElement>(null)
  const ambient = useRef<HTMLCanvasElement>(null)
  const lastDraw = useRef(0)
  const player = useRef<TimelinePlayer | null>(null)
  const control = useRef<Control | null>(null)
  const seekVersion = useRef(0)
  const seekPending = useRef(false)
  const seekResume = useRef(false)
  const chrome = useChrome(playing, false)

  useEffect(() => {
    let live = true
    setFootage(null)
    setError('')
    void engine
      .timeline(id)
      .then((data) => live && setFootage(data))
      .catch((reason: unknown) => live && setError(String(reason)))
    return () => {
      live = false
    }
  }, [id])

  const start = footage?.availableStartMs ?? 0
  const span = footage ? footage.availableEndMs - footage.availableStartMs : 0
  const gone = footage !== null && span <= 0

  const drawAmbient = useCallback(() => {
    const element = video.current
    const canvas = ambient.current
    if (!element || !canvas || !element.videoWidth) return
    const context = canvas.getContext('2d')
    if (!context) return
    try {
      context.drawImage(element, 0, 0, canvas.width, canvas.height)
      lastDraw.current = Date.now()
      setShown(true)
    } catch {
      setShown(false)
    }
  }, [])

  useEffect(() => {
    if (!playing || gone) return
    const timer = window.setInterval(() => {
      if (ambientDue(lastDraw.current, Date.now())) drawAmbient()
    }, 500)
    return () => window.clearInterval(timer)
  }, [playing, gone, drawAmbient])

  // One player for the session: it loads the stretch around the newest moment, parks there, and the
  // picture plays as the chunks arrive. The player owns the MediaSource, so it must be closed too.
  useEffect(() => {
    const element = video.current
    if (!footage || !element || gone) return
    const session: TimelineSession = {
      id: footage.session,
      title: footage.title,
      availableStartMs: footage.availableStartMs,
      availableEndMs: footage.availableEndMs,
      chunks: footage.chunks,
    }
    const instance = new TimelinePlayer(element, { onState: (next) => setState(next) })
    player.current = instance
    void instance
      .load(session, Math.max(session.availableStartMs, session.availableEndMs - CLIP_DEFAULT_MS))
      .then(() => {
        setAt(clampSeek(instance.positionMs() - session.availableStartMs, span))
        return element.play()
      })
      .catch(() => undefined)
    const timer = window.setInterval(() => {
      setAt(clampSeek(instance.positionMs() - session.availableStartMs, span))
    }, 250)
    const onPlay = () => setPlaying(true)
    const onPause = () => setPlaying(false)
    element.addEventListener('play', onPlay)
    element.addEventListener('pause', onPause)
    return () => {
      window.clearInterval(timer)
      element.removeEventListener('play', onPlay)
      element.removeEventListener('pause', onPause)
      instance.close()
      player.current = null
      seekVersion.current += 1
      seekPending.current = false
      seekResume.current = false
    }
  }, [footage, gone, span])

  useEffect(() => {
    const element = video.current
    if (!element) return
    element.volume = volume
    element.muted = muted
  }, [volume, muted])

  const seekSession = useCallback((ms: number) => {
    const instance = player.current
    const element = video.current
    if (!instance || !element) return
    const next = clampSeek(ms, span)
    setAt(next)
    seekResume.current ||= !element.paused
    seekPending.current = true
    const version = ++seekVersion.current
    void instance.seek(start + next, () => {
      if (version !== seekVersion.current || !seekResume.current) return false
      seekResume.current = false
      return true
    }).then(() => {
      if (version !== seekVersion.current) return
      seekPending.current = false
      if (seekResume.current && element.paused) void element.play().catch(() => undefined)
      seekResume.current = false
    }).catch(() => {
      if (version === seekVersion.current) {
        seekPending.current = false
        seekResume.current = false
      }
    })
  }, [span, start])

  useEffect(() => {
    const object: Control = {
      toggle: () => {
        const element = video.current
        if (!element) return
        chrome.reveal()
        if (seekPending.current) {
          if (seekResume.current || !element.paused) {
            seekVersion.current += 1
            seekPending.current = false
            seekResume.current = false
            element.pause()
          } else seekResume.current = true
          return
        }
        if (element.paused) void element.play().catch(() => undefined)
        else element.pause()
      },
      seekBy: (deltaMs) => {
        const instance = player.current
        const element = video.current
        if (!instance || !element) return
        chrome.reveal()
        seekSession(instance.positionMs() - start + deltaMs)
      },
      seekTo: (edge) => {
        const instance = player.current
        if (!instance) return
        chrome.reveal()
        seekSession(edge === 'start' ? 0 : span)
      },
      mute: () => {
        const element = video.current
        if (!element) return
        chrome.reveal()
        element.muted = !element.muted
        setMuted(element.muted)
      },
      reveal: chrome.reveal,
    }
    control.current = object
    return () => {
      if (control.current === object) control.current = null
    }
  }, [span, start, chrome.reveal, seekSession])

  // A paused picture carries one quiet play mark; nothing pops when it is toggled.
  const toggle = () => control.current?.toggle()
  const seek = (ms: number) => {
    chrome.reveal()
    seekSession(ms)
  }
  const setVolumeTo = (value: number) => {
    setVolume(value)
    setMuted(value <= 0)
  }
  const mute = () => {
    const element = video.current
    if (!element) return
    element.muted = !element.muted
    setMuted(element.muted)
  }
  const clicks = usePictureClicks(toggle, () => void onFullscreen(), Boolean(footage && !gone))

  const trim = () => {
    void (async () => {
      if (fullscreen) await onFullscreen(false)
      await attempt('The Timeline did not open', () => engine.openTimeline(id))
    })()
  }

  const actions = (
    <>
      <Button variant="primary" icon="scissors" onPress={trim}>Trim</Button>
      <IconButton icon="folder" label="Show the footage in its folder" onPress={() => void attempt('The folder did not open', () => engine.timelineFolder(id))} />
    </>
  )
  const status = state === 'buffering' ? 'Loading…' : state === 'codec' ? 'Windows cannot decode this here' : state === 'error' ? 'The footage stopped playing' : ''

  return (
    <div className="preview-root" data-idle={chrome.hidden && playing} onPointerMove={chrome.reveal}>
      <div className={clsx('preview-ambient', !shown && 'preview-fallback')} aria-hidden>
        {footage && !gone && <canvas ref={ambient} width={160} height={90} className="preview-ambient-image" />}
      </div>
      <div className="preview-scrim" aria-hidden />
      <div className="preview-picture" onClick={clicks.onClick} onDoubleClick={clicks.onDoubleClick}>
        {footage && !gone && (
          <video
            ref={video}
            playsInline
            className="preview-media"
            style={{ viewTransitionName: 'clip-pane' }}
            onLoadedData={drawAmbient}
            onSeeked={drawAmbient}
            onVolumeChange={(event) => {
              setMuted(event.currentTarget.muted)
              setVolume(event.currentTarget.volume)
            }}
            onError={() => setState('error')}
          />
        )}
      </div>
      <Frame />
      {footage && !gone && state === 'ready' && !playing && (
        <button
          type="button"
          className="preview-pause"
          aria-label="Play"
          onClick={(event) => {
            event.stopPropagation()
            toggle()
          }}
          onDoubleClick={(event) => event.stopPropagation()}
        >
          <Icon name="play" size={22} />
        </button>
      )}
      {!footage && !error && <div className="preview-notice preview-glass">Reading the footage…</div>}
      {error && <Notice>{error}</Notice>}
      {gone && (
        <Notice>
          This footage has expired.
          <div className="mt-2 text-[13px]">The Timeline keeps footage for a while, then lets it go.</div>
        </Notice>
      )}
      <TopChrome
        name={footage?.title || 'Timeline'}
        temporary
        actions={actions}
        hidden={chrome.hidden}
        onHover={chrome.setHovering}
        onActivity={chrome.reveal}
        onDrag={(event) => {
          chrome.reveal()
          if (!fullscreen) void titlebarMouseDown(event, false)
        }}
      />
      {footage && !gone && (
        <Transport
          playing={playing}
          at={at}
          span={span}
          onToggle={toggle}
          onSeek={seek}
          onSkip={(deltaMs) => seek(clampSeek(at + deltaMs, span))}
          volume={volume}
          muted={muted}
          onVolume={setVolumeTo}
          onMute={mute}
          fullscreen={fullscreen}
          onFullscreen={() => void onFullscreen()}
          hidden={chrome.hidden}
          onHover={chrome.setHovering}
          onActivity={chrome.reveal}
        />
      )}
      {status && <div className="preview-glass preview-status">{status}</div>}
    </div>
  )
}

export function Preview({ onEdit }: { onEdit: (files: string[]) => void }) {
  const [files, setFiles] = useState<string[]>([])
  const [index, setIndex] = useState(0)
  const [info, setInfo] = useState<Record<string, Info | 'failed'>>({})
  const [fullscreen, setFullscreenState] = useState(false)
  const control = useRef<Control | null>(null)

  useEffect(() => {
    void isFullscreen().then(setFullscreenState)
  }, [])

  const flip = useCallback(async (want = !fullscreen) => {
    setFullscreenState(await setFullscreen(want))
  }, [fullscreen])

  // The files this window was opened with, and the ones the panel sends while it is open.
  useEffect(() => {
    void shell<{ files: string[] }>('get_launch').then((launch) => launch?.files?.length && setFiles(launch.files)).catch(() => undefined)
    const offPreview = onShell('preview-files', (list) => {
      if (Array.isArray(list) && list.length) {
        setFiles(list as string[])
        setIndex(0)
      }
    })
    // "Trim in editor" from the panel while the preview is open: become the editor with it.
    const offEdit = onShell('editor-files', (list) => Array.isArray(list) && onEdit(list as string[]))
    return () => {
      offPreview()
      offEdit()
    }
  }, [onEdit])

  const path = files[index] ?? ''
  const session = sessionIdOf(path)
  const current = useMemo(() => (path && !session ? item(path) : null), [path, session])

  useEffect(() => {
    if (!path || session || info[path] || current?.kind !== 'clip') return
    let live = true
    void runEngine(['--media-info', path])
      .then((raw) => {
        if (!live) return
        const data = raw as { ok: boolean; duration: number; bytes: number; width: number; height: number; sample_aspect_ratio: number; video_codec: string }
        setInfo((all) => ({ ...all, [path]: data.ok ? { duration: data.duration, bytes: data.bytes, width: data.width, height: data.height, sample_aspect_ratio: data.sample_aspect_ratio, video_codec: data.video_codec } : 'failed' }))
      })
      .catch(() => live && setInfo((all) => ({ ...all, [path]: 'failed' })))
    return () => {
      live = false
    }
  }, [path, session, info, current])

  // Stepping through a folder must not leave one probe entry per file behind: the preview keeps
  // details only for the files it was opened with.
  useEffect(() => {
    const live = new Set(files)
    setInfo((all) => {
      const kept = Object.entries(all).filter(([file]) => live.has(file))
      return kept.length === Object.keys(all).length ? all : Object.fromEntries(kept)
    })
  }, [files])

  const step = useCallback((delta: number) => {
    if (files.length > 1) setIndex((i) => (i + delta + files.length) % files.length)
  }, [files.length])

  const edit = async (list: string[]) => {
    if (fullscreen) await flip(false)
    onEdit(list)
  }

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        event.preventDefault()
        if (fullscreen) void flip(false)
        else void close()
        return
      }
      const target = event.target as HTMLElement | null
      if (target?.closest('input, textarea, select, button, a, [role="dialog"], [role="slider"]')) return
      const command: PreviewCommand | null = previewCommand(event, Boolean(current))
      if (!command) return
      event.preventDefault()
      control.current?.reveal()
      if (command.kind === 'toggle') control.current?.toggle()
      else if (command.kind === 'seek') control.current?.seekBy(command.deltaMs)
      else if (command.kind === 'seekTo') control.current?.seekTo(command.edge)
      else if (command.kind === 'step') step(command.delta)
      else if (command.kind === 'mute') control.current?.mute()
      else if (command.kind === 'fullscreen') void flip()
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [current, flip, fullscreen, step])

  if (path && session) {
    return <SessionView key={session} id={session} fullscreen={fullscreen} onFullscreen={flip} />
  }

  if (!current) {
    return (
      <div className="preview-root">
        <div className="preview-ambient preview-fallback" aria-hidden />
        <div className="preview-scrim" aria-hidden />
        <Frame />
        <TopChrome
          name="preview"
          hidden={false}
          onHover={() => undefined}
          onActivity={() => undefined}
          onDrag={(event) => {
            if (!fullscreen) void titlebarMouseDown(event, false)
          }}
        />
        <div className="preview-notice preview-glass">Nothing to preview.</div>
      </div>
    )
  }

  const actions = (
    <>
      <Button
        variant="primary"
        icon="scissors"
        onPress={() => void edit([path])}
      >
        {current.kind === 'clip' ? 'Trim' : 'Edit'}
      </Button>
      <IconButton
        icon="share"
        label="Share (opens in the BuffrX panel)"
        onPress={() => void attempt('The share sheet did not open', () => engine.share(path))}
      />
      <IconButton
        icon="copy"
        label="Copy to clipboard"
        onPress={() => void attempt('Not copied', () => engine.copy([path])).then((r) => r !== undefined && toast('success', current.kind === 'clip' ? 'Clip copied' : 'Image copied', 'Paste it into Discord, a chat or a folder'))}
      />
      <IconButton icon="folder" label="Show in folder" onPress={() => void attempt('The folder did not open', () => engine.reveal(current))} />
      <IconButton icon="external" label="Open in the default app" onPress={() => void attempt('The file did not open', () => engine.open(current))} />
    </>
  )

  return (
    <ClipView
      key={path}
      path={path}
      kind={current.kind}
      details={info[path]}
      actions={actions}
      counter={files.length > 1 ? `${index + 1} / ${files.length}` : undefined}
      siblings={files.length > 1}
      control={control}
      fullscreen={fullscreen}
      onFullscreen={() => void flip()}
      onStep={step}
    />
  )
}
