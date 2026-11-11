import React, { Suspense, lazy, useCallback, useEffect, useState } from 'react'
import { flushSync } from 'react-dom'
import { createRoot } from 'react-dom/client'
import { Toasts } from './components/chrome'
import { launchView, transport } from './lib/bridge'
import { initialise } from './lib/store'
import './index.css'

// The editor process's page. The panel, its compact layout, Settings and the tray panel are drawn by
// the engine itself (src/engine/ui); this page is the editor and the clip preview, one window that a
// preview turns into the editor when Edit is pressed. The shell says which it opened as
// (window.__BUFFRX_VIEW); a browser uses ?view=.
const { view } = launchView()
const loadEditor = () => import('./views/Editor')
const Editor = lazy(() => loadEditor().then((module) => ({ default: module.Editor })))
const Preview = lazy(() => import('./views/Preview').then((module) => ({ default: module.Preview })))
const PanelClip = lazy(() => import('./views/PanelClip').then((module) => ({ default: module.PanelClip })))
// A preview can become the editor at any moment: have its code ready, so nothing the engine sends during
// the switch arrives before the editor listens.
if (view === 'preview') void loadEditor()

// The WebView's own context menu (Reload, Inspect…) is not part of the product.
if (transport === 'tauri') window.addEventListener('contextmenu', (event) => event.preventDefault())

function App() {
  const [mode, setMode] = useState<{ view: 'editor' | 'preview'; files?: string[] }>({ view: view === 'preview' ? 'preview' : 'editor' })
  useEffect(() => {
    void initialise()
  }, [])
  // Trim turns the preview into the editor: the picture pane morphs into the editor's (same
  // view-transition-name in both) and the rest cross-fades. Without the API, or when the person asked for
  // less motion, it is the plain switch.
  const edit = useCallback((files: string[]) => {
    const next = () => flushSync(() => setMode({ view: 'editor', files }))
    if (!document.startViewTransition || matchMedia('(prefers-reduced-motion: reduce)').matches) {
      setMode({ view: 'editor', files })
      return
    }
    document.startViewTransition(async () => {
      await loadEditor()
      next()
      // The editor mounts through Suspense: give it a beat so the transition lands on the editor, not a blank.
      await new Promise((done) => setTimeout(done, 60))
    })
  }, [])
  // The taskbar says what the window is now.
  useEffect(() => {
    if (transport !== 'tauri') return
    void import('@tauri-apps/api/window').then(({ getCurrentWindow }) =>
      getCurrentWindow().setTitle(mode.view === 'preview' ? 'BuffrX Preview' : 'BuffrX Editor'),
    )
  }, [mode.view])
  return (
    <>
      <Suspense fallback={null}>
        {mode.view === 'preview' ? <Preview onEdit={edit} /> : <Editor initialFiles={mode.files} />}
      </Suspense>
      <Toasts />
    </>
  )
}

// The panel's clip page (`?view=panel`, the engine's WebView2) plays one clip and has none of the window's chrome.
createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    {view === 'panel' ? <Suspense fallback={null}><PanelClip /></Suspense> : <App />}
  </React.StrictMode>,
)
