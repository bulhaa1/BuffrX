// BuffrX Demo: the interactive demos. Each one is a function (root, timers) that builds one small working thing, set up the way the
// app's own control works. site.js registers them and owns the helpers they use ($, $$, el, k, mmss, toast, BXGames, reduce, isPhone). Nothing here
// makes a request: the one video is a file of this site, and nothing is stored.
const PG = {};

const CLIP = 'games/rivals-clip.mp4', POSTER = 'games/rivals.jpg';
const wait = (target, type, ms = 3500) => new Promise((done) => { const t = setTimeout(done, ms); target.addEventListener(type, () => { clearTimeout(t); done(); }, { once: true }); });
// a seeked video has not always painted its new frame yet: wait for the frame, or a short moment
const painted = (v) => new Promise((done) => { const t = setTimeout(done, 350); if (v.requestVideoFrameCallback) v.requestVideoFrameCallback(() => { clearTimeout(t); done(); }); else setTimeout(() => { clearTimeout(t); done(); }, 90); });
const clamp = (v, lo = 0, hi = 1) => Math.max(lo, Math.min(hi, v));
const loud = (n, seed = 1) => Array.from({ length: n }, (_, i) => { const x = i / n; return clamp(0.16 + 0.5 * Math.abs(Math.sin(x * 21 + seed) * Math.cos(x * 6.3 + seed * 2)) + (i % 31 < 3 ? 0.28 : 0), 0.08, 1); });

// ---- the frames of the one clip: taken once, shared by the Timeline's hover picture and the library ----
const FILM = {
  n: 16, w: 128, h: 72, p: null, cv: null,
  load() {
    if (this.p) return this.p;
    this.p = (async () => {
      if (isPhone.matches || reduce) return null;
      const v = document.createElement('video'); v.muted = true; v.preload = 'auto'; v.src = CLIP;
      await wait(v, 'loadeddata', 8000); if (v.readyState < 2) return null;
      const cv = document.createElement('canvas'); cv.width = this.w * this.n; cv.height = this.h;
      const g = cv.getContext('2d'), d = v.duration || 8, got = new Set();
      if (v.requestVideoFrameCallback) {
        // played through once, fast and muted, taking a frame for each slot as it goes by: a decoder that is running always has the frame
        await new Promise((done) => {
          const stop = setTimeout(done, 9000);
          const tick = (now, meta) => {
            const i = clamp(Math.floor((meta.mediaTime / d) * this.n), 0, this.n - 1);
            if (!got.has(i)) { got.add(i); g.drawImage(v, i * this.w, 0, this.w, this.h); }
            if (got.size >= this.n || v.ended) { clearTimeout(stop); done(); } else v.requestVideoFrameCallback(tick);
          };
          v.addEventListener('ended', () => { clearTimeout(stop); done(); }, { once: true });
          v.playbackRate = 4; v.play().then(() => v.requestVideoFrameCallback(tick)).catch(done);
        });
        v.pause();
        for (let i = 0; i < this.n; i++) if (!got.has(i)) { const near = [...got].sort((x, y) => Math.abs(x - i) - Math.abs(y - i))[0]; if (near !== undefined) g.drawImage(cv, near * this.w, 0, this.w, this.h, i * this.w, 0, this.w, this.h); }
      } else {
        for (let i = 0; i < this.n; i++) { v.currentTime = Math.min(d - 0.05, ((i + 0.5) / this.n) * d); await wait(v, 'seeked', 2500); await painted(v); g.drawImage(v, i * this.w, 0, this.w, this.h); }
      }
      this.cv = cv; return cv;
    })();
    return this.p;
  },
  // the frame at fraction f of the clip, drawn into a canvas; false until the frames are ready
  thumb(target, f) {
    if (!this.cv) return false;
    const i = clamp(Math.floor(f * this.n), 0, this.n - 1);
    target.getContext('2d').drawImage(this.cv, i * this.w, 0, this.w, this.h, 0, 0, target.width, target.height); return true;
  },
};

// ---- Timeline: the real one's layout, always open: the session on a bar you look back along, markers, a clip you pick, and the picture
// on a glass card. The one gameplay clip is the whole session. ----
PG.timeline = (s, c) => {
  const MENU = [['Save', [['stay', 'Save clip', 'Enter'], ['edit', 'Save and edit', 'E']]], ['Share', [['copy', 'Save and copy', 'Ctrl+C'], ['share', 'Save and share', 'Shift+S']]]];
  const ICON = { play: 'M8 5v14l11-7L8 5Z', pause: 'M7 5h4v14H7zM13 5h4v14h-4z', scissors: 'M6 9a3 3 0 1 0 0-6 3 3 0 0 0 0 6Zm0 12a3 3 0 1 0 0-6 3 3 0 0 0 0 6ZM8.2 7.8 20 18M8.2 16.2 20 6', flag: 'M5 21V4h12l-2 4 2 4H5' };
  const svg = (d, fill) => `<svg viewBox="0 0 24 24" aria-hidden="true"${fill ? ' class="fill"' : ''}><path d="${d}"/></svg>`;
  s.innerHTML = `<div class="tlx" data-k="root" data-state="timeline">
    <div class="tlx-ov" data-k="ov">
      <div class="tlx-bd" aria-hidden="true"></div>
      <div class="tlx-panel">
        <div class="tlx-top"><strong>Timeline [beta]</strong><span class="tlx-state"><i></i>Writing</span><span class="tlx-span" data-k="span"></span><span class="tlx-grow"></span><time data-k="clock"></time>
          <button type="button" data-k="vc" aria-pressed="false">View clips</button><button type="button" data-k="hv" aria-pressed="false">Hide video</button></div>
        <div class="tlx-range"><div class="tlx-ruler" data-k="ruler"></div><div class="tlx-mt" data-k="mt"></div>
          <div class="tlx-bar" data-k="bar" tabindex="0" role="slider" aria-label="Playhead. Left and right move it, M adds a marker, C starts a clip, Enter saves it." aria-valuemin="0" aria-valuemax="100" aria-valuenow="100">
            <i class="tlx-cells" aria-hidden="true"></i><canvas class="tlx-loud" data-k="loud" width="1000" height="30" aria-hidden="true"></canvas><i class="tlx-fill" data-k="fill"></i>
            <div class="tlx-cut" data-k="cut" hidden><b class="tlx-edge l" data-e="a" title="Drag the start"></b><b class="tlx-edge r" data-e="b" title="Drag the end"></b></div>
            <i class="tlx-head" data-k="head"></i>
            <div class="tlx-bubble" data-k="bubble" hidden><canvas width="112" height="63" data-k="bimg"></canvas><span data-k="btime"></span></div></div></div>
        <div class="tlx-ctl">
          <div class="tlx-l"><button type="button" data-k="mark">${svg('M12 21s-6-5.6-6-10.5a6 6 0 0 1 12 0C18 15.4 12 21 12 21ZM12 8v5M9.5 10.5h5')}Marker</button><button type="button" data-k="rmark" hidden>Remove marker</button><button type="button" data-k="mute" aria-pressed="true" title="Sound">Muted</button><button type="button" data-k="speed" title="Playback speed">1&times;</button><span class="tlx-t" data-k="time"></span></div>
          <div class="tlx-tr"><button type="button" data-k="b1">&minus;1 s</button><button type="button" class="tlx-play" data-k="play" aria-label="Play"></button><button type="button" data-k="f1">+1 s</button></div>
          <div class="tlx-r"><button type="button" data-k="live" aria-pressed="false">Live</button>
            <button type="button" class="primary" data-k="clip">${svg(ICON.scissors)}Clip</button>
            <div class="tlx-clipbar" data-k="clipbar" hidden><button type="button" data-k="pclip" aria-pressed="false">Play clip</button><button type="button" data-k="cancel">Cancel</button>
              <div class="tlx-save"><button type="button" class="primary" data-k="save">Save clip</button><button type="button" class="primary" data-k="smenu" aria-label="More ways to save" aria-haspopup="menu" aria-expanded="false">${svg('M6 9l6 6 6-6')}</button>
                <div class="tlx-menu" data-k="menu" role="menu" hidden>${MENU.map(([h, items]) => `<span class="tlx-mh">${h}</span>${items.map(([id, name, key]) => `<button type="button" role="menuitem" data-then="${id}">${name}<kbd>${key}</kbd></button>`).join('')}`).join('')}</div></div></div></div>
        </div>
      </div>
      <div class="tlx-stage"><div class="tlx-card" data-k="card"><video class="tlx-v" data-k="vid" src="${CLIP}" poster="${POSTER}" muted playsinline preload="none"></video><button class="tlx-sp" data-k="sp" type="button" aria-label="Play"></button><span class="tlx-age" data-k="age"></span></div>
        <div class="tlx-lib" data-k="lib" hidden><div class="tlx-grid" data-k="grid"></div><p class="tlx-empty" data-k="empty">No clips yet. Pick a part of the bar and save it.</p></div></div>
    </div></div>`;
  const q = (key) => k(s, key), root = q('root'), vid = q('vid'), bar = q('bar'), ov = q('ov');
  let dur = 8, at = 1, cut = null, marks = [], selMark = -1, saved = [], isOpen = true, visible = false, loaded = false, failed = false, rate = 1, loop = false, drag = null;
  const ago = (f) => (f >= 0.995 ? 'now' : `-${mmss((1 - f) * dur)}`);
  const setIcon = () => { q('play').innerHTML = svg(vid.paused ? ICON.play : ICON.pause, true); q('sp').innerHTML = svg(ICON.play, true); q('sp').hidden = !vid.paused ? true : false; q('play').setAttribute('aria-label', vid.paused ? 'Play' : 'Pause'); q('pclip').setAttribute('aria-pressed', String(loop && !vid.paused)); };
  // a seek asked for while another is still under way is kept and made when that one lands, so the last place asked for wins
  let want = null;
  const seek = () => { if (!loaded || failed) return; want = Math.min(dur - 0.04, at * dur); if (!vid.seeking) { vid.currentTime = want; want = null; } };
  vid.addEventListener('seeked', () => { if (want !== null) { vid.currentTime = want; want = null; } });
  const paint = () => {
    q('head').style.setProperty('--x', `${(at * 100).toFixed(2)}%`); q('fill').style.setProperty('--x', `${(at * 100).toFixed(2)}%`);
    bar.setAttribute('aria-valuenow', Math.round(at * 100)); q('time').textContent = `${mmss(at * dur)} / ${mmss(dur)}`; q('age').textContent = at >= 0.995 ? 'live' : `${mmss((1 - at) * dur)} ago`;
    q('cut').hidden = !cut; q('clipbar').hidden = !cut; q('clip').hidden = !!cut;
    if (cut) { q('cut').style.setProperty('--a', `${(cut.a * 100).toFixed(2)}%`); q('cut').style.setProperty('--b', `${(cut.b * 100).toFixed(2)}%`); }
    q('mt').replaceChildren(...marks.map((m, i) => { const x = el('button', 'tlx-mk' + (i === selMark ? ' on' : ''), svg(ICON.flag, true)); x.type = 'button'; x.style.setProperty('--x', `${(m * 100).toFixed(2)}%`); x.setAttribute('aria-label', `Marker at ${mmss(m * dur)}`); x.addEventListener('click', () => { selMark = i; at = m; if (!vid.paused) pause(); seek(); paint(); }); return x; }));
    q('rmark').hidden = selMark < 0; q('vc').textContent = saved.length ? `View clips (${saved.length})` : 'View clips'; q('live').setAttribute('aria-pressed', String(at >= 0.995));
  };
  const rulerDraw = () => {
    q('ruler').replaceChildren(...Array.from({ length: 5 }, (_, i) => { const f = i / 4, x = el('span', 'tlx-rl' + (i === 4 ? ' end' : '') + (i === 4 ? ' now' : ''), ago(f)); x.style.setProperty('--x', `${f * 100}%`); return x; }));
    q('span').textContent = `Session ${mmss(dur)}`;
  };
  const ensure = () => { if (loaded || failed) return; vid.preload = 'auto'; vid.load(); wait(vid, 'loadedmetadata', 8000).then(() => { if (vid.readyState >= 1) { dur = vid.duration || dur; loaded = true; rulerDraw(); paint(); seek(); } }); FILM.load(); };
  vid.addEventListener('error', () => { failed = true; });
  const play = async () => {
    if (failed) return; ensure();
    if (cut && (at < cut.a - 0.001 || at > cut.b - 0.01)) at = cut.a; else if (at >= 0.99) at = 0;
    vid.playbackRate = rate; seek();
    for (let i = 0; i < 3 && (vid.seeking || want !== null); i++) await wait(vid, 'seeked', 800); // start from where the playhead is, not from where the video was
    vid.play().catch(() => {}); setIcon();
  };
  const pause = () => { vid.pause(); loop = false; setIcon(); };
  const toggle = () => (vid.paused ? play() : pause());
  c.every(50, () => {
    if (!isOpen || !loaded || vid.paused) return;
    at = clamp(vid.currentTime / dur);
    if (cut && at >= cut.b) { if (loop) { at = cut.a; vid.currentTime = cut.a * dur; } else pause(); } else if (at >= 0.995) pause();
    paint();
  });
  vid.addEventListener('ended', () => { pause(); at = 1; paint(); });
  // Timeline is always open here: its panel drops in when the part first comes into view
  let arrived = false;
  const arrive = () => { if (arrived) return; arrived = true; ov.classList.add('arriving'); };
  // the bar: a click puts the playhead there, a drag moves it, the edges of a clip move the clip
  const frac = (e) => { const r = bar.getBoundingClientRect(); return clamp((e.clientX - r.left) / r.width); };
  const bubble = (f) => {
    const b = q('bubble'), r = bar.getBoundingClientRect(); b.hidden = false; b.style.setProperty('--x', `${clamp(f * r.width, 60, r.width - 60)}px`);
    q('btime').textContent = ago(f); const cv = q('bimg'); if (!FILM.thumb(cv, f)) { const g = cv.getContext('2d'); g.fillStyle = '#0b0c0d'; g.fillRect(0, 0, cv.width, cv.height); }
  };
  bar.addEventListener('pointermove', (e) => {
    const f = frac(e); bubble(f);
    if (drag === 'head') { at = f; seek(); paint(); } else if (drag === 'a' || drag === 'b') { cut[drag] = drag === 'a' ? clamp(f, 0, cut.b - 0.04) : clamp(f, cut.a + 0.04, 1); paint(); }
  });
  bar.addEventListener('pointerleave', () => { q('bubble').hidden = true; });
  bar.addEventListener('pointerdown', (e) => {
    try { bar.setPointerCapture(e.pointerId); } catch { /* a pointer that is already gone */ }
    bar.focus({ preventScroll: true }); ensure(); const edge = e.target.closest('.tlx-edge');
    if (edge) { drag = edge.dataset.e; return; }
    drag = 'head'; selMark = -1; at = frac(e); if (!vid.paused && !loop) pause(); seek(); paint();
  });
  const up = () => { drag = null; };
  bar.addEventListener('pointerup', up); bar.addEventListener('pointercancel', up);
  const step = (d) => { pause(); at = clamp(at + d / dur); seek(); paint(); };
  q('b1').addEventListener('click', () => step(-1)); q('f1').addEventListener('click', () => step(1));
  q('play').addEventListener('click', toggle); q('sp').addEventListener('click', toggle); vid.addEventListener('click', toggle);
  q('live').addEventListener('click', () => { pause(); at = 1; seek(); paint(); });
  q('speed').addEventListener('click', () => { rate = rate === 1 ? 2 : rate === 2 ? 0.5 : 1; vid.playbackRate = rate; q('speed').innerHTML = rate === 0.5 ? '0.5&times;' : `${rate}&times;`; });
  q('mute').addEventListener('click', () => { vid.muted = !vid.muted; q('mute').setAttribute('aria-pressed', String(vid.muted)); q('mute').textContent = vid.muted ? 'Muted' : 'Sound on'; });
  q('mark').addEventListener('click', () => { if (!marks.some((m) => Math.abs(m - at) < 0.01)) marks.push(at); selMark = marks.findIndex((m) => Math.abs(m - at) < 0.01); toast(`<b>Marker</b> at ${mmss(at * dur)}`); paint(); });
  q('rmark').addEventListener('click', () => { if (selMark >= 0) { marks.splice(selMark, 1); selMark = -1; paint(); } });
  q('hv').addEventListener('click', () => { const on = root.classList.toggle('novideo'); q('hv').setAttribute('aria-pressed', String(on)); });
  q('vc').addEventListener('click', () => { const on = q('lib').hidden; q('lib').hidden = !on; q('card').hidden = on; q('vc').setAttribute('aria-pressed', String(on)); if (on) libDraw(); });
  // a clip: the seconds before the playhead, with handles to pull, played on repeat, saved
  const startClip = () => { if (cut) return; const len = Math.min(3 / dur, Math.max(0.2, at)); cut = { a: Math.max(0, at - len), b: Math.max(len, at) }; paint(); };
  q('clip').addEventListener('click', startClip);
  q('pclip').addEventListener('click', () => { if (loop && !vid.paused) { pause(); return; } loop = true; at = cut.a; play(); loop = true; setIcon(); });
  q('cancel').addEventListener('click', () => { cut = null; loop = false; q('menu').hidden = true; paint(); });
  const libDraw = () => {
    q('empty').hidden = saved.length > 0;
    q('grid').replaceChildren(...saved.map((x, i) => { const card = el('div', 'tlx-clip', `<canvas width="160" height="90"></canvas><b>${x.len.toFixed(1)} s</b><small>Clip ${i + 1}</small>`); if (!FILM.thumb($('canvas', card), x.a)) { const g = $('canvas', card).getContext('2d'); g.fillStyle = '#16181a'; g.fillRect(0, 0, 160, 90); } return card; }));
  };
  const saveClip = (then) => {
    if (!cut) return; const len = (cut.b - cut.a) * dur; saved.unshift({ a: cut.a, b: cut.b, len }); pause(); cut = null; q('menu').hidden = true; paint();
    const msg = { stay: 'saved', edit: 'saved. Opening the editor', copy: 'saved and copied', share: 'saved. Opening Quick Share' }[then];
    toast(`<b>Clip ${msg}</b> &middot; ${len.toFixed(1)} s`);
    if (then === 'edit') setTimeout(() => { location.hash = 'edit'; }, 700); else if (then === 'share') setTimeout(() => { location.hash = 'link'; }, 700);
    if (!q('lib').hidden) libDraw();
  };
  q('save').addEventListener('click', () => saveClip('stay'));
  q('smenu').addEventListener('click', () => { const on = q('menu').hidden; q('menu').hidden = !on; q('smenu').setAttribute('aria-expanded', String(on)); });
  q('menu').addEventListener('click', (e) => { const b = e.target.closest('[data-then]'); if (b) saveClip(b.dataset.then); });
  // the keys the real page has
  const onkey = (e) => {
    if (!visible || e.metaKey || /^(INPUT|SELECT|TEXTAREA)$/.test(e.target.tagName)) return;
    if (!isOpen) return;
    const key = e.key.toLowerCase();
    if (e.code === 'Space') { e.preventDefault(); toggle(); }
    else if (key === 'escape') { e.preventDefault(); if (!q('menu').hidden) q('menu').hidden = true; else if (cut) { cut = null; loop = false; paint(); } }
    else if (key === 'arrowleft') { e.preventDefault(); step(e.shiftKey ? -2 : -0.5); }
    else if (key === 'arrowright') { e.preventDefault(); step(e.shiftKey ? 2 : 0.5); }
    else if (key === 'm' && !e.ctrlKey) { e.preventDefault(); q('mark').click(); }
    else if (key === 'c' && !e.ctrlKey) { e.preventDefault(); startClip(); }
    else if (key === 'l') { e.preventDefault(); q('live').click(); }
    else if (key === 'h') { e.preventDefault(); q('hv').click(); }
    else if (e.key === 'Delete' && selMark >= 0) { e.preventDefault(); q('rmark').click(); }
    else if (e.key === 'Enter' && cut) { e.preventDefault(); saveClip('stay'); }
    else if (key === 'e' && cut) { e.preventDefault(); saveClip('edit'); }
  };
  addEventListener('keydown', onkey, true);
  c.every(1000, () => { const d = new Date(); q('clock').textContent = d.toLocaleTimeString('en-GB'); });
  new IntersectionObserver((es) => { visible = es[0].isIntersecting; if (visible) { ensure(); arrive(); } else pause(); }, { threshold: 0.2 }).observe(s);
  // the sound the bar draws: quiet most of the time, a few loud regions in the accent
  const drawLoud = () => {
    const cv = q('loud'), g = cv.getContext('2d'), n = 160, w = cv.width / n, h = cv.height, v = loud(n, 3), css = getComputedStyle(root);
    g.clearRect(0, 0, cv.width, h);
    v.forEach((x, i) => { g.fillStyle = x > 0.62 ? css.getPropertyValue('--acc') || '#ff6b2c' : 'rgba(230,226,218,0.38)'; const bh = x * (h - 6); g.fillRect(i * w + 1, (h - bh) / 2, w - 2, bh); });
  };
  drawLoud(); new MutationObserver(drawLoud).observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
  rulerDraw(); paint(); setIcon();
};

// ---- Library: clips filed by game. Pick one, then edit, share, copy, rename or delete it ----
PG.library = (s) => {
  const DATA = [['Team wipe', 'rivals', 12], ['Inferno rush', 'cs2', 35], ['Barrel peek', 'cs2', 42, 1], ['Ace', 'valorant', 60], ['Cashout run', 'finals', 20], ['Harbour dive', 'acbf', 42], ['Souk chase', 'acm', 55], ['Low pass', 'msfs', 30], ['Touge run', 'forza', 25]];
  let clips = DATA.map(([name, game, secs, v], i) => ({ id: i, name, game, secs, v: v || 0, mb: Math.max(2, Math.round(secs * 0.68)) }));
  s.innerHTML = `<div class="lb-head"><b data-k="count"></b><div class="lb-chips" data-k="chips" role="group" aria-label="Game"></div></div>
    <div class="lb-grid" data-k="grid" role="list"></div>
    <div class="lb-bar" data-k="bar"><span class="lb-sel" data-k="sel">Pick a clip</span><span class="lb-acts"><button class="btn small" data-a="edit" type="button" disabled>Edit</button><button class="btn small" data-a="share" type="button" disabled>Share</button><button class="btn small" data-a="copy" type="button" disabled>Copy</button><button class="btn small" data-a="rename" type="button" disabled>Rename</button><button class="btn small" data-a="delete" type="button" disabled>Delete</button></span></div>`;
  let game = '', picked = -1;
  const find = () => clips.find((x) => x.id === picked);
  const paintBar = () => { const c = find(); k(s, 'sel').innerHTML = c ? `<b>${c.name}</b> &middot; ${BXGames.name(c.game)} &middot; ${mmss(c.secs)} &middot; ${c.mb} MB` : 'Pick a clip'; $$('[data-a]', s).forEach((b) => { b.disabled = !c; }); $$('.lb-card', s).forEach((x) => x.classList.toggle('on', Number(x.dataset.id) === picked)); };
  const draw = () => {
    const list = clips.filter((c) => !game || c.game === game);
    k(s, 'count').textContent = `Clips ${clips.length}`;
    k(s, 'grid').replaceChildren(...list.map((c) => { const card = el('button', 'lb-card', `<span class="lb-pic">${BXGames.shot(c.game, '', c.v)}<i>${mmss(c.secs)}</i></span><b class="lb-name">${c.name}</b><small>${BXGames.name(c.game)} &middot; ${c.mb} MB</small>`); card.type = 'button'; card.dataset.id = c.id; card.setAttribute('role', 'listitem'); card.addEventListener('click', () => { picked = c.id; paintBar(); }); card.addEventListener('dblclick', () => { picked = c.id; paintBar(); go('edit'); }); return card; }));
    paintBar();
  };
  const go = (id) => { toast(`<b>${id === 'edit' ? 'Opening the editor' : 'Opening Quick Share'}</b>`); setTimeout(() => { location.hash = id === 'edit' ? 'edit' : 'link'; }, 600); };
  const games = [...new Set(clips.map((c) => c.game))];
  [['', 'All'], ...games.map((g) => [g, BXGames.name(g)])].forEach(([v, label]) => { const b = el('button', '', label); b.type = 'button'; b.setAttribute('aria-pressed', String(v === '')); b.addEventListener('click', () => { game = v; $$('button', k(s, 'chips')).forEach((x) => x.setAttribute('aria-pressed', String(x === b))); draw(); }); k(s, 'chips').append(b); });
  s.addEventListener('click', (e) => {
    const b = e.target.closest('[data-a]'); const c = find(); if (!b || !c) return;
    const a = b.dataset.a;
    if (a === 'edit' || a === 'share') go(a);
    else if (a === 'copy') toast(`<b>${c.name}</b> copied. Paste it into a chat.`);
    else if (a === 'rename') {
      const nameEl = $(`.lb-card[data-id="${c.id}"] .lb-name`, s); const input = el('input', 'lb-rename'); input.value = c.name; input.setAttribute('aria-label', 'New name'); nameEl.replaceWith(input); input.focus(); input.select();
      const done = (ok) => { if (ok && input.value.trim()) { c.name = input.value.trim().slice(0, 28); toast(`<b>Renamed</b> to ${c.name}`); } draw(); };
      input.addEventListener('keydown', (ev) => { if (ev.key === 'Enter') done(true); else if (ev.key === 'Escape') done(false); ev.stopPropagation(); });
      input.addEventListener('blur', () => done(true)); input.addEventListener('click', (ev) => ev.stopPropagation());
    } else if (a === 'delete') {
      const card = $(`.lb-card[data-id="${c.id}"]`, s); const gone = c; const at = clips.indexOf(c);
      card.classList.add('out'); setTimeout(() => { clips = clips.filter((x) => x !== gone); picked = -1; draw(); toast(`<b>${gone.name}</b> deleted`); }, 220);
    }
  });
  draw();
};

// ---- Editor: cut a clip the way the app's does. Play it, split it into parts, drag the ends of a part to trim it, crop it, change the speed, save a copy ----
PG.edit = (s, c) => {
  const SHAPES = [['free', 'Free', 1], ['16:9', '16:9', 1], ['9:16', '9:16', 0.316], ['1:1', '1:1', 0.5625]], RATES = [0.5, 1, 2];
  const svg = (d) => `<svg viewBox="0 0 24 24" aria-hidden="true"><path d="${d}"/></svg>`;
  s.innerHTML = `<div class="edx">
    <div class="edx-top">
      <div class="edx-prev"><video class="edx-v" data-k="vid" src="${CLIP}" poster="${POSTER}" muted playsinline preload="none"></video><i class="edx-crop" data-k="crop"></i><button class="edx-pp" data-k="pp" type="button" aria-label="Play"></button></div>
      <div class="edx-side">
        <div class="edx-g"><span>Crop</span><div class="seg" role="group" aria-label="Crop shape" data-k="shape"></div></div>
        <div class="edx-g"><span>Speed</span><div class="seg" role="group" aria-label="Speed" data-k="rate"></div></div>
        <div class="edx-out"><b data-k="len"></b><small data-k="est"></small></div>
      </div>
    </div>
    <div class="edx-bar"><button class="btn small edx-play" data-k="play" type="button" aria-label="Play"></button><time data-k="time"></time><span class="edx-grow"></span>
      <button class="btn small" data-k="split" type="button"><kbd>S</kbd>Split</button><button class="btn small" data-k="undo" type="button" aria-label="Undo"><kbd>Ctrl Z</kbd>Undo</button></div>
    <div class="edx-lane" data-k="lane" tabindex="0" role="group" aria-label="The clip's timeline. Click to move the playhead, drag the ends of the picked part to trim it."><div class="edx-wv" data-k="wv"></div><div class="edx-layer" data-k="layer"></div><i class="edx-ph" data-k="ph"></i></div>
    <div class="edx-foot"><button class="btn primary" data-k="save" type="button">Save a copy</button><div class="edx-prog" data-k="prog" hidden><i data-k="pbar"></i></div><div class="edx-edits" data-k="edits"></div></div>
  </div>`;
  const q = (key) => k(s, key), vid = q('vid'), lane = q('lane'), layer = q('layer');
  q('wv').append(...loud(96, 5).map((v) => { const i = el('i'); i.style.setProperty('--h', `${Math.round(v * 100)}%`); return i; }));
  let dur = 8, loaded = false, failed = false, metaP = null, parts = [{ a: 0, b: 1 }], sel = 0, hist = [], at = 0, shape = 'free', rate = 1, drag = null, saving = false, edits = 0;
  const ICON_PLAY = svg('M8 5v14l11-7L8 5Z'), ICON_PAUSE = svg('M7 5h4v14H7zM13 5h4v14h-4z');
  const total = () => parts.reduce((t, p) => t + (p.b - p.a), 0) * dur / rate;
  const crop = () => SHAPES.find((x) => x[0] === shape)[2];
  // the file is fetched when someone first presses play or touches the strip; everything waits for its length
  const ensure = () => {
    if (!metaP) { metaP = new Promise((done) => { vid.preload = 'auto'; vid.load(); wait(vid, 'loadedmetadata', 8000).then(() => { if (vid.readyState >= 1) { dur = vid.duration || dur; loaded = true; } else failed = true; done(); }); }); }
    return metaP;
  };
  vid.addEventListener('error', () => { failed = true; });
  const where = (f) => parts.findIndex((p) => f >= p.a - 0.001 && f < p.b);
  // ---- drawing: the parts are built when they change shape, only moved while an end is dragged ----
  const buildParts = () => {
    layer.replaceChildren(...parts.map((p, i) => {
      const d = el('div', 'edx-part' + (i === sel ? ' on' : ''), i === sel ? '<b class="edx-h l" data-e="a" title="Drag to trim the start"></b><b class="edx-h r" data-e="b" title="Drag to trim the end"></b>' : '');
      d.dataset.i = i; return d;
    }));
    place();
  };
  const place = () => {
    $$('.edx-part', layer).forEach((d, i) => { const p = parts[i]; if (p) { d.style.setProperty('--a', `${p.a * 100}%`); d.style.setProperty('--b', `${p.b * 100}%`); } });
    $$('.edx-gap', layer).forEach((g) => g.remove());
    let prev = 0; const gaps = [];
    parts.forEach((p) => { if (p.a > prev + 0.001) gaps.push([prev, p.a]); prev = p.b; });
    if (prev < 0.999) gaps.push([prev, 1]);
    gaps.forEach(([a, b]) => { const g = el('div', 'edx-gap'); g.style.setProperty('--a', `${a * 100}%`); g.style.setProperty('--b', `${b * 100}%`); layer.prepend(g); });
    info();
  };
  const head = () => {
    q('ph').style.setProperty('--x', `${at * 100}%`); q('time').textContent = `${(at * dur).toFixed(1)} / ${dur.toFixed(1)} s`;
    const playing = !vid.paused; q('play').innerHTML = playing ? ICON_PAUSE : ICON_PLAY; q('play').setAttribute('aria-label', playing ? 'Pause' : 'Play'); q('pp').innerHTML = ICON_PLAY; q('pp').hidden = playing;
    q('split').disabled = !parts.some((p) => at > p.a + 0.02 && at < p.b - 0.02);
  };
  const info = () => {
    const len = total(), mb = Math.max(1, Math.round(len * 0.68 * crop()));
    q('len').textContent = `Keeping ${len.toFixed(1)} s`; q('est').textContent = `${parts.length} part${parts.length > 1 ? 's' : ''} · ≈ ${mb} MB`;
    q('crop').className = `edx-crop${shape === '9:16' ? ' c916' : shape === '1:1' ? ' c11' : ''}`;
    $$('button', q('shape')).forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.s === shape)));
    $$('button', q('rate')).forEach((b) => b.setAttribute('aria-pressed', String(Number(b.dataset.r) === rate)));
    q('undo').disabled = !hist.length; q('save').disabled = saving; head();
  };
  SHAPES.forEach(([id, label]) => { const b = el('button', '', label); b.type = 'button'; b.dataset.s = id; b.addEventListener('click', () => { shape = id; info(); }); q('shape').append(b); });
  RATES.forEach((r) => { const b = el('button', '', `${r}&times;`); b.type = 'button'; b.dataset.r = r; b.addEventListener('click', () => { rate = r; vid.playbackRate = r; info(); }); q('rate').append(b); });
  // ---- playing: only the parts you kept are played, in order ----
  const seek = () => { if (loaded && !failed) vid.currentTime = Math.min(dur - 0.04, at * dur); };
  const play = async () => {
    await ensure(); if (failed) return;
    if (where(at) < 0) { const nx = parts.find((p) => p.a >= at) || parts[0]; at = nx.a; }
    seek(); vid.playbackRate = rate; try { await vid.play(); } catch { /* the browser said no */ } head();
  };
  const pause = () => { vid.pause(); head(); };
  const toggle = () => (vid.paused ? play() : pause());
  c.every(40, () => {
    if (!loaded || vid.paused) return;
    const t = vid.currentTime / dur;
    if (where(t) < 0) { const nx = parts.find((p) => p.a >= t); if (nx) { vid.currentTime = nx.a * dur; at = nx.a; } else { vid.pause(); at = parts[0].a; seek(); } } else at = t;
    head();
  });
  vid.addEventListener('ended', () => { vid.pause(); at = parts[0].a; seek(); head(); });
  q('pp').addEventListener('click', toggle); q('play').addEventListener('click', toggle); vid.addEventListener('click', toggle);
  // ---- the strip: a click puts the playhead there and picks the part under it; the ends of the picked part are handles ----
  const frac = (e) => { const r = lane.getBoundingClientRect(); return clamp((e.clientX - r.left) / r.width); };
  lane.addEventListener('pointerdown', (e) => {
    try { lane.setPointerCapture(e.pointerId); } catch { /* a pointer that is already gone */ }
    lane.focus({ preventScroll: true }); ensure();
    const h = e.target.closest('.edx-h');
    if (h) { drag = { e: h.dataset.e, before: JSON.stringify({ parts, sel }) }; return; }
    const f = frac(e), i = where(f); at = f; const changed = i >= 0 && i !== sel; if (changed) sel = i; if (!vid.paused) vid.pause(); seek(); if (changed) buildParts(); head();
  });
  lane.addEventListener('pointermove', (e) => {
    if (!drag) return; const f = frac(e), p = parts[sel], prev = parts[sel - 1], next = parts[sel + 1];
    if (drag.e === 'a') p.a = clamp(f, prev ? prev.b : 0, p.b - 0.05); else p.b = clamp(f, p.a + 0.05, next ? next.a : 1);
    at = drag.e === 'a' ? p.a : p.b; seek(); place();
  });
  const up = () => { if (!drag) return; if (JSON.stringify({ parts, sel }) !== drag.before) { hist.push(drag.before); if (hist.length > 30) hist.shift(); } drag = null; info(); };
  lane.addEventListener('pointerup', up); lane.addEventListener('pointercancel', up);
  // ---- split and undo ----
  const split = () => {
    const i = parts.findIndex((p) => at > p.a + 0.02 && at < p.b - 0.02); if (i < 0) { toast('<b>Move the playhead</b> inside a part to split it', 'tip'); return; }
    hist.push(JSON.stringify({ parts, sel })); const p = parts[i]; parts.splice(i, 1, { a: p.a, b: at }, { a: at, b: p.b }); sel = i + 1; buildParts(); info(); toast(`<b>Split</b> at ${(at * dur).toFixed(1)} s &middot; drag an end to trim`);
  };
  const undo = () => { const h = hist.pop(); if (!h) return; const o = JSON.parse(h); parts = o.parts; sel = Math.min(o.sel, parts.length - 1); buildParts(); info(); };
  q('split').addEventListener('click', split); q('undo').addEventListener('click', undo);
  s.addEventListener('keydown', (e) => {
    if (e.target.closest('input, select')) return;
    const key = e.key.toLowerCase();
    if (key === 's' && !e.ctrlKey) { e.preventDefault(); split(); }
    else if (key === 'z' && e.ctrlKey) { e.preventDefault(); undo(); }
    else if (e.code === 'Space') { e.preventDefault(); toggle(); }
    else if (key === 'arrowleft' || key === 'arrowright') { e.preventDefault(); if (!vid.paused) vid.pause(); at = clamp(at + (key === 'arrowright' ? 1 : -1) * (e.shiftKey ? 1 : 0.25) / dur); seek(); head(); }
  });
  q('save').addEventListener('click', () => {
    if (saving) return; saving = true; vid.pause(); q('prog').hidden = false; info();
    const len = total(), mb = Math.max(1, Math.round(len * 0.68 * crop())), steps = reduce ? 1 : 14; let n = 0;
    c.every(90, () => { n++; q('pbar').style.setProperty('--p', `${Math.round((n / steps) * 100)}%`); if (n >= steps) { c.dispose(); saving = false; q('prog').hidden = true; edits++; const chip = el('span', 'edx-chip', `<b>Cashout-edit${edits > 1 ? ` ${edits}` : ''}.mp4</b><small>${len.toFixed(1)} s &middot; ${mb} MB${shape === 'free' ? '' : ` &middot; ${shape}`}</small>`); q('edits').prepend(chip); toast(`<b>Saved a copy</b> to Edits &middot; ${len.toFixed(1)} s`); info(); } });
  });
  new IntersectionObserver((es, o) => { if (es[0].isIntersecting) { ensure(); o.disconnect(); } }, { threshold: 0.15 }).observe(s);
  new IntersectionObserver((es) => { if (!es[0].isIntersecting && !vid.paused) pause(); }, { threshold: 0 }).observe(s);
  buildParts(); info();
};

// ---- Game capture: what BuffrX records. Games + display follows your game by itself, Display is one monitor, Window is one window ----
PG.auto = (s, c) => {
  const MONITORS = [['Monitor 1', '2560 \u00d7 1440 \u00b7 main'], ['Monitor 2', '1920 \u00d7 1080']];
  const APPS = [['BuffrX', 'buffrx-engine.exe'], ['Discord', 'discord.exe'], ['File Explorer', 'explorer.exe'], ['Mozilla Firefox', 'firefox.exe'], ['Windows Terminal', 'windowsterminal.exe']];
  const GAME = ['Marvel Rivals', 'marvelrivals.exe'];
  const MODES = [['auto', 'Games + display'], ['display', 'Display'], ['window', 'Window']];
  const WHAT = { auto: 'Auto switches from your display to your game: it captures your display while no game is running, detects a game by itself the moment one starts, and goes back to your display when it closes.', display: 'Captures one whole monitor, whatever is on it. Pick the monitor.', window: 'Captures a single window. Pick the one you want.' };
  const MON = '<svg class="ac-ic" viewBox="0 0 24 24" aria-hidden="true"><rect x="2.5" y="4" width="19" height="12.5"/><path d="M9 20.5h6M12 16.5v4"/></svg>';
  const WIN = '<svg class="ac-ic" viewBox="0 0 24 24" aria-hidden="true"><rect x="3" y="5" width="18" height="14"/><path d="M3 9h18"/></svg>';
  s.innerHTML = `<div class="seg ac-seg" role="group" aria-label="What to capture" data-k="modes"></div>
    <p class="ac-what" data-k="what"></p>
    <ul class="ac-list" data-k="list"></ul>
    <div class="row-btns joined" data-k="acts"><button class="btn small" data-k="start" type="button">Start the game</button><button class="btn small" data-k="tab" type="button" disabled>Alt-tab away</button><button class="btn small" data-k="close" type="button" disabled>Close the game</button></div>
    <p class="ac-now" aria-live="polite" data-k="now"></p>`;
  let mode = 'auto', game = false, taken = false, monitor = 0, win = 1, away = false;
  const now = k(s, 'now'), list = k(s, 'list');
  const say = (t) => { now.innerHTML = t; };
  const readout = () => {
    if (mode === 'display') return `Capturing <b>${MONITORS[monitor][0]}</b>, whatever is on it.`;
    if (mode === 'window') return win === 'game' ? `Capturing <b>${GAME[0]}</b>.` : `Capturing <b>${APPS[win][0]}</b>. Pick another window to switch.`;
    return taken ? `Capturing <b>${GAME[0]}</b>. Auto found the game, and you picked nothing.` : `Capturing <b>${MONITORS[monitor][0]}</b> until a game starts.`;
  };
  const row = (icon, name, meta, on, pick) => {
    const li = el('li', 'ac-row' + (on ? ' cap' : ''), `${icon}<b>${name}</b><span>${meta}</span>${on ? '<i class="ac-tag">Capturing</i>' : ''}`);
    if (pick) { li.tabIndex = 0; li.setAttribute('role', 'button'); li.addEventListener('click', pick); li.addEventListener('keydown', (ev) => { if (ev.key === 'Enter' || ev.key === ' ') { ev.preventDefault(); pick(); } }); }
    return li;
  };
  const gameRow = (on) => { const li = row(`<span class="ac-thumb">${BXGames.shot('rivals', '', 1)}</span>`, GAME[0], GAME[1], on, mode === 'window' ? () => { win = 'game'; draw(true); } : null); li.classList.add('ac-g'); return li; };
  const draw = (changed) => {
    const rows = [];
    if (mode === 'display') MONITORS.forEach(([n, m], i) => rows.push(row(MON, n, m, monitor === i, () => { monitor = i; draw(true); })));
    else if (mode === 'auto') { rows.push(row(MON, MONITORS[monitor][0], MONITORS[monitor][1], !taken)); if (game) rows.push(gameRow(taken)); }
    else { APPS.forEach(([n, e], i) => rows.push(row(WIN, n, e, win === i, () => { win = i; draw(true); }))); if (game) rows.push(gameRow(win === 'game')); }
    list.replaceChildren(...rows);
    $$('button', k(s, 'modes')).forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.m === mode)));
    k(s, 'what').textContent = WHAT[mode];
    k(s, 'acts').hidden = mode === 'display';
    k(s, 'start').disabled = game; k(s, 'tab').disabled = !game; k(s, 'close').disabled = !game;
    list.classList.toggle('pick', mode !== 'auto');
    if (changed !== 'keep') say(readout());
  };
  MODES.forEach(([m, label]) => { const b = el('button', '', label); b.type = 'button'; b.dataset.m = m; b.addEventListener('click', () => { c.dispose(); mode = m; draw(); }); k(s, 'modes').append(b); });
  k(s, 'start').addEventListener('click', () => {
    c.dispose(); game = true; taken = false; away = false; draw('keep');
    if (mode === 'auto') { say('A fullscreen game started&hellip;'); c.later(() => { taken = true; draw(); toast(`<b>${GAME[0]}</b> detected &middot; switched from the display`); }, reduce ? 0 : 700); }
    else say(`<b>${GAME[0]}</b> is running, but you picked another window.`);
  });
  k(s, 'tab').addEventListener('click', () => { c.dispose(); away = true; say(taken ? 'You alt-tabbed to Discord. <b>It keeps the game.</b>' : readout()); });
  k(s, 'close').addEventListener('click', () => { c.dispose(); const was = taken; game = false; taken = false; away = false; if (win === 'game') win = 1; draw('keep'); say(mode === 'auto' ? `The game closed. Back to <b>${MONITORS[monitor][0]}</b>.` : readout()); if (was) toast(`<b>${MONITORS[monitor][0]}</b> &middot; no game running, back to the display`); });
  draw();
};

// ---- Hotkeys: the app's own. Pick a key to change it, press one to try it. Controllers work too ----
PG.hotkeys = (s, c) => {
  const ACTIONS = [
    { id: 'save', name: 'Save clip', kb: 'Alt+1', does: 'The last minute becomes a clip.' },
    { id: 'quick', name: 'Quick clip', kb: 'Ctrl+Alt+1', does: 'A short clip: the last 20 seconds.' },
    { id: 'shot', name: 'Screenshot', kb: 'Alt+2', does: 'A screenshot is saved.' },
    { id: 'record', name: 'Record', kb: 'Alt+3', does: '' },
    { id: 'timeline', name: 'Timeline', kb: 'Alt+4', does: 'Timeline opens over your game.' },
  ];
  const PAD = '<svg viewBox="0 0 24 24" aria-hidden="true"><rect x="2.5" y="8" width="19" height="10" rx="5"/><path d="M7 11v4M5 13h4M16 12h.01M18 14h.01"/></svg>';
  s.innerHTML = `<div class="hx-top"><span class="hx-pill">${PAD}Controller supported</span></div><ul class="hx-list" data-k="list" aria-label="Hotkeys"></ul>`;
  const bound = Object.fromEntries(ACTIONS.map((x) => [x.id, x.kb])); let rec = null, recording = false;
  const caps = (chord) => chord.split('+').map((x) => `<kbd>${x}</kbd>`).join('<i>+</i>');
  const draw = () => k(s, 'list').replaceChildren(...ACTIONS.map((x) => {
    const on = rec === x.id, li = el('li', 'hx-row' + (on ? ' rec' : ''), `<b>${x.id === 'record' && recording ? 'Stop recording' : x.name}</b><button class="hx-bind${on ? ' on' : ''}" type="button" data-id="${x.id}" aria-label="${x.name} key: ${bound[x.id]}. Press to change it.">${on ? '<em>Press the keys…</em>' : caps(bound[x.id])}</button><button class="btn small hx-try" type="button" data-try="${x.id}">Try it</button>`);
    li.dataset.id = x.id; return li;
  }));
  const fire = (id) => {
    const x = ACTIONS.find((y) => y.id === id), row = $(`.hx-row[data-id="${id}"]`, s);
    if (id === 'record') { recording = !recording; toast(recording ? '<b>Recording started</b>' : '<b>Recording saved</b>'); draw(); } else toast(`<b>${x.name}</b> &middot; ${x.does}`);
    const r = $(`.hx-row[data-id="${id}"]`, s) || row; if (r) { r.classList.add('hit'); c.later(() => r.classList.remove('hit'), 650); }
  };
  const keyName = (e) => { const code = e.code; if (/^Key[A-Z]$/.test(code)) return code.slice(3); if (/^Digit\d$/.test(code)) return code.slice(5); if (/^F\d{1,2}$/.test(code)) return code; if (/^Numpad\d$/.test(code)) return `Num${code.slice(6)}`; return { Space: 'Space', Backquote: '`', Minus: '-', Equal: '=', Comma: ',', Period: '.', Slash: '/', Insert: 'Insert', Home: 'Home', End: 'End', PageUp: 'PgUp', PageDown: 'PgDn' }[code] || ''; };
  const chordOf = (e) => { const key = keyName(e); return key ? [e.ctrlKey && 'Ctrl', e.altKey && 'Alt', e.shiftKey && 'Shift', key].filter(Boolean).join('+') : ''; };
  s.addEventListener('click', (e) => {
    const t = e.target.closest('[data-try]'); if (t) { fire(t.dataset.try); return; }
    const b = e.target.closest('.hx-bind'); if (!b) return;
    rec = rec === b.dataset.id ? null : b.dataset.id; draw(); if (rec) $('.hx-bind.on', s).focus();
  });
  s.addEventListener('keydown', (e) => {
    if (rec) {
      e.preventDefault(); e.stopPropagation();
      if (e.key === 'Escape') { rec = null; draw(); return; }
      if (['Control', 'Alt', 'Shift', 'Meta'].includes(e.key)) { const m = [e.ctrlKey && 'Ctrl', e.altKey && 'Alt', e.shiftKey && 'Shift'].filter(Boolean); $('.hx-bind.on', s).innerHTML = `<em>${m.length ? `${m.join('+')}+…` : 'Press the keys…'}</em>`; return; }
      const chord = chordOf(e); if (!chord) return;
      if (chord.split('+').length > 3) { toast('<b>At most three keys</b> in a chord'); return; }
      const other = ACTIONS.find((x) => x.id !== rec && bound[x.id] === chord);
      if (other) { toast(`<b>${chord}</b> already belongs to ${other.name}`, 'taken'); return; }
      bound[rec] = chord; const nm = ACTIONS.find((x) => x.id === rec).name; rec = null; draw(); toast(`<b>${nm}</b> is now ${chord.replace(/\+/g, ' + ')}`); return;
    }
    if (e.metaKey) return; const chord = chordOf(e), hit = ACTIONS.find((x) => bound[x.id] === chord);
    if (hit) { e.preventDefault(); e.stopPropagation(); fire(hit.id); }
  });
  draw();
};

// ---- Performance mode: Off, On, or Auto. Auto makes the capture lighter only while a game from your list is running ----
PG.performance = (s) => {
  const GAMES = ['Counter-Strike 2', 'Marvel Rivals', 'The Finals'];
  const WHAT = { off: 'Always the full settings, whatever you play.', auto: 'The lighter capture turns on by itself while a game on your list is running, and goes back to the full settings when you close it.', on: 'Always the lighter capture, for every game.' };
  s.innerHTML = `<div class="seg pf-seg" role="group" aria-label="Performance mode" data-k="mode"><button type="button" data-m="off">Off</button><button type="button" data-m="auto">Auto</button><button type="button" data-m="on">On</button></div>
    <p class="pf-what" data-k="what"></p>
    <div class="pf-cols"><div class="pf-col on" data-k="full"><small>Full</small><b>2560 &times; 1440</b><b>120 fps</b><b>AV1</b></div><div class="pf-col" data-k="light"><small>Performance</small><b>1920 &times; 1080</b><b>60 fps</b><b>H.264, fast</b></div></div>
    <div class="pf-games" data-k="games" hidden></div>`;
  let mode = 'off', running = null; const picked = new Set(['Counter-Strike 2', 'Marvel Rivals']);
  const light = () => mode === 'on' || (mode === 'auto' && running !== null && picked.has(running));
  let was = false;
  const draw = (say) => {
    $$('button', k(s, 'mode')).forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.m === mode)));
    k(s, 'what').textContent = WHAT[mode];
    k(s, 'full').classList.toggle('on', !light()); k(s, 'light').classList.toggle('on', light());
    k(s, 'games').hidden = mode !== 'auto';
    k(s, 'games').replaceChildren(...GAMES.map((g) => {
      const row = el('div', 'pf-g' + (running === g ? ' live' : ''), `<label><input type="checkbox" ${picked.has(g) ? 'checked' : ''}> ${g}${running === g ? ' <i>running</i>' : ''}</label><button class="btn small" type="button">${running === g ? 'Close game' : 'Start game'}</button>`);
      $('input', row).addEventListener('change', (e) => { e.target.checked ? picked.add(g) : picked.delete(g); draw(true); });
      $('button', row).addEventListener('click', () => { running = running === g ? null : g; draw(true); });
      return row;
    }));
    if (say && light() !== was) toast(light() ? '<b>Performance mode on</b> &middot; a lighter capture' : '<b>Performance mode off</b> &middot; full settings');
    was = light();
  };
  $$('button', k(s, 'mode')).forEach((b) => b.addEventListener('click', () => { mode = b.dataset.m; draw(true); }));
  draw(false);
};

// ---- Buffer and quick clip: the buffer keeps the last stretch of your game; Save takes all of it, Quick clip only the newest part ----
PG.buffer = (s, c) => {
  const LIMITS = [[30, '30 s'], [60, '1 min'], [120, '2 min'], [300, '5 min']], QUICK = 20, CELLS = 40;
  s.innerHTML = `<div class="bf-top"><div class="seg" role="group" aria-label="Buffer length" data-k="len"></div><button class="btn small" data-k="power" type="button">Start</button></div>
    <div class="bf-count"><b data-k="held">0:00</b><span data-k="of"></span></div>
    <div class="bf-tape" data-k="tape" aria-hidden="true">${'<i></i>'.repeat(CELLS)}</div>
    <div class="row-btns joined"><button class="btn small primary" data-k="save" type="button" disabled><kbd>Alt</kbd><kbd>1</kbd>Save clip</button><button class="btn small" data-k="quick" type="button" disabled><kbd>Ctrl</kbd><kbd>Alt</kbd><kbd>1</kbd>Quick clip</button></div>
    <div class="bf-saved" data-k="saved"></div>`;
  const cells = [...k(s, 'tape').children], st = (t) => { k(s, 'st').innerHTML = t; };
  let limit = 60, held = 0, on = false, saved = [];
  const draw = () => {
    k(s, 'held').textContent = mmss(held); k(s, 'of').textContent = `/ ${mmss(limit)}`;
    const n = Math.round((held / limit) * CELLS); cells.forEach((q, i) => q.classList.toggle('on', i >= CELLS - n));
    $$('button', k(s, 'len')).forEach((b) => b.setAttribute('aria-pressed', String(Number(b.dataset.s) === limit)));
    k(s, 'power').textContent = on ? 'Stop' : 'Start'; k(s, 'save').disabled = k(s, 'quick').disabled = !on || held < 2;
  };
  LIMITS.forEach(([sec, label]) => { const b = el('button', '', label); b.type = 'button'; b.dataset.s = sec; b.addEventListener('click', () => { limit = sec; held = Math.min(held, limit); draw(); st(`The buffer keeps the last <b>${label}</b>.`); }); k(s, 'len').append(b); });
  k(s, 'power').addEventListener('click', () => { on = !on; if (!on) { held = 0; st('Buffer stopped. Nothing is kept.'); } else st('Buffering. It fills a little every second (sped up here).'); draw(); });
  c.every(250, () => { if (on && held < limit) { held = Math.min(limit, held + 2); draw(); } });
  let made = 0;
  const keep = (secs, what) => { const G = BXGames.ORDER, game = G[made % G.length], v = Math.floor(made / G.length) % 3; made++; const chip = el('span', 'bf-chip', `${BXGames.shot(game, '', v)}<b>${mmss(secs)}</b>`); saved = [chip, ...saved].slice(0, 5); k(s, 'saved').replaceChildren(...saved); st(`<b>${what}</b> &middot; ${mmss(secs)}`); };
  k(s, 'save').addEventListener('click', () => keep(held, 'Clip saved'));
  k(s, 'quick').addEventListener('click', () => keep(Math.min(held, QUICK), 'Quick clip saved'));
  draw();
};

// ---- Quick Share: the share sheet on your PC, the link page on your friend's, and the clip crossing straight between them ----
PG.link = (s, c) => {
  const HOURS = [1, 4, 12, 24], GB = 1.8, TOTAL = 5200;
  const LINK = 'buffrx.app/l/#AV7xK2mQ9d\u2026';
  s.innerHTML = `<div class="sh" data-state="idle">
    <section class="sh-pc sh-me" aria-label="Your PC">
      <header class="sh-bar"><span>Share clip</span><em class="sh-pill" data-k="pill">Ready</em></header>
      <div class="sh-body">
        <div class="sh-clip"><span class="sh-thumb">${BXGames.shot('rivals', '', 2)}</span><span class="sh-name"><b>SquadWipe.mp4</b><small>1.8 GB &middot; 4:12</small></span></div>
        <div class="sh-field"><span class="sh-label">Link lasts</span><div class="seg sh-hours" role="group" aria-label="Link lasts" data-k="hours"></div></div>
        <button class="sh-sw" type="button" role="switch" aria-checked="false" data-k="hide"><span>Hide my IP</span><i class="sq" aria-hidden="true"></i></button>
        <button class="btn primary sh-go" type="button" data-k="copy">Copy link</button>
        <div class="sh-live" data-k="live" hidden>
          <div class="sh-url"><span data-k="url"></span><small data-k="left"></small></div>
          <div class="sh-stats"><span data-k="watch">0 watching</span><button class="btn small" type="button" data-k="stop">Stop</button></div>
        </div>
      </div>
    </section>
    <div class="sh-wire" data-k="wire" aria-hidden="true"><i class="sh-line"></i><span class="sh-packets" data-k="pk"></span><b data-k="route">no link</b><small data-k="speed"></small></div>
    <section class="sh-pc sh-fr" aria-label="Your friend's PC">
      <header class="sh-bar"><span data-k="frtitle">Friend's browser</span></header>
      <div class="sh-body sh-page" data-k="page"></div>
    </section>
  </div>`;
  const T = (key, html) => { k(s, key).innerHTML = html; }, st = (t) => { k(s, 'st').innerHTML = t; }, root = s.firstElementChild;
  let hours = 24, hide = false, state = 'idle', p = 0, remain = 0, timer = 0;
  const left = () => { const h = Math.floor(remain / 3600), m = Math.floor((remain % 3600) / 60), x = remain % 60; return `${h}:${String(m).padStart(2, '0')}:${String(x).padStart(2, '0')} left`; };
  HOURS.forEach((h) => { const b = el('button', '', `${h} hr`); b.type = 'button'; b.dataset.h = h; b.addEventListener('click', () => { hours = h; if (state === 'linked') { remain = hours * 3600; T('left', left()); } draw(); }); k(s, 'hours').append(b); });
  const PILL = { idle: 'Ready', linked: 'Link live', sending: 'Sending', done: 'Sent', stopped: 'Stopped' };
  const friend = () => {
    const pg = k(s, 'page');
    if (state === 'idle') { T('frtitle', 'Friend\'s browser'); pg.innerHTML = '<div class="sh-empty"><span>Waiting for a link</span></div>'; }
    else if (state === 'linked') { T('frtitle', LINK.slice(0, 26)); pg.innerHTML = '<h4>A friend sent you a clip.</h4><p>It comes straight from their PC. Nothing is uploaded.</p><button class="btn primary sh-open" type="button" data-k="open">Open in BuffrX</button>'; k(s, 'open').addEventListener('click', send); }
    else if (state === 'sending' || state === 'done') { T('frtitle', 'Friend\'s BuffrX'); pg.innerHTML = `<div class="sh-rev" data-k="rev">${BXGames.shot('rivals', '', 2)}</div><div class="sh-meter" data-k="meter">${'<i></i>'.repeat(28)}</div><b data-k="frs"></b>`; }
    else if (state === 'stopped') { T('frtitle', 'Friend\'s browser'); pg.innerHTML = '<h4>This link has stopped.</h4><p>Ask your friend for a new one.</p>'; }
  };
  const draw = () => {
    $$('button', k(s, 'hours')).forEach((b) => b.setAttribute('aria-pressed', String(Number(b.dataset.h) === hours)));
    k(s, 'hide').setAttribute('aria-checked', String(hide));
    const live = state === 'linked' || state === 'sending' || state === 'done';
    k(s, 'live').hidden = !live; k(s, 'copy').hidden = live; k(s, 'copy').textContent = state === 'stopped' || state === 'done' ? 'Copy a new link' : 'Copy link';
    root.dataset.state = state; T('pill', PILL[state]);
    T('route', state === 'idle' ? 'no link' : state === 'stopped' ? 'closed' : hide ? 'via relay' : 'direct');
    k(s, 'stop').disabled = !live || state === 'done';
    T('watch', state === 'sending' ? '1 watching' : state === 'done' ? '1 watched' : '0 watching');
    friend();
  };
  const reset = () => { clearInterval(timer); timer = 0; p = 0; state = 'idle'; k(s, 'pk').replaceChildren(); T('speed', ''); };
  const copy = () => {
    reset(); state = 'linked'; remain = hours * 3600; T('url', LINK); T('left', left());
    timer = setInterval(() => { remain = Math.max(0, remain - 1); if (state === 'linked' || state === 'sending' || state === 'done') T('left', left()); }, 1000);
    c.dispose(); draw(); toast('<b>Link copied</b> &middot; no IP address inside');
  };
  const send = () => {
    if (state !== 'linked') return;
    state = 'sending'; p = 0; draw();
    const rate = hide ? 8 : 38, total = TOTAL * (hide ? 1.5 : 1), steps = Math.round(total / 110); let n = 0;
    T('frs', 'Connecting&hellip;'); T('speed', 'connecting');
    c.later(() => {
      c.every(110, () => {
        n++; p = Math.min(100, (n / steps) * 100);
        if (!reduce) { const q = el('i', 'sh-pk'); q.style.setProperty('--y', `${(Math.random() * 16 - 8).toFixed(1)}px`); k(s, 'pk').append(q); c.later(() => q.remove(), 900); }
        const cells = [...k(s, 'meter').children], lit = Math.round((p / 100) * cells.length); cells.forEach((q, i) => q.classList.toggle('on', i < lit));
        $('.gshot', k(s, 'rev')).style.setProperty('clip-path', `inset(0 ${100 - p}% 0 0)`);
        T('frs', `Receiving &middot; ${Math.round(p)}% &middot; ${(GB * p / 100).toFixed(2)} of ${GB.toFixed(2)} GB`); T('speed', `${rate} MB/s`);
        if (p >= 100) { c.dispose(); state = 'done'; draw(); T('frs', '<b>Arrived and playing</b>'); T('speed', 'done'); $('.gshot', k(s, 'rev')).style.setProperty('clip-path', 'none'); [...k(s, 'meter').children].forEach((q) => q.classList.add('on')); toast('<b>SquadWipe.mp4</b> arrived'); }
      });
    }, reduce ? 0 : 500);
  };
  const stop = () => { c.dispose(); clearInterval(timer); timer = 0; state = 'stopped'; draw(); T('speed', ''); k(s, 'pk').replaceChildren(); toast('<b>Link stopped</b> &middot; dead at once'); };
  k(s, 'copy').addEventListener('click', copy); k(s, 'stop').addEventListener('click', stop);
  k(s, 'hide').addEventListener('click', () => { if (state === 'sending') return; hide = !hide; draw(); });
  draw();
};
