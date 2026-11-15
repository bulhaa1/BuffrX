// Six-stream editor mixer probe: a real Tauri/WebView2 editor page driven over the DevTools protocol.
// Companion of tools/audio-mixer-six-runtime.ps1; run through that script, never against the live editor.
//
//   node tools/audio-mixer-six-probe.mjs --phase=six|mix --port=9347 --engine-pid=<editor pid>
//     --data=<dir> --clips=<clips dir> --artifacts=<dir> --clip=<the fixture opened>
//
// Phase six: a 6-stream complete aggregate (mix + microphone + four applications). Phase mix: an
// incomplete aggregate, whose levels are locked but whose sound must still play.
const { readdirSync, statSync, existsSync, writeFileSync } = await import('node:fs')
const { join } = await import('node:path')
const { spawnSync } = await import('node:child_process')

const arg = (name, fallback = '') => (process.argv.find((a) => a.startsWith(`--${name}=`)) || `--${name}=${fallback}`).slice(name.length + 3)
const phase = arg('phase')
const port = arg('port', '9347')
const editorPid = Number(arg('editor-pid', '0'))
const dataRoot = arg('data')
const clipsDir = arg('clips')
const artifacts = arg('artifacts')

const rows = []
let failures = 0
function check(name, ok, detail = '') {
  if (!ok) failures++
  rows.push(`${ok ? 'ok  ' : 'FAIL'} ${name}`.padEnd(78) + detail)
}
const near = (value, target, tolerance) => Math.abs(value - target) <= tolerance

async function target() {
  for (let i = 0; i < 60; i++) {
    try {
      const reply = await fetch(`http://127.0.0.1:${port}/json`)
      const pages = (await reply.json()).filter((t) => t.type === 'page')
      if (pages.length) return pages[0].webSocketDebuggerUrl
    } catch {}
    await new Promise((r) => setTimeout(r, 500))
  }
  throw new Error('the editor page never answered on the debug port')
}

function session(url) {
  const socket = new WebSocket(url)
  let next = 1
  const waiting = new Map()
  const ready = new Promise((resolve, reject) => {
    socket.addEventListener('open', () => resolve())
    socket.addEventListener('error', () => reject(new Error('websocket error')))
    socket.addEventListener('close', () => resolve())
  })
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data)
    const entry = waiting.get(message.id)
    if (!entry) return
    waiting.delete(message.id)
    if (message.error) entry.reject(new Error(JSON.stringify(message.error)))
    else entry.resolve(message.result)
  })
  const call = async (method, params = {}) => {
    const id = next++
    const answered = new Promise((resolve, reject) => waiting.set(id, { resolve, reject }))
    socket.send(JSON.stringify({ id, method, params }))
    return answered
  }
  const evaluate = async (expression) => {
    const result = await call('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true, userGesture: true })
    if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text)
    return result.result.value
  }
  return { ready, call, evaluate, close: () => socket.close() }
}

// Installed before the page loads: the gain nodes the page builds, the <audio> copies it makes and the
// interval it follows them with, so doubling and polling can be counted rather than assumed.
const instrument = `(() => {
  const proto = window.AudioContext && window.AudioContext.prototype;
  if (!proto || proto.__bxProbe) return;
  proto.__bxProbe = true;
  const createGain = proto.createGain;
  proto.createGain = function (...args) { const g = createGain.apply(this, args); (window.__gains = window.__gains || []).push(g); return g; };
  const RealAudio = window.Audio;
  window.__audios = [];
  window.Audio = function (src) { const el = new RealAudio(); if (src) el.src = src; window.__audios.push(el); return el; };
  window.Audio.prototype = RealAudio.prototype;
  const setI = window.setInterval, clearI = window.clearInterval;
  window.__timers = new Map();
  window.setInterval = function (fn, ms, ...rest) { const id = setI.call(window, fn, ms, ...rest); window.__timers.set(id, ms); return id; };
  window.clearInterval = function (id) { window.__timers.delete(id); return clearI.call(window, id); };
})()
true`

const helpers = `window.__p = {
  text: (el) => ((el || document.body).innerText || '').trim().replace(/\\s+/g, ' '),
  byLabel: (label) => [...document.querySelectorAll('[aria-label]')].find((e) => e.getAttribute('aria-label') === label) || null,
  clickLabel: (label) => { const e = window.__p.byLabel(label); if (!e) return false; e.click(); return true },
  strips: () => [...document.querySelectorAll('input.fader')].map((i) => {
    const r = i.getBoundingClientRect()
    const card = i.closest('[data-muted], div.border')
    return { name: (i.getAttribute('aria-label') || '').replace(/^Volume of /, ''), value: Number(i.value), orientation: i.getAttribute('aria-orientation'), valuetext: i.getAttribute('aria-valuetext'), width: Math.round(r.width), height: Math.round(r.height), top: Math.round(r.top), bottom: Math.round(r.bottom), left: Math.round(r.left), right: Math.round(r.right), card: card ? (() => { const c = card.getBoundingClientRect(); return { top: Math.round(c.top), bottom: Math.round(c.bottom), left: Math.round(c.left), right: Math.round(c.right) } })() : null }
  }),
  lanes: () => [...document.querySelectorAll('[data-lane]')].map((e) => { const s = e.querySelector('span[title]'); const b = e.querySelector('button[aria-label]'); return { ordinal: Number(e.getAttribute('data-lane')), name: s ? s.title : '', label: b ? b.getAttribute('aria-label') : '', button: !!b } }),
  foldedLane: () => { const s = [...document.querySelectorAll('span[title]')].find((x) => x.title.indexOf('Every track you hear') === 0); return s ? { text: window.__p.text(s), title: s.title } : null },
  combined: () => !!document.querySelector('[aria-label="Combined level"]'),
  expandControl: () => !!window.__p.byLabel('Expand audio into one lane per track'),
  collapseControl: () => !!window.__p.byLabel('Collapse audio into one combined waveform'),
  gutter: (label) => {
    const b = document.querySelector('.area-timeline [aria-label="' + label + '"]')
    if (!b || !b.parentElement) return null
    const r = b.getBoundingClientRect()
    const c = b.parentElement.getBoundingClientRect()
    return { label, left: Math.round(r.left), right: Math.round(r.right), cellLeft: Math.round(c.left), cellRight: Math.round(c.right), scrollWidth: b.scrollWidth, clientWidth: b.clientWidth, fits: r.left >= c.left + 7 && r.right <= c.right - 7 && b.scrollWidth <= b.clientWidth + 1 }
  },
  focus: () => { const a = document.activeElement; return a ? { tag: a.tagName, label: a.getAttribute('aria-label') || '', track: a.getAttribute('data-audio-track') } : null },
  buttons: (name) => ['Mute', 'Listen alone to'].map((kind) => { const e = window.__p.byLabel(kind + ' ' + name); if (!e) return null; const r = e.getBoundingClientRect(); return { kind, text: window.__p.text(e), pressed: e.getAttribute('aria-pressed'), box: { left: Math.round(r.left), right: Math.round(r.right), top: Math.round(r.top), bottom: Math.round(r.bottom) }, disabled: e.disabled } }),
  pane: () => { const p = document.querySelector('[role=tabpanel]'); if (!p) return null; const r = p.getBoundingClientRect(); return { scrollTop: Math.round(p.scrollTop), scrollHeight: Math.round(p.scrollHeight), clientHeight: Math.round(p.clientHeight), top: Math.round(r.top), bottom: Math.round(r.bottom), left: Math.round(r.left), right: Math.round(r.right), width: Math.round(r.width) } },
  paneScroll: () => { const pane = document.querySelector('[role=tabpanel]'); return pane ? Math.round(pane.scrollTop) : -1 },
  setPaneScroll: (top) => { const p = document.querySelector('[role=tabpanel]'); if (!p) return -1; p.scrollTop = top; return Math.round(p.scrollTop) },
  gains: () => (window.__gains || []).map((g) => g.gain.value),
  audios: () => (window.__audios || []).map((a) => ({ src: a.src, paused: a.paused, t: a.currentTime, rate: a.playbackRate, ready: a.readyState, err: a.error ? a.error.message : '' })),
  timers: (ms) => [...(window.__timers || new Map()).values()].filter((v) => v === ms).length,
  nameBox: (name) => { const e = [...document.querySelectorAll('span[title]')].find((s) => s.title === name); if (!e) return null; const r = e.getBoundingClientRect(); return { title: e.title, text: window.__p.text(e), scrollHeight: e.scrollHeight, clientHeight: e.clientHeight, scrollWidth: e.scrollWidth, clientWidth: e.clientWidth, box: { left: Math.round(r.left), right: Math.round(r.right), top: Math.round(r.top), bottom: Math.round(r.bottom) } } },
  playing: () => { const v = document.querySelector('video'); return v ? { paused: v.paused, t: v.currentTime, rate: v.playbackRate, muted: v.muted, volume: v.volume } : null },
  view: () => ({ width: window.innerWidth, height: window.innerHeight, dpr: window.devicePixelRatio, overflow: document.body.scrollWidth - window.innerWidth }),
  inside: (box, pane) => !!box && !!pane && box.left >= pane.left - 1 && box.right <= pane.right + 1 && box.top >= pane.top - 1 && box.bottom <= pane.bottom + 1,
}
true`

async function waitFor(page, expression, timeoutMs = 15000, what = expression) {
  const deadline = Date.now() + timeoutMs
  while (Date.now() < deadline) {
    if (await page.evaluate(expression)) return true
    await new Promise((r) => setTimeout(r, 150))
  }
  throw new Error(`timed out waiting for ${what}`)
}

async function screenshot(page, name) {
  // No requestAnimationFrame wait: an occluded WebView can throttle it forever, and a probe must not hang.
  await new Promise((r) => setTimeout(r, 250))
  try {
    const shot = await Promise.race([
      page.call('Page.captureScreenshot', { format: 'png', captureBeyondViewport: false, fromSurface: false }),
      new Promise((_, reject) => setTimeout(() => reject(new Error('the screenshot timed out')), 8000)),
    ])
    const file = join(artifacts, name)
    writeFileSync(file, Buffer.from(shot.data, 'base64'))
    return file
  } catch {
    return ''
  }
}

async function press(page, expression) {
  const point = await page.evaluate(`(() => {
    const e = ${expression};
    if (!e || e.disabled) return null;
    e.scrollIntoView({ block: 'center' });
    const r = e.getBoundingClientRect();
    return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
  })()`)
  if (!point) return false
  await page.call('Input.dispatchMouseEvent', { type: 'mouseMoved', x: point.x, y: point.y })
  await page.call('Input.dispatchMouseEvent', { type: 'mousePressed', x: point.x, y: point.y, button: 'left', clickCount: 1 })
  await page.call('Input.dispatchMouseEvent', { type: 'mouseReleased', x: point.x, y: point.y, button: 'left', clickCount: 1 })
  return true
}
const pressLabel = (page, label) => press(page, `window.__p.byLabel(${JSON.stringify(label)})`)
const pressText = (page, text, selector) =>
  press(page, `[...document.querySelectorAll(${JSON.stringify(selector || 'button')})].find((x) => window.__p.text(x).toUpperCase() === ${JSON.stringify(text.toUpperCase())})`)

async function wheelOver(page, expression, deltaY) {
  const point = await page.evaluate(`(() => {
    const e = ${expression};
    if (!e) return null;
    e.scrollIntoView({ block: 'center' });
    const r = e.getBoundingClientRect();
    return { x: r.x + r.width / 2, y: r.y + r.height / 2 };
  })()`)
  if (!point) return false
  await page.call('Input.dispatchMouseEvent', { type: 'mouseWheel', x: point.x, y: point.y, deltaX: 0, deltaY })
  await new Promise((r) => setTimeout(r, 350))
  return true
}

async function setFader(page, name, keys) {
  await page.evaluate(`(() => { const i = document.querySelector('input[aria-label="Volume of ${name}"]'); if (i) { i.focus(); } return !!i })()`)
  for (const [key, code, vk] of keys) {
    await page.call('Input.dispatchKeyEvent', { type: 'rawKeyDown', key, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk })
    await page.call('Input.dispatchKeyEvent', { type: 'keyUp', key, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk })
  }
  await new Promise((r) => setTimeout(r, 250))
}

/** A real resize of the editor window (no pointer involved): WM_GETMINMAXINFO still clamps it. */
function resizeEditor(width, height) {
  if (!editorPid) return { ok: false, why: 'no editor pid' }
  const script = `Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public static class BxWin { [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hgt, uint flags); }'; $p = Get-Process -Id ${editorPid} -ErrorAction SilentlyContinue; if (-not $p -or $p.MainWindowHandle -eq 0) { 'NO_WINDOW'; exit 0 }; $ok = [BxWin]::SetWindowPos($p.MainWindowHandle, [IntPtr]::Zero, 0, 0, ${Math.round(width)}, ${Math.round(height)}, 0x0002 -bor 0x0004 -bor 0x0010); if ($ok) { 'RESIZED' } else { 'REFUSED' }`
  const run = spawnSync('powershell', ['-NoProfile', '-EncodedCommand', Buffer.from(script, 'utf16le').toString('base64')], { encoding: 'utf8' })
  return { ok: (run.stdout || '').includes('RESIZED'), why: (run.stdout || run.stderr || '').trim() }
}

const url = await target()
const page = session(url)
await page.ready
await page.call('Page.enable')
await page.call('Runtime.enable')
await page.call('Page.addScriptToEvaluateOnNewDocument', { source: instrument })
await page.call('Page.reload', { ignoreCache: true })
await new Promise((r) => setTimeout(r, 1200))
await waitFor(page, `document.readyState === 'complete' && !!document.querySelector('.editor-shell')`, 30000, 'the editor page')
await page.evaluate(helpers)
await new Promise((r) => setTimeout(r, 800))
const bodyText = async () => page.evaluate('window.__p.text(document.body)')

try {
  if (phase === 'six') {
    const names = ['Microphone', 'Discord', 'Google Chrome', 'Desktop Secondary Display Capture With A Very Long Name', 'Game']
    const join = names.join(',')

    await waitFor(page, `window.__p.gains().length >= 6 && window.__p.gains().slice(-5).join(',') === '1,1,1,1,1'`, 30000, 'the five copies to be ready')
    await waitFor(page, `window.__p.combined() && window.__p.strips().length === 0`, 30000, 'the one combined level on a new clip')
    const foldedGains = await page.evaluate('window.__p.gains()')
    const foldedLane = await page.evaluate('window.__p.foldedLane()')
    check('the sound starts folded into one combined lane', !!foldedLane && foldedLane.text.toUpperCase() === 'COMBINED', JSON.stringify(foldedLane))
    check('the folded lane offers Expand and no Collapse', (await page.evaluate('window.__p.expandControl()')) && !(await page.evaluate('window.__p.collapseControl()')), '')
    const foldedGutter = await page.evaluate(`window.__p.gutter('Expand audio into one lane per track')`)
    check('the folded lane\'s Expand control sits inside the gutter without clipping', !!foldedGutter && foldedGutter.fits, JSON.stringify(foldedGutter))
    check('the folded lane draws no track lane or ordinal', (await page.evaluate('window.__p.lanes()')).length === 0, JSON.stringify(await page.evaluate('window.__p.lanes()')))

    await pressLabel(page, 'Expand audio into one lane per track')
    await waitFor(page, `window.__p.strips().length === 5 && window.__p.lanes().length === 5`, 5000, 'the five source strips after Expand')
    check('expanding the fold changes no source gain or mute', JSON.stringify(await page.evaluate('window.__p.gains()')) === JSON.stringify(foldedGains), `${JSON.stringify(foldedGains)} -> ${JSON.stringify(await page.evaluate('window.__p.gains()'))}`)

    const strips = await page.evaluate('window.__p.strips()')
    check('five editable strips for six container streams (the derived mix has none)', strips.length === 5, JSON.stringify(strips.map((s) => s.name)))
    check('the five strips are the mix\'s sources, in file order', strips.map((s) => s.name).join(',') === join, strips.map((s) => s.name).join(','))
    check('every strip is an upright vertical fader', strips.every((s) => s.height > s.width && s.orientation === 'vertical'), JSON.stringify(strips.map((s) => `${s.width}x${s.height}`)))
    check('the strip heights agree', new Set(strips.map((s) => s.height)).size === 1, JSON.stringify(strips.map((s) => s.height)))

    const lanes = await page.evaluate('window.__p.lanes()')
    check('five visible stem lanes, none for the derived mix', lanes.length === 5, JSON.stringify(lanes))
    check('the lanes are the five sources, in file order', lanes.map((l) => l.name).join(',') === join, lanes.map((l) => l.name).join(','))
    check('the lanes carry the file ordinals 2..6 (stream 1 stays hidden, not renumbered)', lanes.map((l) => l.ordinal).join(',') === '2,3,4,5,6', lanes.map((l) => l.ordinal).join(','))
    check('every lane label is a button that says it shows the mixer', lanes.every((l) => l.button && /Show it in the mixer/.test(l.label)), JSON.stringify(lanes.map((l) => l.label)))
    check('the expanded lanes offer Collapse', await page.evaluate('window.__p.collapseControl()'), '')
    const expandedGutter = await page.evaluate(`window.__p.gutter('Collapse audio into one combined waveform')`)
    check('the expanded lanes\' Collapse control sits inside the gutter without clipping', !!expandedGutter && expandedGutter.fits, JSON.stringify(expandedGutter))

    // One lane and its fader are one control: the label puts the keyboard on that track's level.
    const discord = lanes.find((l) => l.name === 'Discord')
    await pressLabel(page, discord.label)
    await waitFor(page, `window.__p.focus() && window.__p.focus().track === '2'`, 5000, 'the Discord fader to take the keyboard')
    const focused = await page.evaluate('window.__p.focus()')
    check('a lane label puts the keyboard on its own fader', focused && focused.label === 'Volume of Discord', JSON.stringify(focused))

    await pressLabel(page, 'Collapse audio into one combined waveform')
    await waitFor(page, `!!window.__p.foldedLane() && window.__p.lanes().length === 0`, 5000, 'the Collapse to fold the lanes')
    check('Collapse returns the sound to one combined lane', true, '')
    await pressLabel(page, 'Expand audio into one lane per track')
    await waitFor(page, `window.__p.lanes().length === 5`, 5000, 'the lanes after expanding again')

    const gains = await page.evaluate('window.__p.gains()')
    check('the derived mix is silent once its sources are ready', gains[0] === 0, JSON.stringify(gains))
    check('every stem copy is at unity: one path per source, no doubling', gains.slice(-5).every((g) => near(g, 1, 0.001)), JSON.stringify(gains))
    check('the page explains the derived mix instead of showing it', (await bodyText()).toLowerCase().includes('saved audio combines these tracks'), '')
    check('no strip is locked', !(await bodyText()).toLowerCase().includes('levels locked'), '')

    // Every strip's own controls exist and answer.
    let buttonsOk = true
    for (const name of names) {
      const found = await page.evaluate(`window.__p.buttons(${JSON.stringify(name)})`)
      const strip = strips.find((s) => s.name === name)
      if (!found || found.length !== 2 || found.some((b) => !b || b.disabled) || !strip) buttonsOk = false
      else if (found.some((b) => Math.abs(b.box.right - b.box.left) < 8)) buttonsOk = false
    }
    check('all five strips have their Mute and Listen alone buttons', buttonsOk, '')

    const paneNow = await page.evaluate('window.__p.pane()')
    check('the audio pane is a real scrolling list', !!paneNow && paneNow.scrollHeight >= paneNow.clientHeight, JSON.stringify(paneNow))
    const shot = await screenshot(page, `six-mixer-${Date.now()}.png`)
    check('a screenshot of the six-stream mixer was written', existsSync(shot), shot)

    // A real resize to the editor's own minimum (1270x816 logical): the narrowest window it can be.
    const view0 = await page.evaluate('window.__p.view()')
    const real = resizeEditor(1270 * view0.dpr, 816 * view0.dpr)
    check('the editor window was really resized (Win32, no pointer)', real.ok, real.why)
    await new Promise((r) => setTimeout(r, 700))
    const viewMin = await page.evaluate('window.__p.view()')
    check('the real window now reports the minimum width', Math.abs(viewMin.width - 1270) <= 4, JSON.stringify(viewMin))
    const minOverflow = await page.evaluate('window.__p.view()')
    check('the real minimum window has no horizontal overflow', minOverflow.overflow <= 1, JSON.stringify(minOverflow))
    const minLong = await page.evaluate(`window.__p.nameBox(${JSON.stringify(names[3])})`)
    check('the long source name keeps its full tooltip and truncates inside the compact card', !!minLong && minLong.title === names[3] && minLong.text === names[3].toUpperCase() && minLong.box.right - minLong.box.left >= 72 && minLong.box.right - minLong.box.left <= 88 && minLong.scrollWidth > minLong.clientWidth, JSON.stringify(minLong))
    const minPane = await page.evaluate('window.__p.pane()')
    check('the audio pane overflows at the real minimum window and scrolls there', minPane.scrollHeight > minPane.clientHeight + 1, JSON.stringify(minPane))
    const minLanes = await page.evaluate(`(() => { const pane = document.querySelector('.area-timeline').getBoundingClientRect(); return [...document.querySelectorAll('[data-lane]')].map((e) => { const r = e.getBoundingClientRect(); return { ordinal: Number(e.getAttribute('data-lane')), left: Math.round(r.left), right: Math.round(r.right), inside: r.left >= pane.left - 1 && r.right <= pane.right + 1 && r.left >= 0 && r.right <= window.innerWidth } }) })()`)
    check('every expanded lane stays inside the sequence pane at the real minimum width', minLanes.length === 5 && minLanes.every((l) => l.inside), JSON.stringify(minLanes))
    check('the note says Listen alone is preview-only', (await bodyText()).toLowerCase().includes('the eye listens alone in the preview'), '')
    const minShot = await screenshot(page, `six-min-${Date.now()}.png`)
    check('a screenshot of the really resized minimum window was written', existsSync(minShot), minShot)
    await page.evaluate(`(() => { const e = [...document.querySelectorAll('span[title]')].find((s) => s.title === ${JSON.stringify(names[3])}); if (e) e.scrollIntoView({ block: 'center' }); return !!e })()`)
    await new Promise((r) => setTimeout(r, 400))
    const longWrapShot = await screenshot(page, `six-min-long-${Date.now()}.png`)
    check('a screenshot of the long name at the real minimum width was written', existsSync(longWrapShot), longWrapShot)

    // The overflow width is emulated (the native minimum is 1270 logical): the pane, the wheel and the
    // pointer are still the browser's own.
    await page.call('Emulation.setDeviceMetricsOverride', { width: 900, height: 560, deviceScaleFactor: 1, mobile: false })
    await new Promise((r) => setTimeout(r, 500))
    check('the narrow overflow viewport is emulated (900x560), not a native size', true, 'Emulation.setDeviceMetricsOverride')
    const emu = await page.evaluate('window.__p.view()')
    check('the emulated narrow viewport has no horizontal overflow', emu.overflow <= 1, JSON.stringify(emu))
    const paneBefore = await page.evaluate('window.__p.pane()')
    check('the audio pane overflows and can scroll', paneBefore.scrollHeight > paneBefore.clientHeight + 1, JSON.stringify(paneBefore))

    // All five faders and button pairs are reachable: each is scrolled into the pane's viewport.
    let reachable = true
    for (const name of names) {
      const reach = await page.evaluate(`(() => {
        const i = document.querySelector('input[aria-label="Volume of ${name}"]');
        if (!i) return null;
        i.scrollIntoView({ block: 'center' });
        const r = i.getBoundingClientRect();
        const b = window.__p.buttons(${JSON.stringify(name)});
        const pane = window.__p.pane();
        const hit = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2);
        return { fader: window.__p.inside({ left: r.left, right: r.right, top: r.top, bottom: r.bottom }, pane), hit: hit === i || (hit && hit.closest('input') === i), buttons: b.every((x) => x && window.__p.inside(x.box, pane)), pane: !!pane };
      })()`)
      if (!reach || !reach.fader || !reach.hit || !reach.buttons) reachable = false
    }
    check('every fader and both buttons are reachable by scrolling the pane', reachable, '')

    const resizedStrips = await page.evaluate('window.__p.strips()')
    const rowsByTop = new Map()
    for (const s of resizedStrips) {
      const key = s.card ? s.card.top : s.top
      if (!rowsByTop.has(key)) rowsByTop.set(key, [])
      rowsByTop.get(key).push(s)
    }
    const aligned = resizedStrips.every((s) => s.card && s.left >= s.card.left && s.right <= s.card.right && s.top >= s.card.top && s.bottom <= s.card.bottom) &&
      [...rowsByTop.values()].every((row) => row.length === 1 || new Set(row.map((s) => s.top)).size === 1)
    check('faders in a row share one top and sit inside their cards', aligned, JSON.stringify(resizedStrips.map((s) => ({ n: s.name.slice(0, 10), top: s.top, cardTop: s.card?.top }))))

    // Wheel over a fader: five points a notch, and the pane must not move with it.
    const target = names[4]
    await wheelOver(page, `document.querySelector('input[aria-label="Volume of ${target}"]')`, 0)
    const levelBefore = (await page.evaluate('window.__p.strips()')).find((s) => s.name === target).value
    const paneAtWheel = await page.evaluate('window.__p.paneScroll()')
    const gainsAtWheel = (await page.evaluate('window.__p.gains()')).length
    check('the wheel turns over the fader', await wheelOver(page, `document.querySelector('input[aria-label="Volume of ${target}"]')`, -100), '')
    const levelAfter = (await page.evaluate('window.__p.strips()')).find((s) => s.name === target).value
    check('one wheel notch lifts the level five points', levelAfter === Math.min(200, levelBefore + 5), `${levelBefore} -> ${levelAfter}`)
    const wheelGains = await page.evaluate('window.__p.gains()')
    check('the new level reaches that stem copy\'s gain', near(wheelGains[wheelGains.length - 1], levelAfter / 100, 0.0001), JSON.stringify(wheelGains))
    check('the wheel over the level does not scroll the pane', (await page.evaluate('window.__p.paneScroll()')) === paneAtWheel, `${paneAtWheel} -> ${await page.evaluate('window.__p.paneScroll()')}`)
    check('a wheel level change writes no new gain graph', (await page.evaluate('window.__p.gains()')).length === gainsAtWheel, '')
    await wheelOver(page, `document.querySelector('input[aria-label="Volume of ${target}"]')`, 100)
    check('the other way of the wheel returns the level', (await page.evaluate('window.__p.strips()')).find((s) => s.name === target).value === levelBefore, '')

    // Wheel over a name: the pane scrolls, no level moves.
    const nameSpan = `[...document.querySelectorAll('span[title]')].find((s) => s.title === ${JSON.stringify(names[0])})`
    await wheelOver(page, nameSpan, 0)
    const paneMiddle = await page.evaluate('window.__p.paneScroll()')
    check('the wheel over the name is the pane\'s', await wheelOver(page, nameSpan, 400), '')
    check('the pane really scrolled', (await page.evaluate('window.__p.paneScroll()')) !== paneMiddle, `${paneMiddle} -> ${await page.evaluate('window.__p.paneScroll()')}`)
    check('scrolling over the name changes no level', (await page.evaluate('window.__p.strips()')).find((s) => s.name === target).value === levelBefore, '')
    const narrowShot = await screenshot(page, `six-narrow-${Date.now()}.png`)
    check('a screenshot of the narrow mixer was written', existsSync(narrowShot), narrowShot)
    await page.call('Emulation.clearDeviceMetricsOverride', {})
    await new Promise((r) => setTimeout(r, 400))

    // Listen alone on one source: only that copy is heard, the mix stays silent, names and ordinals do not move.
    await page.evaluate(`window.__p.clickLabel('Listen alone to ${names[1]}')`)
    await new Promise((r) => setTimeout(r, 350))
    const soloGains = await page.evaluate('window.__p.gains()')
    check('Listen alone isolates one source and keeps the derived mix silent', soloGains[0] === 0 && soloGains[1] === 0 && near(soloGains[2], 1, 0.001) && soloGains.slice(3).every((g) => g === 0), JSON.stringify(soloGains))
    const soloLanes = await page.evaluate('window.__p.lanes()')
    check('listening alone moves no lane identity or ordinal', soloLanes.map((l) => `${l.ordinal}:${l.name}`).join('|') === lanes.map((l) => `${l.ordinal}:${l.name}`).join('|'), JSON.stringify(soloLanes))
    await page.evaluate(`window.__p.clickLabel('Listen alone to ${names[1]}')`)
    await new Promise((r) => setTimeout(r, 300))
    await pressText(page, 'Reset')
    await new Promise((r) => setTimeout(r, 300))
    const resetGains = await page.evaluate('window.__p.gains()')
    check('Reset restores every source and keeps the mix silent', resetGains[0] === 0 && resetGains.slice(-5).every((g) => near(g, 1, 0.001)), JSON.stringify(resetGains))

    // Playback: five copies in step, one 250 ms follow timer, no sixth stream.
    await pressLabel(page, 'Play (Space)')
    await waitFor(page, 'window.__p.playing() && !window.__p.playing().paused', 6000, 'the picture to play')
    await waitFor(page, `window.__p.timers(250) === 1 && window.__p.audios().filter((a) => !a.paused && a.src).length === 5`, 12000, 'the five copies to play')
    check('playback runs five copied tracks from one follow timer', true, '')
    await new Promise((r) => setTimeout(r, 900))
    const sync = await page.evaluate(`(() => { const v = window.__p.playing(); const a = window.__p.audios().filter((x) => x.src); return a.length === 5 ? { v: v.t, drift: Math.max(...a.map((x) => Math.abs(x.t - v.t))) } : null })()`)
    check('all five copies stay in step with the picture', !!sync && sync.drift <= 0.15, JSON.stringify(sync))
    const playShot = await screenshot(page, `six-playing-${Date.now()}.png`)
    check('a screenshot of the playing mixer was written', existsSync(playShot), playShot)
    await pressLabel(page, 'Pause (Space)')
    await waitFor(page, 'window.__p.timers(250) === 0', 5000, 'the follow timer to stop on pause')

    // A library row adds its file to the sequence; the sequence's own clip button is what selects it.
    const beforeSwitch = (await page.evaluate('window.__p.audios()')).length
    const beforeClips = await page.evaluate(`document.querySelectorAll('[aria-label^="Sequence clip "]').length`)
    const switched = await press(page, `[...document.querySelectorAll('button')].find((b) => window.__p.text(b).toUpperCase().startsWith('IMPORTED PAIR') && !window.__p.text(b).toUpperCase().includes('-EDIT'))`)
    check('the other library row was pressed', switched, '')
    await waitFor(page, `document.querySelectorAll('[aria-label^="Sequence clip "]').length === ${beforeClips + 1}`, 20000, 'the other clip to join the sequence')
    const selected = await pressLabel(page, 'Sequence clip 2: Imported Pair.mp4')
    check('the other clip was selected in the sequence', selected, '')
    try {
      await waitFor(page, `window.__p.combined() && window.__p.strips().length === 0`, 20000, 'the other clip to open folded')
      await pressLabel(page, 'Expand audio into one lane per track')
      await waitFor(page, `window.__p.strips().map((s) => s.name).join(',') === 'Track 1,Track 2'`, 20000, 'the other clip to open')
    } catch (error) {
      const diag = await page.evaluate(`({ strips: window.__p.strips().map((s) => s.name), text: window.__p.text(document.body).slice(0, 300) })`)
      await screenshot(page, `six-switch-failed-${Date.now()}.png`)
      throw new Error(`${error.message} ${JSON.stringify(diag)}`)
    }
    const released = await page.evaluate(`window.__p.audios().slice(0, ${beforeSwitch})`)
    check('leaving the clip released every copied track', released.length === beforeSwitch && released.every((a) => a.src === '' && a.paused), JSON.stringify(released.map((a) => ({ src: a.src, paused: a.paused }))))

    // And selecting the six-stream clip again rebuilds the same five identities from the bound sidecar.
    const back = await pressLabel(page, 'Sequence clip 1: Six Aggregate.mp4')
    check('the six-stream clip was reselected', back, '')
    await waitFor(page, `window.__p.combined() && window.__p.strips().length === 0`, 20000, 'the six-stream clip to reopen folded')
    await pressLabel(page, 'Expand audio into one lane per track')
    await waitFor(page, `window.__p.strips().length === 5`, 20000, 'the five strips after reopening')
    const again = await page.evaluate('window.__p.lanes()')
    check('the reopened file keeps the same five identities and ordinals', again.map((l) => `${l.ordinal}:${l.name}`).join('|') === lanes.map((l) => `${l.ordinal}:${l.name}`).join('|'), JSON.stringify(again))
    await waitFor(page, `window.__p.audios().filter((a) => a.src).length === 5`, 25000, 'the reopened copies')
    const reopened = await page.evaluate('window.__p.gains().slice(-6)')
    check('the reopened file rebuilds one silent mix plus five copies, no doubling', reopened[0] === 0 && reopened.slice(1).every((g) => near(g, 1, 0.001)), JSON.stringify(reopened))

    // Window close is the runtime script's cleanup check; nothing is pressed here.
  }

  if (phase === 'mix') {
    await waitFor(page, `window.__p.text(document.body).includes('LEVELS LOCKED')`, 25000, 'the locked mixer')
    const strips = await page.evaluate('window.__p.strips()')
    check('an incomplete mix offers no level control', strips.length === 0, JSON.stringify(strips))
    const lanes = await page.evaluate('window.__p.lanes()')
    check('the locked mix draws one lane with the file ordinal of the mix', lanes.length === 1 && lanes[0].ordinal === 1, JSON.stringify(lanes))
    check('the locked mix lane is plain text, not a button that cannot reach a fader', lanes.length === 1 && !lanes[0].button && !/Show it in the mixer/.test(lanes[0].label), JSON.stringify(lanes))
    check('the locked mix lane title carries the lock reason', lanes.length === 1 && /levels are locked/i.test(lanes[0].name), JSON.stringify(lanes))
    const text = await bodyText()
    check('the lock says why and that playing and saving still work', text.toLowerCase().includes('levels locked') && text.toLowerCase().includes('its mix holds sound no single track carries') && /still work/i.test(text), '')
    await waitFor(page, `window.__p.gains().length >= 1`, 10000, 'the mix graph')
    const mixGains = await page.evaluate('window.__p.gains()')
    check('the fallback mix plays at full level: it is audible, not muted', near(mixGains[0], 1, 0.001), JSON.stringify(mixGains))
    const element = await page.evaluate(`(() => { const v = document.querySelector('video'); return v ? { muted: v.muted, volume: v.volume } : null })()`)
    check('the picture\'s own sound is unmuted', !!element && element.muted === false && near(element.volume, 1, 0.001), JSON.stringify(element))

    await pressLabel(page, 'Play (Space)')
    await waitFor(page, 'window.__p.playing() && !window.__p.playing().paused', 6000, 'the picture to play')
    await new Promise((r) => setTimeout(r, 1200))
    const timers = await page.evaluate('window.__p.timers(250)')
    const audios = await page.evaluate('window.__p.audios()')
    check('a locked mix makes no follow timer and copies no track', timers === 0 && audios.length === 0, `timers ${timers}, copies ${audios.length}`)
    check('playing a locked mix adds no gain node', (await page.evaluate('window.__p.gains()')).length === mixGains.length, '')
    check('the picture is playing its own sound with no extra stream', (await page.evaluate('window.__p.playing()')).paused === false, '')
    const shot = await screenshot(page, `mix-audible-${Date.now()}.png`)
    check('a screenshot of the locked, audible mix was written', existsSync(shot), shot)
    await pressLabel(page, 'Pause (Space)')
  }

  // Closing the window is the cleanup check: the driver looks for a media child left behind.
  await pressLabel(page, 'Close')
  await new Promise((r) => setTimeout(r, 1500))
} catch (error) {
  check('the probe ran to the end', false, error?.message || String(error))
} finally {
  for (const row of rows) console.log(row)
  console.log(`audio mixer six (${phase}): ${failures === 0 ? 'all checks passed' : `${failures} FAILED`}`)
  page.close()
}
process.exit(failures === 0 ? 0 : 1)
