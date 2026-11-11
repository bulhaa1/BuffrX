import { useEffect, useRef, useState } from 'react'
import { createRoot } from 'react-dom/client'
import { call, mediaUrl, onEngine } from '../lib/bridge'
import { fromEngine } from '../lib/convert'
import { bytes, duration, stem } from '../lib/format'

interface Job {
  id: string; state: string; progress: number; error?: string; destination?: string; sourceBytes: number; bytes: number
  limitBytes: number; duration: number; fitsLimit: boolean; planPossible: boolean; planCopy: boolean; planHeight: number
}
interface Link { id: string; file: string; url: string; created: number; expires: number; viewers: number; watching: number; note?: string }
interface Webhook { id: string; name: string; hint: string }

function track(pending: Set<Promise<unknown>>, work: Promise<unknown>) {
  pending.add(work)
  void work.finally(() => pending.delete(work))
}

function ShareSheet({ file, close, pending }: { file: string; close: () => void; pending: Set<Promise<unknown>> }) {
  const image = /\.(png|jpe?g)$/i.test(file)
  const [tab, setTab] = useState<'link' | 'discord'>('link')
  const [fit, setFit] = useState(false)
  const [limit, setLimit] = useState<number | null>(null)
  const [minutes, setMinutes] = useState(60)
  const [job, setJob] = useState<Job | null>(null)
  const [link, setLink] = useState<Link | null>(null)
  const [hooks, setHooks] = useState<Webhook[]>([])
  const [webhookId, setWebhookId] = useState('')
  const [message, setMessage] = useState('')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  const [notice, setNotice] = useState('')
  const dialog = useRef<HTMLDivElement>(null)
  const mounted = useRef(true)
  const jobId = useRef('')
  useEffect(() => {
    mounted.current = true
    dialog.current?.focus()
    void call<{ share_limit_mib?: number }>('config.get').then((config) => {
      if (mounted.current) setLimit(config.share_limit_mib || 20)
    }).catch(() => { if (mounted.current) setLimit(20) })
    void call<{ webhooks?: Webhook[] }>('share.webhook.get').then((data) => {
      if (mounted.current) {
        setHooks(data.webhooks ?? [])
        setWebhookId(data.webhooks?.[0]?.id ?? '')
      }
    }).catch(() => {})
    const links = (data: unknown) => {
      const result = data as { links?: Link[] }
      if (mounted.current) setLink(result.links?.find((entry) => entry.file.toLowerCase() === file.toLowerCase() && entry.expires * 1000 > Date.now()) ?? null)
    }
    void call('link.list').then(links).catch(() => {})
    const offLinks = onEngine('link.changed', links)
    const offShare = onEngine('share.changed', (data) => {
      const next = fromEngine<Job>(data)
      if (mounted.current && next.id === jobId.current) {
        setJob(next)
      }
    })
    return () => { mounted.current = false; offLinks(); offShare() }
  }, [file])
  useEffect(() => {
    if (limit === null) return
    let active = true
    setJob(null); setError('')
    const preparing = call('share.prepare', { file, fit, limit_mib: limit }).then(async (data) => {
      const next = fromEngine<Job>(data)
      if (!active) { await call('share.cancel', { id: next.id }).catch(() => {}); return }
      jobId.current = next.id
      setJob(next)
      // A short fit can finish before the prepare reply reaches the sheet.
      const current = await call('share.get', { id: next.id }).catch(() => null)
      if (active && current) setJob((previous) => previous?.state === 'posting' || previous?.state === 'posted' ? previous : fromEngine<Job>(current))
    }).catch((reason: unknown) => { if (active) setError(String(reason)) })
    track(pending, preparing)
    return () => {
      active = false
      const id = jobId.current
      jobId.current = ''
      if (id) track(pending, call('share.cancel', { id }).catch(() => {}))
    }
  }, [file, fit, limit, pending])
  useEffect(() => {
    const key = (event: KeyboardEvent) => {
      if (event.key === 'Escape') { event.preventDefault(); event.stopImmediatePropagation(); close() }
      if (event.key !== 'Tab' || !dialog.current) return
      const focusable = [...dialog.current.querySelectorAll<HTMLElement>('button:not(:disabled),input:not(:disabled),select:not(:disabled)')]
      if (!focusable.length) return
      const first = focusable[0], last = focusable[focusable.length - 1]
      if (event.shiftKey && (document.activeElement === first || document.activeElement === dialog.current)) { event.preventDefault(); last.focus() }
      else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first.focus() }
    }
    window.addEventListener('keydown', key, true)
    return () => window.removeEventListener('keydown', key, true)
  }, [close])
  const run = async (fn: () => Promise<void>) => {
    if (busy) return
    setBusy(true); setError(''); setNotice('')
    try { await fn() } catch (reason) { if (mounted.current) setError(reason instanceof Error ? reason.message : String(reason)) }
    finally { if (mounted.current) setBusy(false) }
  }
  const copyLink = () => void run(async () => {
    // A link made before the short-link service existed keeps the URL it was made with. Copying it again
    // asks the engine for the link once more, which is what shortens it. The ask repeats the lifetime this
    // link already has: the sheet's own default would make a second link beside it and leave this one
    // serving, with a different expiry in each hand.
    const again = link && link.created > 0 ? Math.round((link.expires - link.created) / 60) : minutes
    const made = link && !link.url.includes('#') ? link : await (() => { const job = call<{ id: string; url: string; expires: number; note?: string }>('link.share', { file, minutes: again }); track(pending, job); return job })()
    const next = { ...made, file, created: 'created' in made ? made.created : (link?.created ?? 0), viewers: 'viewers' in made ? made.viewers : 0, watching: 'watching' in made ? made.watching : 0 } as Link
    if (!mounted.current) {
      // The sheet was closed while the helper was taking the link: stop it rather than leave a live
      // download nobody can see or stop.
      if (!link) await call('link.stop', { id: made.id }).catch(() => {})
      return
    }
    setLink(next)
    await call('link.copy', { id: next.id })
    // The engine says why a share stayed on the long link; saying "copied" alone hides a failed shorten.
    if (mounted.current) setNotice(made.note || 'Link copied. Paste it into a chat.')
  })
  const copyFile = () => void run(async () => {
    if (!job) return
    await call('share.copy', { id: job.id })
    if (mounted.current) setNotice('Ready to paste into a chat or folder.')
  })
  const post = () => void run(async () => {
    if (!job) return
    const destination = hooks.find((hook) => hook.id === webhookId)?.name ?? 'Discord'
    setJob({ ...job, state: 'posting', progress: 0, destination, error: undefined })
    const request = call<Job>('share.discord', { id: job.id, message, webhook_id: webhookId }); track(pending, request)
    let next: Job
    try { next = fromEngine<Job>(await request) }
    catch (reason) { if (mounted.current) setJob((current) => current?.state === 'posting' ? { ...job, state: 'ready' } : current); throw reason }
    if (!mounted.current) {
      // The sheet closed before the upload answered: ask the engine to stop what is left.
      await call('share.cancel', { id: job.id }).catch(() => {})
      return
    }
    // An event may report progress or completion before the command reply arrives.
    setJob((current) => current?.state === 'posting' && current.progress <= next.progress ? next : current)
  })
  const stop = () => void run(async () => {
    if (!link) return
    await call('link.stop', { id: link.id })
    if (mounted.current) { setLink(null); setNotice('Link stopped.') }
  })
  return <div className="timeline-share-backdrop" onMouseDown={(event) => { if (event.target === event.currentTarget) close() }}>
    <div ref={dialog} className="timeline-share" role="dialog" aria-modal="true" aria-labelledby="timeline-share-title" tabIndex={-1}>
      <header><strong id="timeline-share-title">Share {image ? 'image' : 'clip'}</strong><button onClick={close}>Close</button></header>
      <div className="timeline-share-picture">{image ? <img src={mediaUrl(file)} alt="" /> : <video src={mediaUrl(file)} muted autoPlay loop playsInline />}</div>
      <div className="timeline-share-name" title={stem(file)}>{stem(file)}</div>
      {job && <div className="timeline-share-meta">{job.duration > 0 ? `${duration(job.duration)} · ` : ''}{bytes(job.sourceBytes)}</div>}
      <div className="timeline-share-tabs"><button aria-pressed={tab === 'link'} onClick={() => setTab('link')}>BuffrX link</button><button aria-pressed={tab === 'discord'} onClick={() => setTab('discord')}>Discord</button></div>
      {tab === 'link' ? <div className="timeline-share-content">
        {link ? <><strong>Live · {link.watching ? `${link.watching} watching` : `${link.viewers} downloads`}</strong><span>Expires {new Date(link.expires * 1000).toLocaleString()}</span><input aria-label="BuffrX link" readOnly value={link.url} onFocus={(event) => event.currentTarget.select()} /></> : <><label>Link lasts <select value={minutes} onChange={(event) => setMinutes(Number(event.target.value))}><option value={60}>1 hour</option><option value={240}>4 hours</option><option value={720}>12 hours</option><option value={1440}>24 hours</option></select></label><p>Your friend opens it in BuffrX. Full quality, sent from this PC.</p></>}
      </div> : <div className="timeline-share-content">
        <label><input type="checkbox" checked={fit} disabled={busy || job?.state === 'posting'} onChange={(event) => setFit(event.target.checked)} /> Shrink for Discord</label>
        <label>Upload limit <select value={limit ?? 20} disabled={!fit || busy || job?.state === 'posting'} onChange={(event) => setLimit(Number(event.target.value))}>{[20, 50, 100, 500, ...(limit && ![20, 50, 100, 500].includes(limit) ? [limit] : [])].map((size) => <option key={size} value={size}>{size} MB</option>)}</select></label>
        <p role="status">{!job ? 'Checking the file…' : job.state === 'fitting' ? `Preparing ${Math.round(job.progress * 100)}%` : job.state === 'posting' ? job.progress >= 1 ? 'Upload sent. Waiting for Discord to confirm.' : `Uploading to ${job.destination}: ${Math.round(job.progress * 100)}%` : job.state === 'posted' ? `Posted to ${job.destination}. Discord confirmed the post.` : job.error ? `Post: ${job.error}` : job.state === 'failed' ? 'Could not prepare this file.' : fit && !job.planCopy ? `Prepared to fit ${limit} MB.` : 'The file will be sent as it is.'}</p>
        {hooks.length > 0 && <><label>Server <select value={webhookId} disabled={job?.state === 'posting'} onChange={(event) => setWebhookId(event.target.value)}>{hooks.map((hook) => <option key={hook.id} value={hook.id}>{hook.name}</option>)}</select></label><input aria-label="Discord message" placeholder="Message (optional)" value={message} maxLength={300} onChange={(event) => setMessage(event.target.value)} /></>}
        {!hooks.length && <p>Set a Discord webhook in Settings to post directly. You can still copy the file.</p>}
      </div>}
      {error && <p className="timeline-share-error" role="alert">{error}</p>}{notice && <p className="timeline-share-notice" role="status">{notice}</p>}
      <footer>{tab === 'link' ? <>{link && <button disabled={busy} onClick={stop}>Stop link</button>}<button className="primary" disabled={busy} onClick={copyLink}>{busy ? 'Making…' : 'Copy link'}</button></> : <><button disabled={busy || (job?.state !== 'ready' && job?.state !== 'posted')} onClick={copyFile}>Copy {image ? 'image' : 'clip'}</button>{hooks.length > 0 && <button className="primary" disabled={busy || (job?.state !== 'ready' && job?.state !== 'posted') || !job.fitsLimit} onClick={post}>{job?.state === 'posting' ? 'Posting…' : job?.state === 'posted' ? 'Post again' : 'Post to Discord'}</button>}</>}</footer>
    </div>
  </div>
}

export function mountShare(element: HTMLElement, file: string, close: () => void) {
  const root = createRoot(element)
  const pending = new Set<Promise<unknown>>()
  root.render(<ShareSheet file={file} close={close} pending={pending} />)
  return async () => {
    root.unmount()
    await Promise.allSettled([...pending])
  }
}
