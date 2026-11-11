// The link page's states, without a browser: what the readout says and what it offers.
// Runs the real link.js against a stub DOM, so a change that breaks a state fails here.
//
// Usage: node tools/link-page-test.mjs [--file <link.js>]

import { readFileSync } from 'node:fs';
import { resolve, dirname, join } from 'node:path';
import vm from 'node:vm';

const args = process.argv.slice(2);
const fileArg = args.indexOf('--file');
const target = resolve(fileArg >= 0 ? args[fileArg + 1] : 'site-main/l/link.js');
const source = readFileSync(target, 'utf8');
const html = readFileSync(join(dirname(target), 'index.html'), 'utf8');

function ticket(expiresAt = 0) {
  const body = Buffer.alloc(78);
  body[0] = 2;
  body.writeUInt32BE(expiresAt || Math.floor(Date.now() / 1000) + 3600, 65);
  const relay = Buffer.from('~1aps1-1');
  body[69] = relay.length;
  relay.copy(body, 70);
  return body.subarray(0, 70 + relay.length).toString('base64url');
}

function element(id) {
  const node = {
    id,
    className: '',
    textContent: '',
    hidden: false,
    href: '',
    title: '',
    style: {},
    listeners: {},
    addEventListener(name, fn) { this.listeners[name] = fn; },
    append() {},
    appendChild(c) { return c; },
    setAttribute(name, value) { this[name] = value; },
    removeAttribute(name) { delete this[name]; },
    select() {},
    remove() {},
  };
  return node;
}

function run({ windows = true, focused = true, expiresAt = 0, rawHash }) {
  const ids = ['card', 'kicker', 'title', 'who', 'facts', 'live', 'livetext', 'clock', 'state', 'acts', 'open', 'copy', 'copylabel', 'get', 'under', 'deadacts', 'get2'];
  const nodes = Object.fromEntries(ids.map((id) => [id, element(id)]));
  // copyAddress names the label inside a button, so the icon survives the answer.
  for (const [id, node] of Object.entries(nodes)) {
    node.querySelector = (selector) => (selector === '.lbl' ? (nodes[id + 'label'] ?? node) : null);
  }
  const timers = [];
  const document = {
    hidden: false,
    title: '',
    body: { appendChild() {} },
    getElementById: (id) => nodes[id] ?? null,
    addEventListener: () => {},
    createElement: () => element('created'),
    execCommand: () => true,
    // Whether the page has focus at the 7 s look: the only signal the handoff leaves behind.
    hasFocus: () => focused,
  };
  const location = { hash: rawHash ?? ('#' + ticket(expiresAt) + '?n=Alex'), href: 'https://buffrx.app/l/' };
  const window = {
    listeners: {},
    addEventListener(name, fn) { this.listeners[name] = fn; },
    opener: null,
    close() {},
  };
  const sandbox = {
    document,
    window,
    location,
    navigator: {
      userAgent: windows ? 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)' : 'Mozilla/5.0 (Macintosh)',
      clipboard: { writeText: async () => {} },
    },
    setTimeout: (fn, ms = 0) => { timers.push({ fn, ms }); return timers.length; },
    setInterval: () => 0,
    clearInterval: () => {},
    URLSearchParams,
    Date,
    console,
  };
  sandbox.globalThis = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(source, sandbox);

  return { nodes, timers, location };
}

// Timers run in the order a browser would fire them: the shortest first, whatever order they were set.
function flush(timers) {
  for (const t of [...timers].sort((a, b) => a.ms - b.ms)) t.fn();
}

let failures = 0;
function check(name, ok, detail = '') {
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${name}${detail ? ' — ' + detail : ''}`);
  if (!ok) failures++;
}

// 1. Windows, nothing takes over: the readout must say so and offer every way forward.
{
  const { nodes, timers, location } = run({ windows: true });
  check('the sender is named', nodes.who.textContent === 'Alex', nodes.who.textContent);
  check('the clock is running', /^\d\d:\d\d:\d\d$/.test(nodes.clock.textContent), nodes.clock.textContent);
  check('it says it is opening, and what Windows will ask', nodes.state.textContent.includes('opening buffrx'), nodes.state.textContent);
  check('the protocol is handed over', location.href.startsWith('buffrx://l/'), location.href.slice(0, 18));
  check('the way in is offered at once', nodes.open.hidden === false && nodes.acts.hidden === false);
  flush(timers);
  check('nothing took over: it says the browser had no app to hand the link to', nodes.state.textContent.includes('did not answer'), nodes.state.textContent);
  check('the live dot goes to waiting', nodes.livetext.textContent === 'waiting', nodes.livetext.textContent);
  check('the download is offered', nodes.get.hidden === false);
  check('copying is offered', nodes.copy.hidden === false);
}

// 2. Windows, the app took the handoff.
{
  const { nodes, timers } = run({ windows: true, focused: false });
  flush(timers);
  check('the app opened: the readout says so', nodes.state.textContent.includes('buffrx is opening'), nodes.state.textContent);
  check('and offers copying', nodes.copy.hidden === false);
  check('the download stays available to everyone', nodes.get.hidden === false);
}

// 3. Windows, the browser asked and the person said no: focus came back, so nothing opened.
{
  const { nodes, timers } = run({ windows: true, focused: true });
  flush(timers);
  check('a dismissed permission dialog is not a launch', nodes.state.textContent.includes('did not answer'), nodes.state.textContent);
  check('the cell goes to waiting', nodes.livetext.textContent === 'waiting', nodes.livetext.textContent);
  check('the way in is still offered', nodes.open.hidden === false && nodes.copy.hidden === false);
}

// 4. A phone or a Mac: no handoff, and the one useful action is to carry the link.
{
  const { nodes, location } = run({ windows: false });
  check('it names the PC the link needs', nodes.state.textContent.includes('open this on the windows pc'), nodes.state.textContent);
  check('it hands nothing over', location.href === 'https://buffrx.app/l/');
  check('the link can be copied', nodes.copy.hidden === false);
  check('the download stays available, even on a Mac', nodes.get.hidden === false);
}

// 5. A 24-hour link keeps its clock.
{
  const { nodes } = run({ windows: true, expiresAt: Math.floor(Date.now() / 1000) + 86400 });
  check('a 24-hour link shows its clock', /^24:00:0\d$/.test(nodes.clock.textContent), nodes.clock.textContent);
}

// 6. Nothing to read: a garbage payload is said plainly, and the clip furniture goes.
{
  const { nodes } = run({ windows: true, rawHash: '#nope' });
  check('an unfinished link is refused', nodes.title.textContent.includes('not a BuffrX link'), nodes.title.textContent);
  check('the dead card takes over', nodes.card['data-dead'] === '' && nodes.under.hidden === true);
  check('the actions are the dead ones', nodes.deadacts.hidden === false && nodes.acts.hidden === true);
  check('it says what to do instead', nodes.state.textContent.includes('copy the whole address'), nodes.state.textContent);
}

// 7. A link that ran out: same card, and the sender is named so the ask can be aimed.
{
  const { nodes } = run({ windows: true, expiresAt: Math.floor(Date.now() / 1000) - 3 * 3600 });
  check('an expired link says so', nodes.title.textContent.includes('expired'), nodes.title.textContent);
  check('the dead card takes over', nodes.card['data-dead'] === '' && nodes.under.hidden === true);
  check('the clock is gone', nodes.clock.hidden === true);
  check('it names who to ask', nodes.state.textContent.includes('ask Alex'), nodes.state.textContent);
}

// 8. The copy button.
{
  const { nodes } = run({ windows: true, focused: false });
  nodes.copy.listeners.click();
  setImmediate(() => {
    check('copying says so', nodes.copylabel.textContent === 'copied' || nodes.copylabel.textContent === 'select the address bar', nodes.copylabel.textContent);
    const gets = html.match(/href="https:\/\/buffrx\.app\/"/g) || [];
    check('every download link points at buffrx.app', gets.length >= 2 && !html.includes('href="../"'), String(gets.length));
    const icons = html.match(/<svg class="ico"/g) || [];
    check('every action carries a drawn icon', icons.length === 3, String(icons.length));
    console.log(failures === 0 ? '\nlink page: all states right' : `\nlink page: ${failures} failed`);
    process.exit(failures === 0 ? 0 : 1);
  });
}
