// BuffrX landing page: a crosshair for a cursor, like a game's. Four arms and a dot at the pointer that open a little while it moves and
// snap shut on a click; over anything you can press, four brackets close in from the crosshair and lock onto it. Only for a mouse (a
// touch screen has no pointer to draw), never when the visitor turns it off, and with no easing for reduced motion. The page's own
// cursor comes back the moment the crosshair is off. Nothing here reads or sends anything: it follows the pointer and nothing else.
const BXCross = (() => {
'use strict';
const fine = matchMedia('(hover: hover) and (pointer: fine)');
const mq = matchMedia('(prefers-reduced-motion: reduce)');
const TARGET = 'a[href], button:not(:disabled), [role="radio"], [role="switch"], [data-lock]';
const supported = () => fine.matches;
let enabled = false, built = false, root, box, px = -100, py = -100, seen = false, target = null, pad = 6, gap = 5, press = false, raf = 0, last = 0;
let b = { x: 0, y: 0, w: 28, h: 28 }; // the brackets' own rectangle, easing toward its goal

function build() {
  built = true;
  root = document.createElement('div'); root.className = 'xh'; root.setAttribute('aria-hidden', 'true');
  root.innerHTML = '<i class="xa n"></i><i class="xa e"></i><i class="xa s"></i><i class="xa w"></i><i class="xd"></i>';
  box = document.createElement('div'); box.className = 'xl'; box.setAttribute('aria-hidden', 'true');
  box.innerHTML = '<i class="tl"></i><i class="tr"></i><i class="bl"></i><i class="br"></i>';
  document.body.append(box, root);
  addEventListener('pointermove', move, { passive: true });
  addEventListener('pointerdown', (e) => { if (e.pointerType === 'mouse') { press = true; gap = 2; kick(); } }, { passive: true });
  addEventListener('pointerup', () => { press = false; gap = 11; kick(); }, { passive: true });
  document.documentElement.addEventListener('mouseleave', () => { seen = false; root.classList.remove('on'); box.classList.remove('on'); target = null; });
  // a scroll can bring a different thing under a still pointer
  addEventListener('scroll', () => { if (seen) { aim(document.elementFromPoint(px, py)); kick(); } }, { passive: true, capture: true });
  document.addEventListener('visibilitychange', () => { if (document.hidden) stop(); });
}
function goal() {
  if (target && target.isConnected) {
    const r = target.getBoundingClientRect();
    if (r.width > 6 && r.height > 6 && r.width < 920 && r.height < 460) { const p = press ? 2 : pad; return { x: r.left - p, y: r.top - p, w: r.width + p * 2, h: r.height + p * 2, lock: true }; }
  }
  return { x: px - 14, y: py - 14, w: 28, h: 28, lock: false };
}
function aim(from) {
  const el = from instanceof Element ? from.closest(TARGET) : null;
  if (el === target) return;
  // coming onto a target the brackets start from the crosshair, so they visibly close in
  if (el && !(target && target.isConnected)) b = { x: px - 22, y: py - 22, w: 44, h: 44 };
  target = el;
}
function move(e) {
  if (e.pointerType !== 'mouse') return;
  px = e.clientX; py = e.clientY;
  const speed = Math.hypot(e.movementX || 0, e.movementY || 0);
  if (!press) gap = Math.min(16, gap + speed * 0.12);
  aim(e.target);
  if (!seen) { seen = true; document.documentElement.classList.add('xh-on'); root.classList.add('on'); }
  kick();
}
function kick() { if (!raf && enabled) { last = performance.now(); raf = requestAnimationFrame(tick); } }
function stop() { cancelAnimationFrame(raf); raf = 0; }
function tick(now) {
  raf = 0;
  const dt = Math.max(0, Math.min(0.05, (now - last) / 1000)); last = now;
  const g = goal(), k = mq.matches ? 1 : 1 - Math.exp(-dt * 22);
  let moving = false;
  for (const p of ['x', 'y', 'w', 'h']) { const d = g[p] - b[p]; if (Math.abs(d) > 0.3) { b[p] += d * k; moving = true; } else b[p] = g[p]; }
  const rest = press ? 2 : 5; gap += (rest - gap) * (mq.matches ? 1 : 1 - Math.exp(-dt * 9)); if (Math.abs(gap - rest) > 0.1) moving = true;
  root.style.setProperty('--x', px + 'px'); root.style.setProperty('--y', py + 'px'); root.style.setProperty('--g', gap.toFixed(1) + 'px');
  box.style.setProperty('--x', b.x.toFixed(1) + 'px'); box.style.setProperty('--y', b.y.toFixed(1) + 'px'); box.style.setProperty('--w', b.w.toFixed(1) + 'px'); box.style.setProperty('--h', b.h.toFixed(1) + 'px');
  root.classList.toggle('lock', g.lock); box.classList.toggle('on', seen && g.lock);
  // a locked target can move under a still pointer (a scroll, a reveal), so keep following it
  if (moving || g.lock) { raf = requestAnimationFrame(tick); last = now; }
}
function on(yes) {
  enabled = yes && supported();
  if (enabled && !built) build();
  document.documentElement.classList.toggle('xh-on', enabled && seen);
  if (built) { root.hidden = box.hidden = !enabled; if (!enabled) { stop(); target = null; } }
}
return { on, supported };
})();
