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

  // ---- buttons that lean toward the pointer and settle back ----
  const magnet = () => {
    if (reduce || !fine) return;
    const sel = '.cta .btn, .nav .btn, .hk, .get .btn';
    addEventListener('pointermove', (e) => {
      const b = e.target.closest && e.target.closest(sel);
      if (!b || b.hasAttribute('disabled') || b.getAttribute('aria-disabled') === 'true') return;
      const r = b.getBoundingClientRect(), dx = (e.clientX - (r.left + r.width / 2)) / r.width, dy = (e.clientY - (r.top + r.height / 2)) / r.height;
      b.style.setProperty('--lx', `${(dx * 10).toFixed(1)}px`); b.style.setProperty('--ly', `${(dy * 8).toFixed(1)}px`); b.classList.add('lean');
    });
    addEventListener('pointerout', (e) => {
      const b = e.target.closest && e.target.closest(sel);
      if (b && !b.contains(e.relatedTarget)) { b.classList.remove('lean'); b.style.removeProperty('--lx'); b.style.removeProperty('--ly'); }
    });
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
    // CLIP IT: a camera flash lights the page as the first line is taken
    const flash = document.createElement('div'); flash.id = 'flash'; flash.setAttribute('aria-hidden', 'true'); document.body.append(flash); setTimeout(() => flash.remove(), 1200);
    const GLYPHS = 'ABCDEFGHJKLMNPRSTUVWXYZ0123456789#%&*+<>';
    requestAnimationFrame(() => root.classList.add('title-go')); // the flash, the lines and the text below start together, from this frame
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
    // SEND IT: once the line has landed, a copy of it is sent: it stretches, tilts and streaks away to the top right
    setTimeout(() => {
      const st = document.querySelector('#h1 .st3'); if (!st || !st.animate) return;
      const copy = document.createElement('span'); copy.className = 'sent'; copy.setAttribute('aria-hidden', 'true'); copy.textContent = st.textContent.trim();
      st.append(copy);
      const run = copy.animate([
        { transform: 'translate(0, 0) skewX(0) scaleX(1)', opacity: 1, offset: 0 },
        { transform: 'translate(-14px, 4px) skewX(6deg) scaleX(0.96)', opacity: 1, offset: 0.18 },
        { transform: 'translate(70vw, -46vh) skewX(-28deg) scaleX(2.2)', opacity: 0, offset: 1 },
      ], { duration: 820, easing: 'cubic-bezier(0.5, 0, 0.9, 0.5)', fill: 'forwards' });
      for (let i = 0; i < 3; i++) {
        const line = document.createElement('i'); line.className = 'streak'; line.style.top = `${28 + i * 22}%`; st.append(line);
        line.animate([{ transform: 'translateX(0) scaleX(0.2)', opacity: 0 }, { transform: 'translateX(18vw) scaleX(1.4)', opacity: 0.9, offset: 0.5 }, { transform: 'translateX(60vw) scaleX(0.4)', opacity: 0 }], { duration: 760, delay: 60 + i * 50, easing: 'cubic-bezier(0.5, 0, 0.9, 0.5)', fill: 'both' }).onfinish = () => line.remove();
      }
      run.onfinish = () => copy.remove();
    }, 1750);
  };

  const go = () => { decode(); net(); inertia(); magnet(); };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', go); else go();
})();
