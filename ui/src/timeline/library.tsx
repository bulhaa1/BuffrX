// The Timeline's library: every saved clip and screenshot, laid out on the picture's own stage. Choosing one
// plays it in the preview card itself (the same card the live footage plays in), and the card's border says it
// is a saved file. The playback controls are the clip preview's own (views/Preview.tsx), not a copy.
import { useEffect, useMemo, useRef, useState, type ReactNode } from 'react'
import { createPortal } from 'react-dom'
import { createRoot } from 'react-dom/client'
import { Icon } from '../components/icons'
import { engine, type MediaItem } from '../lib/api'
import { call, onEngine, thumbnailUrl, mediaUrl } from '../lib/bridge'
import { runEngine } from '../lib/media'
import { day, duration } from '../lib/format'
import { ClipView, type Control, type Info } from '../views/Preview'
import './library.css'

type Tab = 'clip' | 'screenshot'

/** The file's name as a person calls it: no extension. */
const titleOf = (item: MediaItem) => item.name.replace(/\.[a-z0-9]{2,4}$/i, '')

/** Where it came from, when that says something: a game's folder, not the library's own. */
const sourceOf = (item: MediaItem) => (item.folder && !/^(clips|recordings|edits)$/i.test(item.folder) ? item.folder : '')

function Card({ item, choose }: { item: MediaItem; choose: () => void }) {
  const element = useRef<HTMLButtonElement>(null)
  const [thumbnail, setThumbnail] = useState(item.thumbnail)
  const [failed, setFailed] = useState(false)
  useEffect(() => {
    setThumbnail(item.thumbnail)
    setFailed(false)
  }, [item.file, item.thumbnail])
  // The engine makes a clip's picture when its card comes into view, never for the whole library at once.
  useEffect(() => {
    if (thumbnail || item.kind !== 'clip' || !element.current) return
    let active = true
    const observer = new IntersectionObserver((entries) => {
      if (!entries.some((entry) => entry.isIntersecting)) return
      observer.disconnect()
      void call<{ thumbnail: string }>('clips.thumbnail', { file: item.file }).then((reply) => {
        if (active && reply.thumbnail) setThumbnail(reply.thumbnail.split(/[\\/]/).pop() ?? '')
      }).catch(() => {})
    }, { rootMargin: '160px' })
    observer.observe(element.current)
    return () => { active = false; observer.disconnect() }
  }, [item.file, item.kind, thumbnail])
  const src = thumbnail ? thumbnailUrl(thumbnail) : item.kind === 'screenshot' ? mediaUrl(item.file) : ''
  const source = sourceOf(item)
  return <button ref={element} type="button" className="tl-card" onClick={choose} title={titleOf(item)}>
    <span className="tl-card-thumb">
      {src && !failed
        ? <img src={src} alt="" loading="lazy" decoding="async" draggable={false} onError={() => setFailed(true)} />
        : <Icon name={item.kind === 'clip' ? 'film' : 'image'} size={28} />}
      {item.kind === 'clip' && item.duration > 0 && <span className="tl-card-length">{duration(item.duration)}</span>}
      <span className="tl-card-play" aria-hidden="true"><Icon name={item.kind === 'clip' ? 'play' : 'expand'} size={20} /></span>
    </span>
    <span className="tl-card-name">{titleOf(item)}</span>
    {source && <small className="tl-card-source">{source}</small>}
  </button>
}

/** The files newest first, under the day they were saved. */
function Days({ items, choose }: { items: MediaItem[]; choose: (item: MediaItem) => void }) {
  const groups = useMemo(() => {
    const out: { label: string; items: MediaItem[] }[] = []
    for (const item of [...items].sort((a, b) => b.mtime - a.mtime)) {
      const label = day(item.mtime) || 'Earlier'
      const last = out[out.length - 1]
      if (last && last.label === label) last.items.push(item)
      else out.push({ label, items: [item] })
    }
    return out
  }, [items])
  return <>{groups.map((group) => <section key={group.label} className="tl-day">
    <h3 className="tl-day-head"><span>{group.label}</span><small>{group.items.length}</small></h3>
    <div className="tl-grid">{group.items.map((item) => <Card key={item.file} item={item} choose={() => choose(item)} />)}</div>
  </section>)}</>
}

function Library({ back, share, previewHost, contextHost }: {
  back: () => void
  share: (item: MediaItem) => void
  previewHost: HTMLElement
  contextHost: HTMLElement
}) {
  const [tab, setTab] = useState<Tab>('clip')
  const [videos, setVideos] = useState<MediaItem[] | null>(null)
  const [shots, setShots] = useState<MediaItem[] | null>(null)
  const [selected, setSelected] = useState<MediaItem | null>(null)
  const [info, setInfo] = useState<Info | 'failed'>()
  const [error, setError] = useState('')
  const control = useRef<Control | null>(null)
  useEffect(() => {
    let active = true
    const load = () => {
      void Promise.all([engine.clips(), engine.screenshots()]).then(([a, b]) => {
        if (active) { setVideos(a); setShots(b); setError('') }
      }).catch(() => { if (active) setError('Could not load the library.') })
    }
    load()
    const off = onEngine('library.changed', load)
    return () => { active = false; off() }
  }, [])
  // What is on the card decides how the page dresses it: the border names a saved file, never the live footage.
  useEffect(() => {
    previewHost.hidden = !selected
    document.body.classList.toggle('library-selected', Boolean(selected))
    if (selected) document.body.dataset.saved = selected.kind
    else delete document.body.dataset.saved
    return () => {
      previewHost.hidden = true
      document.body.classList.remove('library-selected')
      delete document.body.dataset.saved
    }
  }, [previewHost, selected])
  useEffect(() => {
    setInfo(undefined)
    if (!selected || selected.kind !== 'clip') return
    let active = true
    // Arrowing through the grid must not start one engine probe per card: only the card that stays
    // selected for a moment is read. The picture itself starts at once; the probe is for the copy.
    const timer = window.setTimeout(() => {
      void runEngine(['--media-info', selected.file]).then((result) => {
        if (active) setInfo((result as { ok?: boolean }).ok ? result as Info : 'failed')
      }).catch(() => { if (active) setInfo('failed') })
    }, 150)
    return () => { active = false; window.clearTimeout(timer) }
  }, [selected])
  useEffect(() => {
    const key = (event: KeyboardEvent) => {
      if (document.body.classList.contains('sharing')) return
      if (event.key === 'Escape') {
        event.preventDefault(); event.stopPropagation()
        if (selected) setSelected(null); else back()
        return
      }
      if (selected?.kind !== 'clip') return
      const target = event.target
      if (target instanceof HTMLElement && target.closest('input,textarea,select')) return
      if (event.key === ' ') {
        if (target instanceof HTMLElement && target.matches('button:focus-visible')) return
        event.preventDefault(); event.stopPropagation(); control.current?.toggle()
      } else if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
        event.preventDefault(); event.stopPropagation()
        control.current?.seekBy((event.key === 'ArrowLeft' ? -1 : 1) * (event.shiftKey ? 10000 : 5000))
      } else if (event.key.toLowerCase() === 'm') {
        event.preventDefault(); event.stopPropagation(); control.current?.mute()
      }
    }
    window.addEventListener('keydown', key, true)
    return () => window.removeEventListener('keydown', key, true)
  }, [back, selected])
  const items = tab === 'clip' ? videos : shots
  const noun = tab === 'clip' ? 'videos' : 'screenshots'
  const hint: ReactNode = selected
    ? <>Playing <strong>{titleOf(selected)}</strong> · Esc for the library</>
    : <>Choose a {tab === 'clip' ? 'video' : 'screenshot'} to play it here · Esc for the Timeline</>
  return <>
    {createPortal(<>
      <button type="button" className="tl-back" onClick={back} title="Back to the Timeline (Esc)"><Icon name="chevronLeft" size={14} />Timeline</button>
      <strong>Library</strong>
      <span>{hint}</span>
    </>, contextHost)}
    <section className="tl-lib" data-state={selected ? 'playing' : 'grid'} aria-label="Saved media" aria-hidden={selected ? true : undefined}>
      <header className="tl-lib-head">
        <div className="tl-lib-tabs" role="tablist">
          <button type="button" role="tab" aria-selected={tab === 'clip'} onClick={() => setTab('clip')}>Videos <small>{videos?.length ?? ''}</small></button>
          <button type="button" role="tab" aria-selected={tab === 'screenshot'} onClick={() => setTab('screenshot')}>Screenshots <small>{shots?.length ?? ''}</small></button>
        </div>
      </header>
      {error && <p className="tl-lib-note tl-lib-error" role="alert">{error}</p>}
      <div className="tl-lib-scroll">
        {items === null
          ? <p className="tl-lib-note">Loading the library…</p>
          : items.length === 0
            ? <div className="tl-lib-empty"><Icon name={tab === 'clip' ? 'film' : 'image'} size={34} /><strong>No {noun} saved yet</strong><span>{tab === 'clip' ? 'Clips you save from the Timeline appear here.' : 'Screenshots you take appear here.'}</span></div>
            : <Days items={items} choose={setSelected} />}
      </div>
    </section>
    {selected && createPortal(<ClipView key={selected.file} path={selected.file} kind={selected.kind} details={info}
      actions={<>
        <button type="button" className="tl-action" onClick={() => setSelected(null)} title="Back to the library (Esc)"><Icon name="grid" size={15} />Library</button>
        <button type="button" className="tl-action" onClick={() => share(selected)} title="Share this file"><Icon name="share" size={15} />Share</button>
      </>}
      siblings={false} control={control} fullscreen={false} onFullscreen={() => {}} onStep={() => {}} embedded persistent />, previewHost)}
  </>
}

export function mountLibrary(element: HTMLElement, previewHost: HTMLElement, contextHost: HTMLElement, back: () => void, share: (item: MediaItem) => void) {
  const root = createRoot(element)
  root.render(<Library previewHost={previewHost} contextHost={contextHost} back={back} share={share} />)
  return () => root.unmount()
}
