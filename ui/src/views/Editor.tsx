// The editor: its own window and process. Files arrive from a folder drop, the native picker, the library on its
// left, the panel, a clip preview or the launch. A video joins the sequence, edited in order and joined by one
// export; a picture opens in the screenshot editor. The sequence stays where it was while a screenshot is open.

import { useCallback, useEffect, useRef, useState } from 'react'
import { Titlebar } from '../components/chrome'
import { Button, DialogBox } from '../components/ui'
import { engine } from '../lib/api'
import { onEngine, onShell, shell, transport } from '../lib/bridge'
import { timelineEditorChunks, timelineFolderSession } from '../lib/editor_timeline'
import { fileName } from '../lib/format'
import { editedCount, inheritLook, newClip, type SequenceClip } from '../lib/sequence'
import { refreshConfig, toast, useApp } from '../lib/store'
import { dropSession, leaseSession, releaseLeases, renewLeases, setConnection, syncSession, type LeaseBook, type LeasePorts } from '../lib/timeline_leases'
import { close } from '../lib/window'
import { IMAGE, VIDEO, loadDetails } from './editor-parts'
import { LibraryRail } from './editor-panels'
import { ImageEditor } from './ImageEditor'
import { SequenceEditor } from './SequenceEditor'

/** `initialFiles`: what the preview in this window was showing when Edit was pressed. */
export function Editor({ initialFiles }: { initialFiles?: string[] } = {}) {
  const [embeddedReady, setEmbeddedReady] = useState(false)
  useEffect(() => {
    if (!window.__BUFFRX_EMBEDDED) return
    let live = true
    void shell('editor_ready').then(() => { if (live) setEmbeddedReady(true) })
    return () => { live = false }
  }, [])
  const config = useApp((s) => s.config)
  const [clips, setClips] = useState<SequenceClip[]>([])
  /** Files being read: a spinner on their row in the library until they are clips. */
  const [loading, setLoading] = useState<string[]>([])
  const [timelineLoading, setTimelineLoading] = useState('')
  const [timelineProblem, setTimelineProblem] = useState('')
  const [focusClip, setFocusClip] = useState<{ id: string; serial: number } | null>(null)
  const [shot, setShot] = useState<string | null>(null)
  const [shotDirty, setShotDirty] = useState(false)
  const [exporting, setExporting] = useState(false)
  const [hover, setHover] = useState(false)
  /** A library row is being dragged: every drop area shows what it will take. */
  const [dragging, setDragging] = useState(false)
  const [closing, setClosing] = useState(false)
  /** What to do once the person agrees to leave a screenshot that has unsaved changes. */
  const [pending, setPending] = useState<(() => void) | null>(null)
  const allowClose = useRef(false)
  const needsCloseConfirmation = useRef(false)
  // Every video path this window has taken: one clip appears once, and a removed one can come back.
  const known = useRef(new Set<string>())
  // Read by `add`, which must stay the same function for the window's listeners: a re-subscribe per
  // clip would add a clip again on every change.
  const shotRef = useRef<string | null>(null)
  const dirtyRef = useRef(false)
  shotRef.current = shot
  dirtyRef.current = shotDirty
  // A drop reaches this page twice, from the page's own listener and from the shell's event: the repeat is ignored.
  const recent = useRef({ key: '', at: 0 })
  const leases = useRef<LeaseBook>({
    leases: new Set(), files: new Map(), failures: new Set(), generations: new Map(), attempts: new Map(),
    pinned: new Set(), releases: new Set(), chains: new Map(), closed: false, offline: false, connection: 0,
  })
  // The engine's lease calls, handed to the lease book so every pin and release of one session keeps its order.
  const ports = useRef<LeasePorts>({
    pin: (id) => engine.pinEditorTimeline(id),
    unpin: (id) => engine.unpinEditorTimeline(id),
    source: (id) => engine.timeline(id),
  }).current
  const sessionLoading = useRef(new Set<string>())
  const loadedSessions = useRef(new Set<string>())
  const sessionSources = useRef(new Map<string, string>())
  const stopping = useRef(false)

  useEffect(() => {
    const problem = 'Timeline footage could not be protected. Reconnect to BuffrX before saving.'
    // A renewal reply from before a session was removed writes nothing: renewLeases checks the lease is still
    // the one it was sent for. So a removed or re-opened session cannot leave a stale warning behind.
    // Each session publishes as it settles, so one slow request cannot hide another's failure; nothing is
    // published once the window is closing.
    const publish = () => { if (!stopping.current) setTimelineProblem(leases.current.failures.size ? problem : '') }
    const renew = () => void renewLeases(leases.current, ports, publish)
    const off = onEngine('connection', (state) => {
      // A renewal that was already on its way when the link dropped lands later and proves nothing, so the
      // failure stays until a pass after reconnection has verified the footage. setConnection moves the
      // epoch, which is what makes such a reply stale even after the link is back.
      setConnection(leases.current, state === 'connected')
      if (!leases.current.leases.size) return
      if (state === 'connected') renew()
      else {
        for (const id of leases.current.leases) leases.current.failures.add(id)
        setTimelineProblem(problem)
      }
    })
    const timer = window.setInterval(renew, 120000)
    return () => { window.clearInterval(timer); off() }
  }, [ports])

  useEffect(() => {
    if (!config) void refreshConfig()
  }, [config])

  /** Runs `action` now, or once the person has agreed to leave a screenshot that has unsaved changes. */
  const leaving = useCallback((action: () => void) => {
    // Two drops in one gesture: both wait behind the same question instead of the second replacing the first.
    if (shotRef.current && dirtyRef.current) setPending((earlier) => () => { earlier?.(); action() })
    else action()
  }, [])

  const loadVideos = useCallback(async (videos: string[]) => {
    for (const path of videos) known.current.add(path.toLowerCase())
    setLoading((list) => [...list, ...videos])
    for (const path of videos) {
      try {
        const details = await loadDetails(path)
        if (!details.ok) throw new Error(details.error ?? 'This file could not be read')
        setClips((list) => [...list, inheritLook(newClip({
          path,
          name: fileName(path),
          duration: details.duration,
          width: details.width,
          height: details.height,
          fps: details.fps,
          bytes: details.bytes,
          audio: details.audio,
        }), list[0])])
      } catch (error) {
        known.current.delete(path.toLowerCase())
        toast('error', 'Not opened', `${fileName(path)}: ${error instanceof Error ? error.message : String(error)}`)
      } finally {
        setLoading((list) => list.filter((entry) => entry !== path))
      }
    }
  }, [])

  const loadTimeline = useCallback(async (id: string) => {
    if (sessionLoading.current.has(id) || stopping.current) return
    sessionLoading.current.add(id)
    leaseSession(leases.current, id)
    setTimelineLoading('Preparing Timeline footage…')
    // The window can close while any of these are on their way; its release is already queued behind the pin by
    // then, so the load stops instead of pinning again, merging footage or reading files nobody will see.
    const stopped = () => stopping.current || leases.current.closed
    try {
      await syncSession(leases.current, id, ports)
      if (stopped()) return
      const source = await engine.editorTimelineSource(id)
      if (stopped()) return
      if (source.session !== id) throw new Error('The requested Timeline session is no longer available.')
      const plan = timelineEditorChunks(source.chunks)
      const path = sessionSources.current.get(id) ?? await shell<string>('timeline_merge', { files: plan.chunks.map((chunk) => chunk.file) })
      if (stopped()) return
      sessionSources.current.set(id, path)
      const key = path.toLowerCase()
      let entry: SequenceClip | undefined
      if (!known.current.has(key)) {
        const details = await loadDetails(path)
        if (stopped()) return
        if (!details.ok || details.duration <= 0) throw new Error(details.error ?? 'The Timeline footage could not be read')
        entry = { ...newClip({ path, name: id, duration: details.duration, width: details.width,
          height: details.height, fps: details.fps, bytes: details.bytes, audio: details.audio }), timelineSession: id }
        known.current.add(key)
      }
      loadedSessions.current.add(id)
      leases.current.files.set(id, plan.chunks.map((chunk) => chunk.file.toLowerCase()))
      if (entry) setClips((list) => [...list, inheritLook(entry!, list[0])])
      setFocusClip((old) => ({ id: key, serial: (old?.serial ?? 0) + 1 }))
      if (plan.hasGap || plan.changed) toast('info', 'Timeline footage opened',
        plan.hasGap ? 'The session has gaps. Recorded parts are placed next to each other.' : 'Capture settings changed during this session. Its parts are kept in order.')
    } catch (error) {
      // Closing already owes this session its release, so nothing is dropped or queued again here.
      if (stopped()) return
      toast('error', 'Timeline footage did not open', error instanceof Error ? error.message : String(error))
      if (!loadedSessions.current.has(id)) {
        dropSession(leases.current, id)
        if (!leases.current.failures.size) setTimelineProblem('')
        void syncSession(leases.current, id, ports).catch(() => undefined)
      }
    } finally {
      sessionLoading.current.delete(id)
      if (!stopping.current) setTimelineLoading(sessionLoading.current.size ? 'Preparing Timeline footage…' : '')
    }
  }, [ports])

  const forgetClip = useCallback((id: string) => {
    const key = id.toLowerCase()
    const removed = clips.find((entry) => entry.id === key)
    known.current.delete(key)
    setClips((list) => list.filter((entry) => entry.id !== key))
    const session = removed?.timelineSession
    if (!session) return
    // A removed Timeline clip no longer needs its lease: leaving it pinned keeps the session's
    // footage on disk past the retention window until the editor closes. Its failure goes with it,
    // or the warning and the blocked save outlive the footage they are about.
    if (clips.some((entry) => entry.id !== key && entry.timelineSession === session)) return
    dropSession(leases.current, session)
    loadedSessions.current.delete(session)
    sessionSources.current.delete(session)
    if (!leases.current.failures.size) setTimelineProblem('')
    // Queued behind a pin that is still in flight: the engine ends up holding nothing for this session.
    void syncSession(leases.current, session, ports).catch(() => undefined)
  }, [clips, ports])

  const add = useCallback(async (paths: string[]) => {
    const key = paths.join('|').toLowerCase()
    const now = Date.now()
    if (recent.current.key === key && now - recent.current.at < 1500) return
    recent.current = { key, at: now }
    const sessions = paths.map(timelineFolderSession).filter(Boolean)
    if (sessions.length) {
      leaving(() => { setShot(null); setShotDirty(false); for (const id of sessions) void loadTimeline(id) })
      paths = paths.filter((path) => !timelineFolderSession(path))
      if (!paths.length) return
    }
    const fresh = paths.filter((path) => VIDEO.test(path) || IMAGE.test(path))
    if (fresh.length < paths.length) toast('info', 'Some files were skipped', 'The editor opens MP4, MKV, MOV, WebM, PNG and JPEG.')
    const images = fresh.filter((path) => IMAGE.test(path))
    const videos = fresh.filter((path) => VIDEO.test(path) && !known.current.has(path.toLowerCase()))
    if (!videos.length) {
      // Pictures only: the first opens in the screenshot editor, beside the sequence.
      if (!images.length) return
      if (images.length > 1) toast('info', 'One picture at a time', 'The screenshot editor opens the first one.')
      const path = images[0]
      if (path.toLowerCase() !== shotRef.current?.toLowerCase()) leaving(() => { setShotDirty(false); setShot(path) })
      return
    }
    // A sequence is video; a picture dropped with videos is left out rather than half-opened.
    if (images.length) toast('info', 'Pictures were skipped', 'A sequence is videos only. Open a picture on its own.')
    leaving(() => { setShot(null); setShotDirty(false); void loadVideos(videos) })
  }, [leaving, loadTimeline, loadVideos])

  // Files from the launch (or the preview this window was), from the engine (editor.open while this
  // window is open, a clip previewed from the panel included), and drops.
  useEffect(() => {
    if (initialFiles?.length) void add(initialFiles)
    else void shell<{ files: string[] }>('get_launch').then((launch) => launch?.files?.length && void add(launch.files)).catch(() => undefined)
    const offFiles = onShell('editor-files', (files) => Array.isArray(files) && void add(files as string[]))
    const offPreview = onShell('preview-files', (files) => Array.isArray(files) && void add(files as string[]))
    let offDrop: (() => void) | undefined
    let disposed = false
    if (transport === 'tauri') {
      void import('@tauri-apps/api/webview').then(({ getCurrentWebview }) =>
        getCurrentWebview().onDragDropEvent((event) => {
          if (event.payload.type === 'over' || event.payload.type === 'enter') setHover(true)
          else if (event.payload.type === 'leave') setHover(false)
          else if (event.payload.type === 'drop') {
            setHover(false)
            void add(event.payload.paths)
          }
        }).then((off) => {
          // The window can close while the import is still resolving: let go of the listener at once
          // rather than leave it on a page that is gone.
          if (disposed) off()
          else offDrop = off
        }),
      )
    }
    return () => {
      disposed = true
      offFiles()
      offPreview()
      offDrop?.()
    }
  }, [add]) // eslint-disable-line react-hooks/exhaustive-deps -- initialFiles is read once, at mount

  const choose = async () => {
    try {
      const paths = await shell<string[]>('pick_clips')
      if (paths?.length) await add(paths)
    } catch (error) {
      toast('error', 'The file picker did not open', error instanceof Error ? error.message : String(error))
    }
  }

  const edited = editedCount(clips)
  needsCloseConfirmation.current = !!edited || shotDirty || exporting
  const closeEditor = useCallback(() => {
    if (stopping.current) return
    allowClose.current = true
    stopping.current = true
    const returning = window.__BUFFRX_EMBEDDED ? shell('editor_leaving').catch(() => undefined) : Promise.resolve()
    // Every pin this window may hold, in its session's own order, and every one of them owed a release: a pin
    // whose reply never arrived still leaves nothing behind on the engine. Closing waits a second for them,
    // not the pipe's minute: a stalled source read or pin reply must not keep the window open, and a pin left
    // behind expires on the engine's own lease rather than holding the footage for ever.
    void Promise.all([returning, releaseLeases(leases.current, ports)]).finally(() => { void close() })
  }, [ports])
  useEffect(() => {
    if (transport !== 'tauri') return
    let off: (() => void) | undefined
    let disposed = false
    void import('@tauri-apps/api/window').then(({ getCurrentWindow }) =>
      getCurrentWindow().onCloseRequested((event) => {
        if (allowClose.current) return
        if (!needsCloseConfirmation.current) {
          event.preventDefault()
          closeEditor()
          return
        }
        event.preventDefault()
        setClosing(true)
      }).then((unlisten) => { if (disposed) unlisten(); else off = unlisten }),
    )
    return () => { disposed = true; off?.() }
  }, [closeEditor])
  const leaveShot = () => {
    setShot(null)
    setShotDirty(false)
  }
  const closeWindow = () => {
    // Exporting counts too: closing kills the export and deletes its partial file.
    if (needsCloseConfirmation.current) setClosing(true)
    else closeEditor()
  }

  return (
    <div className={`flex h-full flex-col bg-canvas${window.__BUFFRX_EMBEDDED && embeddedReady ? ' editor-arrive' : ''}`}>
      <Titlebar title="Editor" maximise={false} onClose={closeWindow} />
      <div className="editor-body min-h-0 flex-1 px-2 pb-2 pt-2">
        <LibraryRail
          taken={new Set(clips.map((entry) => entry.id))}
          loading={loading}
          warning={timelineProblem || timelineLoading || null}
          openShot={shot}
          onAdd={(paths) => void add(paths)}
          onReturn={() => leaving(leaveShot)}
          onRemove={forgetClip}
          onBrowse={() => void choose()}
          onDragChange={setDragging}
        />
        <div className="relative flex min-h-0 min-w-0 flex-col">
          {/* The sequence stays mounted under a screenshot: an export in flight reports to it. */}
          <div className={shot ? 'hidden' : 'min-h-0 flex-1'}>
            <SequenceEditor
              clips={clips}
              setClips={setClips}
              hover={hover}
              dragging={dragging}
              suspended={shot !== null}
              clipsDir={config?.clipsDir ?? ''}
              focusClip={focusClip}
              sourceProblem={timelineProblem || timelineLoading}
              onBrowse={() => void choose()}
              onForget={forgetClip}
              onExportingChange={setExporting}
            />
          </div>
          {shot && (
            <ImageEditor key={shot} path={shot} clipsDir={config?.clipsDir ?? ''} hover={hover || dragging} clips={clips.length} onDirtyChange={setShotDirty}
              onBack={() => leaving(leaveShot)} />
          )}
        </div>
      </div>
      <DialogBox
        isOpen={closing || pending !== null}
        onClose={() => { setClosing(false); setPending(null) }}
        title={closing ? 'Close the editor?' : 'Leave this screenshot?'}
        actions={
          <>
            <Button variant="ghost" onPress={() => { setClosing(false); setPending(null) }}>Keep editing</Button>
            <Button variant="danger" onPress={() => {
              const run = pending
              const wasClosing = closing
              setClosing(false)
              setPending(null)
              if (wasClosing) closeEditor()
              else {
                setShotDirty(false)
                run?.()
              }
            }}>{exporting ? (edited || shotDirty ? 'Close anyway' : 'Stop export and close') : 'Discard changes'}</Button>
          </>
        }
      >
        {closing && !shotDirty && edited > 0 && (
          <> {edited === 1 ? 'One clip has an edit in progress.' : `${edited} clips have edits in progress.`} Your original files stay as they are.</>
        )}
        {shotDirty && <> This screenshot has changes that have not been saved. Your original files stay as they are.</>}
        {exporting && <> An export is still running; closing stops it and removes its partial file.</>}
      </DialogBox>
    </div>
  )
}
