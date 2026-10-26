// BuffrX landing page: one scrolling page whose tabs scroll to their sections. A theme button themes the whole page (the preview windows
// follow it and can take a theme of their own), the hero plays a clip being recorded, cut and shared on the app's own window, Features
// holds six small demonstrations (each a control and a line of result), Privacy says why it is private and needs no cloud, and Compare sets
// BuffrX against one other app at a time. No requests are made, nothing is stored but the visitor's own choices, and reduced motion
// gets the resting state.
'use strict';

const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const el = (tag, cls, html) => { const e = document.createElement(tag); if (cls) e.className = cls; if (html != null) e.innerHTML = html; return e; };
const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
const THEMES = ['clutch', 'overtime', 'respawn', 'afterglow', 'killcam', 'lobby', 'headshot', 'frostbite', 'toxic', 'nightshift', 'highscore', 'ghillie', 'sandstorm', 'arcade'];
const name = (id) => id[0].toUpperCase() + id.slice(1);
const cellRow = (host, n, cls = '') => { host.style.setProperty('grid-template-columns', `repeat(${n}, minmax(0, 1fr))`); host.replaceChildren(...Array.from({ length: n }, () => el('i', cls))); return [...host.children]; };
const k = (root, key) => $(`[data-k="${key}"]`, root);
const mmss = (s) => `${Math.floor(s / 60)}:${String(Math.round(s) % 60).padStart(2, '0')}`;

// timers that belong to one demo
function ctx() {
  const ids = new Set();
  return {
    later(fn, ms) { const t = setTimeout(() => { ids.delete(t); fn(); }, ms); ids.add(t); },
    every(ms, fn) { ids.add(setInterval(fn, ms)); },
    dispose() { ids.forEach((t) => { clearTimeout(t); clearInterval(t); }); ids.clear(); },
  };
}

// ---- what the visitor chose: the site's theme, the preview's own, its window, and the background ----
const store = { site: 'clutch', bg: true, cross: true };
try { Object.assign(store, JSON.parse(localStorage.getItem('buffrx-site')) || {}); } catch { /* no storage: the choices just do not persist */ }
if (!THEMES.includes(store.site)) store.site = 'clutch';
if (reduce) store.bg = false;
const keep = () => { try { localStorage.setItem('buffrx-site', JSON.stringify(store)); } catch { /* no storage */ } };

// ---- the bar: it firms up once the page moves, and its ink follows the section in view ----
const nav = $('#nav'), tabs = $('#tabs'), ink = $('#ink'), links = $$('a', tabs);
const SPY = [['why', '#why'], ['why', '#about'], ['why', '#reasons'], ['features', '#features'], ['privacy', '#privacy'], ['compare', '#compare'], ['roadmap', '#roadmap'], ['faq', '#faq'], ['pricing', '#pricing'], ['get', '#get']];
let lit = null;
function light(id) {
  if (id === lit) return; lit = id;
  links.forEach((a) => { const on = a.getAttribute('href') === '#' + id; if (on) a.setAttribute('aria-current', 'true'); else a.removeAttribute('aria-current'); if (on) { ink.style.setProperty('--ink-x', a.offsetLeft + 'px'); ink.style.setProperty('--ink-w', a.offsetWidth + 'px'); if (tabs.scrollWidth > tabs.clientWidth) tabs.scrollTo({ left: a.offsetLeft - 24, behavior: reduce ? 'auto' : 'smooth' }); } });
}
let spying = false, aimed = false, aim = 0, lastY = 0;
const stacked = matchMedia('(max-width: 1220px)');
function spy() {
  spying = false;
  nav.classList.toggle('stuck', scrollY > 8);
  const dy = scrollY - lastY; lastY = scrollY;
  if (stacked.matches) { if (scrollY < 80 || dy < -4) nav.classList.remove('min'); else if (dy > 4 && scrollY > 160 && pop.hidden) nav.classList.add('min'); } else nav.classList.remove('min');
  if (aimed) return; // a tab was clicked: the ink is already at the target, so the scroll passing other sections must not drag it back
  const line = innerHeight * 0.4;
  let id = 'why';
  SPY.forEach(([section, sel]) => { if ($(sel).getBoundingClientRect().top <= line) id = section; });
  if (innerHeight + scrollY >= document.documentElement.scrollHeight - 4) id = 'get';
  light(id);
}
const settle = () => { aimed = false; spy(); };
// While a clicked tab's scroll is under way the ink stays on its target; it is released once the page has stopped moving for a moment.
addEventListener('scroll', () => { if (aimed) { clearTimeout(aim); aim = setTimeout(settle, 180); } if (!spying) { spying = true; requestAnimationFrame(spy); } }, { passive: true });
addEventListener('resize', () => { const on = lit; lit = null; light(on || 'why'); });
const showAt = (sel) => { const t = $(sel); if (t) $$('[data-r]', t).concat(t.matches('[data-r]') ? [t] : []).forEach((e) => e.classList.add('in')); };
if (location.hash.length > 1) showAt(location.hash);
addEventListener('hashchange', () => showAt(location.hash));
links.forEach((a) => a.addEventListener('click', () => { showAt(a.getAttribute('href')); light(a.getAttribute('href').slice(1)); aimed = true; clearTimeout(aim); aim = setTimeout(settle, 1200); }));
document.fonts.ready.then(() => { const on = lit; lit = null; light(on || 'why'); });

// ---- reveals: each element enters once, the ones that arrive together a beat apart ----
const isPhone = matchMedia('(max-width: 640px), (pointer: coarse) and (max-height: 500px)'); // a phone, either way up; keep in step with the `@media` block marked "phone" in site.css
const reveal = new IntersectionObserver((entries) => {
  let i = 0;
  entries.filter((e) => e.isIntersecting).forEach((e) => { e.target.style.setProperty('--d', i++ * 90); e.target.classList.add('in'); reveal.unobserve(e.target); });
}, { threshold: 0.12, rootMargin: '0px 0px -6% 0px' });
$$('[data-r]').forEach((e) => reveal.observe(e)); // armed first: a failure further down must never leave the page hidden
if (isPhone.matches) $$('[data-r]').forEach((e) => e.classList.add('in')); // a phone gets everything at once: nothing waits to scroll into view

// ---- the hero: one clip, recorded, cut and shared, over and over, until the visitor pauses it or presses a key of their own ----
const hstage = $('.hero-stage'), hfit = $('#hfit'), hwin = $('#hwin'), pauseBtn = $('#pause'), hknote = $('#hknote');
let hmode = null, hdemo = null, hact = null, held = reduce, onScreen = false, manual = false;
const hks = $$('.hk'), hk = (act) => $(`.hk[data-act="${act}"]`), keys = $$('.hk [data-key]');
const press = (down) => keys.forEach((c) => c.classList.toggle('down', down));
const touch = matchMedia('(hover: none)').matches;
const HINT = [touch ? 'Tap a key to try it.' : 'Click a key, or press it on your keyboard.', 'Recording with Alt+3.', 'Cutting it in the editor.', 'Sharing it with Insta Share.'];
const phase = (n) => { hknote.textContent = HINT[n] || HINT[0]; hk('record').classList.toggle('on', n === 1); };
function mount(mode) {
  hmode = mode; BXWindow.render(hwin, mode, { held: 38, fresh: true }); hwin.dataset.theme = store.site;
  hdemo = BXWindow.demo(hwin, { press, phase }); hact = BXWindow.actions(hwin); BXWindow.fit(hstage, hfit, hwin, mode, { max: 1 });
  hk('record').setAttribute('aria-pressed', 'false'); hk('view').setAttribute('aria-pressed', 'false'); phase(0);
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
// or a recording of theirs is still running (then it checks again three seconds later).
let userPaused = false, resumeT = 0;
function armResume() {
  clearTimeout(resumeT);
  if (!manual || userPaused || reduce) return;
  resumeT = setTimeout(() => { if (hact.recording()) armResume(); else setHeld(false); }, 3000);
}
function setHeld(v) {
  clearTimeout(resumeT);
  held = v; pauseBtn.textContent = v ? 'Play demo' : 'Pause demo';
  if (!v && manual) { hact.stop(); manual = false; mount(hmode); hknote.setAttribute('aria-live', 'off'); } // back to the story, in a clean window
  else if (v && hdemo && !manual) hdemo.settle();
  playing();
}
// A key of the visitor's own: the story stops, the window starts clean, and the key does what it does in the app.
const SAID = { save: 'Clip saved: it landed first in the library.', shot: 'Screenshot saved: a notice, and no new clip.', view: 'Alt+4 opens this small viewer over your game. Click a clip to step through.' };
// The notice sits at the window's foot: if that is below the fold, scroll it up (smoothly, unless motion is off).
function bringIntoView() {
  const r = hfit.getBoundingClientRect(), dy = Math.min(r.bottom - innerHeight + 24, r.top - $('#nav').offsetHeight - 8);
  if (dy > 4) scrollBy({ top: dy, behavior: reduce ? 'auto' : 'smooth' });
}
function use(act) {
  if (!hact) return;
  if (!manual) { manual = true; hdemo.pause(); hact.stop(); mount(hmode); held = true; pauseBtn.textContent = 'Play demo'; hknote.setAttribute('aria-live', 'polite'); }
  const b = hk(act), on = hact[act]();
  bringIntoView();
  const caps = $$('kbd', b); caps.forEach((c) => c.classList.add('down')); setTimeout(() => caps.forEach((c) => c.classList.remove('down')), 180);
  if (act === 'record' || act === 'view') b.setAttribute('aria-pressed', String(!!on));
  hknote.textContent = act === 'record' ? (on ? 'Recording. Press Alt+3 again to stop it.' : 'Recording saved: it landed first in the library.') : act === 'view' && !on ? 'Viewer closed.' : SAID[act];
  armResume();
}
hks.forEach((b) => b.addEventListener('click', () => use(b.dataset.act)));
addEventListener('keydown', (e) => {
  if (e.altKey && e.ctrlKey && !e.metaKey && !e.shiftKey && onScreen && hact && hact.viewing() && (e.code === 'ArrowLeft' || e.code === 'ArrowRight')) { e.preventDefault(); hact.step(e.code === 'ArrowRight' ? 1 : -1); armResume(); return; }
  if (!e.altKey || e.ctrlKey || e.metaKey || e.shiftKey || !onScreen) return;
  const act = { Digit1: 'save', Digit2: 'shot', Digit3: 'record', Digit4: 'view' }[e.code];
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
const tbtn = $('#themebtn'), pop = $('#pop'), slist = $('#siteThemes'), bgsw = $('#bgsw');
// A chosen theme spreads out from the theme button as a circular reveal (a View Transition); without that, or with reduced motion, it just swaps.
function setSite(id, { focus = false, ripple = false } = {}) {
  const apply = () => {
    store.site = id; document.documentElement.dataset.theme = id; if (ripple) keep();
    $('#themename').textContent = name(id); $('#themesw').dataset.theme = id; tbtn.setAttribute('aria-label', `Theme: ${name(id)}`);
    $('meta[name="theme-color"]').setAttribute('content', getComputedStyle(document.documentElement).getPropertyValue('--bx-ground').trim());
    $$('button', slist).forEach((b) => { const on = b.dataset.id === id; b.setAttribute('aria-checked', String(on)); b.tabIndex = on ? 0 : -1; if (on && focus) b.focus(); });
    $$('.bxp').forEach((w) => { w.dataset.theme = id; });
    BXRec.colors();
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
function setBg(on) { store.bg = on; bgsw.setAttribute('aria-checked', String(on)); BXRec.on(on && !isPhone.matches); keep(); } // the animated background is for bigger screens: a phone spends its battery on the page
bgsw.addEventListener('click', () => setBg(!store.bg));
// the crosshair cursor: for a mouse only, so a touch screen never sees the switch
const xhsw = $('#xhsw');
function setCross(on) { store.cross = on; xhsw.setAttribute('aria-checked', String(on)); BXCross.on(on); keep(); }
xhsw.hidden = !BXCross.supported();
xhsw.addEventListener('click', () => setCross(!store.cross));
if (reduce) { bgsw.disabled = true; bgsw.title = 'Your system asks for less motion'; }
setSite(store.site); setBg(store.bg); setCross(store.cross);

// ---- features: six parts, each one a demonstration ----
const demos = {};

// One library: every clip filed by game; the filter and the pick do what the panel's do.
demos.library = (s) => {
  const NAMES = { rivals: ['Team wipe'], cs2: ['Inferno rush', 'Barrel peek'], valorant: ['Ace'], finals: ['Cashout run'], acbf: ['Harbour dive'], acm: ['Souk chase'], msfs: ['Low pass'], forza: ['Touge run'] }; // one name per picture, so no two cards show the same frame
  const GAMES = Object.keys(NAMES), LENS = [12, 35, 42, 60, 20, 48, 30, 55, 25];
  // one clip per picture, dealt round the games so the library reads as a mix
  let n = 0; const CLIPS = [0, 1].flatMap((v) => GAMES.filter((g) => NAMES[g][v]).map((g) => { const secs = LENS[(n++ * 5) % LENS.length]; return [NAMES[g][v], g, mmss(secs), `${Math.max(2, Math.round(secs * 0.68))} MB`, v]; }));
  s.innerHTML = `<label class="lib-f"><span class="sr">Game</span><select data-k="f"></select></label><div class="lib" data-k="grid" tabindex="0" role="region" aria-label="Clips, scrolls"></div>
    <div class="row-btns joined" data-k="acts"><button class="btn small" data-k="e" type="button" disabled>Edit</button><button class="btn small" data-k="s" type="button" disabled>Share</button><button class="btn small" data-k="c" type="button" disabled>Copy</button><button class="btn small" data-k="r" type="button" disabled>Rename</button></div>
    <p class="status" data-k="st" aria-live="polite">Pick a clip.</p>`;
  s.classList.add('top');
  const sel = k(s, 'f'), grid = k(s, 'grid'); let game = '', picked = -1;
  const act = { e: 'opens in the editor.', s: 'goes to the Share sheet.', c: 'is copied, ready to paste.', r: 'can be renamed in place.' };
  const draw = () => {
    grid.replaceChildren(...CLIPS.map((c, i) => [c, i]).filter(([c]) => !game || c[1] === game).map(([c, i]) => {
      const b = el('button', 'lclip' + (i === picked ? ' on' : ''), `${BXGames.shot(c[1], '', c[4])}<b>${c[0]}</b><small>${BXGames.name(c[1])} &middot; ${c[2]}</small>`);
      b.type = 'button'; b.addEventListener('click', () => { picked = i; draw(); k(s, 'st').innerHTML = `<b>${c[0]}</b> &middot; ${c[2]} &middot; ${c[3]}. Press a key below, or <kbd>E</kbd> <kbd>S</kbd> <kbd>C</kbd> <kbd>F2</kbd>.`; }); return b;
    }));
    $$('button', k(s, 'acts')).forEach((b) => { b.disabled = picked < 0; });
    grid.scrollTop = 0;
  };
  [['', `All games (${CLIPS.length})`], ...GAMES.map((g) => [g, `${BXGames.name(g)} (${CLIPS.filter((c) => c[1] === g).length})`])].forEach(([v, label]) => sel.append(new Option(label, v)));
  sel.addEventListener('change', () => { game = sel.value; picked = -1; k(s, 'st').textContent = 'Pick a clip.'; draw(); });
  $$('button', k(s, 'acts')).forEach((b) => b.addEventListener('click', () => { if (picked >= 0) k(s, 'st').innerHTML = `<b>${CLIPS[picked][0]}</b> ${act[b.dataset.k]}`; }));
  draw();
};

// Editing built in: drag the ends of the kept part, pick a crop shape, save a copy.
demos.edit = (s) => {
  const TOTAL = 60, MB = 41 / 60, SHAPES = { free: ['Free', 1], '16:9': ['16:9', 1], '9:16': ['9:16', 0.316], '1:1': ['1:1', 0.5625] };
  s.innerHTML = `<div class="ed-view">${BXGames.shot('cs2', '', 1)}<i class="ed-crop" data-k="crop"></i></div>
    <div class="ed-lane" data-k="lane"><div class="wv" data-k="wv"></div><i class="cut l"></i><i class="cut r"></i><i class="keep"><b class="hd" role="slider" tabindex="0" data-edge="in" aria-label="Start of the part you keep"></b><b class="hd r" role="slider" tabindex="0" data-edge="out" aria-label="End of the part you keep"></b></i></div>
    <div class="ed-info"><span data-k="keep"></span><span data-k="est"></span></div>
    <div class="chips2" role="group" aria-label="Crop shape" data-k="shape"></div>
    <div class="row-btns joined"><button class="btn small" data-k="reset" type="button">Keep it all</button><button class="btn small primary" data-k="save" type="button">Save a copy</button></div>
    <p class="status" data-k="st" aria-live="polite">Drag the ends of the lit part, or focus one and use the arrow keys.</p>`;
  k(s, 'wv').append(...Array.from({ length: 72 }, (_, i) => el('i', 'h' + (1 + Math.floor(Math.abs(Math.sin(i * 0.37) * Math.cos(i * 0.11)) * 7)))));
  const lane = k(s, 'lane'), hds = $$('.hd', s); let a = 0, b = TOTAL, shape = 'free';
  const MINLEN = 2;
  const paint = () => {
    lane.style.setProperty('--in', `${(a / TOTAL) * 100}%`); lane.style.setProperty('--out', `${(b / TOTAL) * 100}%`);
    hds[0].setAttribute('aria-valuenow', Math.round(a)); hds[1].setAttribute('aria-valuenow', Math.round(b));
    hds[0].setAttribute('aria-valuetext', mmss(a)); hds[1].setAttribute('aria-valuetext', mmss(b));
    k(s, 'keep').innerHTML = `Keeping <b>${mmss(a)}</b> &rarr; <b>${mmss(b)}</b> (${(b - a).toFixed(1)} s)`;
    k(s, 'est').innerHTML = `&asymp; <b>${Math.max(1, Math.round((b - a) * MB * SHAPES[shape][1]))} MB</b>`;
    k(s, 'crop').className = 'ed-crop' + (shape === '9:16' ? ' c916' : shape === '1:1' ? ' c11' : '');
    $$('button', k(s, 'shape')).forEach((x) => x.setAttribute('aria-pressed', String(x.dataset.s === shape)));
  };
  hds.forEach((h, idx) => {
    h.setAttribute('aria-valuemin', 0); h.setAttribute('aria-valuemax', TOTAL);
    const move = (t) => { if (idx === 0) a = Math.max(0, Math.min(t, b - MINLEN)); else b = Math.min(TOTAL, Math.max(t, a + MINLEN)); paint(); };
    h.addEventListener('pointerdown', (e) => { h.setPointerCapture(e.pointerId); h.focus(); });
    h.addEventListener('pointermove', (e) => { if (!h.hasPointerCapture(e.pointerId)) return; const r = lane.getBoundingClientRect(); move(((e.clientX - r.left) / r.width) * TOTAL); });
    h.addEventListener('keydown', (e) => { const step = e.shiftKey ? 5 : 1; if (e.key === 'Home' || e.key === 'End') { e.preventDefault(); move(e.key === 'Home' ? 0 : TOTAL); return; } const d = e.key === 'ArrowLeft' || e.key === 'ArrowDown' ? -step : e.key === 'ArrowRight' || e.key === 'ArrowUp' ? step : 0; if (!d) return; e.preventDefault(); move((idx === 0 ? a : b) + d); });
  });
  Object.entries(SHAPES).forEach(([id, [label]]) => { const x = el('button', '', label); x.type = 'button'; x.dataset.s = id; x.addEventListener('click', () => { shape = id; paint(); }); k(s, 'shape').append(x); });
  k(s, 'reset').addEventListener('click', () => { a = 0; b = TOTAL; shape = 'free'; paint(); k(s, 'st').textContent = 'The whole clip again.'; });
  k(s, 'save').addEventListener('click', () => { k(s, 'st').innerHTML = `<b>Saved a copy</b> to Edits: ${mmss(b - a)}${shape === 'free' ? '' : ', cropped ' + shape}. The original is not touched.`; });
  a = 6; b = 52; paint();
};

// Two sizes of one app: the full panel, and the quick panel from the tray. Both wear the site's theme.
demos.sizes = (s) => {
  s.innerHTML = `<div class="seg" role="group" aria-label="Window size"><button type="button" data-mode="compact" aria-pressed="false">Compact</button><button type="button" data-mode="panel" aria-pressed="true">Expanded</button></div>
    <div class="size-stage" data-k="stage"><div class="win-fit" data-k="fit"><div class="bxp" data-k="win" role="img"></div></div></div>
    <p class="status" data-k="st" aria-live="polite"></p>`;
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

// Auto game capture: the source section of the app, simplified. Games + Display follows the game you start; Display stays on the
// screen; Window captures the one you pick. A fullscreen game is taken after half a second, an alt-tab to something else keeps it,
// and closing it goes back to the screen.
demos.auto = (s, c) => {
  const APPS = [['BuffrX', 'buffrx-engine.exe'], ['Discord', 'discord.exe'], ['File Explorer', 'explorer.exe'], ['Mozilla Firefox', 'firefox.exe'], ['Windows Terminal', 'windowsterminal.exe']];
  const GAME = ['Marvel Rivals', 'marvelrivals.exe'];
  const MODES = [['auto', 'Games + display'], ['display', 'Display'], ['window', 'Window']];
  s.innerHTML = `<div class="seg ac-seg" role="group" aria-label="What to capture" data-k="modes"></div>
    <ul class="ac-list" data-k="list"></ul>
    <div class="row-btns joined" data-k="acts"><button class="btn small" data-k="start" type="button">Start the game</button><button class="btn small" data-k="tab" type="button" disabled>Alt-tab away</button><button class="btn small" data-k="close" type="button" disabled>Close the game</button></div>
    <p class="ac-now" aria-live="polite" data-k="now"></p>`;
  let mode = 'auto', game = false, taken = false, focus = 0, picked = 1;
  const list = k(s, 'list'), now = k(s, 'now');
  const target = () => (mode === 'display' ? null : mode === 'window' ? (picked === -1 ? 'game' : picked) : (taken ? 'game' : null));
  const rows = () => [...APPS.map(([t, e], i) => [t, e, i]), ...(game ? [[GAME[0], GAME[1], 'game']] : [])];
  const draw = () => {
    const tg = target();
    list.replaceChildren(...rows().map(([t, e, id]) => {
      const on = tg === id, li = el('li', 'ac-row' + (on ? ' cap' : '') + (focus === id ? ' fg' : '') + (id === 'game' ? ' ac-g' : ''),
        `${id === 'game' ? `<span class="ac-thumb">${BXGames.shot('rivals', '', 1)}</span>` : '<svg class="ac-ic" viewBox="0 0 24 24" aria-hidden="true"><rect x="3" y="5" width="18" height="14"/><path d="M3 9h18"/></svg>'}<b>${t}</b><span>${e}</span>${on ? '<i class="ac-tag">Capturing</i>' : ''}`);
      if (mode === 'window') { li.tabIndex = 0; li.setAttribute('role', 'button'); const pick = () => { picked = id; draw(); }; li.addEventListener('click', pick); li.addEventListener('keydown', (ev) => { if (ev.key === 'Enter' || ev.key === ' ') { ev.preventDefault(); pick(); } }); }
      return li;
    }));
    $$('button', k(s, 'modes')).forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.m === mode)));
    k(s, 'start').disabled = game; k(s, 'tab').disabled = !game; k(s, 'close').disabled = !game;
    list.classList.toggle('pick', mode === 'window');
    const name = tg === null ? null : tg === 'game' ? GAME[0] : APPS[tg][0];
    now.innerHTML = mode === 'window' ? (name ? `Capturing <b>${name}</b>. Pick another window to switch.` : 'Pick the window to capture.') : name ? `Capturing <b>${name}</b>.` : mode === 'display' ? 'Capturing <b>your display</b>, whatever is on it.' : 'Capturing <b>your display</b> until a game starts.';
  };
  const setNow = (t) => { now.innerHTML = t; };
  MODES.forEach(([m, label]) => { const b = el('button', '', label); b.type = 'button'; b.dataset.m = m; b.addEventListener('click', () => { c.dispose(); mode = m; if (m === 'window' && picked === -1 && !game) picked = 1; draw(); }); k(s, 'modes').append(b); });
  k(s, 'start').addEventListener('click', () => {
    c.dispose(); game = true; focus = 'game'; taken = false; draw();
    if (mode === 'auto') { setNow('A fullscreen game started&hellip;'); c.later(() => { taken = true; draw(); setNow('Capturing <b>' + GAME[0] + '</b>. It took half a second, and you picked nothing.'); }, reduce ? 0 : 700); }
    else if (mode === 'window') { setNow('The game is running, but you picked another window.'); }
  });
  k(s, 'tab').addEventListener('click', () => { c.dispose(); focus = 1; draw(); setNow(taken ? 'You alt-tabbed to Discord. <b>It keeps the game.</b>' : now.innerHTML); });
  k(s, 'close').addEventListener('click', () => { c.dispose(); game = false; taken = false; focus = 0; if (picked === 'game') picked = 1; draw(); setNow(mode === 'display' ? 'Capturing <b>your display</b>, whatever is on it.' : 'The game closed. Back to <b>your display</b>.'); });
  draw();
};

// Insta Share: make the link, a friend clicks it, the clip crosses straight from one PC to the other (packets on a wire, the picture
// arriving as it goes), and Stop ends it at once. Nothing is uploaded first.
demos.link = (s, c) => {
  const KEY = '<svg class="is-key" viewBox="0 0 24 24" aria-hidden="true"><circle cx="8" cy="12" r="4"/><path d="M12 12h9M18 12v4M15 12v3"/></svg>';
  const CELLS = 32;
  s.innerHTML = `<ol class="is-steps" data-k="steps" aria-label="The three steps"><li class="on" data-s="1">Make the link</li><li data-s="2">Friend clicks it</li><li data-s="3">It plays</li></ol>
    <div class="is-stage">
      <div class="is-pc" data-k="me"><div class="is-bar"><i></i><i></i><i></i><span>Your PC</span></div><div class="is-shot">${BXGames.shot('finals', '', 0)}</div><div class="is-foot"><b data-k="mes">Ready</b></div></div>
      <div class="is-wire" data-k="wire"><i class="is-line"></i><span class="is-packets" data-k="pk"></span><small data-k="wl">no link</small></div>
      <div class="is-pc" data-k="fr"><div class="is-bar"><i></i><i></i><i></i><span>Friend's BuffrX</span></div><div class="is-shot is-rev" data-k="rev">${BXGames.shot('finals', '', 0)}</div><div class="is-foot"><b data-k="frs">Waiting</b></div></div>
    </div>
    <div class="is-meter" data-k="meter" aria-hidden="true">${'<i></i>'.repeat(CELLS)}</div>
    <div class="is-link" data-k="link">${KEY}<span class="is-lt" data-k="lt">no link yet</span><span class="is-exp" role="group" aria-label="Link lasts"><button type="button" data-h="1" aria-pressed="false">1 h</button><button type="button" data-h="24" aria-pressed="true">24 h</button></span><i data-k="lc"></i></div>
    <div class="is-act"><button class="btn primary" data-k="go" type="button">Make a link</button><button class="btn" data-k="stop" type="button" disabled>Stop</button></div>
    <p class="status" data-k="st" aria-live="polite">Make a link, and send a 1.8 GB clip with no upload first.</p>`;
  const T = (key, t) => { k(s, key).innerHTML = t; }, rev = k(s, 'rev'), pk = k(s, 'pk'), cells = [...k(s, 'meter').children], root = s, go = k(s, 'go');
  const LINK = 'buffrx://l/AV7xK2mQ9d\u2026';
  let state = 'idle', p = 0, typed = 0, hours = 24;
  const mode = (m) => { root.dataset.state = m; };
  const step = (n) => $$('li', k(s, 'steps')).forEach((li) => li.classList.toggle('on', Number(li.dataset.s) === n));
  const reveal = (v) => { rev.style.setProperty('clip-path', `inset(0 ${100 - v}% 0 0)`); const lit = Math.round(v / 100 * CELLS); cells.forEach((q, i) => q.classList.toggle('on', i < lit)); };
  const packet = () => { const q = el('i', 'is-pk'); q.style.setProperty('--y', `${(Math.random() * 18 - 9).toFixed(1)}px`); pk.append(q); c.later(() => q.remove(), 1000); };
  const apply = () => {
    go.disabled = state === 'sending';
    go.textContent = state === 'idle' || state === 'stopped' ? 'Make a link' : state === 'linked' ? 'Friend clicks the link' : state === 'done' ? 'Send another' : 'Sending\u2026';
    k(s, 'stop').disabled = !(state === 'linked' || state === 'sending');
  };
  const reset = () => { c.dispose(); pk.replaceChildren(); p = 0; reveal(0); mode('idle'); };
  const make = () => {
    reset(); state = 'linked'; typed = 0; mode('linked'); step(1); T('mes', 'Making a link'); T('frs', 'Waiting'); T('wl', 'link is live'); T('st', 'A link carries a public key and an expiry, never an IP address.'); apply();
    k(s, 'lc').textContent = '';
    c.every(40, () => { typed++; T('lt', LINK.slice(0, typed)); if (typed >= LINK.length) { c.dispose(); T('mes', `Link made &middot; ${hours} h`); k(s, 'lc').textContent = 'copied'; step(2); } });
  };
  const send = () => {
    c.dispose(); state = 'sending'; p = 0; mode('sending'); step(2); T('mes', 'Sending'); T('frs', 'Receiving'); T('wl', 'connecting&hellip;'); T('st', 'Straight to your friend. No upload first, and no server of ours.'); apply();
    c.later(() => {
      T('wl', 'direct &middot; encrypted');
      c.every(110, () => {
        if (!reduce) { packet(); if (Math.random() < 0.6) packet(); }
        p = Math.min(100, p + 3 + Math.random() * 3); reveal(p); T('frs', `Receiving &middot; ${Math.round(p)}% &middot; ${(p * 0.018).toFixed(2)} of 1.80 GB`);
        if (p >= 100) { c.dispose(); state = 'done'; mode('done'); step(3); T('mes', 'Sent'); T('frs', '<b>Arrived and playing</b>'); T('wl', 'done'); T('st', 'The clip is in your friend\'s "Shared with me", checked before it got there.'); apply(); }
      });
    }, reduce ? 0 : 600);
  };
  go.addEventListener('click', () => { if (state === 'linked') send(); else make(); });
  k(s, 'stop').addEventListener('click', () => { reset(); state = 'stopped'; mode('stopped'); step(1); T('mes', 'Link stopped'); T('frs', 'Refused'); T('wl', 'closed'); T('lt', 'link stopped'); k(s, 'lc').textContent = ''; T('st', 'Stop ends the link at once.'); apply(); });
  $$('.is-exp button', s).forEach((b) => b.addEventListener('click', () => { hours = Number(b.dataset.h); $$('.is-exp button', s).forEach((x) => x.setAttribute('aria-pressed', String(x === b))); if (state === 'linked' && typed >= LINK.length) T('mes', `Link made &middot; ${hours} h`); }));
  reveal(0); apply();
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

// Keyboard first: the library is driven with the keys the app uses, and the mouse is never needed.
demos.keys = (s) => {
  const CLIPS = [['Team wipe', 'rivals', '0:12', 2], ['Inferno rush', 'cs2', '0:35', 2], ['Ace', 'valorant', '0:42', 0], ['Cashout run', 'finals', '0:25', 0], ['Souk chase', 'acm', '1:00', 0], ['Last corner', 'forza', '0:55', 0]];
  const KEYS = [['\u2190 \u2191 \u2193 \u2192', 'Move'], ['Enter', 'Preview'], ['E', 'Edit'], ['S', 'Share'], ['C', 'Copy'], ['F2', 'Rename'], ['Del', 'Delete']];
  s.innerHTML = `<div class="kb" tabindex="0" role="group" aria-label="Clip library. Use the arrow keys to move, and Enter, E, S, C, F2 and Delete to act on a clip." data-k="kb"><div class="kb-grid" data-k="grid"></div><span class="kb-hint" data-k="hint">Tab to here, then use the keys</span></div>
    <div class="kb-keys" data-k="keys"></div><p class="status" data-k="st" aria-live="polite">Click the library or Tab to it, then use the keyboard. No mouse needed.</p>`;
  let list = CLIPS.slice(), sel = 0, named = {};
  const grid = k(s, 'grid'), kb = k(s, 'kb'), st = (t) => { k(s, 'st').innerHTML = t; };
  const draw = () => {
    grid.replaceChildren(...list.map((c, i) => { const d = el('div', 'kb-card' + (i === sel ? ' on' : ''), `${BXGames.shot(c[1], '', c[3])}<b>${named[c[0]] || c[0]}</b><small>${BXGames.name(c[1])} &middot; ${c[2]}</small>`); return d; }));
    k(s, 'hint').hidden = document.activeElement === kb;
  };
  const cap = (label) => { const b = $$('.kcap', k(s, 'keys')).find((x) => x.dataset.k === label); if (b) { b.classList.add('down'); setTimeout(() => b.classList.remove('down'), 180); } };
  const act = (key) => {
    if (!list.length) { list = CLIPS.slice(); sel = 0; st('The library is back.'); draw(); return; }
    const c = list[sel], cols = 3;
    if (key === 'ArrowRight') sel = Math.min(list.length - 1, sel + 1);
    else if (key === 'ArrowLeft') sel = Math.max(0, sel - 1);
    else if (key === 'ArrowDown') sel = Math.min(list.length - 1, sel + cols);
    else if (key === 'ArrowUp') sel = Math.max(0, sel - cols);
    else if (key === 'Enter') st(`<b>${named[c[0]] || c[0]}</b> opens in the preview. Esc closes it.`);
    else if (key === 'e') st(`<b>${named[c[0]] || c[0]}</b> opens in the editor: trim, split, crop.`);
    else if (key === 's') st(`<b>${named[c[0]] || c[0]}</b> goes to the Share sheet: an Insta Share link, or Discord.`);
    else if (key === 'c') st(`<b>${named[c[0]] || c[0]}</b> is copied, ready to paste into a chat.`);
    else if (key === 'F2') { named[c[0]] = `${c[0]} (renamed)`; st('Renamed in place. Type, then Enter.'); }
    else if (key === 'Delete') { list.splice(sel, 1); sel = Math.min(sel, list.length - 1); st(list.length ? 'Deleted. Press any key on an empty library to bring it back.' : 'Empty. Press any key to bring the library back.'); }
    draw();
  };
  const label = { ArrowLeft: KEYS[0][0], ArrowRight: KEYS[0][0], ArrowUp: KEYS[0][0], ArrowDown: KEYS[0][0], Enter: 'Enter', e: 'E', s: 'S', c: 'C', F2: 'F2', Delete: 'Del' };
  KEYS.forEach(([cap_, what]) => { const b = el('button', 'kcap', `<kbd>${cap_}</kbd><span>${what}</span>`); b.type = 'button'; b.dataset.k = cap_; b.tabIndex = -1; b.addEventListener('click', () => { const key = { 'Enter': 'Enter', E: 'e', S: 's', C: 'c', F2: 'F2', Del: 'Delete' }[cap_] || 'ArrowRight'; act(key); cap(cap_); }); k(s, 'keys').append(b); });
  kb.addEventListener('keydown', (e) => {
    const key = e.key.length === 1 ? e.key.toLowerCase() : e.key;
    if (!(key in label) || e.ctrlKey || e.altKey || e.metaKey) return;
    e.preventDefault(); cap(label[key]); act(key);
  });
  kb.addEventListener('focus', draw); kb.addEventListener('blur', draw);
  // armed: the box closes in with an accent frame and takes focus, so the first key press already does something
  const arm = () => { kb.classList.remove('arm'); void kb.offsetWidth; kb.classList.add('arm'); kb.focus({ preventScroll: true }); draw(); st('<b>Ready.</b> The library has the keyboard: press an arrow key.'); };
  s.addEventListener('bx:arm', arm);
  s.addEventListener('pointerdown', (e) => { if (!kb.contains(e.target) && !e.target.closest('button, .kb-keys')) arm(); });
  kb.addEventListener('animationend', () => kb.classList.remove('arm'));
  draw();
};

// The game keeps focus. Alt+1 saves a clip (a sound, not a card, over a fullscreen game), and Alt+4 shows it at once in a small window that never takes the focus.
// The game is a real clip the maker recorded with BuffrX (games/rivals-clip.mp4); with no video it falls back to the frame.
demos.first = (s, c) => {
  s.innerHTML = `<div class="game focused"><video class="gvid" src="games/rivals-clip.mp4" poster="games/rivals.jpg" muted loop playsinline preload="none" aria-hidden="true" tabindex="-1"></video><span class="hud">Marvel Rivals &middot; has focus</span></div>
    <div class="row-btns joined"><button class="btn small" data-k="fs" type="button" aria-pressed="true">Fullscreen game: on</button><button class="btn small" data-k="save" type="button"><kbd>Alt</kbd><kbd>1</kbd>Save clip</button><button class="btn small" data-k="view" type="button" aria-pressed="false"><kbd>Alt</kbd><kbd>4</kbd>View clip</button></div>
    <p class="status" data-k="st" aria-live="polite">The game keeps focus.</p>`;
  const g = $('.game', s), vid = $('video', s), AGES = ['now', '6m', '12m', '31m', '58m'];
  // plays only while it is on screen, and not at all for reduced motion (the poster stays)
  vid.addEventListener('error', () => { vid.replaceWith(el('div', 'gvid', BXGames.shot('rivals'))); });
  if (!reduce && !isPhone.matches) new IntersectionObserver(([e]) => { if (e.isIntersecting) vid.play().catch(() => {}); else vid.pause(); }, { threshold: 0.2 }).observe(g); // a phone keeps the still frame: no video to download or decode
  let viewer = null, saved = 0, pos = 0, names = ['Round 12', 'Last second', 'Ace', 'Final lap', 'Photo finish'], games = ['rivals', 'cs2', 'valorant', 'forza', 'msfs'], vars = [1, 1, 0, 1, 1];
  const st = (t) => { k(s, 'st').innerHTML = t; };
  const fs = () => k(s, 'fs').getAttribute('aria-pressed') === 'true';
  const paint = () => {
    viewer.innerHTML = `<div class="vpic">${BXGames.shot(games[pos], '', vars[pos])}<span class="vtop"><b>${names[pos]} &middot; ${pos === 0 ? 'just now' : AGES[pos] + ' ago'}</b></span><i class="vbar"></i></div><div class="vstrip">${AGES.map((a, i) => `<button type="button" tabindex="-1" data-i="${i}" class="${i === pos ? 'on' : ''}" aria-label="${names[i]}, ${a}">${BXGames.shot(games[i], '', vars[i])}<small>${a}</small></button>`).join('')}</div><small class="vhint">ctrl alt &larr; &rarr; step &middot; alt 4 close</small>`;
  };
  const onkey = (e) => { if (e.ctrlKey && e.altKey && (e.code === 'ArrowLeft' || e.code === 'ArrowRight')) { e.preventDefault(); pos = Math.max(0, Math.min(4, pos + (e.code === 'ArrowRight' ? 1 : -1))); paint(); } };
  const open = () => {
    pos = 0; viewer = el('div', 'viewer'); paint(); g.append(viewer); k(s, 'view').setAttribute('aria-pressed', 'true');
    viewer.addEventListener('click', (e) => { const b = e.target.closest('button'); if (b) { pos = Number(b.dataset.i); paint(); } });
    document.addEventListener('keydown', onkey);
    st(saved ? '<b>There it is, the clip you just made.</b> Click a clip or press <kbd>Ctrl</kbd><kbd>Alt</kbd><kbd>&larr;</kbd> <kbd>&rarr;</kbd> to step. The game keeps focus: the viewer never takes it.' : '<b>Your newest clip, over the game.</b> Click a clip or press <kbd>Ctrl</kbd><kbd>Alt</kbd><kbd>&larr;</kbd> <kbd>&rarr;</kbd> to step. The game keeps focus.');
  };
  const shut = () => { document.removeEventListener('keydown', onkey); viewer.remove(); viewer = null; k(s, 'view').setAttribute('aria-pressed', 'false'); st('The game keeps focus.'); };
  k(s, 'fs').addEventListener('click', () => { const on = !fs(); k(s, 'fs').setAttribute('aria-pressed', String(on)); k(s, 'fs').textContent = `Fullscreen game: ${on ? 'on' : 'off'}`; });
  k(s, 'save').addEventListener('click', () => {
    saved++; names = [`Round ${12 + saved}`, ...names].slice(0, 5); games = [['finals', 'acbf', 'acm', 'msfs', 'forza'][(saved - 1) % 5], ...games].slice(0, 5); vars = [0, ...vars].slice(0, 5);
    if (fs()) st('No card over a fullscreen game. <span class="sound"><i></i><i></i><i></i><i></i></span> <b>A sound says it saved.</b> Press <kbd>Alt</kbd><kbd>4</kbd> to see it now.');
    else { const t = el('div', 'toast', '<div><span class="tag">Saved</span> 1:00</div>'); g.append(t); st('Not fullscreen, so the card shows over the game. Press <kbd>Alt</kbd><kbd>4</kbd> to see the clip now.'); c.later(() => { t.classList.add('out'); c.later(() => t.remove(), 260); }, 1800); }
    if (viewer) { pos = 0; paint(); }
  });
  k(s, 'view').addEventListener('click', () => (viewer ? shut() : open()));
};

const FEATURES = [
  { id: 'library', col: 0, name: 'One library for everything', head: 'Every clip, filed by game.',
    facts: ['Recordings, saved moments and screenshots in one place, filed by game.', 'Play any clip in the preview, then trim it in the same window.', 'Rename, copy, show in folder or delete from a clip\'s menu, or with a key: E edits it and S sends it.'] },
  { id: 'edit', col: 1, name: 'Editing built in', head: 'Cut it where it lives.',
    facts: ['Trim, split and delete parts on a timeline, set each audio track\'s volume, and undo.', 'Crop to a shape such as 9:16, for a vertical clip.', 'Where it can, a cut is copied without loss. Otherwise it is re-encoded on your GPU at the original quality, so the cut lands exactly where you put it.'] },
  { id: 'sizes', more: true, name: 'Two sizes, one app', head: 'The whole panel, or just the quick one.', wide: true,
    facts: ['Expanded is the main window: the buffer, one key per action, and your whole library.', 'Compact is a small quick panel that opens from the tray. Movable, and it stays where you put it.', 'Both come in fourteen themes. Use the theme button in the bar and the windows follow it.'] },
  { id: 'auto', more: true, wide: true, name: 'Auto game capture', head: 'It finds your game, so you pick nothing.',
    facts: ['Start a fullscreen game and BuffrX switches to it after half a second.', 'Alt-tab to a chat and it keeps the game. Close the game and it goes back to your screen.', 'Or choose for yourself: Games + display, one display, or a single window.'] },
  { id: 'link', star: true, wide: true, name: 'Insta Share', head: 'Send the clip, not a copy of it.',
    facts: ['A link in a clip\'s Share sheet sends it straight to a friend: up to 4 GB, in full quality. No upload first, and no server of ours. Your friend needs BuffrX (free), and it works while your PC is on.', 'The link never carries your IP address. Stop ends it at once.', 'What arrives is checked before it enters "Shared with me".'] },
  { id: 'discord', more: true, wide: true, name: 'Discord-ready in one tick', head: 'Too big for the chat? It fits itself.',
    facts: ['Tick "Make it ready for Discord" and BuffrX picks the biggest picture that fits your limit: 20, 50, 100 or 500 MiB.', 'A clip that already fits goes as it is.'] },
  { id: 'keys', more: true, name: 'Keyboard first', head: 'Made to be used without the mouse.', wide: true,
    facts: ['Tab walks the panes. The arrows move over your clips, and Enter previews, E edits, S shares, C copies, F2 renames and Del deletes.', 'Four hotkeys work from inside a game, and 1 to 4 switch profiles. A comma opens Settings, Esc backs out, and F1 lists every key.', 'The mouse works too. It just is never required.'] },
  { id: 'first', more: true, name: 'The game comes first', head: 'It never takes the game\'s focus.', wide: true,
    facts: ['A fullscreen game gets a sound that it saved, not a card.', 'Alt+4 shows the clip you just made, instantly, in a small window over the game that never takes its focus. Ctrl+Alt+Left and Right step through your last five.', 'The recording mark is never in a recording.'] },
];
const fine = matchMedia('(hover: hover) and (pointer: fine)').matches;
// two columns of tiles: each card sits under the one above it, so a card that shrinks lets the next one move up
const bcols = [el('div', 'bcol'), el('div', 'bcol')]; $('#bento').append(...bcols);
const PHONE_CARDS = ['library', 'edit', 'link']; // a phone gets the three cards that sell the app; the rest is built only where there is room to try it
const lite = isPhone.matches;
FEATURES.forEach((f) => {
  if (lite && !PHONE_CARDS.includes(f.id)) return;
  const card = el('article', 'fcard' + (f.wide ? ' wide' : '') + (f.star ? ' star' : ''), `<div class="fcopy"><h3>${f.name}</h3><p class="head">${f.head}</p><ul class="facts">${f.facts.map((x) => Array.isArray(x) ? `<li class="measured">${x[0]}</li>` : `<li>${x}</li>`).join('')}</ul></div><div class="demo"></div>`);
  if (f.more) { card.id = `more-${f.id}`; card.setAttribute('role', 'tabpanel'); card.setAttribute('aria-labelledby', `tab-${f.id}`); card.hidden = true; $('#morePanel').append(card); }
  else { card.dataset.r = 'scale'; if (f.star) $('#bento').prepend(card); else bcols[f.col || 0].append(card); reveal.observe(card); if (isPhone.matches) card.classList.add('in'); }
  try { demos[f.id]($('.demo', card), ctx()); } catch (err) { console.error(err); }
  // the tile follows the pointer: a bright edge and a soft light under it, and a slight lean toward it (not while it is over the demo, which is for pressing)
  card.addEventListener('pointermove', (e) => {
    const r = card.getBoundingClientRect(), px = (e.clientX - r.left) / r.width, py = (e.clientY - r.top) / r.height, lean = !reduce && fine && !e.target.closest('.demo');
    card.style.setProperty('--mx', e.clientX - r.left + 'px'); card.style.setProperty('--my', e.clientY - r.top + 'px');
    card.style.setProperty('--ang', `${(Math.atan2(py - 0.5, (px - 0.5) * (r.width / r.height)) * 180 / Math.PI + 90).toFixed(1)}deg`);
    card.style.setProperty('--ry', lean ? `${((px - 0.5) * 3).toFixed(2)}deg` : '0deg'); card.style.setProperty('--rx', lean ? `${(-(py - 0.5) * 3).toFixed(2)}deg` : '0deg');
  });
  card.addEventListener('pointerleave', () => { card.style.setProperty('--rx', '0deg'); card.style.setProperty('--ry', '0deg'); });
});
// four more demos, one at a time: tabs, arrow keys move between them
(function more() {
  if (lite) { $('#more').hidden = true; return; }
  const list = FEATURES.filter((f) => f.more), tabs = $('#moreTabs'), panel = $('#morePanel'), phone = matchMedia('(max-width: 720px)');
  let current = list[0].id, userPick = false;
  const vis = () => (phone.matches ? list.filter((f) => f.id !== 'keys') : list); // a phone has no keyboard to try
  const mark = (id, focus) => { current = id; list.forEach((f) => { const on = f.id === id, b = $(`#tab-${f.id}`); b.setAttribute('aria-selected', String(on)); b.tabIndex = on ? 0 : -1; if (on && focus) b.focus({ preventScroll: true }); }); const t = $(`#tab-${id}`); if (tabs.scrollWidth > tabs.clientWidth) tabs.scrollTo({ left: t.offsetLeft - 16, behavior: reduce ? 'auto' : 'smooth' }); };
  // a desktop shows one card at a time; a phone lays them side by side and swipes
  const pick = (id, focus) => {
    mark(id, focus);
    if (phone.matches) { const i = vis().findIndex((f) => f.id === id); panel.scrollTo({ left: i * panel.clientWidth, behavior: reduce ? 'auto' : 'smooth' }); return; }
    list.forEach((f) => { $(`#more-${f.id}`).hidden = f.id !== id; });
    $$('video').forEach((v) => { if (v.closest('[hidden]')) v.pause(); });
    if (id === 'keys' && userPick) $('#more-keys .demo').dispatchEvent(new Event('bx:arm'));
  };
  list.forEach((f) => { const b = el('button', '', f.name); b.type = 'button'; b.id = `tab-${f.id}`; b.setAttribute('role', 'tab'); b.setAttribute('aria-controls', `more-${f.id}`); b.addEventListener('click', () => { userPick = true; pick(f.id); userPick = false; }); tabs.append(b); });
  tabs.addEventListener('keydown', (e) => {
    const i = vis().findIndex((f) => f.id === current), n = vis().length;
    const to = e.key === 'ArrowRight' ? (i + 1) % n : e.key === 'ArrowLeft' ? (i + n - 1) % n : e.key === 'Home' ? 0 : e.key === 'End' ? n - 1 : -1;
    if (to >= 0) { e.preventDefault(); pick(vis()[to].id, true); }
  });
  let ticking = false, armT = 0;
  panel.addEventListener('scroll', () => { if (!phone.matches || ticking) return; ticking = true; requestAnimationFrame(() => { ticking = false; const v = vis(), f = v[Math.max(0, Math.min(v.length - 1, Math.round(panel.scrollLeft / panel.clientWidth)))]; if (f.id !== current) { mark(f.id); if (f.id === 'keys') clearTimeout(armT), armT = setTimeout(() => $('#more-keys .demo').dispatchEvent(new Event('bx:arm')), 350); } }); }, { passive: true });
  const layout = () => { if (phone.matches) list.forEach((f) => { $(`#more-${f.id}`).hidden = false; }); if (!vis().some((f) => f.id === current)) current = vis()[0].id; pick(current); };
  phone.addEventListener('change', layout);
  layout();
})();


// ---- pricing: two plans, one app, both free. The upgrade button runs a receipt for nothing, and a burst of zeros. ----
(function pricing() {
  const PER = { month: 'per month, billed never', year: 'per year, billed never. Saves 0%', life: 'once, paid never' };
  const WAS = { month: '$9.99', year: '$99.99', life: '$299' };
  const per = $('[data-k="per"]'), amt = $('[data-k="amt"]'), was = $('#pricing s'), btn = $('#upgrade'), out = $('#receipt'), prem = $('.plan.prem'), c = ctx(); let times = 0, rolled = false;
  $$('.prem .perks li').forEach((li, i) => li.style.setProperty('--n', i));
  // the price counts down to nothing
  const roll = () => {
    const start = parseFloat(was.textContent.slice(1)), t0 = performance.now(), len = 900;
    if (reduce) { amt.textContent = '$0'; return; }
    const step = () => { const q = Math.min(1, (performance.now() - t0) / len), v = start * (1 - q) * (1 - q); amt.textContent = q >= 1 ? '$0' : `$${v.toFixed(2)}`; if (q < 1) requestAnimationFrame(step); };
    step();
  };
  new IntersectionObserver((es, o) => { if (es[0].isIntersecting && !rolled) { rolled = true; setTimeout(roll, 350); o.disconnect(); } }, { threshold: 0.5 }).observe(prem);
  $$('#billing button').forEach((b) => b.addEventListener('click', () => { $$('#billing button').forEach((x) => x.setAttribute('aria-pressed', String(x === b))); per.textContent = PER[b.dataset.p]; was.textContent = WAS[b.dataset.p]; roll(); }));
  // the whole screen throws a party: the page shakes, a shine wipes across, $0 and FREE rain from the top, and a stamp slams down
  let party = null;
  const endParty = () => { if (!party) return; party.remove(); party = null; removeEventListener('keydown', escParty); };
  const escParty = (e) => { if (e.key === 'Escape') endParty(); };
  const zeros = () => {
    if (reduce || party) return;
    const W = innerWidth, H = innerHeight, WORDS = ['$0', 'FREE', '$0.00', 'GPL', 'NO CATCH', '$0', 'OPEN SOURCE'];
    const TAPE = 'THIS IS A JOKE &middot; IT IS ALL FREE &middot; NOTHING WAS CHARGED &middot; OPEN SOURCE, ALWAYS &middot; WE ARE JUST TROLLING YOU &middot; ';
    party = el('div', 'party', `<i class="party-shine"></i><div class="party-tape top"><span>${TAPE.repeat(6)}</span></div><div class="party-tape bot"><span>${TAPE.repeat(6)}</span></div><div class="party-card"><b data-k="h">Upgrading you to Premium&hellip;</b><span data-k="s">Charging <em>$0.00</em> to a card that does not exist.</span><i class="party-bar"><u></u></i></div><div class="party-stamp">JUST KIDDING</div>`);
    party.setAttribute('aria-hidden', 'true'); document.body.append(party); addEventListener('keydown', escParty);
    const main = $('main'); main.classList.remove('shake'); void main.offsetWidth; main.classList.add('shake');
    const chips = Array.from({ length: Math.min(90, Math.round(W / 16)) }, (_, i) => {
      const z = el('i', 'zero rain' + (i % 5 === 0 ? ' alt' : i % 7 === 0 ? ' ink' : ''), WORDS[i % WORDS.length]); z.style.setProperty('left', `${Math.random() * W}px`); z.style.setProperty('top', '-40px'); party.append(z);
      z.animate([{ transform: `translateY(0) rotate(${(Math.random() - 0.5) * 40}deg)`, opacity: 1 }, { transform: `translate(${(Math.random() - 0.5) * 160}px, ${H + 80}px) rotate(${(Math.random() - 0.5) * 540}deg)`, opacity: 1 }], { duration: 1500 + Math.random() * 1500, delay: 250 + Math.random() * 900, easing: 'cubic-bezier(.4, .1, .6, 1)', fill: 'both' });
      return z;
    });
    const wipe = $('.party-shine', party); wipe.animate([{ transform: 'translateX(-60vw) skewX(-18deg)' }, { transform: 'translateX(170vw) skewX(-18deg)' }], { duration: 1000, delay: 150, easing: 'cubic-bezier(.5, 0, .2, 1)', fill: 'both' });
    const card = $('.party-card', party); card.animate([{ transform: 'scale(0.3) rotate(-8deg)', opacity: 0 }, { transform: 'scale(1.08) rotate(-2deg)', opacity: 1, offset: 0.6 }, { transform: 'scale(1) rotate(-2deg)', opacity: 1 }], { duration: 520, delay: 300, easing: 'cubic-bezier(.16, 1, .3, 1)', fill: 'both' });
    $('.party-bar u', party).animate([{ transform: 'scaleX(0)' }, { transform: 'scaleX(1)' }], { duration: 1100, delay: 700, easing: 'steps(12, end)', fill: 'both' });
    const stamp = $('.party-stamp', party); stamp.animate([{ transform: 'translate(-50%, -50%) scale(4) rotate(-18deg)', opacity: 0 }, { transform: 'translate(-50%, -50%) scale(1) rotate(-12deg)', opacity: 1, offset: 0.7 }, { transform: 'translate(-50%, -50%) scale(1.06) rotate(-12deg)', opacity: 1 }], { duration: 380, delay: 2300, easing: 'cubic-bezier(.5, 0, 1, .6)', fill: 'both' });
    // the card plays it straight, then turns round on you
    c.later(() => { const h = $('[data-k="h"]', party), sp = $('[data-k="s"]', party); if (!h) return; h.textContent = 'Just kidding.'; sp.innerHTML = 'There is no Premium. <em>Everything is free and open source,</em> and always was. You were trolled.'; card.classList.add('flip'); }, 1700);
    party.animate([{ opacity: 1 }, { opacity: 1, offset: 0.86 }, { opacity: 0 }], { duration: 5600, fill: 'forwards' }).onfinish = endParty;
    party.style.setProperty('pointer-events', 'auto'); party.addEventListener('pointerdown', endParty);
    return chips;
  };
  btn.addEventListener('click', () => {
    if (btn.getAttribute('aria-disabled') === 'true') return;
    c.dispose(); btn.setAttribute('aria-disabled', 'true'); out.replaceChildren(); times++; zeros();
    const lines = [[0, 'Contacting the billing department&hellip;'], [800, 'Card number: not needed.'], [1600, 'Total today: <b>$0.00</b>. Tax: $0.00. Tip: please don\'t.'], [2500, times > 1 ? '<b>You are Premium again.</b> Still nothing changed.' : '<b>You are Premium now.</b> You already were. Nothing changed, and that is the feature.']];
    lines.forEach(([ms, html]) => c.later(() => out.append(el('p', '', html)), ms));
    c.later(() => out.append(el('p', '', 'Want the real thing? <a href="https://github.com/bulhaa1" target="_blank" rel="noopener noreferrer">Follow BuffrX on GitHub</a>.')), 3300);
    c.later(() => { btn.removeAttribute('aria-disabled'); btn.textContent = 'Upgrade again \u00b7 $0'; }, 2700);
  });
})();

// ---- faq: the common questions, each one opens on press ----
(function faq() {
  const QA = [
    ['Is it really free? What is the catch?', '<p>Free, and there is no catch. It is open source (GPL), the source is published at release, and there is no account, no trial and no paid tier. <a href="#pricing">The Pricing page</a> says it with a straight face.</p>'],
    ['Why choose BuffrX when I already have my GPU brand\'s software?', '<p>Keep it, if it already does everything you want. BuffrX is for what it does not do.</p><p>A brand\'s overlay only works on that brand\'s cards. <b>BuffrX works on NVIDIA, AMD and Intel, and falls back to your CPU</b>, so your clips and your settings follow you when you change cards. It also keeps everything in one place: a library filed by game, an <b>editor built in</b>, and <b>Insta Share</b> to send a clip to a friend without uploading it first. <a href="#compare">See the comparison.</a></p>'],
    ['Will it cost me frames in my game?', '<p>It is built to stay out of the way: one process, your GPU\'s hardware encoder, a buffer that does no GPU work when it is off, and a viewer that never takes the game\'s focus.</p><p>We do not publish a frame-rate number until it has been measured properly on real hardware, so you will not find one here yet.</p>'],
    ['Does it work with my game?', '<p>Yes, including games with anti-cheat. BuffrX records your screen or a game window, and it finds a fullscreen game by itself and switches to it.</p>'],
    ['Which GPUs and codecs does it support?', '<p>NVIDIA, AMD and Intel, with <b>H.264, HEVC and AV1</b> where your card has them. No AV1 hardware? It records H.264 and tells you. With no hardware encoder at all it uses your CPU.</p>'],
    ['Is it Windows only? What about Linux and macOS?', '<p><b>Windows only at launch.</b> Linux is on the <a href="#roadmap">roadmap</a>, after the launch. The interface is built so that only the window host has to change. macOS is not on the roadmap.</p>'],
    ['Does it collect my data?', '<p>No. There are no accounts, no telemetry and no server of ours. It goes online only when you press a button: the update check, a post to Discord, or an Insta Share link. <a href="#privacy">See how.</a></p>'],
    ['How does Insta Share work? Does my friend need BuffrX?', '<p>Yes, your friend needs BuffrX (it is free). You copy a link from a clip, and the clip goes straight from your PC to theirs: up to <b>4 GB</b>, in full quality, with nothing uploaded first. The link lasts 1 or 24 hours, <b>Stop ends it at once</b>, and it works while your PC is on. If you would rather, post to Discord, fitted to its size limit.</p>'],
    ['Where do my clips go, and how much memory does the buffer use?', '<p>Clips are ordinary video files in a folder you choose, filed by game. The buffer keeps the last minute in memory (up to 512 MB by default, and never more than an eighth of your RAM). Your buffer length is a promise: if it would not fit, the quality gives way, not the length.</p>'],
    ['Can I stream with it?', '<p>No. BuffrX records, edits and shares clips; it is not a streamer. If you stream, use OBS for that, and BuffrX for your clips.</p>'],
    ['When does it come out?', '<p>November 2026. Follow the project on GitHub and you will see it the day it lands.</p>'],
  ];
  const host = $('#qa');
  QA.forEach(([q, a], i) => {
    const row = el('div', 'q' + (i === 0 ? ' open' : ''), `<h3><button type="button" aria-expanded="${i === 0}" aria-controls="qa-${i}"><span>${q}</span><span class="pm" aria-hidden="true"></span></button></h3><div class="a" id="qa-${i}" role="region"><div>${a}</div></div>`);
    const b = $('button', row);
    b.addEventListener('click', () => { const open = !row.classList.contains('open'); row.classList.toggle('open', open); b.setAttribute('aria-expanded', String(open)); });
    host.append(row);
  });
})();

// ---- compare: BuffrX against one other app at a time ----
const F = 'full', H = 'half', N = 'none';
const ROWS = [['Any GPU', 'NVIDIA, AMD or Intel in one app'], ['Stand-alone app', 'Not part of a driver or launcher suite'], ['Instant replay', 'Saves the moment after it happens'], ['Editor built in', 'Trim and crop without another app'], ['Direct sharing', 'A clip goes straight to a friend, no upload first'], ['No tracking', 'No telemetry, no account'], ['Works offline', 'Record, edit and keep clips with no internet'], ['Encoder control', 'Codec, speed, rate, look-ahead'], ['Price', 'Free or paid. Open source is marked'], ['Customizability', 'Themes, keys, settings, plugins']];
const US = [[F, 'NVENC, AMF, QSV and CPU'], [F, 'Its own small app'], [F, 'One key, the last minute'], [F, 'Trim, split, crop, export'], [F, 'Insta Share, up to 4 GB'], [F, 'None, and no account'], [F, 'Fully offline'], [F, 'Every codec, every knob'], [F, 'Free · Open source'], [F, 'Every setting, 14 themes, your keys']];
// In the order shown on the page: the driver apps first, Revidi last. Each cell is from the product's own pages or a published review (Oct 2026).
const THEM = [
  { id: 'nvidia', name: 'NVIDIA App', desc: 'Driver overlay', better: 'if your GPU is NVIDIA and you never want to touch a setting.',
    cells: [[N, 'GeForce only'], [N, 'Part of the NVIDIA App'], [F, 'Instant Replay'], [N, 'Gallery to view, no editor'], [N, 'Files or uploads'], [H, 'Optional usage and crash data'], [F, 'Capture works offline'], [H, 'Quality presets'], [F, 'Free with the GPU'], [H, 'Overlay and presets']] },
  { id: 'amd', name: 'AMD Adrenalin', desc: 'Driver overlay', better: 'if your GPU is AMD and you never want to touch a setting.',
    cells: [[N, 'Radeon only'], [N, 'Part of AMD Software'], [F, 'Instant Replay'], [H, 'Basic trim'], [N, 'Files or uploads'], [H, 'Opt-out program'], [F, 'Capture works offline'], [H, 'Quality presets'], [F, 'Free with the GPU'], [H, 'Overlay and presets']] },
  { id: 'xbox', name: 'Xbox Game Bar', desc: 'Built into Windows', better: 'if you only want a quick capture and never want to install anything.',
    cells: [[F, 'Any GPU'], [N, 'Built into Windows'], [F, 'Record the last 30 seconds'], [N, 'Opens another app'], [N, 'Files or uploads'], [N, 'Windows diagnostic data'], [F, 'Works offline'], [H, '30 or 60 fps, standard or high'], [F, 'Free, part of Windows'], [N, 'Few settings']] },
  { id: 'obs', name: 'OBS Studio', desc: 'Recorder and streamer', trade: [['Setup difficulty', 'Scenes, sources and encoder are set up by hand'], ['Editing', 'None built in'], ['Sharing', 'None built in']], better: 'if you stream, or want scenes, overlays and plugins. BuffrX does not stream.',
    cells: [[F, 'All encoders'], [F, 'Its own app'], [F, 'Replay Buffer'], [N, 'None built in'], [N, 'None built in'], [H, 'An ID with the update check'], [F, 'Works offline'], [F, 'Everything, by hand'], [F, 'Free · Open source'], [F, 'Everything, plus plugins']] },
  { id: 'medal', name: 'Medal.tv', desc: 'Cloud platform', better: 'if you want a social feed and cloud storage for your clips.',
    cells: [[F, 'Any GPU'], [H, 'Its own app, with extras'], [F, 'Instant clip'], [F, 'Clip editor'], [N, 'Upload is the share path'], [N, 'Personal info and app activity'], [H, 'Local clips, cloud sign-in'], [H, 'Quality presets'], [H, 'Free, Premium available'], [H, 'Some settings']] },
  { id: 'outplayed', name: 'Outplayed', desc: 'An Overwolf app', better: 'if you want highlights found from game events in the games it supports.',
    cells: [[F, 'Any GPU'], [N, 'Needs Overwolf'], [F, 'Instant replay'], [H, 'Editor, watermark without Premium'], [N, 'Uploads to Outplayed.tv'], [N, 'Ad-supported'], [H, 'Local clips, online platform'], [H, 'Quality presets'], [H, 'Free, Premium available'], [H, 'Some settings']] },
  { id: 'steelseries', name: 'SteelSeries Moments', desc: 'Part of GG', better: 'if you already use SteelSeries GG and want clips next to its other tools.',
    cells: [[F, 'Any GPU'], [N, 'Part of SteelSeries GG'], [F, 'Instant replay'], [F, 'Crop, effects, text'], [N, 'Files or uploads'], [H, 'Optional account'], [F, 'Works offline'], [H, 'Quality presets'], [F, 'Free'], [H, 'Some settings']] },
  { id: 'revidi', name: 'Revidi', desc: 'Cloud based', trade: [['Data collection', 'Extensive'], ['Network and disk', 'Keeps using both, even when idle']], better: 'if a link made on save, with an upload, is how you like to send clips.',
    cells: [[F, 'OBS encoders'], [F, 'Its own app'], [F, 'One hotkey, last 60 s'], [H, 'Trim and re-upload'], [N, 'Cloud based'], [N, 'Data collection'], [H, 'Local saves, cloud links'], [H, 'Encoder, fps, bitrate'], [H, 'Free, Premium $4.99/mo'], [H, 'Some settings']] },
];
const MARKS = { full: 'Has it', half: 'Partly', none: 'Does not' };
const SHAPE = {
  full: '<rect x="1.5" y="1.5" width="17" height="17"/><path d="M6 10.5l2.8 2.8L14 7.5"/>',
  half: '<rect x="1.5" y="1.5" width="17" height="17"/><path class="h" d="M1.5 1.5H10v17H1.5Z"/>',
  none: '<rect x="1.5" y="1.5" width="17" height="17"/><path d="M6.5 10h7"/>',
};
const score = (cells) => cells.reduce((n, [m]) => n + (m === F ? 1 : m === H ? 0.5 : 0), 0);
function renderCompare(id) {
  const t = THEM.find((x) => x.id === id) || THEM[0];
  $$('#chips button').forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.id === t.id)));
  const cell = ([m, txt], cls, who) => `<td class="${cls}" data-l="${who}"><div class="mark"><svg class="mk ${m}" viewBox="0 0 20 20" aria-hidden="true">${SHAPE[m]}</svg><span><b class="sr">${MARKS[m]}: </b>${txt}</span></div></td>`;
  $('#cmp').innerHTML = `<thead><tr><th>Feature</th><th class="us">BuffrX<small>Free, GPL</small></th><th>${t.name}<small>${t.desc}</small></th></tr></thead><tbody>` +
    ROWS.map(([a, b], r) => `<tr><th scope="row">${a}<small>${b}</small></th>${cell(US[r], 'us', 'BuffrX')}${cell(t.cells[r], '', t.name)}</tr>`).join('') + `</tbody><tfoot><tr><th scope="row">Score<small>A full mark is one, a partial mark is half</small></th><td class="us" data-l="BuffrX">${score(US)} / ${ROWS.length}</td><td data-l="${t.name}">${score(t.cells)} / ${ROWS.length}</td></tr></tfoot>`;
  $$('#cmp tbody tr').forEach((tr, r) => $$('.mk', tr).forEach((m, c) => { m.style.setProperty('--r', r); m.style.setProperty('--c', c); }));
  const list = (m) => t.cells.map(([x, txt], i) => [x, txt, i]).filter(([x]) => x === m).slice(0, 4).map(([, txt, i]) => `<li><b>${ROWS[i][0]}</b><span>${txt}</span></li>`).join('') || '<li><span>None on this list.</span></li>';
  raceSet(t);
  $('#verdict').innerHTML = `<div><h3>Best for</h3><p>Pick <b>${t.name}</b> instead ${t.better}</p></div><div><h3>Strengths</h3><ul>${list(F)}</ul></div><div><h3>Trade-offs</h3><ul>${t.trade ? t.trade.map(([a, b]) => `<li><b>${a}</b><span>${b}</span></li>`).join('') : list(N)}</ul></div>`;
}
// The head-to-head plays as a race: the rows are taken one by one, each mark lands in a bar of cells, and the two scores tally up.
const raceC = ctx(); let raceT = null, raceSeen = false;
function raceSet(t) {
  raceT = t; raceC.dispose();
  const host = $('#race'); host.innerHTML = `<div><span>BuffrX</span><div class="cells" data-k="u"></div><b data-k="us">0</b></div><div><span>${t.name}</span><div class="cells" data-k="t"></div><b data-k="th">0</b></div>`;
  cellRow(k(host, 'u'), ROWS.length); cellRow(k(host, 't'), ROWS.length);
  $$('#cmp tbody tr').forEach((tr) => tr.classList.toggle('pend', !reduce && !raceSeen && !isPhone.matches));
  if (reduce || raceSeen) raceRun();
}
function raceRun() {
  const host = $('#race'), rows = $$('#cmp tbody tr'), U = [...k(host, 'u').children], T = [...k(host, 't').children], t = raceT;
  const cls = (m) => (m === F ? 'l3' : m === H ? 'half' : ''), val = (m) => (m === F ? 1 : m === H ? 0.5 : 0);
  let a = 0, b = 0;
  const take = (i) => {
    a += val(US[i][0]); b += val(t.cells[i][0]); U[i].className = cls(US[i][0]); T[i].className = cls(t.cells[i][0]);
    k(host, 'us').textContent = a; k(host, 'th').textContent = b;
    rows[i].classList.remove('pend'); rows[i].classList.add('flash'); if (val(US[i][0]) > val(t.cells[i][0])) rows[i].classList.add('beat');
  };
  rows.forEach((tr) => tr.classList.remove('flash', 'beat'));
  if (reduce || isPhone.matches) { ROWS.forEach((_, i) => take(i)); rows.forEach((tr) => tr.classList.remove('flash', 'pend')); return; }
  rows.forEach((tr) => tr.classList.add('pend'));
  ROWS.forEach((_, i) => raceC.later(() => take(i), 250 + i * 230));
  raceC.later(() => { k(host, 'us').classList.add('stamp'); }, 250 + ROWS.length * 230);
}
new IntersectionObserver((es, o) => { if (es[0].isIntersecting) { raceSeen = true; o.disconnect(); raceRun(); } }, { threshold: 0.35 }).observe($('#cmp'));
THEM.forEach((t) => { const b = el('button', '', t.name); b.type = 'button'; b.dataset.id = t.id; b.setAttribute('aria-pressed', 'false'); b.addEventListener('click', () => { renderCompare(t.id); raceSeen = true; raceRun(); }); $('#chips').append(b); });
renderCompare(THEM[0].id);

// ---- start: place the bar's ink for wherever the page loaded ----
spy();

// the roadmap's light runs the width of the line, so it needs to know how wide the line is
(function roadWidth() { const road = $('.road'); if (road) { reveal.observe(road); if (isPhone.matches) road.classList.add('in'); } if (road) new ResizeObserver(() => road.style.setProperty('--roadw', `${road.clientWidth}px`)).observe(road); })();

// finer parts come in one by one as they are reached: the rows of a list, the stops of the roadmap, each question, each plan
(function parts() {
  $$('.nevers li, .priv-h, .stage li, #qa .q, .plan, .reqs > div, .chips, .verdict, .lib-f, .more-tabs, .site-foot').forEach((e) => {
    if (e.closest('[data-r]') && !e.matches('.stage li, .nevers li, #qa .q, .reqs > div')) return; // a part inside a revealed block rides in with it
    if (e.dataset.r) return;
    e.dataset.r = 'up'; reveal.observe(e); if (isPhone.matches) e.classList.add('in');
  });
})();
