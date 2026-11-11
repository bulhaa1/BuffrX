import { useCallback, useEffect, useRef, useState } from 'react'
import { onShell, setMediaRoots, shell, type MediaRoot } from '../lib/bridge'
import { themeId } from '../lib/themes'
import { previewCommand } from '../lib/preview'
import { ClipView, type Control } from './Preview'

// The clip the panel plays: this page runs in WebView2 inside the engine's own window, so there is no window of its
// own and nothing to close but the clip. The engine names the file; the page tells the engine when the picture can
// play or cannot be shown, and when Escape hands the panel back.
export function PanelClip() {
  const [path, setPath] = useState('')
  const [failed, setFailed] = useState(false)
  const control = useRef<Control | null>(null)
  const report = useCallback((state: 'ready' | 'broken') => {
    void shell(state === 'ready' ? 'clip_ready' : 'clip_failed').catch(() => undefined)
  }, [])

  useEffect(() => {
    let live = true
    void shell<{ files?: string[]; media?: MediaRoot[]; theme?: string }>('get_launch')
      .then((launch) => {
        if (!live) return
        // The folders and the theme come first: the picture's URL is built from the folders, and the page wears the
        // panel's own theme (the ids themes.css keys by).
        setMediaRoots(launch.media ?? [])
        document.documentElement.dataset.theme = themeId(launch.theme)
        const first = launch.files?.[0]
        if (first) setPath(first)
        else {
          // Nothing to play is a failure the panel must hear, or its spinner never ends.
          setFailed(true)
          report('broken')
        }
      })
      .catch(() => {
        if (!live) return
        setFailed(true)
        report('broken')
      })
    const off = onShell('preview-files', (list) => {
      if (!Array.isArray(list) || typeof list[0] !== 'string') return
      setFailed(false)
      setPath(list[0])
    })
    return () => {
      live = false
      off()
    }
  }, [report])

  // The preview window's keys that mean something without its chrome: play, seek and mute. Escape hands the panel back.
  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') {
        event.preventDefault()
        void shell('panel_close').catch(() => undefined)
        return
      }
      // A focused control keeps the keys it owns: Space on the play button must not also toggle here.
      const target = event.target as HTMLElement | null
      if (target?.closest('input, textarea, select, button, a, [role="dialog"], [role="slider"]')) return
      const command = previewCommand(event, Boolean(path))
      if (!command) return
      event.preventDefault()
      control.current?.reveal()
      if (command.kind === 'toggle') control.current?.toggle()
      else if (command.kind === 'seek') control.current?.seekBy(command.deltaMs)
      else if (command.kind === 'seekTo') control.current?.seekTo(command.edge)
      else if (command.kind === 'mute') control.current?.mute()
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [path])

  const onState = report

  if (!path) return failed ? <div className="preview-root"><div className="preview-notice preview-glass">This clip could not be shown here.</div></div> : null
  return (
    <ClipView
      key={path}
      path={path}
      kind="clip"
      actions={null}
      siblings={false}
      control={control}
      fullscreen={false}
      onFullscreen={() => undefined}
      onStep={() => undefined}
      embedded
      persistent
      onState={onState}
    />
  )
}
