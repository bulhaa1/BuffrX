// BuffrX Demo: a hands-on demo of the app, not the app. The hero window is a simplified panel the visitor can press (the four
// hotkeys, a recording, the editor, Quick Share, the Timeline), and each card below is one more part to try. A theme button themes the
// whole page. No requests are made, nothing is stored but the visitor's own choices, and reduced motion gets the resting state.
'use strict';

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const el = (tag, cls, html) => { const e = document.createElement(tag); if (cls) e.className = cls; if (html != null) e.innerHTML = html; return e; };
const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
const THEMES = ['clutch', 'overtime', 'respawn', 'afterglow', 'killcam', 'lobby', 'headshot', 'frostbite', 'toxic', 'nightshift', 'highscore', 'ghillie', 'sandstorm', 'arcade'];
const name = (id) => id[0].toUpperCase() + id.slice(1);
const cellRow = (host, n, cls = '') => { host.style.setProperty('grid-template-columns', `repeat(${n}, minmax(0, 1fr))`); host.replaceChildren(...Array.from({ length: n }, () => el('i', cls))); return [...host.children]; };
// A demo's result lines (the ones that begin in bold) come up as a notice over the stage, like the app's own, instead of as text under it.
let toastT = 0;
function toast(html, tag = 'done') {
  const box = $('#toast'); if (!box) return;
  box.innerHTML = `<span class="tag">${tag}</span><span class="msg">${html}</span>`; box.classList.add('on'); clearTimeout(toastT); toastT = setTimeout(() => box.classList.remove('on'), 2800);
}
const GHOST = (() => { const p = document.createElement('p'); new MutationObserver(() => { if (/^\s*<b>/.test(p.innerHTML)) toast(p.innerHTML); }).observe(p, { childList: true, characterData: true, subtree: true }); return p; })();
const k = (root, key) => $(`[data-k="${key}"]`, root) || (key === 'st' ? GHOST : null);
const mmss = (s) => `${Math.floor(s / 60)}:${String(Math.round(s) % 60).padStart(2, '0')}`;

// timers that belong to one demo. A part that is not on screen is paused: its loops stop, and start again when it comes back.
function ctx() {
  const ids = new Set(), loops = new Set();
  let paused = false;
  return {
    later(fn, ms) { const t = setTimeout(() => { ids.delete(t); fn(); }, ms); ids.add(t); },
    every(ms, fn) { const l = { ms, fn, id: paused ? 0 : setInterval(fn, ms) }; loops.add(l); },
    dispose() { ids.forEach((t) => clearTimeout(t)); ids.clear(); loops.forEach((l) => clearInterval(l.id)); loops.clear(); },
    pause() { paused = true; loops.forEach((l) => { clearInterval(l.id); l.id = 0; }); },
    resume() { paused = false; loops.forEach((l) => { if (!l.id) l.id = setInterval(l.fn, l.ms); }); },
  };
}

// ---- what the visitor chose: the site's theme, the preview's own, its window, and the background ----
const store = { site: 'clutch', cross: true, tried: [] };
try { Object.assign(store, JSON.parse(localStorage.getItem('buffrx-site')) || {}); } catch { /* no storage: the choices just do not persist */ }
if (!THEMES.includes(store.site)) store.site = 'clutch';
const keep = () => { try { localStorage.setItem('buffrx-site', JSON.stringify(store)); } catch { /* no storage */ } };

// ---- the bar: it firms up once the page moves, and folds away on the way down when the screen is narrow ----
const nav = $('#nav');
let spying = false, lastY = 0;
const stacked = matchMedia('(max-width: 1220px)');
function spy() {
  spying = false;
  nav.classList.toggle('stuck', scrollY > 8);
  const dy = scrollY - lastY; lastY = scrollY;
  if (stacked.matches) { if (scrollY < 80 || dy < -4) nav.classList.remove('min'); else if (dy > 4 && scrollY > 160 && pop.hidden) nav.classList.add('min'); } else nav.classList.remove('min');
}
addEventListener('scroll', () => { if (!spying) { spying = true; requestAnimationFrame(spy); } }, { passive: true });

// ---- reveals: each element enters once, the ones that arrive together a beat apart ----
const isPhone = matchMedia('(max-width: 640px), (pointer: coarse) and (max-height: 500px)'); // a phone, either way up; keep in step with the `@media` block marked "phone" in site.css
const reveal = new IntersectionObserver((entries) => {
  let i = 0;
  entries.filter((e) => e.isIntersecting).forEach((e) => { e.target.style.setProperty('--d', i++ * 90); e.target.classList.add('in'); reveal.unobserve(e.target); });
}, { threshold: 0.12, rootMargin: '0px 0px -6% 0px' });
$$('[data-r]').forEach((e) => reveal.observe(e)); // armed first: a failure further down must never leave the page hidden
if (isPhone.matches) $$('[data-r]').forEach((e) => e.classList.add('in')); // a phone gets everything at once: nothing waits to scroll into view

// ---- the hero: one clip, recorded, cut and shared, over and over, until the visitor pauses it or presses a key of their own ----
const hstage = $('.hero-stage'), hfit = $('#hfit'), hwin = $('#hwin'), pauseBtn = $('#pause'), hknote = document.createElement('p'); // the hint line is gone: this only absorbs what the story used to write there
let hmode = null, hdemo = null, hact = null, held = reduce, onScreen = false, manual = false;
const hks = $$('.hk'), hk = (act) => $(`.hk[data-act="${act}"]`), keys = $$('.hk [data-key]');
const press = (down) => keys.forEach((c) => c.classList.toggle('down', down));
const touch = matchMedia('(hover: none)').matches;
const HINT = [touch ? 'Tap a key to try it.' : 'Click a key, or press it on your keyboard.', 'Recording with Alt+3.', 'Cutting it in the editor.', 'Sharing it with Quick Share.'];
const phase = (n) => { hknote.textContent = HINT[n] || HINT[0]; hk('record').classList.toggle('on', n === 1); };
function mount(mode) {
  hmode = mode; BXWindow.render(hwin, mode, { held: 38, fresh: true }); hwin.dataset.theme = store.site;
  hdemo = BXWindow.demo(hwin, { press, phase }); hact = BXWindow.actions(hwin); BXWindow.fit(hstage, hfit, hwin, mode, { max: 1 });
  hk('record').setAttribute('aria-pressed', 'false'); hk('timeline').setAttribute('aria-pressed', 'false'); phase(0);
}
function hero() {
  const mode = innerWidth < 700 ? 'compact' : 'panel';
  if (mode === hmode) { BXWindow.fit(hstage, hfit, hwin, mode, { max: 1 }); return; }
  if (hdemo) hdemo.pause(); if (hact) hact.stop();
  mount(mode); manual = false;
  if (held) hdemo.settle(); else if (onScreen) hdemo.play();
}
const playing = () => { if (!hdemo) return; if (held || !onScreen || document.hidden) hdemo.pause(); else hdemo.play(); };
// After the visitor's own key the story waits three quiet seconds and then carries on, unless they paused it themselves
// or a recording or the Timeline of theirs is still open (then it checks again three seconds later).
let userPaused = false, resumeT = 0;
function armResume() {
  clearTimeout(resumeT);
  if (!manual || userPaused || reduce) return;
  resumeT = setTimeout(() => { if (hact.recording() || hact.viewing()) armResume(); else setHeld(false); }, 3000); // a recording or an open Timeline is the visitor still at work
}
function setHeld(v) {
  clearTimeout(resumeT);
  held = v; pauseBtn.textContent = v ? 'Play demo' : 'Pause demo';
  if (!v && manual) { hact.stop(); manual = false; mount(hmode); hknote.setAttribute('aria-live', 'off'); } // back to the story, in a clean window
  else if (v && hdemo && !manual) hdemo.settle();
  playing();
}
// Reset: the story starts again from a clean window.
function resetHero() { clearTimeout(resumeT); userPaused = false; if (hact) hact.stop(); manual = false; mount(hmode); hknote.setAttribute('aria-live', 'off'); setHeld(false); }
// A key of the visitor's own: the story stops, the window starts clean, and the key does what it does in the app.
const SAID = { save: 'Clip saved: it landed first in the library.', shot: 'Screenshot saved: a notice, and no new clip.', timeline: 'Timeline is open. Move over the strip to look back, drag to pick a part, then Save clip.' };
// The notice sits at the window's foot: if that is below the fold, scroll it up (smoothly, unless motion is off).
function bringIntoView() { hfit.scrollIntoView({ block: 'nearest', behavior: reduce ? 'auto' : 'smooth' }); }
function use(act) {
  if (!hact) return;
  if (!manual) { manual = true; hdemo.pause(); hact.stop(); mount(hmode); held = true; pauseBtn.textContent = 'Play demo'; hknote.setAttribute('aria-live', 'polite'); }
  const b = hk(act), on = hact[act]();
  bringIntoView();
  const caps = $$('kbd', b); caps.forEach((c) => c.classList.add('down')); setTimeout(() => caps.forEach((c) => c.classList.remove('down')), 180);
  if (act === 'record' || act === 'timeline') b.setAttribute('aria-pressed', String(!!on));
  hknote.textContent = act === 'record' ? (on ? 'Recording. Press Alt+3 again to stop it.' : 'Recording saved: it landed first in the library.') : act === 'timeline' && !on ? 'Back in the game.' : SAID[act];
  armResume();
}
hks.forEach((b) => b.addEventListener('click', () => use(b.dataset.act)));
hwin.addEventListener('bx:tlclosed', () => { hk('timeline').setAttribute('aria-pressed', 'false'); hknote.textContent = 'Back in the game.'; armResume(); });
const isTyping = (e) => /^(INPUT|SELECT|TEXTAREA)$/.test(e.target.tagName) || e.target.isContentEditable;
addEventListener('keydown', (e) => {
  // with the Timeline open, the arrows move along its strip, Enter saves the part, Esc goes back to the game
  if (onScreen && hact && hact.viewing() && !e.altKey && !e.ctrlKey && !e.metaKey && !isTyping(e)) {
    if (e.code === 'ArrowLeft' || e.code === 'ArrowRight') { e.preventDefault(); hact.step(e.code === 'ArrowRight' ? 1 : -1); armResume(); return; }
    if (e.code === 'Enter') { e.preventDefault(); hact.commit(); armResume(); return; }
    if (e.code === 'Space') { e.preventDefault(); hact.toggle(); armResume(); return; }
    if (e.code === 'Escape') { e.preventDefault(); use('timeline'); return; }
  }
  if (!e.altKey || e.ctrlKey || e.metaKey || e.shiftKey || !onScreen) return;
  const act = { Digit1: 'save', Digit2: 'shot', Digit3: 'record', Digit4: 'timeline' }[e.code];
  if (act) { e.preventDefault(); use(act); }
});
pauseBtn.addEventListener('click', () => { userPaused = !held; setHeld(!held); });
hwin.addEventListener('pointerdown', armResume); // a click on a clip in the viewer is the visitor too
document.addEventListener('visibilitychange', playing);
new IntersectionObserver((es) => { onScreen = es[0].isIntersecting; playing(); }, { threshold: 0.25 }).observe(hfit);
new ResizeObserver(hero).observe(hstage);
setHeld(reduce);
hero();

// ---- the site's theme: the windows drawn on the page follow it ----
const themeButton = (id, onPick) => { const b = el('button', 'th', `<span class="sw3" data-theme="${id}" aria-hidden="true"><i></i><i></i><i></i></span>${name(id)}`); b.type = 'button'; b.dataset.id = id; b.setAttribute('role', 'radio'); b.addEventListener('click', () => onPick(id)); return b; };
const arrows = (list, current, pick) => list.addEventListener('keydown', (e) => {
  const i = THEMES.indexOf(current()), n = THEMES.length;
  const to = e.key === 'ArrowRight' || e.key === 'ArrowDown' ? (i + 1) % n : e.key === 'ArrowLeft' || e.key === 'ArrowUp' ? (i + n - 1) % n : e.key === 'Home' ? 0 : e.key === 'End' ? n - 1 : -1;
  if (to < 0) return; e.preventDefault(); pick(THEMES[to], true);
});

// ---- the whole site's theme: one button, a list, and switches for the background and the cursor ----
const tbtn = $('#themebtn'), pop = $('#pop'), slist = $('#siteThemes');
// A chosen theme spreads out from the theme button as a circular reveal (a View Transition); without that, or with reduced motion, it just swaps.
function setSite(id, { focus = false, ripple = false } = {}) {
  const apply = () => {
    store.site = id; document.documentElement.dataset.theme = id; if (ripple) keep();
    $('#themename').textContent = name(id); $('#themesw').dataset.theme = id; tbtn.setAttribute('aria-label', `Theme: ${name(id)}`);
    $('meta[name="theme-color"]').setAttribute('content', getComputedStyle(document.documentElement).getPropertyValue('--bx-ground').trim());
    $$('button', slist).forEach((b) => { const on = b.dataset.id === id; b.setAttribute('aria-checked', String(on)); b.tabIndex = on ? 0 : -1; if (on && focus) b.focus(); });
    $$('.bxp').forEach((w) => { w.dataset.theme = id; });
  };
  if (!ripple || reduce || !document.startViewTransition || id === store.site) { apply(); return; }
  const r = tbtn.getBoundingClientRect(), x = r.left + r.width / 2, y = r.top + r.height / 2, end = Math.hypot(Math.max(x, innerWidth - x), Math.max(y, innerHeight - y));
  document.startViewTransition(apply).ready.then(() => document.documentElement.animate(
    { clipPath: [`circle(0px at ${x}px ${y}px)`, `circle(${end}px at ${x}px ${y}px)`] },
    { duration: 800, easing: 'cubic-bezier(.22, 1, .36, 1)', pseudoElement: '::view-transition-new(root)' })).catch(() => {});
}
function openPop() { pop.hidden = false; tbtn.setAttribute('aria-expanded', 'true'); $('[aria-checked="true"]', slist).focus(); }
function closePop(back) { if (pop.hidden) return; pop.hidden = true; tbtn.setAttribute('aria-expanded', 'false'); if (back) tbtn.focus(); }
THEMES.forEach((id) => slist.append(themeButton(id, (t) => setSite(t, { ripple: true }))));
arrows(slist, () => store.site, (t, f) => setSite(t, { focus: f, ripple: true }));
tbtn.addEventListener('click', () => (pop.hidden ? openPop() : closePop(false)));
document.addEventListener('pointerdown', (e) => { if (!pop.hidden && !pop.contains(e.target) && !tbtn.contains(e.target)) closePop(false); });
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closePop(true); });
// the crosshair cursor: for a mouse only, so a touch screen never sees the switch
const xhsw = $('#xhsw');
function setCross(on) { store.cross = on; xhsw.setAttribute('aria-checked', String(on)); BXCross.on(on); keep(); }
xhsw.hidden = !BXCross.supported();
xhsw.addEventListener('click', () => setCross(!store.cross));
setSite(store.site); setCross(store.cross);

// ---- features: six parts, each one a demonstration ----
const demos = Object.assign({}, PG); // the newer tiles live in demos.js

// Two sizes of one app: the full panel, and the quick panel from the tray. Both wear the site's theme.
demos.sizes = (s) => {
  s.innerHTML = `<div class="seg" role="group" aria-label="Window size"><button type="button" data-mode="compact" aria-pressed="false">Compact</button><button type="button" data-mode="panel" aria-pressed="true">Expanded</button></div>
    <div class="size-stage" data-k="stage"><div class="win-fit" data-k="fit"><div class="bxp" data-k="win" role="img"></div></div></div>
`;
  const stage = k(s, 'stage'), fit = k(s, 'fit'), win = k(s, 'win'), NOTE = { panel: 'Expanded: the buffer, one key per action, and your library.', compact: 'Compact: the quick panel from the tray. Small, movable, and it stays where you put it.' };
  let mode = 'panel';
  const place = () => {
    const cs = getComputedStyle(stage); BXWindow.fit(stage, fit, win, mode, { pad: parseFloat(cs.paddingLeft) + parseFloat(cs.paddingRight), max: 1 });
    stage.style.setProperty('height', `${parseFloat(fit.style.height) + parseFloat(cs.paddingTop) + parseFloat(cs.paddingBottom)}px`);
  };
  const draw = () => {
    BXWindow.render(win, mode, { held: 42 }); win.dataset.theme = store.site; win.setAttribute('aria-label', `The BuffrX ${mode === 'panel' ? 'expanded' : 'compact'} window`);
    $$('button', s).forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.mode === mode)));
    k(s, 'st').textContent = NOTE[mode]; place();
  };
  $$('.seg button', s).forEach((b) => b.addEventListener('click', () => {
    if (b.dataset.mode === mode) return; mode = b.dataset.mode;
    if (reduce) { draw(); return; }
    fit.classList.add('swap'); setTimeout(() => { draw(); fit.classList.remove('swap'); }, 190);
  }));
  new ResizeObserver(place).observe(stage);
  draw();
};

// Fit it for Discord: the same arithmetic as the app (src/engine/share_math.cpp). A clip that fits goes as it is; otherwise the
// largest picture whose floor rate the budget covers, with one 128 kbps audio track, 8% headroom, and the rate never above 150 Mbps.
demos.discord = (s) => {
  const W = 2560, H = 1440, SRC_BPS = 3.98e6; // a 1440p, 60 fps capture runs at about 32 Mbps
  s.innerHTML = `<div class="dc-pic"><canvas width="320" height="180" data-k="cv" role="img" aria-label="The clip as Discord would get it"></canvas><span class="dc-badge" data-k="badge"></span></div>
    <label class="dc-len"><span>Length</span><input type="range" min="10" max="180" step="5" value="60" data-k="len"><b data-k="lv">1:00</b></label>
    <div class="seg dc-seg" role="group" aria-label="Discord upload limit" data-k="lim"></div>
    <p class="dc-sum" aria-live="polite"><span data-k="from"></span><i aria-hidden="true">&rarr;</i><b data-k="to"></b></p>`;
  let limit = 20; const cv = k(s, 'cv'), g = cv.getContext('2d'), img = new Image(); img.src = BXGames.src('acm');
  const fit = (secs, mib) => {
    const cap = mib * 1024 * 1024, bytes = secs * SRC_BPS;
    if (bytes <= cap) return { copy: true, bytes };
    const video = (cap * 8 / 1000 * 0.92) / secs - 128, short = Math.min(W, H);
    let pick = null, last = null;
    for (const step of [short, ...[1440, 1080, 720, 540, 480, 360].filter((x) => x < short)]) {
      const sc = step / short, w = Math.max(2, Math.round(W * sc) & ~1), h = Math.max(2, Math.round(H * sc) & ~1); last = { w, h };
      if (video >= w * h * 0.0018) { pick = { w, h }; break; }
    }
    if (!pick) { if (video < 250) return { none: true }; pick = last; }
    const kbps = Math.min(video, 150000);
    return { ...pick, kbps, bytes: ((kbps + 128) * 1000 / 8) * secs };
  };
  // the picture is drawn at the size that would be sent and scaled up, so a smaller picture looks softer
  const paint = (res) => {
    const tiny = document.createElement('canvas'), f = Math.max(0.12, Math.min(1, res / 1440));
    tiny.width = Math.round(320 * f); tiny.height = Math.round(180 * f);
    if (img.complete) tiny.getContext('2d').drawImage(img, 0, 0, tiny.width, tiny.height);
    g.imageSmoothingEnabled = true; g.clearRect(0, 0, 320, 180); g.drawImage(tiny, 0, 0, 320, 180);
  };
  const draw = () => {
    const secs = Number(k(s, 'len').value), r = fit(secs, limit), orig = Math.round(secs * SRC_BPS / 1048576);
    k(s, 'lv').textContent = mmss(secs);
    $$('button', k(s, 'lim')).forEach((b) => b.setAttribute('aria-pressed', String(Number(b.dataset.m) === limit)));
    k(s, 'from').textContent = `${orig} MiB clip`;
    if (r.none) { paint(360); k(s, 'badge').textContent = 'Too long'; k(s, 'to').textContent = 'Will not fit'; return; }
    paint(r.copy ? 1440 : Math.min(r.w, r.h));
    k(s, 'badge').textContent = r.copy ? 'As it is' : `${Math.min(r.w, r.h)}p`;
    k(s, 'to').textContent = r.copy ? `fits in ${limit} MiB` : `${Math.round(r.bytes / 1048576)} MiB of ${limit}`;
  };
  [20, 50, 100, 500].forEach((m) => { const b = el('button', '', `${m} MiB`); b.type = 'button'; b.dataset.m = m; b.addEventListener('click', () => { limit = m; draw(); }); k(s, 'lim').append(b); });
  k(s, 'len').addEventListener('input', draw); img.addEventListener('load', draw);
  draw();
};

// ---- the demo: parts listed on the left, one at a time on the stage ----
const TOUR = [
  ['Start', [['app', 'The app']]],
  ['Capture', [['buffer', 'Buffer and quick clip'], ['timeline', 'Timeline', 'beta'], ['auto', 'Game capture']]],
  ['Clips', [['library', 'Library'], ['edit', 'Editor']]],
  ['Share', [['link', 'Quick Share'], ['discord', 'Discord']]],
  ['Setup', [['hotkeys', 'Hotkeys'], ['performance', 'Performance mode'], ['sizes', 'Two sizes']]],
  ['Next', [['get', 'Get BuffrX']]],
];
const TRY = (id) => id !== 'get'; // the last stop is a door to the real thing, not a demo
const ORDER = TOUR.flatMap(([, items]) => items.map((i) => i[0]));
const META = Object.fromEntries(TOUR.flatMap(([, items]) => items.map(([id, name, tag]) => [id, { name, tag }])));
const DEMOS = ORDER.filter(TRY);
const sidenav = $('#sidenav'), panels = $('#panels'), stage = $('#stage'), tried = new Set((store.tried || []).filter((x) => DEMOS.includes(x)));
TOUR.forEach(([group, items]) => {
  sidenav.append(el('p', 'pg-g', group));
  items.forEach(([id, name, tag]) => { const a = el('a', 'pg-i', `<i class="tick" aria-hidden="true"></i><span>${name}</span>${tag ? `<em>${tag}</em>` : ''}`); a.href = `#${id}`; a.dataset.id = id; sidenav.append(a); });
});
$('#cells').append(...DEMOS.map(() => el('i')));
const GUIDE = {
  app: ['This is the BuffrX panel. It keeps the last moments of your game, and each key turns them into something you keep.', ['Press <b>Alt+1</b> to save a clip', 'Press <b>Alt+3</b> to record, and again to stop', 'Press <b>Alt+4</b> to open Timeline']],
  buffer: ['The buffer keeps the last stretch of your game, so a great moment can be saved after it happens.', ['Pick how long it keeps', 'Press <b>Start</b> and let it fill', '<b>Save clip</b> keeps it all, <b>Quick clip</b> keeps the newest 20 seconds']],
  timeline: ['Timeline keeps your whole session, so you can go back and find a moment you missed.', ['Click the bar to look back, hover it to preview', 'Press <b>Clip</b> and drag its ends to pick a part', 'Press <b>Save clip</b>, then <b>View clips</b> to see it']],
  auto: ['BuffrX finds your game by itself, and goes back to your display when no game is running.', ['Pick <b>Games + display</b>, then press <b>Start the game</b>', 'Watch auto switch from your display to the game', 'Press <b>Alt-tab away</b>, then <b>Close the game</b>: it goes back to your display']],
  library: ['Every clip, recording and screenshot in one place, filed by game.', ['Pick a game to filter the list', 'Click a clip to pick it', 'Edit, share, copy, rename or delete it']],
  edit: ['Cut a clip without leaving BuffrX.', ['Press play, or click the strip to move the playhead', 'Press <b>Split</b>, then drag the ends of a part to trim it', 'Pick a crop and a speed, then <b>Save a copy</b>']],
  link: ['Quick Share sends a clip straight to a friend. Nothing is uploaded first.', ['Pick how long the link lasts', 'Press <b>Copy link</b>', 'Play your friend: press <b>Open in BuffrX</b>']],
  discord: ['Too big for the chat? BuffrX shrinks the clip to fit your Discord limit.', ['Drag the length of the clip', 'Pick your Discord limit', 'See the size it will be sent at']],
  hotkeys: ['Every action has a key, and you can change it. Controllers work too.', ['Press <b>Try it</b> to see what a key does', 'Click a key, then press the new one to change it']],
  performance: ['Performance mode makes the capture lighter for demanding games. Auto turns it on only while a game from your list is running.', ['Pick <b>Off</b>, <b>Auto</b> or <b>On</b>', 'In Auto, tick the games, then press <b>Start game</b>', 'Watch the settings change, and change back when the game closes']],
  sizes: ['The panel comes in two sizes: the full window, and a small quick panel.', ['Pick <b>Compact</b> or <b>Expanded</b>', 'Look at what each one shows']],
};
const guideEl = (id) => { if (!GUIDE[id]) return null; const g = el('div', 'pg-guide', `<p>${GUIDE[id][0]}</p><ol>${GUIDE[id][1].map((t) => `<li><span>${t}</span></li>`).join('')}</ol>`); $('ol', g).style.setProperty('--n', GUIDE[id][1].length); return g; };
const made = { app: $('#p-app') };
{ const g = guideEl('app'); if (g) $('.pg-ph', made.app).after(g); }
const GET = `<div class="pg-get"><img class="pg-wordmark" src="buffrx-logo.svg" alt="BuffrX" width="200" height="68"><h3>That was the demo.</h3><p>The real BuffrX is a free Windows app: it keeps the last moments of your game, lets you cut them and sends them to your friends.</p><div class="pg-getbtns"><a class="btn primary big" href="https://buffrx.app" target="_blank" rel="noopener noreferrer">Go to buffrx.app</a><a class="btn big" href="https://github.com/bulhaa1/BuffrX" target="_blank" rel="noopener noreferrer">See it on GitHub</a></div></div>`;
const buildPanel = (id) => {
  const p = el('section', 'pg-panel', `<header class="pg-ph"><h2>${META[id].name}</h2>${META[id].tag ? `<span class="pg-tag">${META[id].tag}</span>` : ''}<span class="grow"></span><button class="pg-reset" type="button">Reset</button></header><div class="demo pg-demo">${id === 'get' ? GET : ''}</div>`);
  { const g = guideEl(id); if (g) $('.pg-ph', p).after(g); }
  p.dataset.id = id; p.setAttribute('aria-label', META[id].name); p.hidden = true; if (id === 'get') $('.pg-reset', p).remove(); panels.append(p); return p;
};
// a demo is built the first time it is opened (so a video or a strip of frames is fetched only when someone looks at it)
const init = (id) => {
  const p = made[id]; if (id === 'app' || id === 'get' || p.dataset.ready) return; p.dataset.ready = '1';
  p._ctx = ctx(); try { demos[id]($('.demo', p), p._ctx); } catch (err) { console.error(err); }
};
const resetDemo = (id) => {
  if (id === 'app') { resetHero(); return; }
  const p = made[id]; if (p._ctx) p._ctx.dispose();
  const fresh = el('div', 'demo pg-demo'); $('.demo', p).replaceWith(fresh); delete p.dataset.ready; init(id);
};
let cur = 'app';
const paintProgress = () => {
  $$('#cells i').forEach((c, i) => c.classList.toggle('on', tried.has(DEMOS[i])));
  $('#progtxt').textContent = `${tried.size} of ${DEMOS.length} tried`;
  $$('.pg-i', sidenav).forEach((a) => a.classList.toggle('done', tried.has(a.dataset.id)));
};
const markTried = () => { if (!TRY(cur) || tried.has(cur)) return; tried.add(cur); store.tried = [...tried]; keep(); paintProgress(); };
function show(id) {
  if (!ORDER.includes(id)) id = 'app';
  cur = id; if (!made[id]) made[id] = buildPanel(id); init(id);
  $$('.pg-panel', panels).forEach((p) => { p.hidden = p.dataset.id !== id; });
  Object.entries(made).forEach(([key, p]) => { if (p._ctx) { if (key === id) p._ctx.resume(); else p._ctx.pause(); } }); // only the part in view keeps its loops running
  $$('.pg-i', sidenav).forEach((a) => { if (a.dataset.id === id) { a.setAttribute('aria-current', 'page'); a.scrollIntoView({ block: 'nearest' }); } else a.removeAttribute('aria-current'); });
  stage.scrollTop = 0;
  document.title = `${META[id].name} - BuffrX Demo`;
  const i = ORDER.indexOf(id);
  $('#prev').disabled = i === 0; $('#next').disabled = i === ORDER.length - 1; $('#next').textContent = i === ORDER.length - 2 ? 'Finish' : 'Next';
  paintProgress();
}
const go = (d) => { const j = ORDER.indexOf(cur) + d; if (ORDER[j]) location.hash = ORDER[j]; };
$('#prev').addEventListener('click', () => go(-1)); $('#next').addEventListener('click', () => go(1));
addEventListener('hashchange', () => show(location.hash.slice(1)));
panels.addEventListener('click', (e) => { if (e.target.closest('.pg-reset')) { resetDemo(cur); return; } });
// pressing anything in a demo counts it as tried
panels.addEventListener('pointerdown', (e) => { if (e.target.closest('button, input, select, a, [role], .tl-bar, .hx-b, .hk, .win-fit, .lclip')) markTried(); });
panels.addEventListener('click', (e) => { if (e.target.closest('button:not(.pg-reset), .lclip, .hx-b, [role="button"]')) markTried(); });
panels.addEventListener('keydown', () => markTried());
addEventListener('keydown', (e) => { if (e.altKey && /^Digit[1-4]$/.test(e.code) && cur === 'app') markTried(); });
// the list moves with the arrow keys
sidenav.addEventListener('keydown', (e) => {
  const links = $$('.pg-i', sidenav), i = links.indexOf(document.activeElement);
  if (i < 0 || !['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(e.key)) return;
  e.preventDefault(); links[e.key === 'Home' ? 0 : e.key === 'End' ? links.length - 1 : Math.max(0, Math.min(links.length - 1, i + (e.key === 'ArrowDown' ? 1 : -1)))].focus();
});
show(location.hash.slice(1) || 'app');

// ---- the start screen: what this is, and where the real thing is ----
(function start() {
  const box = $('#start'); if (!box) return;
  const shell = [$('.pg-shell'), $('.pg-foot'), $('#nav')];
  const close = () => { box.classList.remove('on'); setTimeout(() => { box.hidden = true; }, reduce ? 0 : 320); shell.forEach((x) => x && x.removeAttribute('inert')); document.documentElement.classList.remove('pg-start-open'); stage.focus({ preventScroll: true }); };
  box.hidden = false; document.documentElement.classList.add('pg-start-open'); shell.forEach((x) => x && x.setAttribute('inert', ''));
  requestAnimationFrame(() => { box.classList.add('on'); $('#startgo').focus({ preventScroll: true }); });
  $('#startgo').addEventListener('click', close);
  addEventListener('keydown', (e) => { if (!box.hidden && (e.key === 'Escape' || e.key === 'Enter')) { e.preventDefault(); close(); } });
})();
