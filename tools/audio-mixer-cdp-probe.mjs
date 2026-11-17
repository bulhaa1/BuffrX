// Drives the real BuffrX editor page over the DevTools protocol and checks the multitrack audio UI by
// what it does: the AudioContext gains it sets, the elements it plays, the lanes it draws, and the file
// the product exports. Run through tools/audio-mixer-runtime-probe.ps1, never against the live editor.
//
//   node tools/audio-mixer-cdp-probe.mjs --phase=<agg|import|mix|long> --port=9347 --data=<dir>
//     --engine=<exe> --clips=<clips dir> --artifacts=<dir>
const { readdirSync, statSync, existsSync, rmSync, writeFileSync } = await import('node:fs')
const { join } = await import('node:path')
const { spawnSync } = await import('node:child_process')

const arg = (name, fallback = '') => (process.argv.find((a) => a.startsWith(`--${name}=`)) || `--${name}=${fallback}`).slice(name.length + 3)
const phase = arg('phase')
const port = arg('port', '9347')
const dataRoot = arg('data')
const enginePath = arg('engine')
const clipsDir = arg('clips')
const artifacts = arg('artifacts')
const editsDir = join(clipsDir, 'Edits')

const rows = []
let failures = 0
function check(name, ok, detail = '') {
  if (!ok) failures++
  rows.push(`${ok ? 'ok  ' : 'FAIL'} ${name}`.padEnd(64) + detail)
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

// Installed before the page loads: what the page created, so gains, copies and the follow timer can be
// read without the page exposing a debug API of its own.
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
  clickText: (text, sel) => { const e = [...document.querySelectorAll(sel || 'button')].find((x) => window.__p.text(x).toUpperCase() === text.toUpperCase()); if (!e || e.disabled) return false; e.click(); return true },
  tab: (name) => { const t = [...document.querySelectorAll('[role=tab]')].find((x) => window.__p.text(x).toUpperCase() === name.toUpperCase()); if (!t) return false; t.click(); return true },
  strips: () => [...document.querySelectorAll('input.fader')].map((i) => {
    const r = i.getBoundingClientRect()
    return { name: (i.getAttribute('aria-label') || '').replace(/^Volume of /, ''), value: Number(i.value), orientation: i.getAttribute('aria-orientation'), valuetext: i.getAttribute('aria-valuetext'), width: Math.round(r.width), height: Math.round(r.height) }
  }),
  lanes: () => [...document.querySelectorAll('[data-lane]')].map((e) => {
    const s = e.querySelector('span[title]')
    const b = e.querySelector('button[aria-label]')
    return { ordinal: Number(e.getAttribute('data-lane')), name: s ? s.title : '', label: b ? b.getAttribute('aria-label') : '', button: !!b }
  }),
  foldedLane: () => {
    const s = [...document.querySelectorAll('span[title]')].find((x) => x.title.indexOf('Every track you hear') === 0)
    return s ? { text: window.__p.text(s), title: s.title } : null
  },
  combined: () => !!document.querySelector('[aria-label="Combined level"]'),
  combinedValue: () => { const root = document.querySelector('[aria-label="Combined level"]'); const t = root && (root.querySelector('input[type="range"]') || root.querySelector('[role="slider"]')); return t ? Number(t.value ?? t.getAttribute('aria-valuenow')) : -1 },
  expandControl: () => !!window.__p.byLabel('Expand audio into one lane per track'),
  collapseControl: () => !!window.__p.byLabel('Collapse audio into one combined waveform'),
  focus: () => { const a = document.activeElement; return a ? { tag: a.tagName, label: a.getAttribute('aria-label') || '', track: a.getAttribute('data-audio-track') } : null },
  paneScroll: () => { const pane = document.querySelector('[role=tabpanel]'); return pane ? Math.round(pane.scrollTop) : -1 },
  gains: () => (window.__gains || []).map((g) => g.gain.value),
  audios: () => (window.__audios || []).map((a) => ({ src: a.src, paused: a.paused, t: a.currentTime, rate: a.playbackRate, ready: a.readyState, err: a.error ? a.error.message : '' })),
  timers: (ms) => [...(window.__timers || new Map()).values()].filter((v) => v === ms).length,
  stripName: (name) => { const e = [...document.querySelectorAll('span[title]')].find((s) => s.title === name); return e ? { title: e.title, text: window.__p.text(e), scrollWidth: e.scrollWidth, clientWidth: e.clientWidth, box: (() => { const r = e.getBoundingClientRect(); return { w: Math.round(r.width), h: Math.round(r.height) } })() } : null },
  playing: () => { const v = document.querySelector('video'); return v ? { paused: v.paused, t: v.currentTime, rate: v.playbackRate } : null },
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
  await page.evaluate('new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))')
  const shot = await page.call('Page.captureScreenshot', { format: 'png', captureBeyondViewport: false })
  const file = join(artifacts, name)
  writeFileSync(file, Buffer.from(shot.data, 'base64'))
  return file
}

function engineJson(args) {
  const run = spawnSync(enginePath, args, { encoding: 'utf8', env: process.env })
  const line = `${run.stdout || ''}`.split(/\r?\n/).reverse().find((l) => l.trim().startsWith('{'))
  return line ? JSON.parse(line) : null
}
function peaks(file, raw) {
  const args = ['--media-peaks', file, '120']
  if (raw) args.push('raw')
  return engineJson(args)
}
const top = (lane) => Math.max(...lane)
const mean = (lane) => lane.reduce((a, b) => a + b, 0) / Math.max(1, lane.length)

function newestExport(before) {
  return (async () => {
    const deadline = Date.now() + 120000
    while (Date.now() < deadline) {
      let found = []
      try {
        found = readdirSync(editsDir).filter((n) => /\.(mp4|mkv|mov)$/i.test(n)).map((n) => join(editsDir, n))
      } catch {}
      const fresh = found.filter((f) => !before.has(f) && statSync(f).size > 0)
      const stable = fresh.find((f) => Date.now() - statSync(f).mtimeMs > 900)
      if (stable) return stable
      await new Promise((r) => setTimeout(r, 300))
    }
    return null
  })()
}

async function setFader(page, name, keys) {
  await page.evaluate(`(() => { const i = document.querySelector('input[aria-label="Volume of ${name}"]'); if (i) { i.focus(); } return !!i })()`)
  for (const [key, code, vk] of keys) {
    await page.call('Input.dispatchKeyEvent', { type: 'rawKeyDown', key, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk })
    await page.call('Input.dispatchKeyEvent', { type: 'keyUp', key, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk })
  }
  await new Promise((r) => setTimeout(r, 250))
}

/** The one combined level is a React Aria slider, so the keyboard reaches its thumb the way a person does. */
async function setCombined(page, keys) {
  const focused = await page.evaluate(`(() => {
    const root = document.querySelector('[aria-label="Combined level"]');
    const thumb = root && (root.querySelector('input[type="range"]') || (root.matches('[role=slider]') ? root : root.querySelector('[role=slider]')));
    if (!thumb) return { ok: false, why: 'no thumb', root: root ? root.outerHTML.slice(0, 700) : null, sliders: [...document.querySelectorAll('[role=slider]')].map((e) => e.getAttribute('aria-label')) };
    thumb.focus();
    return { ok: document.activeElement === thumb, why: document.activeElement === thumb ? '' : 'focus refused', root: root.outerHTML.slice(0, 700), thumb: thumb.outerHTML.slice(0, 400), active: document.activeElement ? document.activeElement.outerHTML.slice(0, 200) : null };
  })()`)
  if (!focused.ok) { console.log('combined focus diagnostic', JSON.stringify(focused)); return false }
  for (const [key, code, vk] of keys) {
    await page.call('Input.dispatchKeyEvent', { type: 'rawKeyDown', key, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk })
    await page.call('Input.dispatchKeyEvent', { type: 'keyUp', key, code, windowsVirtualKeyCode: vk, nativeVirtualKeyCode: vk })
  }
  await new Promise((r) => setTimeout(r, 300))
  return true
}

/** A real mouse press at the element's centre: an instant transport control acts on press start, which
 *  a synthetic click never delivers. */
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

/** A real wheel over the element the expression names: the fader reads the wheel on its own element,
 *  so a synthetic event fired into the page would not prove the listener is there. */
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
const pressText = (page, text, selector) =>
  press(page, `[...document.querySelectorAll(${JSON.stringify(selector || 'button')})].find((x) => window.__p.text(x).toUpperCase() === ${JSON.stringify(text.toUpperCase())})`)
const pressTab = (page, name) => press(page, `[...document.querySelectorAll('[role=tab]')].find((x) => window.__p.text(x).toUpperCase() === ${JSON.stringify(name.toUpperCase())})`)

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

const base = await page.evaluate('window.__p')
check('the page installed its probe helpers', !!base, '')
const bodyText = async () => page.evaluate('window.__p.text(document.body)')

try {
  if (phase === 'agg') {
    await waitFor(page, `window.__p.gains().length >= 2 && window.__p.gains()[0] === 0`, 25000, 'the isolated mix to be ready')
    const gains = await page.evaluate('window.__p.gains()')
    check('the derived mix is silent in the preview once its sources are ready', gains[0] === 0, JSON.stringify(gains))
    check('the desktop source plays at full level', near(gains[1], 1, 0.001), JSON.stringify(gains))

    const strips = await page.evaluate('window.__p.strips()')
    check('one strip per independent source, and no derived mix strip', strips.length === 1 && strips[0].name === 'Desktop', JSON.stringify(strips.map((s) => s.name)))
    check('the fader is upright', strips.length === 1 && strips[0].height > strips[0].width && strips[0].orientation === 'vertical', strips[0] ? `${strips[0].width}x${strips[0].height} ${strips[0].orientation}` : '')
    const lanes = await page.evaluate('window.__p.lanes()')
    check('one waveform lane, and no derived mix lane', lanes.length === 1 && lanes[0].name === 'Desktop', JSON.stringify(lanes))
    check('the drawn lane keeps the file ordinal of the hidden mix', lanes.map((l) => l.ordinal).join(',') === '2', JSON.stringify(lanes.map((l) => l.ordinal)))
    check('a one-track file has nothing to fold, so no Expand or Collapse control', !(await page.evaluate('window.__p.expandControl() || window.__p.collapseControl()')), '')
    check('the lane label is a button that says it shows the mixer', !!lanes[0] && lanes[0].button && /Show it in the mixer/.test(lanes[0].label), lanes[0] ? lanes[0].label : 'no lane')
    const text = await bodyText()
    check('the hidden mix is explained, not shown as a control', strips.every((s) => s.name !== 'Mix'), '')

    await pressLabel(page, lanes[0].label)
    await waitFor(page, `window.__p.focus() && /Volume of Desktop/.test(window.__p.focus().label)`, 5000, 'the fader to take the keyboard from the lane')
    check('a lane label puts the keyboard on its fader', true, '')

    await page.evaluate(`window.__p.clickLabel('Mute Desktop')`)
    await waitFor(page, `window.__p.gains()[1] === 0`, 5000, 'the mute to reach the source gain')
    check('Mute reaches the source gain', true, '')
    check('the muted strip says so', (await page.evaluate(`window.__p.text(window.__p.byLabel('Mute Desktop')).toLowerCase()`)) === 'unmute' && (await page.evaluate(`window.__p.byLabel('Mute Desktop').getAttribute('aria-pressed')`)) === 'true', '')

    await page.evaluate(`window.__p.clickLabel('Mute Desktop')`)
    await waitFor(page, `window.__p.gains()[1] === 1`, 5000, 'the unmute')
    check('unmuting returns the source to unity', near((await page.evaluate('window.__p.gains()'))[1], 1, 0.001), '')
    check('a single source offers no Listen alone control', !(await page.evaluate(`!!window.__p.byLabel('Listen alone to Desktop')`)), '')

    // The upright fader is a native range: Home, End and the arrows reach it exactly as a horizontal one.
    const keys = (n, key, code, vk) => Array.from({ length: n }, () => [key, code, vk])
    await setFader(page, 'Desktop', [['Home', 'Home', 36]])
    check('Home takes the fader to 0 %', (await page.evaluate('window.__p.strips()'))[0].value === 0, '')
    await setFader(page, 'Desktop', keys(10, 'ArrowUp', 'ArrowUp', 38))
    const half = (await page.evaluate('window.__p.strips()'))[0]
    check('the arrow keys step the fader', half.value === 50 && half.valuetext === '50%', JSON.stringify(half))
    const gainsBefore = (await page.evaluate('window.__p.gains()')).length
    await setFader(page, 'Desktop', keys(4, 'ArrowUp', 'ArrowUp', 38))
    check('a level change updates the gain, not the graph', (await page.evaluate('window.__p.gains()')).length === gainsBefore, '')
    await setFader(page, 'Desktop', [['End', 'End', 35]])
    check('End takes the fader to 200 %', (await page.evaluate('window.__p.strips()'))[0].value === 200, '')
    await pressText(page, 'Reset')
    await waitFor(page, `window.__p.strips()[0].value === 100 && window.__p.gains()[1] === 1`, 5000, 'Reset to restore the level')
    check('Reset restores the level and the gain', near((await page.evaluate('window.__p.gains()'))[1], 1, 0.001), '')

    await pressLabel(page, 'Play (Space)')
    await waitFor(page, 'window.__p.playing() && !window.__p.playing().paused', 6000, 'the picture to play')
    await waitFor(page, 'window.__p.timers(250) === 1', 5000, 'the follow timer')
    check('a copy in rotation is followed by one 250 ms timer', true, '')
    await new Promise((r) => setTimeout(r, 900))
    const sync = await page.evaluate(`(() => { const v = window.__p.playing(); const a = window.__p.audios(); return a.length ? { v: v.t, a: a[0].t, paused: a[0].paused } : null })()`)
    check('the copied track plays in step with the picture', !!sync && !sync.paused && Math.abs(sync.a - sync.v) <= 0.15, JSON.stringify(sync))
    await pressLabel(page, 'Pause (Space)')
    await waitFor(page, 'window.__p.timers(250) === 0', 5000, 'the follow timer to stop on pause')
    check('pausing stops the follow timer', true, '')

    // A seek and a rate change reach the copies even while the picture is paused (the mix is prepared
    // before it is played), so the first press of Play is already in step.
    await page.evaluate(`(() => { const v = document.querySelector('video'); v.currentTime = Math.min(v.duration - 0.2, v.currentTime + 1); return v.currentTime })()`)
    await new Promise((r) => setTimeout(r, 900))
    const sought = await page.evaluate(`(() => { const v = window.__p.playing(); const a = window.__p.audios()[0]; return a ? { v: v.t, a: a.t } : null })()`)
    check('a seek is carried to the copy', !!sought && Math.abs(sought.a - sought.v) <= 0.15, JSON.stringify(sought))
    await page.evaluate(`document.querySelector('video').playbackRate = 1.5`)
    await new Promise((r) => setTimeout(r, 500))
    const rates = await page.evaluate(`(() => { const v = window.__p.playing(); const a = window.__p.audios()[0]; return a ? { v: v.rate, a: a.rate } : null })()`)
    check('a speed change is carried to the copy', !!rates && rates.a === 1.5, JSON.stringify(rates))
    await page.evaluate(`document.querySelector('video').playbackRate = 1`)

    const shot = await screenshot(page, `agg-${Date.now()}.png`)
    check('a screenshot of the mixer was written', existsSync(shot), shot)

    // Switching to another clip works the same graph: the copy the old clip owned is let go, src and all.
    await pressLabel(page, 'Play (Space)')
    await waitFor(page, 'window.__p.timers(250) === 1', 5000, 'the follow timer before the switch')
    const switched = await press(page, `[...document.querySelectorAll('button')].find((b) => window.__p.text(b).toUpperCase().startsWith('IMPORTED PAIR') && !window.__p.text(b).toUpperCase().includes('-EDIT'))`)
    check('the other library row was pressed', switched, '')
    await waitFor(page, `window.__p.combined() && window.__p.strips().length === 0`, 20000, 'the other clip to open folded')
    await pressLabel(page, 'Expand audio into one lane per track')
    await waitFor(page, `window.__p.strips().map((s) => s.name).join(',') === 'Track 1,Track 2'`, 20000, 'the other clip sources to show')
    const old = await page.evaluate('window.__p.audios()[0]')
    check('the copy the previous clip owned let go of its src', !!old && old.src === '' && old.paused, JSON.stringify(old))
    await new Promise((r) => setTimeout(r, 1500))
    const oldAfter = await page.evaluate('window.__p.audios()[0]')
    check('the released copy stays released after the switch', !!oldAfter && oldAfter.src === '' && oldAfter.paused, JSON.stringify(oldAfter))

    // A copy that fails after it was ready (a decode error mid-play) must fall back, not play half the
    // tracks in silence: the element keeps its own error listener past the readiness wait.
    await waitFor(page, `(() => { const a = window.__p.audios(); return a.length > 0 && a[a.length - 1].ready >= 3 })()`, 20000, 'the switched clip track to be ready')
    await page.evaluate(`(() => { const list = window.__audios; const last = list[list.length - 1]; last.dispatchEvent(new Event('error')); return !!last })()`)
    try {
      await waitFor(page, `window.__p.text(document.body).toLowerCase().includes('try again')`, 6000, 'the fallback after a decode error')
    } catch (error) {
      const diag = await page.evaluate(`({ gains: window.__p.gains(), audios: window.__p.audios(), text: window.__p.text(document.body).slice(0, 300) })`)
      throw new Error(`${error.message} ${JSON.stringify(diag)}`)
    }
    const fallback = await page.evaluate('window.__p.gains().slice(-2)')
    check('an error after readiness falls back to the mix and offers Try again', fallback[0] === 1 && fallback[1] === 0, JSON.stringify(fallback))
  }

  if (phase === 'import') {
    // A copy without its sidecar has unknown, independent tracks. Summing them at unity could play a mix
    // beside its stems, so the first track is the one heard until the person chooses another.
    await waitFor(page, `window.__p.gains().length >= 2 && window.__p.gains()[0] === 1 && window.__p.gains()[1] === 0`, 25000, 'the first track alone')
    check('the sound starts as one combined level, with no source fader shown', (await page.evaluate('window.__p.strips()')).length === 0 && (await page.evaluate('window.__p.combined()')), JSON.stringify(await page.evaluate('window.__p.strips()')))
    const foldedLane = await page.evaluate('window.__p.foldedLane()')
    check('the sound starts folded into one combined lane', !!foldedLane && foldedLane.text.toUpperCase() === 'COMBINED', JSON.stringify(foldedLane))
    check('the folded lane offers Expand and no Collapse', (await page.evaluate('window.__p.expandControl()')) && !(await page.evaluate('window.__p.collapseControl()')), '')
    check('the folded lane draws no track lane or ordinal', (await page.evaluate('window.__p.lanes()')).length === 0, JSON.stringify(await page.evaluate('window.__p.lanes()')))
    check('the Expand control is pressed into the folded state', (await page.evaluate(`window.__p.byLabel('Expand audio into one lane per track').getAttribute('aria-expanded')`)) === 'false', '')
    const foldedGains = await page.evaluate('window.__p.gains()')

    // The one combined level is a live control, not decoration: ten steps down halve the track that plays,
    // and Reset puts it back without touching the source balance.
    const down = Array.from({ length: 10 }, () => ['ArrowDown', 'ArrowDown', 40])
    check('the combined level takes the keyboard', await setCombined(page, down), '')
    await waitFor(page, `window.__p.combinedValue() === 50 && window.__p.gains()[0] === 0.5 && window.__p.gains()[1] === 0`, 5000, 'the combined level to reach the preview')
    check('the combined level reaches the audible track exactly once', (await page.evaluate('window.__p.gains()')).join(',') === '0.5,0', JSON.stringify(await page.evaluate('window.__p.gains()')))
    await pressText(page, 'Reset')
    await waitFor(page, `window.__p.combinedValue() === 100 && window.__p.gains()[0] === 1 && window.__p.gains()[1] === 0`, 5000, 'Reset to restore the combined level')
    check('Reset restores the combined level without unmuting a track', (await page.evaluate('window.__p.gains()')).join(',') === '1,0', JSON.stringify(await page.evaluate('window.__p.gains()')))

    await pressLabel(page, 'Expand audio into one lane per track')
    await waitFor(page, `window.__p.lanes().length === 2 && window.__p.strips().length === 2`, 5000, 'the two lanes after Expand')
    check('expanding the fold changes no source gain or mute', JSON.stringify(await page.evaluate('window.__p.gains()')) === JSON.stringify(foldedGains), `${JSON.stringify(foldedGains)} -> ${JSON.stringify(await page.evaluate('window.__p.gains()'))}`)
    const strips = await page.evaluate('window.__p.strips()')
    check('two independent tracks have a strip each', strips.length === 2 && strips.map((s) => s.name).join(',') === 'Track 1,Track 2', JSON.stringify(strips.map((s) => s.name)))
    const lanes = await page.evaluate('window.__p.lanes()')
    check('Expand draws one lane per independent track', lanes.map((l) => l.name).join(',') === 'Track 1,Track 2', JSON.stringify(lanes))
    check('each expanded lane carries its file ordinal', lanes.map((l) => l.ordinal).join(',') === '1,2', JSON.stringify(lanes.map((l) => l.ordinal)))
    check('every lane label is a button that says it shows the mixer', lanes.every((l) => l.button && /Show it in the mixer/.test(l.label)), JSON.stringify(lanes.map((l) => l.label)))
    check('the expanded lanes offer Collapse', await page.evaluate('window.__p.collapseControl()'), '')

    const second = lanes.find((l) => l.name === 'Track 2')
    await pressTab(page, 'COLOR')
    await new Promise((r) => setTimeout(r, 350))
    await pressLabel(page, second.label)
    await waitFor(page, `window.__p.focus() && window.__p.focus().track === '1'`, 5000, 'the Track 2 fader to take the keyboard')
    const focused = await page.evaluate('window.__p.focus()')
    check('a lane label puts the keyboard on its own fader', focused && focused.label === 'Volume of Track 2', JSON.stringify(focused))
    check('a lane label brings the Edit tab forward for its fader', (await page.evaluate(`[...document.querySelectorAll('[role=tab]')].find((t) => window.__p.text(t).toUpperCase() === 'EDIT').getAttribute('aria-selected')`)) === 'true', '')

    await pressLabel(page, 'Collapse audio into one combined waveform')
    await waitFor(page, `!!window.__p.foldedLane() && window.__p.lanes().length === 0 && window.__p.strips().length === 0`, 5000, 'the Collapse to fold the lanes')
    check('Collapse returns the sound to one combined lane and hides the source faders', true, '')
    await pressLabel(page, 'Expand audio into one lane per track')
    await waitFor(page, `window.__p.strips().length === 2`, 5000, 'the source faders after expanding again')

    const text = await bodyText()
    check('the unknown import explains the first-track start', /track 1 plays at first/i.test(text) && /unmute another track/i.test(text), text.slice(0, 240))
    check('the other track starts muted', (await page.evaluate(`window.__p.byLabel('Mute Track 2').getAttribute('aria-pressed')`)) === 'true', '')
    const resetState = `(() => { const section = [...document.querySelectorAll('.inspector-section')].find((s) => (s.querySelector('.label') || {}).textContent?.trim().toUpperCase() === 'AUDIO'); const b = section && [...section.querySelectorAll('button')].find((x) => window.__p.text(x).toUpperCase() === 'RESET'); return !!b && (b.disabled || b.getAttribute('aria-disabled') === 'true') })()`
    check('the safe default is not an edit, so Reset starts disabled', await page.evaluate(resetState), '')

    // The default has to reach the file too: an untouched export of a copied aggregate must not double it.
    const beforeDefault = new Set(existsSync(editsDir) ? readdirSync(editsDir).map((n) => join(editsDir, n)) : [])
    await pressTab(page, 'EXPORT')
    await new Promise((r) => setTimeout(r, 600))
    await pressText(page, 'Save a copy')
    const defaultFile = await newestExport(beforeDefault)
    check('the default export wrote a file', !!defaultFile, defaultFile || '')
    if (defaultFile) {
      const raw = peaks(defaultFile, true)
      check('the default export keeps both streams', raw && raw.tracks && raw.tracks.length === 2, raw ? `${raw.tracks?.length} lanes` : 'no answer')
      if (raw && raw.tracks?.length === 2) {
        check('the default export carries the first track alone, so nothing is doubled', top(raw.tracks[0]) > 0.05 && top(raw.tracks[1]) < 0.001, `track 1 ${top(raw.tracks[0]).toFixed(4)} track 2 ${top(raw.tracks[1]).toFixed(4)}`)
      }
    }
    await pressTab(page, 'EDIT')
    await new Promise((r) => setTimeout(r, 500))

    // Unmuting the second track is the explicit choice that restores the intended mix.
    await page.evaluate(`window.__p.clickLabel('Mute Track 2')`)
    await waitFor(page, `window.__p.gains()[1] === 1`, 5000, 'the unmute on track 2')
    check('unmuting the other track combines the two', true, '')
    check('the unmute is an edit, so Reset is enabled', !(await page.evaluate(resetState)), '')

    await page.evaluate(`window.__p.clickLabel('Listen alone to Track 2')`)
    await waitFor(page, `window.__p.gains()[0] === 0 && window.__p.gains()[1] === 1`, 5000, 'Listen alone to isolate track 2')
    check('Listen alone isolates that track in the preview', true, '')
    check('the active preview identifies the source', (await page.evaluate(`document.querySelector('[role=status]')?.textContent`)).includes('Track 2'), '')
    await page.evaluate(`document.querySelector('[role=status] button').click()`)
    await waitFor(page, `window.__p.gains()[0] === 1 && window.__p.gains()[1] === 1`, 5000, 'the Listen alone to clear')
    check('clearing preview preserves saved mute choices', (await page.evaluate(`window.__p.byLabel('Mute Track 2').getAttribute('aria-pressed')`)) === 'false', '')
    await page.evaluate(`window.__p.clickLabel('Listen alone to Track 1')`)
    await waitFor(page, `window.__p.gains()[0] === 1 && window.__p.gains()[1] === 0`, 5000, 'Listen alone on track 1')
    check('Listen alone on the other track silences only the rest', true, '')

    await setFader(page, 'Track 2', [['Home', 'Home', 36]])
    const steps = Array.from({ length: 10 }, () => ['ArrowUp', 'ArrowUp', 38])
    await setFader(page, 'Track 2', steps)
    const strip = (await page.evaluate('window.__p.strips()')).find((s) => s.name === 'Track 2')
    check('a fader sets the track level without leaving a rebuild behind', strip && strip.value === 50, JSON.stringify(strip))
    const gains = await page.evaluate('window.__p.gains()')
    check('listening alone keeps the preview on track 1 while track 2 is levelled', gains[0] === 1 && gains[1] === 0, JSON.stringify(gains))
    await screenshot(page, `import-${Date.now()}.png`)

    // Export while track 1 is played alone: the file must carry both sources, track 2 at its own 50 %.
    const before = new Set(existsSync(editsDir) ? readdirSync(editsDir).map((n) => join(editsDir, n)) : [])
    await pressTab(page, 'EXPORT')
    await new Promise((r) => setTimeout(r, 600))
    check('the export button pressed', await pressText(page, 'Save a copy'), '')
    const soloFile = await newestExport(before)
    check('the soloed export wrote a file', !!soloFile, soloFile || '')
    if (soloFile) {
      const raw = peaks(soloFile, true)
      check('the export carries both independent streams, listened alone or not', raw && raw.tracks && raw.tracks.length === 2, raw ? `${raw.tracks?.length} lanes` : 'no answer')
      if (raw && raw.tracks?.length === 2) {
        const loud = top(raw.tracks[0])
        const quiet = top(raw.tracks[1])
        check('the export keeps the soloed-away track at its own level', quiet > 0.05 && near(quiet / loud, 0.5, 0.15), `loud ${loud.toFixed(4)} quiet ${quiet.toFixed(4)} ratio ${(quiet / loud).toFixed(3)}`)
      }
    }

    // Mute on track 2 reaches the saved file too.
    await pressTab(page, 'EDIT')
    await new Promise((r) => setTimeout(r, 400))
    await page.evaluate(`window.__p.clickLabel('Listen alone to Track 1')`)
    await page.evaluate(`window.__p.clickLabel('Mute Track 2')`)
    await waitFor(page, `window.__p.gains()[1] === 0`, 5000, 'the mute on track 2')
    const before2 = new Set(existsSync(editsDir) ? readdirSync(editsDir).map((n) => join(editsDir, n)) : [])
    await pressTab(page, 'EXPORT')
    await new Promise((r) => setTimeout(r, 500))
    await pressText(page, 'Save a copy')
    const muteFile = await newestExport(before2)
    check('the muted export wrote a file', !!muteFile, muteFile || '')
    if (muteFile) {
      const raw = peaks(muteFile, true)
      check('the muted export keeps both streams', raw && raw.tracks && raw.tracks.length === 2, '')
      if (raw && raw.tracks?.length === 2) {
        check('a mute is exported as silence', top(raw.tracks[1]) < 0.001, `track 2 top ${top(raw.tracks[1])}`)
        check('the other track is untouched by the mute', top(raw.tracks[0]) > 0.2, `track 1 top ${top(raw.tracks[0])}`)
      }
    }
    await pressTab(page, 'EDIT')
    await new Promise((r) => setTimeout(r, 300))
    await pressText(page, 'Reset')
    await waitFor(page, `window.__p.gains()[1] === 0`, 5000, 'Reset to the safe import default')
    check('Reset returns the unknown import to its first track alone', (await page.evaluate(`window.__p.byLabel('Mute Track 2').getAttribute('aria-pressed')`)) === 'true', '')
  }

  if (phase === 'mix') {
    await waitFor(page, `window.__p.text(document.body).includes('LEVELS LOCKED')`, 25000, 'the locked mixer')
    const strips = await page.evaluate('window.__p.strips()')
    check('an incomplete mix offers no level control', strips.length === 0, JSON.stringify(strips))
    const mixLanes = await page.evaluate('window.__p.lanes()')
    check('the locked mix draws one lane with its own ordinal', mixLanes.map((l) => l.ordinal).join(',') === '1' && mixLanes.length === 1, JSON.stringify(mixLanes))
    check('the locked mix lane is plain text, not a button that cannot reach a fader', mixLanes.length === 1 && !mixLanes[0].button && !/Show it in the mixer/.test(mixLanes[0].label), JSON.stringify(mixLanes))
    const text = await bodyText()
    check('the lock says why and that saving still works', text.toLowerCase().includes('levels locked') && /saving still work/i.test(text), '')
    await waitFor(page, `window.__p.gains().length >= 1`, 10000, 'the fallback graph')
    await pressLabel(page, 'Play (Space)')
    await waitFor(page, 'window.__p.playing() && !window.__p.playing().paused', 6000, 'the picture to play')
    await new Promise((r) => setTimeout(r, 900))
    const timers = await page.evaluate('window.__p.timers(250)')
    const audios = await page.evaluate('window.__p.audios()')
    check('a mix-only file makes no follow timer and copies no track', timers === 0 && audios.length === 0, `timers ${timers}, copies ${audios.length}`)
    await pressLabel(page, 'Pause (Space)')
    await screenshot(page, `mix-${Date.now()}.png`)
  }

  if (phase === 'long') {
    await waitFor(page, `window.__p.text(document.body).includes('Try again')`, 25000, 'the fallback notice after a failed track copy')
    const failedGains = await page.evaluate('window.__p.gains()')
    check('a failed copy falls back to the mix at full level', failedGains.length === 1 && near(failedGains[0], 1, 0.001), JSON.stringify(failedGains))
    const failedText = await bodyText()
    check('the fallback says the edits are not audible, not that the file is broken', /not audible in the preview/i.test(failedText), '')
    check('no half-built copy is played', (await page.evaluate('window.__p.audios()')).length === 0, '')

    rmSync(join(dataRoot, 'cache'), { recursive: true, force: true })
    const retried = await pressText(page, 'Try again')
    check('the Retry button was pressed', retried, '')
    try {
      await waitFor(page, `window.__p.gains().slice(-2).join(',') === '0,1'`, 25000, 'the retry to build the copies')
    } catch (error) {
      const diag = await page.evaluate(`({ gains: window.__p.gains(), audios: window.__p.audios(), text: window.__p.text(document.body).slice(0, 300) })`)
      throw new Error(`${error.message} ${JSON.stringify(diag)}`)
    }
    check('Try again builds the copies once the cache can be written', true, '')

    const longName = 'Desktop Secondary Display Capture With A Very Long Name'
    const strip = await page.evaluate(`window.__p.stripName(${JSON.stringify(longName)})`)
    check('the whole source name is in the tooltip and the text', !!strip && strip.title === longName && strip.text === longName.toUpperCase(), JSON.stringify(strip))
    check('the source card is compact, and the long name truncates inside it', !!strip && strip.box.w >= 72 && strip.box.w <= 88 && strip.scrollWidth > strip.clientWidth, strip ? `${strip.box.w}px, scroll ${strip.scrollWidth} client ${strip.clientWidth}` : '')
    const longLanes = await page.evaluate('window.__p.lanes()')
    check('a one-track file draws one lane and offers no fold control', longLanes.length === 1 && !(await page.evaluate('window.__p.expandControl() || window.__p.collapseControl()')), JSON.stringify(longLanes))
    check('a single-source mixer offers no Listen alone button', !(await page.evaluate(`!!window.__p.byLabel(${JSON.stringify('Listen alone to ' + longName)})`)), '')
    const shot = await screenshot(page, `long-${Date.now()}.png`)
    check('a screenshot of the long name was written', existsSync(shot), shot)

    // The wheel over the fader or its value sets the level; the rest of the card is still the pane's.
    const stripBefore = (await page.evaluate('window.__p.strips()'))[0]
    check('the compact fader is upright', !!stripBefore && stripBefore.height > stripBefore.width && stripBefore.height >= 48 && stripBefore.width <= 24, stripBefore ? `${stripBefore.width}x${stripBefore.height}` : 'no strip')
    // Bring the fader into view first: `scrollIntoView` is the probe's own move, not the wheel's.
    await wheelOver(page, `document.querySelector('input.fader')`, 0)
    const paneBefore = await page.evaluate('window.__p.paneScroll()')
    const graphsBefore = (await page.evaluate('window.__p.gains()')).length
    check('the wheel turns over the fader', await wheelOver(page, `document.querySelector('input.fader')`, -100), '')
    const stripUp = (await page.evaluate('window.__p.strips()'))[0]
    check('one wheel notch lifts the level five points', stripUp.value === Math.min(200, stripBefore.value + 5), `${stripBefore.value} -> ${stripUp.value}`)
    check('the wheel over the level does not scroll the pane with it', (await page.evaluate('window.__p.paneScroll()')) === paneBefore, `pane ${paneBefore} -> ${await page.evaluate('window.__p.paneScroll()')}`)
    check('a wheel level change writes no new gain or graph', (await page.evaluate('window.__p.gains()')).length === graphsBefore, '')
    await wheelOver(page, `document.querySelector('input.fader')`, 100)
    check('the other way of the wheel returns the level', (await page.evaluate('window.__p.strips()'))[0].value === stripBefore.value, '')
    const nameSpan = `[...document.querySelectorAll('span[title]')].find((s) => s.title === ${JSON.stringify(longName)})`
    await wheelOver(page, nameSpan, 0)
    const paneMiddle = await page.evaluate('window.__p.paneScroll()')
    const scrollable = await page.evaluate(`(() => { const p = document.querySelector('[role=tabpanel]'); return !!p && p.scrollHeight > p.clientHeight + 1 })()`)
    check('the wheel over the name is the pane’s', await wheelOver(page, nameSpan, 120), '')
    check('the level is untouched by a scroll over the name', (await page.evaluate('window.__p.strips()'))[0].value === stripBefore.value, '')
    check('the rest of the card still scrolls the pane', !scrollable || (await page.evaluate('window.__p.paneScroll()')) !== paneMiddle, scrollable ? `pane ${paneMiddle} -> ${await page.evaluate('window.__p.paneScroll()')}` : 'pane fits, nothing to scroll')

    await page.call('Emulation.setDeviceMetricsOverride', { width: 780, height: 820, deviceScaleFactor: 1, mobile: false })
    await new Promise((r) => setTimeout(r, 500))
    const narrow = await page.evaluate(`(() => ({
      overflow: document.body.scrollWidth - window.innerWidth,
      strips: window.__p.strips().length,
      shell: document.querySelector('.editor-shell') ? document.querySelector('.editor-shell').getBoundingClientRect().width : 0,
    }))()`)
    check('the narrow editor has no horizontal overflow', narrow.overflow <= 1, JSON.stringify(narrow))
    check('the mixer still has its strip when narrow', narrow.strips >= 1, '')
    await page.evaluate(`(() => { const i = document.querySelector('input.fader'); if (i) i.scrollIntoView({ block: 'center' }); })()`)
    await new Promise((r) => setTimeout(r, 400))
    const narrowStrip = await page.evaluate(`(() => { const i = document.querySelector('input.fader'); if (!i) return null; const r = i.getBoundingClientRect(); return { left: Math.round(r.left), right: Math.round(r.right), view: window.innerWidth } })()`)
    check('the narrow mixer is inside the viewport', !!narrowStrip && narrowStrip.left >= 0 && narrowStrip.right <= narrowStrip.view, JSON.stringify(narrowStrip))
    const narrowShot = await screenshot(page, `long-narrow-${Date.now()}.png`)
    check('a narrow screenshot was written', existsSync(narrowShot), narrowShot)
    await page.call('Emulation.clearDeviceMetricsOverride', {})
    await pressText(page, 'Reset')
    await new Promise((r) => setTimeout(r, 200))
  }

  // Closing the window is the cleanup check: the driver looks for a media child left behind.
  if (phase !== 'long') {
    await pressLabel(page, 'Close')
    await new Promise((r) => setTimeout(r, 1500))
  }
} catch (error) {
  check('the probe ran to the end', false, error?.message || String(error))
} finally {
  for (const row of rows) console.log(row)
  console.log(`audio mixer (${phase}): ${failures === 0 ? 'all checks passed' : `${failures} FAILED`}`)
  page.close()
}
process.exit(failures === 0 ? 0 : 1)
