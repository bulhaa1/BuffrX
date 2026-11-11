// The packaged host serves media from `http://media.localhost/<absolute path>`; the dev server
// (`tools/dev-bridge.mjs`) serves the same paths under `/media/` and cannot answer that host.
const dev = Boolean((window as unknown as { __buffrxDevBridge?: boolean }).__buffrxDevBridge)

/** A file the engine named, as a URL a `<img>`, a `<video>` or a fetch can reach. */
export const mediaUrl = (path: string) =>
  `${dev ? '/media/' : 'http://media.localhost/'}${encodeURIComponent(path)}`
