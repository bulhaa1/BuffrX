// The packaged host serves media from `http://media.localhost/<absolute path>`; the dev server
// (`tools/dev-bridge.mjs`) serves the same paths under `/media/` and cannot answer that host.
const dev = Boolean((globalThis as unknown as { __buffrxDevBridge?: boolean }).__buffrxDevBridge)
// The Linux host's loopback server needs the per-run token the page was opened with. The tests import
// this module with no browser window, so the search string is optional.
const search = typeof window === 'undefined' || !window.location ? '' : window.location.search
const params = new URLSearchParams(search)
const mediaPort = params.get('media_port')
const mediaToken = params.get('media_token')
const loopback = mediaPort && mediaToken ? `http://127.0.0.1:${mediaPort}/media/` : null
const tokenQuery = mediaToken ? `?media_token=${encodeURIComponent(mediaToken)}` : ''
// Windows and Android serve a custom scheme as http://<name>.localhost; the others as <name>://localhost.
const customMedia = typeof navigator !== 'undefined' && /Windows|Android/i.test(navigator.userAgent)
  ? 'http://media.localhost/'
  : 'media://localhost/'

/** A file the engine named, as a URL a `<img>`, a `<video>` or a fetch can reach. */
export const mediaUrl = (path: string) =>
  dev ? `/media/${encodeURIComponent(path)}`
    : `${loopback ?? customMedia}${encodeURIComponent(path)}${loopback ? tokenQuery : ''}`
