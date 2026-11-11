// Development-only bridge. Serves ui/dist and forwards to the engine's named pipe, so the editor page
// (the panel itself is drawn by the engine) can be checked against live engine data in a browser.
//
// It is not the product's bridge and is never packaged: the packaged shell (src-tauri/) owns the real
// one (invoke + the named pipe). The interface only uses this one when the page carries
// window.__buffrxDevBridge, which only this server injects (see ui/src/lib/bridge.ts).
//
// Run:  node tools/dev-bridge.mjs            (http://127.0.0.1:8787/ — add ?view=preview)
//       node tools/dev-bridge.mjs --port=9000
import { createServer } from 'node:http'
import { mkdir, readFile, writeFile } from 'node:fs/promises'
import { createReadStream, existsSync, readdirSync, statSync, unlinkSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { spawn } from 'node:child_process'
import { connect } from 'node:net'
import { dirname, extname, join, normalize, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const PIPE = `\\\\.\\pipe\\${process.env.BUFFRX_PIPE || 'buffrx'}`
const DIST = resolve(fileURLToPath(new URL('../ui/dist', import.meta.url)))
const PORT = Number(process.argv.find((argument) => argument.startsWith('--port='))?.slice('--port='.length) ?? 8787)
// Thumbnails live beside the engine's config: BUFFRX_DATA_ROOT when the engine was started that way,
// otherwise the per-user data root the engine resolves.
const DATA_ROOT = resolve(
  process.argv.find((argument) => argument.startsWith('--data-root='))?.slice('--data-root='.length) ??
  process.env.BUFFRX_DATA_ROOT ??
  join(process.env.LOCALAPPDATA ?? '.', 'BuffrX'),
)

const CONTENT_TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.woff2': 'font/woff2',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.m4a': 'audio/mp4',
  '.ico': 'image/x-icon',
}

const subscribers = new Set()
const pending = new Map()
let sequence = 0
let pipe = null
let buffer = Buffer.alloc(0)
const ENGINE_EXE = resolve(DIST, '../../runtime/bin/64bit/buffrx-engine.exe')

// The shell commands the interface reaches through `invoke` in the packaged host (src-tauri). This
// bridge answers the same names in development so every surface, the editor included, can be driven
// in a browser against the live engine.
//
// Path safety: a page on 127.0.0.1 must not be able to read or write arbitrary files through this
// endpoint, so a path is only served when it lies inside the engine's data root or inside a folder the
// engine itself reports (clips, screenshots, sessions). Everything else is refused.
let allowedRoots = null
let rootsAt = 0
async function ensureRoots() {
  if (allowedRoots && Date.now() - rootsAt < 10_000) return allowedRoots
  const roots = [resolve(DATA_ROOT)]
  try {
    ensurePipe()
    const answer = await engineRequest('roots', 'storage.usage', {})
    const data = answer?.data ?? {}
    for (const key of ['clips_dir', 'screenshots_dir', 'sessions_root']) {
      if (typeof data[key] === 'string' && data[key]) roots.push(resolve(data[key]))
    }
  } catch {
    // The engine is not up yet: the data root alone is enough for the first attempt.
  }
  allowedRoots = roots
  rootsAt = Date.now()
  return allowedRoots
}

function isAllowed(path) {
  if (typeof path !== 'string' || path === '' || path.includes('..')) return false
  const target = resolve(path).toLowerCase()
  return (allowedRoots ?? []).some((root) => {
    const folder = root.toLowerCase()
    return target === folder || target.startsWith(folder.endsWith('\\') ? folder : `${folder}\\`)
  })
}

function runEngine(args, id) {
  const name = id == null ? '' : String(id)
  if (name) {
    // The page's run and its cancel are two messages and either can arrive first: a name cancelled
    // before its read reached here never starts, and a name already running is refused rather than
    // replaced (a replaced child is one nothing could end).
    const at = readCancels.indexOf(name)
    if (at >= 0) { readCancels.splice(at, 1); throw new Error('cancelled') }
    if (reads.has(name)) throw new Error(`the read ${name} is already running`)
  }
  return new Promise((resolveRun) => {
    const child = spawn(ENGINE_EXE, args, { windowsHide: true, env: { ...process.env, BUFFRX_DATA_ROOT: DATA_ROOT } })
    if (name) reads.set(name, child)
    let out = ''
    child.stdout.on('data', (chunk) => { out += chunk })
    child.on('error', (failure) => {
      if (reads.get(name) === child) reads.delete(name)
      resolveRun(`ENGINE_FAILED: ${failure.message}`)
    })
    child.on('close', () => {
      if (reads.get(name) === child) reads.delete(name)
      resolveRun(out.trim())
    })
  })
}

function broadcastShell(name, data) {
  const frame = `event: shell\ndata: ${JSON.stringify({ name, data })}\n\n`
  for (const response of subscribers) response.write(frame)
}

const exports = new Map()
const reads = new Map()
/** Names cancelled before their read reached `runEngine`, oldest first. Bounded: a name nothing runs
 *  must not grow it, and the page names its reads in order, so the ones just asked for are at the end. */
const readCancels = []
const MAX_READ_CANCELS = 32
let exportId = 0
const timelineCopies = new Set()
process.once('exit', () => { for (const file of timelineCopies) { try { unlinkSync(file) } catch { /* already removed */ } } })

async function shellInvoke(command, args) {
  if (command === 'get_data_root') return DATA_ROOT
  if (command === 'get_launch') {
    // A probe can open the editor on files without a dialog: BUFFRX_DEV_FILES is a `;`-separated list.
    const files = (process.env.BUFFRX_DEV_FILES ?? '').split(';').filter(Boolean)
    return { role: 'editor', view: 'editor', files }
  }
  if (command === 'media_run') {
    await ensureRoots()
    const list = Array.isArray(args?.args) ? args.args.map(String) : []
    if (!['--media-info', '--media-peaks', '--media-keyframes', '--media-thumb', '--media-probe', '--media-audio'].includes(list[0])) throw new Error(`${list[0]} is not something the editor may run`)
    for (const arg of list.slice(1)) if (/[\\/]/.test(arg) && !isAllowed(arg)) throw new Error('that file is outside the BuffrX folders')
    return runEngine(list, args?.id)
  }
  if (command === 'media_read_cancel') {
    const name = String(args?.id ?? '')
    const child = reads.get(name)
    if (child) {
      reads.delete(name)
      child.kill()
    } else if (name && !readCancels.includes(name)) {
      // Registered nowhere yet: the run of this name is a moment behind, and must never start.
      readCancels.push(name)
      while (readCancels.length > MAX_READ_CANCELS) readCancels.shift()
    }
    return null
  }
  if (command === 'timeline_merge') {
    await ensureRoots()
    const files = Array.isArray(args?.files) ? args.files.map(String) : []
    if (!files.length || files.some((file) => !isAllowed(file))) throw new Error('Timeline footage is outside the BuffrX folders')
    if (files.length === 1) return files[0]
    const directory = join(DATA_ROOT, 'editor-cache')
    await mkdir(directory, { recursive: true })
    const output = join(directory, `timeline-dev-${process.pid}-${++exportId}.mp4`)
    const spec = `${output}.spec.json`
    const report = `${output}.join.json`
    for (const file of [output, spec, report]) timelineCopies.add(file)
    try {
      await writeFile(spec, JSON.stringify({ files, output, start_in_first: 0, end_in_last: 1e12 }))
      await runEngine(['--media-join', spec])
      const result = JSON.parse(await readFile(report, 'utf8'))
      if (!result.ok) throw new Error(result.error || 'The Timeline could not be joined')
      if (result.dropped_head > 0) {
        unlinkSync(output)
        await writeFile(spec, JSON.stringify({ inputs: files.map((input) => ({ input })), output, fit_pad: true }))
        const exported = await runEngine(['--media-export', spec])
        if (!exported.split(/\r?\n/).some((line) => line.startsWith('EXPORT_OK'))) throw new Error('The Timeline could not be joined into one video')
      }
      return output
    } catch (error) {
      try { unlinkSync(output) } catch { /* no output */ }
      throw error
    } finally {
      for (const file of [spec, report]) { try { unlinkSync(file) } catch { /* already removed */ } }
    }
  }
  if (command === 'unique_path') {
    await ensureRoots()
    if (!isAllowed(args?.path)) throw new Error('that file is outside the BuffrX folders')
    const path = resolve(args.path)
    const dot = path.lastIndexOf('.')
    const [base, ext] = dot > path.lastIndexOf('\\') ? [path.slice(0, dot), path.slice(dot)] : [path, '']
    if (!existsSync(path)) return path
    for (let n = 2; n < 10000; n++) if (!existsSync(`${base}-${n}${ext}`)) return `${base}-${n}${ext}`
    throw new Error('no free name')
  }
  if (command === 'media_export') {
    await ensureRoots()
    const job = args?.job ?? {}
    // A sequence export carries `inputs[]` and no single `input`; every file in the job is checked.
    const inputs = [job.input, ...(Array.isArray(job.inputs) ? job.inputs.map((entry) => entry?.input) : [])].filter(Boolean)
    if (!inputs.length || inputs.some((path) => !isAllowed(path)) || !isAllowed(job.output)) throw new Error('that file is outside the BuffrX folders')
    await mkdir(dirname(resolve(job.output)), { recursive: true })
    const id = ++exportId
    const jobFile = join(tmpdir(), `buffrx-devbridge-export-${id}.json`)
    await writeFile(jobFile, JSON.stringify(job))
    const child = spawn(ENGINE_EXE, ['--media-export', jobFile], { windowsHide: true, env: { ...process.env, BUFFRX_DATA_ROOT: DATA_ROOT } })
    exports.set(id, { child, output: job.output })
    let last = ''
    let partial = ''
    child.stdout.on('data', (chunk) => {
      partial += chunk
      const lines = partial.split(/\r?\n/)
      partial = lines.pop() ?? ''
      for (const line of lines) {
        if (line.startsWith('PROGRESS ')) broadcastShell('media-progress', { id, value: Number(line.slice(9)) })
        else if (line.trim()) last = line.trim()
      }
    })
    child.on('close', () => {
      if (partial.trim()) last = partial.trim()
      const cancelled = !exports.has(id)
      exports.delete(id)
      const ok = last.startsWith('EXPORT_OK') && !cancelled
      const encoder = (last.match(/encoder=(\S+)/) ?? [])[1] ?? ''
      broadcastShell('media-done', { id, ok, output: job.output, encoder, error: cancelled ? 'cancelled' : ok ? '' : last.replace(/^EXPORT_FAIL /, '') || 'the export stopped' })
    })
    return id
  }
  if (command === 'media_cancel') {
    const entry = exports.get(Number(args?.id))
    if (entry) {
      exports.delete(Number(args.id))
      entry.child.kill()
      setTimeout(() => {
        try {
          unlinkSync(`${entry.output}.part`)
        } catch {
          /* already gone */
        }
      }, 300)
    }
    return null
  }
  if (command === 'write_image') {
    await ensureRoots()
    if (!isAllowed(args?.path) || !/\.(png|jpe?g)$/i.test(args.path)) throw new Error('images can only be written inside the BuffrX folders')
    await mkdir(dirname(resolve(args.path)), { recursive: true })
    await writeFile(resolve(args.path), Buffer.from(Array.isArray(args?.bytes) ? args.bytes : []))
    return null
  }
  throw new Error(`the dev bridge does not implement "${command}"`)
}

function writeFrame(payload) {
  const body = Buffer.from(payload, 'utf8')
  const header = Buffer.alloc(4)
  header.writeUInt32LE(body.length, 0)
  pipe.write(Buffer.concat([header, body]))
}

// The engine echoes the id it was given and parses it as an integer, so the bridge keeps its own
// integer numbering and maps each one back to the caller's id.
function engineRequest(uiId, cmd, args) {
  return new Promise((resolve, reject) => {
    const id = ++sequence
    const timer = setTimeout(() => {
      pending.delete(id)
      reject(new Error(`The engine did not answer "${cmd}" in time`))
    }, cmd === 'clips.list' || cmd === 'screenshots.list' ? 60_000 : cmd === 'app.pick_folder' ? 900_000 : 10_000)
    pending.set(id, { resolve: (message) => resolve({ ...message, id: uiId }), reject, timer })
    writeFrame(JSON.stringify({ id, cmd, args: args ?? {} }))
  })
}

function handleFrame(text) {
  let message
  try {
    message = JSON.parse(text)
  } catch {
    return
  }
  if (typeof message.id === 'number') {
    const entry = pending.get(message.id)
    if (entry) {
      pending.delete(message.id)
      clearTimeout(entry.timer)
      entry.resolve(message)
    }
    return
  }
  for (const response of subscribers) response.write(`data: ${text}\n\n`)
}

function drain() {
  while (buffer.length >= 4) {
    const length = buffer.readUInt32LE(0)
    if (buffer.length < 4 + length) return
    const payload = buffer.subarray(4, 4 + length).toString('utf8')
    buffer = buffer.subarray(4 + length)
    handleFrame(payload)
  }
}

function ensurePipe() {
  if (pipe) return pipe
  pipe = connect(PIPE)
  pipe.on('data', (chunk) => {
    buffer = Buffer.concat([buffer, chunk])
    drain()
  })
  const drop = () => {
    pipe = null
    buffer = Buffer.alloc(0)
    for (const [id, entry] of pending) {
      clearTimeout(entry.timer)
      entry.reject(new Error('The engine connection closed'))
      pending.delete(id)
    }
  }
  pipe.on('error', drop)
  pipe.on('close', drop)
  return pipe
}

function body(request) {
  return new Promise((resolve, reject) => {
    const chunks = []
    request.on('data', (chunk) => chunks.push(chunk))
    request.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')))
    request.on('error', reject)
  })
}

async function serveFile(path, response, injectFlag = false) {
  try {
    let file = await readFile(path)
    if (injectFlag) {
      // The flag is how the interface knows to use this bridge instead of the WebView2 transport or
      // the mock. Nothing in a packaged build sets it.
      file = Buffer.from(
        file.toString('utf8').replace('</head>', '<script>window.__buffrxDevBridge=true</script></head>'),
        'utf8',
      )
    }
    response.writeHead(200, { 'content-type': CONTENT_TYPES[extname(path)] ?? 'application/octet-stream' })
    response.end(file)
    return true
  } catch {
    return false
  }
}

const server = createServer(async (request, response) => {
  // The bridge drives a live engine. A page on another origin must not be able to POST to it, and a
  // DNS-rebinding host must not make it look local.
  const host = String(request.headers.host ?? '')
  const local = [`127.0.0.1:${PORT}`, `localhost:${PORT}`]
  if (!local.includes(host)) {
    response.writeHead(403, { 'content-type': 'text/plain' })
    response.end('not the BuffrX dev bridge')
    return
  }
  const origin = request.headers.origin
  if (origin && !local.some((name) => origin === `http://${name}`)) {
    response.writeHead(403, { 'content-type': 'text/plain' })
    response.end('not the BuffrX dev bridge')
    return
  }
  const url = new URL(request.url ?? '/', `http://127.0.0.1:${PORT}`)

  if (url.pathname === '/__engine/events') {
    response.writeHead(200, {
      'content-type': 'text/event-stream',
      'cache-control': 'no-cache',
      connection: 'keep-alive',
    })
    response.write(': connected\n\n')
    subscribers.add(response)
    request.on('close', () => subscribers.delete(response))
    return
  }

  if (url.pathname === '/__engine/request' && request.method === 'POST') {
    try {
      const payload = JSON.parse(await body(request))
      // The editor asks for its folders before any engine is up; the dev root answers for it.
      if (payload.cmd === 'config.get') {
        response.writeHead(200, { 'content-type': 'application/json; charset=utf-8' })
        response.end(JSON.stringify({
          ok: true,
          data: { clips_dir: join(DATA_ROOT, 'clips'), screenshots_dir: join(DATA_ROOT, 'screenshots'), theme: 'clutch' },
        }))
        return
      }
      // The picker's library without an engine: the dev root's own clips folder, newest first.
      if (payload.cmd === 'clips.list') {
        const dir = join(DATA_ROOT, 'clips')
        const files = existsSync(dir) ? readdirSync(dir).filter((name) => /\.(mp4|mkv|mov|webm|m4v)$/i.test(name)) : []
        const items = files.map((name) => ({
          file: join(dir, name),
          name,
          size: statSync(join(dir, name)).size,
          mtime: statSync(join(dir, name)).mtimeMs / 1000,
          duration: 0,
          thumbnail: '',
          folder: 'Clips',
          kind: 'clip',
        }))
        response.writeHead(200, { 'content-type': 'application/json; charset=utf-8' })
        response.end(JSON.stringify({ ok: true, data: items }))
        return
      }
      ensurePipe()
      const answer = await engineRequest(payload.id, payload.cmd, payload.args)
      response.writeHead(200, { 'content-type': 'application/json; charset=utf-8' })
      response.end(JSON.stringify(answer))
    } catch (error) {
      response.writeHead(502, { 'content-type': 'application/json; charset=utf-8' })
      response.end(JSON.stringify({ error: error instanceof Error ? error.message : 'bridge failure' }))
    }
    return
  }

  if (url.pathname === '/__shell/invoke' && request.method === 'POST') {
    try {
      const payload = JSON.parse(await body(request))
      ensurePipe()
      const answer = await shellInvoke(payload.cmd ?? payload.command, payload.args ?? {})
      response.writeHead(200, { 'content-type': 'application/json; charset=utf-8' })
      response.end(JSON.stringify({ ok: true, data: answer ?? null }))
    } catch (error) {
      response.writeHead(400, { 'content-type': 'application/json; charset=utf-8' })
      response.end(JSON.stringify({ ok: false, error: error instanceof Error ? error.message : 'bridge failure' }))
    }
    return
  }

  if (url.pathname.startsWith('/media/')) {
    await ensureRoots()
    const path = decodeURIComponent(url.pathname.slice('/media/'.length))
    if (!isAllowed(path) || !existsSync(path)) {
      response.writeHead(404)
      response.end()
      return
    }
    const size = statSync(path).size
    const type = CONTENT_TYPES[extname(path).toLowerCase()] ?? (/\.(mp4|m4v)$/i.test(path) ? 'video/mp4' : /\.mkv$/i.test(path) ? 'video/x-matroska' : 'application/octet-stream')
    const range = /bytes=(\d+)-(\d*)/.exec(request.headers.range ?? '')
    if (range) {
      const first = Number(range[1])
      const last = range[2] ? Math.min(Number(range[2]), size - 1) : size - 1
      response.writeHead(206, { 'content-type': type, 'accept-ranges': 'bytes', 'content-range': `bytes ${first}-${last}/${size}`, 'content-length': last - first + 1 })
      createReadStream(path, { start: first, end: last }).pipe(response)
    } else {
      response.writeHead(200, { 'content-type': type, 'accept-ranges': 'bytes', 'content-length': size })
      createReadStream(path).pipe(response)
    }
    return
  }

  if (url.pathname.startsWith('/thumbnails/')) {
    // The product serves these from the WebView2 virtual host; the dev bridge reads the same files
    // out of the engine's data root so the live view shows real frames.
    const name = normalize(decodeURIComponent(url.pathname.slice('/thumbnails/'.length))).replace(/^([/\\])+/, '')
    const file = join(DATA_ROOT, 'thumbnails', name)
    if (file.startsWith(join(DATA_ROOT, 'thumbnails')) && (await serveFile(file, response))) return
    response.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' })
    response.end('no such thumbnail')
    return
  }

  // Static files, with the single page as the fallback for anything that is not a file.
  const relative = normalize(url.pathname).replace(/^([/\\])+/, '')
  const target = join(DIST, relative)
  if (target.startsWith(DIST) && relative && (await serveFile(target, response, extname(target) === '.html'))) return
  if (await serveFile(join(DIST, 'index.html'), response, true)) return
  response.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' })
  response.end('ui/dist is not built: run npm run build in ui/')
})

server.listen(PORT, '127.0.0.1', () => {
  process.stdout.write(`dev bridge on http://127.0.0.1:${PORT}/?live  (engine pipe ${PIPE})\n`)
})
