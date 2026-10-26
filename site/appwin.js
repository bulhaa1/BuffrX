// BuffrX landing page: a simplified panel, quick panel and editor drawn in HTML, in the app's own colour roles (themes.css). They keep the
// app's look (tiled panes with their titles in the border, the accent on the focused one, the cell meters, the capital letters) and show
// only what the story needs, not every control the app has. site.js
// mounts it twice: in the hero, where it plays a record, an edit and a share, and in the Look section, where the visitor picks the
// theme. The icons are the app's own paths (src/engine/ui/ui_icons.cpp). Nothing here makes a request, and the markup never uses a style attribute (the CSP): sizes and positions are set through
// the CSSOM, and the editor's cut is two custom properties on its lane.
const BXWindow = (() => {
'use strict';

// The B of the wordmark (brand/buffrx-mark.svg) and the app's icon paths (src/engine/ui/ui_icons.cpp, ui/src/components/icons.tsx).
const B = 'M3.2 93L42.2 143.4L230 143.4C254.3 148.8 262 168.5 240 184.4C233.3 189.3 225.7 190.8 218 192.8L118.3 192.8L152.2 235.4L234 235.4C243.6 237.4 251.8 238.8 260 244.6C265.3 248.3 266.8 254.1 267.6 260C269.4 274.1 259.7 282.4 247 285.3L96 285.3L95.5 168L42.8 201.5L42.8 336.4L257 336.4C300.7 328.5 320.5 313.6 320.5 266C320.5 250 318.2 235.3 308 222.6C301.7 214.8 292 211.9 284.6 206C289.1 196.6 298.7 189.6 302.3 179C309.6 157.2 304.8 115.4 281 103.7C270.7 98.6 260.6 96.2 250 93L3.2 93Z';
const ICONS = {
  save: 'M12 4v10m0 0-4-4m4 4 4-4M5 17v2a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-2',
  history: 'M4 12a8 8 0 1 0 2.4-5.7L4 8.5M4 4v4.5h4.5M12 8v4.5l3 2',
  camera: 'M4 8a2 2 0 0 1 2-2h2l1.5-2h5L16 6h2a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V8Zm8 1a3.5 3.5 0 1 0 0 7 3.5 3.5 0 0 0 0-7Z',
  settings: 'M12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6Zm7.4 3a7.4 7.4 0 0 0-.1-1.2l2-1.6-2-3.4-2.4 1a7.6 7.6 0 0 0-2-1.2L14.5 3h-4l-.4 2.6a7.6 7.6 0 0 0-2 1.2l-2.4-1-2 3.4 2 1.6a7.4 7.4 0 0 0 0 2.4l-2 1.6 2 3.4 2.4-1a7.6 7.6 0 0 0 2 1.2l.4 2.6h4l.4-2.6a7.6 7.6 0 0 0 2-1.2l2.4 1 2-3.4-2-1.6c.1-.4.1-.8.1-1.2Z',
  scissors: 'M6 9a3 3 0 1 0 0-6 3 3 0 0 0 0 6Zm0 12a3 3 0 1 0 0-6 3 3 0 0 0 0 6ZM8.2 7.8 20 18M8.2 16.2 20 6',
  folder: 'M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7Z',
  minus: 'M6 12h12', square: 'M6 6h12v12H6z', close: 'M6 6l12 12M18 6 6 18',
  compact: 'M4 14h6v6M20 10h-6V4M14 10l7-7M3 21l7-7', expand: 'M15 3h6v6M9 21H3v-6M21 3l-7 7M3 21l7-7',
  dots: 'M5 12h.01M12 12h.01M19 12h.01', mic: 'M12 3a3 3 0 0 0-3 3v6a3 3 0 0 0 6 0V6a3 3 0 0 0-3-3Zm-6 9a6 6 0 0 0 12 0M12 18v3',
  share: 'M12 15V3m-5 5 5-5 5 5M5 13v7h14v-7',
  palette: 'M12 3a9 9 0 1 0 0 18c1 0 1.6-.8 1.6-1.7 0-.4-.2-.8-.4-1.1-.3-.3-.4-.7-.4-1.1 0-.9.7-1.6 1.6-1.6H16a5 5 0 0 0 5-5c0-4.1-4-7.5-9-7.5ZM7.5 12h.01M10 7.5h.01M15 8h.01',
  refresh: 'M20 11a8 8 0 1 0-2.3 5.7M20 4v7h-7', chevron: 'M7 10l5 5 5-5',
  play: 'M8 5v14l11-7L8 5Z', undo: 'M9 14 4 9l5-5M4 9h10a6 6 0 0 1 0 12h-3', trash: 'M5 7h14M10 7V4h4v3M7 7l1 13h8l1-13',
};
const ic = (name, size = 15) => `<svg class="b-ic" viewBox="0 0 24 24" width="${size}" height="${size}" aria-hidden="true"><path d="${ICONS[name]}"/></svg>`;
const mark = (size) => `<svg class="b-mark" viewBox="-18 35 360 360" width="${size}" height="${size}" aria-hidden="true"><path fill="currentColor" d="${B}"/></svg>`;
const mmss = (s) => `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;

// Demo data. Each clip belongs to one of the games in games.js, and its picture is that game's.
const G = BXGames;
const CLIPS = [
  { d: '1:00', secs: 60, game: 'cs2', v: 2, size: '41 MB', at: '7:02 PM' },
  { d: '0:20', secs: 20, game: 'finals', size: '14 MB', at: '6:47 PM' },
  { d: '0:42', secs: 42, game: 'acm', size: '29 MB', at: '6:31 PM' },
  { d: '1:00', secs: 60, game: 'forza', size: '43 MB', at: '5:58 PM' },
];
const FRESH = { d: '0:14', secs: 14, game: 'rivals', size: '10 MB', at: '7:08 PM' }; // the recording a demo makes
const rng = (a) => () => { a |= 0; a = (a + 0x6d2b79f5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };

// Small parts, named as the app names them.
const meter = (n, lit, fromRight = false, k = '') => `<div class="b-meter"${k ? ` data-k="${k}"` : ''}>${Array.from({ length: n }, (_, i) => `<i${(fromRight ? i >= n - lit : i < lit) ? ' class="on"' : ''}></i>`).join('')}</div>`;
const pane = (cls, title, aside, body) => `<section class="b-pn ${cls}">${title ? `<div class="b-t">${title}</div>` : ''}${aside ? `<span class="b-a">${aside}</span>` : ''}${body}</section>`;
const caps = (chord) => `<span class="b-caps">${chord.split('+').map((k) => `<b>${k}</b>`).join('')}</span>`;
const act = (icon, title, detail, chord, cls = '') => `<div class="b-act ${cls}">${icon}<b>${title}</b>${detail ? `<small>&middot; ${detail}</small>` : ''}${caps(chord)}</div>`;
const recAct = (lead) => `<div class="b-act${lead ? ' sel' : ''}" data-k="recrow"><i class="b-rsq"></i><b data-k="rectitle">Record</b><small data-k="recdetail"></small><span class="b-caps"><b>Alt</b><b>3</b></span></div>`;
const btn = (name, size, cls = '') => `<span class="b-ib ${cls}">${ic(name, size)}</span>`;
const toast = '<div class="b-toast" data-k="toast" aria-hidden="true"><div><span class="b-tag" data-k="ttag">done</span><b data-k="ttitle">Clip saved</b></div><small data-k="tdetail">1:00 &middot; ${G.name(FRESH.game)}</small></div>';
const MENU = [['Edit', 'E'], ['Insta Share', 'S'], ['Copy', 'C'], ['Delete', 'Del']];
const menu = '<div class="b-menu" data-k="menu">' + MENU.map(([n, k]) => `<div class="b-mi"><b>${n}</b><small>${k}</small></div>`).join('') + '</div>';
const card = (c, isNew) => `<div class="b-card${isNew ? ' new' : ''}"><div class="b-frame">${G.shot(c.game, '', c.v || 0)}<span class="b-badge">${c.d}</span></div>
  <div class="b-cap"><b>${G.name(c.game)}</b><small>${c.size}</small>${btn('dots', 15)}</div></div>`;
const rec = (c, isNew) => `<div class="b-rec${isNew ? ' new' : ''}"><div class="b-rthumb">${G.shot(c.game, '', c.v || 0)}</div><div><b>${G.name(c.game)}</b><small>${c.d} &middot; ${c.at} &middot; ${c.size}</small></div>${btn('scissors', 14)}${btn('share', 14)}</div>`;

// The Alt+4 viewer, drawn from a list of clips: a picture with the title and age over it, five clips with their ages, the keys that work from a game.
const VIEW_BASE = [['Round 12', 'rivals', 2], ['Last second', 'cs2', 2], ['Ace', 'finals', 0], ['Final lap', 'forza', 0], ['Photo finish', 'msfs', 0]];
const VIEW_AGES = ['now', '6m', '12m', '31m', '58m'];
const viewerBody = (list, pos) => `<div class="b-vwpic">${G.shot(list[pos][1], '', list[pos][2] || 0)}<div class="b-vwtop"><span><b>${list[pos][0]}</b><small>${pos === 0 ? 'just now' : VIEW_AGES[pos] + ' ago'}</small></span><span class="b-vwk">${ic('scissors', 12)}${ic('expand', 12)}${ic('close', 12)}</span></div><i class="b-vwbar"></i></div>
  <div class="b-vwstrip">${list.map(([title, game, v], i) => `<button type="button" tabindex="-1" class="b-vwc${i === pos ? ' on' : ''}" data-i="${i}" aria-label="${title}, ${VIEW_AGES[i]}">${G.shot(game, '', v || 0)}<small>${VIEW_AGES[i]}</small></button>`).join('')}</div>
  <div class="b-vwhint"><span>ctrl alt &larr; &rarr; step</span><span>alt 4 close</span></div>`;
const viewerWin = () => `<div class="b-view" data-k="viewer" aria-hidden="true">${viewerBody(VIEW_BASE, 0)}</div>`;

// ---- the editor: a preview, a timeline whose cut is two properties on its lane, and the export pane ----
function wave(seed, n = 72) { const r = rng(seed); return Array.from({ length: n }, (_, i) => `<i class="h${1 + Math.floor(Math.abs(Math.sin(i * 0.31 + r() * 2)) * 6 + r() * 2)}"></i>`).join(''); }
function editorBody() {
  return `<div class="b-edl">
      ${pane('focus b-edprev', 'preview', '', `<div class="b-stage"><div class="b-vid">${G.shot(FRESH.game)}</div></div>`)}
      ${pane('b-edtl', 'timeline', '', `<div class="b-edctl">${btn('play', 15)}<small data-k="keep">Keeping 0:00 &rarr; 0:14 (14.0 s)</small></div>
        <div class="b-lane" data-k="lane"><div class="b-wv">${wave(5)}</div><i class="b-cut l"></i><i class="b-cut r"></i><i class="b-keep"><u class="b-hd"></u><u class="b-hd r"></u></i><i class="b-ph"></i></div>`)}
    </div>
    ${pane('b-edexp', 'export', '', `<div class="b-edname"><b>2026-10-25 19-08-02</b><small>1080p &middot; 60 fps</small></div>
      <div class="b-note">The cut is exact.<b data-k="est">&asymp; 10 MB &middot; 14.0 s</b></div>
      <div class="b-edgo"><span class="b-pbtn" data-k="savebtn">${ic('save', 14)}Save a copy</span><div class="b-done" data-k="done"><span class="b-tag">saved</span><b>2026-10-25 19-08-02-edit.mp4</b></div></div>`)}`;
}
const edBar = (title) => `<div class="b-bar">${mark(22)}<span class="b-mod">${title}</span><span class="b-grow"></span>${['minus', 'square', 'close'].map((n) => btn(n, 15)).join('')}</div>`;

// `held` is the seconds in the buffer; `fresh` draws the library as it is before a recording (no clip highlighted).
const runPane = (fresh) => pane('', 'run', '', `${act(ic('save'), 'Save clip', '', 'Alt+1', fresh ? '' : 'sel')}${recAct(fresh)}${act(ic('camera'), 'Screenshot', '', 'Alt+2')}`);
function panel(held, fresh) {
  return `
    <div class="b-bar">${mark(22)}<i class="b-dot"></i><span class="b-recbadge" data-k="rectag">rec 0:00</span><span class="b-grow"></span>${['minus', 'square', 'close'].map((n) => btn(n, 15)).join('')}</div>
    <div class="b-body">
      <div class="b-deck">
        ${pane((fresh ? '' : 'focus ') + 'b-buf', 'buffer', '1080p &middot; 60 fps', `<div class="b-count"><b data-k="held">${mmss(held)}</b><span>/ 1:00</span></div>${meter(40, Math.round(held / 60 * 40), true, 'tape')}<div class="b-leg"><span>-1m</span><span>now</span></div>`)}
        ${runPane(fresh)}
      </div>
      <div class="b-rail">${pane((fresh ? 'focus ' : '') + 'b-lib', 'clips', '', `<div class="b-cards" data-k="cards">${CLIPS.map((c, i) => card(c, !fresh && i === 0)).join('')}</div>`)}</div>
    </div>
    <div class="b-over" data-k="over" aria-hidden="true"><div class="b-edwin">${edBar('Editor')}<div class="b-edbody">${editorBody()}</div></div></div>${viewerWin()}${toast}`;
}

function compact(held, fresh) {
  return `
    <div class="b-hdr">${mark(20)}<b>buffrx</b><i class="b-dot"></i><small>${G.name(FRESH.game)}</small><span class="b-grow"></span>${btn('close', 14)}</div>
    <div class="b-cbody">
      ${pane(fresh ? '' : 'focus', 'buffer', '', `<div class="b-cnt"><b data-k="held">${mmss(held)}</b><span>/ 1:00</span></div>${meter(40, Math.round(held / 60 * 40), false, 'tape')}`)}
      ${runPane(fresh)}
      ${pane((fresh ? 'focus ' : '') + 'b-recent', 'recent', '', `<div class="b-rows" data-k="rows">${CLIPS.slice(0, 3).map((c, i) => rec(c, !fresh && i === 0)).join('')}</div>`)}
    </div>${viewerWin()}${toast}`;
}

function editor() {
  return `${edBar('Editor')}<div class="b-edbody">${editorBody()}</div>`;
}

// ---- mounting ----
const SIZE = { panel: [840, 430], compact: [340, 500], editor: [840, 430] };
const VIEW = { panel, compact, editor };
function render(win, mode, { held = 42, fresh = false } = {}) {
  win.className = `bxp ${mode}`; win.innerHTML = VIEW[mode](held, fresh);
  if (mode === 'editor') cut(win, 14.3, 85.7, 40, '0:02', '0:12', '10.0', '7');
}

// The cut, as the editor draws it: where the kept part begins and ends on the lane, and the playhead.
function cut(win, from, to, at, a, b, len, mb) {
  const lane = win.querySelector('[data-k="lane"]'); if (!lane) return;
  lane.style.setProperty('--in', `${from}%`); lane.style.setProperty('--out', `${to}%`); lane.style.setProperty('--t', `${at}%`);
  const set = (k, text) => { const e = win.querySelector(`[data-k="${k}"]`); if (e) e.innerHTML = text; };
  set('keep', `Keeping ${a} &rarr; ${b} (${len} s)`); set('est', `&asymp; ${mb} MB &middot; ${len} s`);
}

// Draw the window at its own size and scale it to the room it has. Returns the scale, or 0 while the section is not showing.
function fit(stage, box, win, mode, { pad = 0, max = 1 } = {}) {
  const [w, h] = SIZE[mode], room = stage.clientWidth - pad;
  if (room <= 0) return 0;
  const s = Math.min(max, room / w);
  box.style.width = `${Math.round(w * s)}px`; box.style.height = `${Math.round(h * s)}px`;
  win.style.width = `${w}px`; win.style.height = `${h}px`; win.style.transform = `scale(${s})`;
  return s;
}

// Move what is already in a list to its new place when something is put in front of it.
function flip(host, change) {
  const before = new Map([...host.children].map((e) => [e, [e.offsetLeft, e.offsetTop]]));
  change();
  [...host.children].forEach((e) => {
    const was = before.get(e);
    if (!was) { e.animate([{ opacity: 0, transform: 'translateY(-16px) scale(0.96)' }, { opacity: 1, transform: 'none' }], { duration: 640, easing: 'cubic-bezier(0.16, 1, 0.3, 1)' }); return; }
    const dx = was[0] - e.offsetLeft, dy = was[1] - e.offsetTop;
    if (dx || dy) e.animate([{ transform: `translate(${dx}px, ${dy}px)` }, { transform: 'none' }], { duration: 640, easing: 'cubic-bezier(0.16, 1, 0.3, 1)' });
  });
}

// What a window does to itself: light the buffer, raise a notice, run the recording clock, land a clip first in the library.
function kit(win) {
  const q = (key) => win.querySelector(`[data-k="${key}"]`);
  const small = win.classList.contains('compact');
  const held = q('held'), tape = q('tape'), notice = q('toast'), list = q(small ? 'rows' : 'cards');
  const row = q('recrow'), badge = q('rectag'), max = list.children.length;
  let hideTimer = 0;
  const cells = () => [...tape.children];
  const light = (secs) => {
    held.textContent = mmss(secs);
    const all = cells(), n = Math.round(secs / 60 * all.length);
    all.forEach((e, i) => e.classList.toggle('on', small ? i < n : i >= all.length - n));
  };
  const note = (tag, title, detail, ms = 0) => { q('ttag').textContent = tag; q('ttitle').textContent = title; q('tdetail').innerHTML = detail; notice.classList.add('on'); clearTimeout(hideTimer); if (ms) hideTimer = setTimeout(() => notice.classList.remove('on'), ms); };
  const hide = () => { clearTimeout(hideTimer); notice.classList.remove('on'); };
  const clock = (s) => { q('recdetail').textContent = small ? `\u00b7 ${mmss(s)}` : mmss(s); if (badge) badge.textContent = `rec ${mmss(s)}`; };
  const recording = (yes) => { win.classList.toggle('recording', yes); row.classList.toggle('rec', yes); q('rectitle').textContent = yes ? 'Stop recording' : 'Record'; if (badge) badge.classList.toggle('on', yes); if (!yes) q('recdetail').textContent = ''; };
  const flash = () => { cells().forEach((e, i) => { if (e.classList.contains('on')) setTimeout(() => e.classList.add('hot'), i * 5); }); setTimeout(() => cells().forEach((e) => e.classList.remove('hot')), 900); };
  // A clip lands first and the oldest one leaves, so the library keeps its size.
  const land = (clip, animate, withMenu = false) => {
    const put = () => {
      [...list.children].forEach((c) => c.classList.remove('new'));
      list.insertAdjacentHTML('afterbegin', small ? rec(clip, true) : card(clip, true));
      if (withMenu && !small) list.firstElementChild.insertAdjacentHTML('beforeend', menu);
    };
    if (animate) flip(list, put); else put();
    if (list.children.length > max) {
      const gone = [...list.children].reverse().find((c) => !c.dataset.gone);
      if (!gone) return;
      gone.dataset.gone = '1';
      if (animate) { gone.animate([{ opacity: 1 }, { opacity: 0 }], { duration: 280, fill: 'forwards' }); setTimeout(() => gone.remove(), 320); } else gone.remove();
    }
  };
  return { q, small, list, notice, light, note, hide, clock, recording, flash, land, cells };
}

// One clip's whole life, played on a mounted window: it is recorded (a key, a counting tag), cut in the editor (the ends are
// pulled in, a copy is saved) and sent (the card's menu, a link copied). `press(down)` lets the page move its own key caps,
// `phase(n)` lets it say which step it is on (1 record, 2 edit, 3 share; 0 none). One cycle is about thirteen seconds.
function demo(win, { press = () => {}, phase = () => {} } = {}) {
  const k = kit(win), q = k.q, over = q('over'), small = k.small;
  let timers = [], on = false;
  const at = (ms, fn) => timers.push(setTimeout(fn, ms));
  const reset = () => {
    k.hide(); k.recording(false); k.cells().forEach((e) => e.classList.remove('hot')); k.light(38);
    k.list.innerHTML = (small ? CLIPS.slice(0, 3).map((c) => rec(c, false)) : CLIPS.map((c) => card(c, false))).join('');
    const v = q('viewer'); if (v) v.classList.remove('on');
    if (over) { over.classList.remove('on'); cut(win, 0, 100, 0, '0:00', '0:14', '14.0', '10'); const d = q('done'); if (d) d.classList.remove('on'); const s = q('savebtn'); if (s) s.classList.remove('down'); }
    phase(0);
  };
  const cycle = () => {
    reset();
    at(500, () => phase(1));
    at(700, () => press(true)); at(860, () => { press(false); k.recording(true); k.clock(0); });
    for (let s = 1; s <= 14; s++) at(1000 + s * 150, () => k.clock(s));
    at(3500, () => press(true));
    at(3660, () => { press(false); k.recording(false); k.note('done', 'Recording saved', `0:14 &middot; ${G.name(FRESH.game)}`); k.land(FRESH, true, true); });
    at(5300, () => k.hide());
    at(5500, () => { phase(2); if (over) over.classList.add('on'); else k.note('done', 'Trimmed', 'Keeping 0:02 &rarr; 0:12'); });
    if (over) {
      at(6100, () => cut(win, 14.3, 100, 20, '0:02', '0:14', '12.0', '8'));
      at(6700, () => cut(win, 14.3, 85.7, 60, '0:02', '0:12', '10.0', '7'));
      at(7500, () => q('savebtn').classList.add('down')); at(7700, () => { q('savebtn').classList.remove('down'); q('done').classList.add('on'); });
      at(8800, () => over.classList.remove('on'));
    }
    at(over ? 9100 : 7300, () => { k.hide(); phase(3); const c = k.list.firstElementChild; if (c) c.classList.add('menu'); });
    at(over ? 9900 : 8100, () => { const c = k.list.firstElementChild; if (c) c.classList.add('pick'); });
    at(over ? 10500 : 8700, () => { const c = k.list.firstElementChild; if (c) c.classList.remove('menu', 'pick'); k.note('done', 'Insta Share link copied', 'Expires in 24 h &middot; no IP address inside'); });
    at(over ? 12600 : 10800, () => k.hide());
    at(over ? 13400 : 11600, () => { if (on) cycle(); });
  };
  return {
    play() { if (on) return; on = true; cycle(); },
    // Stops where it is, never mid-flash: no key down, no lit tape, no notice left hanging.
    pause() { on = false; timers.forEach(clearTimeout); timers = []; press(false); k.hide(); k.cells().forEach((e) => e.classList.remove('hot')); },
    // The finished state with nothing moving: for reduced motion, and for a visitor who pauses before the first recording.
    settle() { this.pause(); reset(); k.light(41); k.land(FRESH, false); },
  };
}

// What the visitor does by hand: the four keys of the panel, each one raising its notice inside the window. Alt+4 opens the
// clip viewer the way it opens over a game (a small window that never takes focus) and shows the newest clip at once.
function actions(win) {
  const k = kit(win), viewer = k.q('viewer');
  let n = 0, began = 0, tick = 0, list = VIEW_BASE.slice(), pos = 0;
  const clip = (secs) => { n++; const m = String(9 + n).padStart(2, '0'); return { d: mmss(secs), secs, game: G.ORDER[(n - 1) % G.ORDER.length], v: Math.floor((n - 1) / G.ORDER.length) + 1, size: `${Math.max(1, Math.round(secs * 0.68))} MB`, at: `7:${m} PM` }; };
  const stopClock = () => { clearInterval(tick); tick = 0; began = 0; };
  const draw = () => { viewer.innerHTML = viewerBody(list, pos); };
  // The newest clip is the first one the viewer shows, so Alt+1 and then Alt+4 shows the clip just made.
  const made = (c) => { list = [[`${G.name(c.game)} ${c.d}`, c.game, c.v], ...list].slice(0, 5); pos = 0; if (viewer.classList.contains('on')) draw(); };
  viewer.addEventListener('click', (e) => { const b = e.target.closest('.b-vwc'); if (b) { pos = Number(b.dataset.i); draw(); } });
  return {
    save() { const c = clip(60); k.flash(); k.note('done', 'Clip saved', `1:00 &middot; ${G.name(c.game)}`, 3000); k.land(c, true); made(c); },
    shot() { k.note('done', 'Screenshot saved', G.name(G.ORDER[n % G.ORDER.length]), 3000); },
    record() {
      if (!began) {
        began = Date.now(); k.recording(true); k.clock(0); k.note('rec', 'Recording started', 'Alt+3 again stops it', 2600);
        tick = setInterval(() => k.clock(Math.floor((Date.now() - began) / 1000)), 250);
        return true;
      }
      const secs = Math.max(1, Math.floor((Date.now() - began) / 1000));
      stopClock(); k.recording(false); const c = clip(secs); k.note('done', 'Recording saved', `${mmss(secs)} &middot; ${G.name(c.game)}`, 3000); k.land(c, true); made(c);
      return false;
    },
    view() { const on = !viewer.classList.contains('on'); if (on) { pos = 0; draw(); } viewer.classList.toggle('on', on); return on; },
    step(d) { pos = Math.max(0, Math.min(list.length - 1, pos + d)); draw(); },
    recording: () => !!began,
    viewing: () => viewer.classList.contains('on'),
    stop() { stopClock(); k.recording(false); k.hide(); viewer.classList.remove('on'); },
  };
}

return { render, fit, demo, actions, cut, SIZE, mark };
})();
