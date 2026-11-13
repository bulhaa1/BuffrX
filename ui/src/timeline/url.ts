// The packaged host serves media from `http://media.localhost/<absolute path>`; the dev server
// (`tools/dev-bridge.mjs`) serves the same paths under `/media/` and cannot answer that host.
const dev = Boolean((window as unknown as { __buffrxDevBridge?: boolean }).__buffrxDevBridge)
const mediaPort = new URLSearchParams(window.location.search).get('media_port')

/** A file the engine named, as a URL a `<img>`, a `<video>` or a fetch can reach. */
export const mediaUrl = (path: string) =>
  `${dev ? '/media/' : mediaPort ? `http://127.0.0.1:${mediaPort}/media/` : 'media://localhost/'}${encodeURIComponent(path)}`
