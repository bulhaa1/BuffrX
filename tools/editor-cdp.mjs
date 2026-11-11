// Drives the editor page over the DevTools protocol: no Playwright, no guessed pixel clicks.
//
//   node tools/editor-cdp.mjs --port=9333 [--eval="<js>"] [--dump]
//
// The page must have been started with BUFFRX_WEBVIEW_DEBUG_PORT set (src-tauri/src/main.rs), which is
// how the probe reaches a real editor window with a real engine behind it.
const port = (process.argv.find((a) => a.startsWith('--port=')) || '--port=9333').slice(7)
const evaluateArg = process.argv.find((a) => a.startsWith('--eval='))
const dump = process.argv.includes('--dump')

async function target() {
  const reply = await fetch(`http://127.0.0.1:${port}/json`)
  const pages = (await reply.json()).filter((t) => t.type === 'page')
  if (!pages.length) throw new Error('no page target')
  return pages[0].webSocketDebuggerUrl
}

function session(url) {
  const socket = new WebSocket(url)
  let next = 1
  const waiting = new Map()
  const ready = new Promise((resolve, reject) => {
    socket.addEventListener('open', () => resolve())
    socket.addEventListener('error', (e) => reject(new Error('websocket: ' + e.message)))
  })
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data)
    const entry = waiting.get(message.id)
    if (!entry) return
    waiting.delete(message.id)
    if (message.error) entry.reject(new Error(JSON.stringify(message.error)))
    else entry.resolve(message.result)
  })
  const send = (method, params = {}) =>
    new Promise((resolve, reject) => {
      const id = next++
      waiting.set(id, { resolve, reject })
      socket.send(JSON.stringify({ id, method, params }))
    })
  const evaluate = async (expression) => {
    const result = await send('Runtime.evaluate', {
      expression,
      returnByValue: true,
      awaitPromise: true,
      userGesture: true,
    })
    if (result.exceptionDetails) throw new Error(result.exceptionDetails.text + ': ' + JSON.stringify(result.exceptionDetails.exception?.description || ''))
    return result.result.value
  }
  return { ready, evaluate, close: () => socket.close() }
}

const url = await target()
const page = session(url)
await page.ready
await page.evaluate('new Promise(r => setTimeout(r, 400))')

if (evaluateArg) {
  const value = await page.evaluate(evaluateArg.slice(7))
  console.log(typeof value === 'string' ? value : JSON.stringify(value, null, 2))
} else if (dump) {
  const inventory = await page.evaluate(`(() => {
    const text = (el) => (el.innerText || el.textContent || '').trim().replace(/\\s+/g, ' ')
    const buttons = [...document.querySelectorAll('button')].map((b) => text(b).slice(0, 40) + '  [' + (b.getAttribute('aria-label') || b.className.split(' ')[0] || '') + ']' + (b.disabled ? ' disabled' : ''))
    const tabs = [...document.querySelectorAll('[role=tab],.tab,[data-tab]')].map((t) => text(t).slice(0, 30) + '  ' + (t.getAttribute('aria-selected') || t.getAttribute('data-state') || ''))
    const body = text(document.body).slice(0, 1200)
    return { title: document.title, buttons, tabs, body, classes: [...document.querySelectorAll('[class]')].slice(0, 60).map((e) => e.className.toString().slice(0, 60)) }
  })()`)
  console.log(JSON.stringify(inventory, null, 2))
} else {
  console.log('ready: ' + (await page.evaluate('document.title')))
}
page.close()
process.exit(0)
