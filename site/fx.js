// A safety net for the entrances, and two small touches of feel the page does not need: a scroll with weight, and buttons that lean
// toward the pointer. The touches stand down for reduced motion and for touch screens; the net always runs. Nothing here makes a request.
'use strict';
(() => {
  const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const fine = matchMedia('(hover: hover) and (pointer: fine)').matches;
  const root = document.documentElement;

  // ---- a scroll with weight: the wheel sets a target and the page eases to it ----
  const inertia = () => {
    if (reduce || !fine) return;
    const doc = document.scrollingElement; let target = 0, cur = 0, raf = 0, last = 0;
    const room = () => doc.scrollHeight - innerHeight;
    // a box that can scroll itself (the library, a table) keeps its own wheel
    const owns = (el, dy) => {
      for (; el && el !== document.body && el !== root; el = el.parentElement) {
        const o = getComputedStyle(el).overflowY;
        if ((o === 'auto' || o === 'scroll') && el.scrollHeight > el.clientHeight + 1 && (dy < 0 ? el.scrollTop > 0 : el.scrollTop < el.scrollHeight - el.clientHeight - 1)) return true;
      }
      return false;
    };
    const stop = () => { if (raf) { cancelAnimationFrame(raf); raf = 0; last = 0; root.style.scrollBehavior = ''; } target = cur = scrollY; };
    const tick = (t) => {
      const dt = Math.min(48, last ? t - last : 16); last = t;
      cur += (target - cur) * (1 - Math.exp(-dt / 95));
      if (Math.abs(target - cur) < 0.5) { scrollTo(0, target); raf = 0; last = 0; root.style.scrollBehavior = ''; return; }
      scrollTo(0, cur); raf = requestAnimationFrame(tick);
    };
    addEventListener('wheel', (e) => {
      if (e.ctrlKey || e.defaultPrevented || Math.abs(e.deltaX) > Math.abs(e.deltaY) || owns(e.target, e.deltaY)) return;
      e.preventDefault();
      if (!raf) { cur = target = scrollY; root.style.scrollBehavior = 'auto'; raf = requestAnimationFrame(tick); }
      target = Math.max(0, Math.min(room(), target + e.deltaY * (e.deltaMode === 1 ? 32 : e.deltaMode === 2 ? innerHeight : 1)));
    }, { passive: false });
    addEventListener('scroll', () => { if (!raf) cur = target = scrollY; }, { passive: true });
    addEventListener('keydown', stop); addEventListener('pointerdown', stop);
    document.addEventListener('click', (e) => { if (e.target.closest('a[href^="#"]')) stop(); });
  };

  // ---- buttons that lean toward the pointer and settle back; the nearest one within reach leans before you reach it ----
  const magnet = () => {
    if (reduce || !fine) return;
    const sel = '.cta .btn, .nav .btn, .hk, .get .btn, .plan .btn', REACH = 80;
    let cur = null;
    const drop = () => { if (!cur) return; cur.classList.remove('lean'); cur.style.removeProperty('--lx'); cur.style.removeProperty('--ly'); cur = null; };
    const fit = (b) => b && !b.hasAttribute('disabled') && b.getAttribute('aria-disabled') !== 'true';
    addEventListener('pointermove', (e) => {
      let b = e.target.closest && e.target.closest(sel);
      if (!fit(b)) b = null;
      if (!b) {
        let best = REACH;
        for (const x of document.querySelectorAll(sel)) {
          if (!fit(x)) continue;
          const r = x.getBoundingClientRect(), gap = Math.hypot(Math.max(0, Math.max(r.left - e.clientX, e.clientX - r.right)), Math.max(0, Math.max(r.top - e.clientY, e.clientY - r.bottom)));
          if (gap < best) { best = gap; b = x; }
        }
      }
      if (b !== cur) { drop(); cur = b; }
      if (!cur) return;
      const r = cur.getBoundingClientRect(), dx = (e.clientX - (r.left + r.width / 2)) / r.width, dy = (e.clientY - (r.top + r.height / 2)) / r.height;
      cur.style.setProperty('--lx', `${Math.max(-5, Math.min(5, dx * 10)).toFixed(1)}px`); cur.style.setProperty('--ly', `${Math.max(-4, Math.min(4, dy * 8)).toFixed(1)}px`); cur.classList.add('lean');
    }, { passive: true });
  };

  // ---- the safety net: a part that has been in view and is still hidden is shown, whatever the observer did ----
  // The entrances hide parts until an observer lights them. If one is ever missed (a tab woken from sleep, a jump past it, a browser
  // that skips a callback), the part would stay blank on screen. This looks again on every scroll and once a second.
  const net = () => {
    let queued = false;
    const sweep = () => {
      queued = false;
      for (const e of document.querySelectorAll('[data-r]:not(.in)')) {
        const r = e.getBoundingClientRect();
        if (r.height && r.top < innerHeight * 0.92 && r.bottom > 0) e.classList.add('in');
      }
    };
    addEventListener('scroll', () => { if (!queued) { queued = true; requestAnimationFrame(sweep); } }, { passive: true });
    addEventListener('resize', sweep); document.addEventListener('visibilitychange', sweep);
    const timer = setInterval(() => { sweep(); if (!document.querySelector('[data-r]:not(.in)')) clearInterval(timer); }, 1000); // stops itself once everything has been shown
  };

  // ---- the headline decodes: each statement turns from noise into its letters as it arrives ----
  const decode = () => {
    if (reduce) { root.classList.add('title-go'); return; }
    const GLYPHS = 'ABCDEFGHJKLMNPRSTUVWXYZ0123456789#%&*+<>';
    requestAnimationFrame(() => root.classList.add('title-go')); // the headline and the text below start together, from this frame
    [[100, '.st1'], [480, '.st2'], [900, '.st3']].forEach(([delay, sel]) => {
      const st = document.querySelector('#h1 ' + sel); if (!st) return;
      const text = st.textContent.trim();
      st.innerHTML = ''; // the real text stays in the page for readers and search; the noise is drawn over it
      const ghost = document.createElement('span'); ghost.className = 'ghost'; ghost.textContent = text;
      const scr = document.createElement('span'); scr.className = 'scr'; scr.setAttribute('aria-hidden', 'true');
      st.append(ghost, scr);
      const t0 = performance.now() + delay, len = 640;
      const frame = (now) => {
        const p = (now - t0) / len;
        if (p >= 1) { scr.remove(); ghost.style.visibility = 'visible'; return; }
        if (p > 0) {
          let out = '';
          for (let i = 0; i < text.length; i++) out += text[i] === ' ' || text[i] === '.' || p > 0.15 + (i / text.length) * 0.8 ? text[i] : GLYPHS[(Math.random() * GLYPHS.length) | 0];
          scr.textContent = out;
        }
        requestAnimationFrame(frame);
      };
      requestAnimationFrame(frame);
      setTimeout(() => { scr.remove(); ghost.style.visibility = 'visible'; }, delay + len + 1500); // a hidden tab pauses the frames: the words still arrive
    });
  };

  const go = () => { decode(); net(); inertia(); magnet(); };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', go); else go();
})();
