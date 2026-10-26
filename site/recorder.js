// BuffrX landing page: the background. A recorder's viewfinder, drawn in faint lines in the page's own colours: corner brackets and a
// rule-of-thirds grid around the screen, an autofocus box that wanders and locks, an exposure readout, a scan line and a tracking box, a status
// block (REC and its timecode, the picture, the encoder, the ring's memory), and at the foot a Waybar-style line with four workspaces and
// the buffer's count. Every few seconds a clip is saved: the brackets flash, a tag appears and the ring fills for a moment. It draws only
// while something moves (about thirty frames a second), and it stops when the tab is hidden, when the visitor turns it off, and for
// reduced motion (one still frame; the setting is followed while the page is open). A mask in the page's CSS keeps the middle calm.
const BXRec = (() => {
'use strict';
const canvas = document.getElementById('bg');
if (!canvas) return { on() {}, colors() {} };
const ctx = canvas.getContext('2d');
const mq = matchMedia('(prefers-reduced-motion: reduce)');
let reduce = mq.matches;
const FRAME = 33, TOP = 96, BAR = 30, TAU = Math.PI * 2, RING = 24;
let W = 0, H = 0, dpr = 1, enabled = true, raf = 0, last = 0, t = 0, saved = 0, flashAt = -9, nextSave = 2.2;
let af = { x: 0, y: 0, tx: 0, ty: 0, from: [0, 0], t0: 0, lock: 0 };
let fg = [230, 226, 218], acc = [255, 107, 44], rec = [255, 90, 90];

const rgb = (hex) => { let h = hex.trim().replace('#', ''); if (h.length === 3) h = h.split('').map((c) => c + c).join(''); return [0, 2, 4].map((i) => parseInt(h.slice(i, i + 2), 16)); };
const rgba = (c, a) => `rgba(${c[0]}, ${c[1]}, ${c[2]}, ${a})`;
const small = () => W < 700;
const margin = () => (small() ? 12 : 22);
const mono = (px) => `${px}px "JetBrains Mono", ui-monospace, monospace`;
const hash = (a, b) => { let h = Math.imul(a, 374761393) + Math.imul(b, 668265263) | 0; h = Math.imul(h ^ (h >>> 13), 1274126177); return ((h ^ (h >>> 16)) >>> 0) / 4294967296; };
const p2 = (n) => String(n).padStart(2, '0');
const mmss = (s) => `${Math.floor(s / 60)}:${p2(Math.floor(s) % 60)}`;
function colors() {
  const cs = getComputedStyle(document.documentElement);
  try { fg = rgb(cs.getPropertyValue('--bx-fg')); acc = rgb(cs.getPropertyValue('--bx-acc')); rec = rgb(cs.getPropertyValue('--bx-rec')); } catch { /* keep the last colours */ }
  if (!raf) draw();
}
const ease = (x) => 1 - Math.pow(1 - Math.min(1, Math.max(0, x)), 3);
function corner(x, y, sx, sy, len) { ctx.moveTo(x + sx * len, y); ctx.lineTo(x, y); ctx.lineTo(x, y + sy * len); }

// The picture's frame: thirds, and four brackets that flash white-hot when a clip is saved.
function frame() {
  const m = margin(), flash = Math.max(0, 1 - (t - flashAt) / 0.7);
  ctx.setLineDash([2, 6]); ctx.lineWidth = 1; ctx.strokeStyle = rgba(fg, 0.12); ctx.beginPath();
  for (const f of [1 / 3, 2 / 3]) { ctx.moveTo(W * f, TOP); ctx.lineTo(W * f, H - BAR); ctx.moveTo(m, TOP + (H - BAR - TOP) * f); ctx.lineTo(W - m, TOP + (H - BAR - TOP) * f); }
  ctx.stroke(); ctx.setLineDash([]);
  ctx.strokeStyle = flash > 0 ? rgba(fg, 0.3 + 0.6 * flash) : rgba(acc, 0.5); ctx.lineWidth = 2 + flash * 1.5; ctx.beginPath();
  const b = H - BAR - 14, l = small() ? 22 : 38;
  corner(m, TOP, 1, 1, l); corner(W - m, TOP, -1, 1, l); corner(m, b, 1, -1, l); corner(W - m, b, -1, -1, l);
  ctx.stroke();
}
// The autofocus box: it drifts to a new spot, settles, and locks for a moment.
function focus() {
  if (small()) return;
  const age = t - af.t0, travel = ease(age / 0.8);
  af.x = af.from[0] + (af.tx - af.from[0]) * travel; af.y = af.from[1] + (af.ty - af.from[1]) * travel;
  const locked = age > 0.9, s = locked ? 22 : 22 + 26 * (1 - travel), a = locked ? 0.9 : 0.35, l = 8;
  ctx.strokeStyle = rgba(locked ? acc : fg, a); ctx.lineWidth = 1.5; ctx.beginPath();
  [[-1, -1], [1, -1], [-1, 1], [1, 1]].forEach(([sx, sy]) => corner(af.x + sx * s, af.y + sy * s, -sx, -sy, l));
  ctx.moveTo(af.x - 4, af.y); ctx.lineTo(af.x + 4, af.y); ctx.moveTo(af.x, af.y - 4); ctx.lineTo(af.x, af.y + 4); ctx.stroke();
  if (locked) { ctx.font = mono(10); ctx.textAlign = 'left'; ctx.textBaseline = 'bottom'; ctx.fillStyle = rgba(acc, 0.8); ctx.fillText('AF LOCK', af.x + s + 6, af.y - s); }
}
function retarget() {
  const left = Math.random() < 0.5, x = (left ? 0.05 + Math.random() * 0.14 : 0.81 + Math.random() * 0.14) * W, y = TOP + 40 + Math.random() * (H - TOP - BAR - 160);
  af = { x: af.x, y: af.y, from: [af.x || x, af.y || y], tx: x, ty: y, t0: t, lock: 0 };
}
// The exposure readout at the left: settings that change now and then, and an exposure scale with a slow marker.
function exposure() {
  if (small()) return;
  const m = margin(), x = m + 8, y = TOP + 18, step = Math.floor(t / 4), pick = (a, k) => a[Math.floor(hash(step, k) * a.length)];
  const rows = [['ISO', pick(['100', '200', '400', '800'], 1)], ['SHUTTER', pick(['1/60', '1/120', '1/250', '1/500'], 2)], ['WB', 'AUTO'], ['FOCUS', 'AF-C']];
  ctx.font = mono(11); ctx.textBaseline = 'middle';
  rows.forEach(([k, v], i) => { ctx.textAlign = 'left'; ctx.fillStyle = rgba(fg, 0.4); ctx.fillText(k, x, y + i * 19); ctx.fillStyle = rgba(fg, 0.62); ctx.fillText(v, x + 76, y + i * 19); });
  const sy = y + 4 * 19 + 10, w = 120;
  ctx.strokeStyle = rgba(fg, 0.3); ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(x, sy); ctx.lineTo(x + w, sy);
  for (let i = 0; i <= 10; i++) { const h = i % 5 === 0 ? 8 : 4; ctx.moveTo(x + (i / 10) * w, sy); ctx.lineTo(x + (i / 10) * w, sy - h); }
  ctx.stroke();
  const mx = x + w * (0.5 + 0.2 * Math.sin(t * 0.5) + 0.06 * Math.sin(t * 1.7));
  ctx.fillStyle = rgba(acc, 0.9); ctx.beginPath(); ctx.moveTo(mx - 4, sy + 4); ctx.lineTo(mx + 4, sy + 4); ctx.lineTo(mx, sy + 10); ctx.fill();
  ctx.fillStyle = rgba(fg, 0.34); ctx.textAlign = 'left'; ctx.fillText('-2   EV   +2', x, sy + 22);
}
// A scan line that sweeps the picture, and thirds crossings that pulse in turn.
function scan() {
  const top = TOP, bottom = H - BAR - 14, y = top + ((t * 60) % (bottom - top + 80)) - 40;
  if (y > top && y < bottom) {
    const g = ctx.createLinearGradient(0, y - 46, 0, y); g.addColorStop(0, rgba(acc, 0)); g.addColorStop(1, rgba(acc, 0.16));
    ctx.fillStyle = g; ctx.fillRect(0, Math.max(top, y - 46), W, y - Math.max(top, y - 46));
    ctx.fillStyle = rgba(acc, 0.22); ctx.fillRect(0, y, W, 1);
  }
  [1 / 3, 2 / 3].forEach((fx, i) => [1 / 3, 2 / 3].forEach((fy, j) => {
    const px = W * fx, py = top + (bottom - top) * fy, p = 0.5 + 0.5 * Math.sin(t * 2.2 - (i * 2 + j) * 1.1);
    ctx.strokeStyle = rgba(fg, 0.16 + 0.3 * p); ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(px - 6, py); ctx.lineTo(px + 6, py); ctx.moveTo(px, py - 6); ctx.lineTo(px, py + 6); ctx.stroke();
  }));
}
// A second, quieter box that tracks something moving on its own path.
function track() {
  if (small()) return;
  const x = W * (0.5 + 0.43 * Math.sin(t * 0.37 + 1)), y = TOP + 60 + (H - TOP - BAR - 180) * (0.5 + 0.5 * Math.sin(t * 0.53)), s = 16;
  ctx.setLineDash([3, 3]); ctx.strokeStyle = rgba(fg, 0.38); ctx.lineWidth = 1; ctx.strokeRect(x - s, y - s, s * 2, s * 2); ctx.setLineDash([]);
  ctx.font = mono(10); ctx.textAlign = 'left'; ctx.textBaseline = 'bottom'; ctx.fillStyle = rgba(fg, 0.4); ctx.fillText('TRK', x - s, y - s - 3);
}
// The status block at the top right: REC and its clock, the picture, the encoder, the ring.
function status() {
  const m = margin(), x = W - m - 8, f = Math.floor(t * 60), clock = `${p2(Math.floor(f / 216000))}:${p2(Math.floor(f / 3600) % 60)}:${p2(Math.floor(f / 60) % 60)}:${p2(f % 60)}`;
  ctx.font = mono(11); ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
  ctx.fillStyle = rgba(fg, 0.6); ctx.fillText(`REC ${clock}`, x, TOP + 18);
  if (Math.floor(t) % 2 === 0 || reduce) { ctx.fillStyle = rgba(rec, 0.9); ctx.beginPath(); ctx.arc(x - ctx.measureText(`REC ${clock}`).width - 12, TOP + 18, 4, 0, TAU); ctx.fill(); }
  ctx.fillStyle = rgba(fg, 0.45);
  if (small()) return;
  ctx.fillText('1080P · 60 FPS', x, TOP + 38); ctx.fillText('H264 · NVENC', x, TOP + 56);
  const held = Math.min(60, 38 + ((t * 2) % 22)), ringMb = Math.round(held * 0.64), lit = Math.round((held / 60) * RING), pulse = Math.max(0, 1 - (t - flashAt) / 0.9);
  ctx.fillText(`RING ${ringMb} / 512 MB`, x, TOP + 78);
  for (let i = 0; i < RING; i++) { ctx.fillStyle = i < lit ? rgba(pulse > 0 ? acc : fg, pulse > 0 ? 0.35 + 0.5 * pulse : 0.3) : rgba(fg, 0.07); ctx.fillRect(x - (RING - i) * 7 + 2, TOP + 92, 5, 8); }
}
// The tag that appears while a clip is saved.
function tag() {
  const age = t - flashAt; if (age > 2.4 || saved === 0) return;
  const a = Math.min(1, age * 6) * Math.min(1, (2.4 - age) * 3), m = margin();
  ctx.font = mono(11); ctx.textAlign = 'left'; ctx.textBaseline = 'middle'; ctx.fillStyle = rgba(acc, 0.9 * a); ctx.fillText(`CLIP ${p2(saved)} SAVED · 1:00`, m + 8, TOP + 18);
}
// The foot of the screen, as a Waybar sets it: workspaces, the buffer, the count.
function bar() {
  const y = H - BAR, m = margin(), ws = saved % 4;
  ctx.strokeStyle = rgba(fg, 0.1); ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(0, y + 0.5); ctx.lineTo(W, y + 0.5); ctx.stroke();
  ctx.font = mono(11); ctx.textBaseline = 'middle';
  for (let i = 0; i < 4; i++) {
    const x = m + i * 22, on = i === ws;
    if (on) { ctx.fillStyle = rgba(acc, 0.85); ctx.fillRect(x, y + 6, 18, 18); } else { ctx.strokeStyle = rgba(fg, 0.16); ctx.strokeRect(x + 0.5, y + 6.5, 17, 17); }
    ctx.textAlign = 'center'; ctx.fillStyle = on ? 'rgba(10, 10, 10, 0.9)' : rgba(fg, 0.3); ctx.fillText(String(i + 1), x + 9, y + 16);
  }
  ctx.textAlign = 'left'; ctx.fillStyle = rgba(fg, 0.46);
  if (!small()) ctx.fillText(`buffering ${mmss(Math.min(60, 38 + ((t * 2) % 22)))} / 1:00`, m + 100, y + 16);
  ctx.textAlign = 'right'; ctx.fillStyle = rgba(fg, 0.46); ctx.fillText(`${saved} clip${saved === 1 ? '' : 's'} saved`, W - m, y + 16);
  if (Math.floor(t) % 2 === 0 || reduce) { ctx.fillStyle = rgba(rec, 0.85); ctx.beginPath(); ctx.arc(m + 90, y + 15.5, 3, 0, TAU); ctx.fill(); }
}

function draw() {
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0); ctx.clearRect(0, 0, W, H);
  if (!enabled) return;
  frame(); scan(); exposure(); track(); focus(); status(); tag(); bar();
}
function advance() {
  if (t - af.t0 > 2.8) retarget();
  if (t < nextSave) return;
  saved++; flashAt = t; nextSave = t + 6 + Math.random() * 3;
}
function tick(now) {
  raf = requestAnimationFrame(tick);
  if (now - last < FRAME) return;
  t += Math.min(0.1, (now - last) / 1000); last = now;
  advance(); draw();
}
function still() { t = 6.6; saved = 2; flashAt = 5.8; af = { x: W * 0.9, y: H * 0.45, from: [W * 0.9, H * 0.45], tx: W * 0.9, ty: H * 0.45, t0: 4, lock: 0 }; draw(); } // the resting picture: focus locked
function run() { if (raf || reduce || !enabled || document.hidden) return; last = performance.now(); raf = requestAnimationFrame(tick); }
function halt() { cancelAnimationFrame(raf); raf = 0; }
function on(yes) { enabled = yes; halt(); if (yes) run(); if (reduce) still(); else draw(); }

function size() {
  dpr = Math.min(2, window.devicePixelRatio || 1); W = innerWidth; H = innerHeight;
  canvas.width = Math.round(W * dpr); canvas.height = Math.round(H * dpr);
}
size(); colors(); retarget(); if (reduce) still(); else draw(); run();
document.addEventListener('visibilitychange', () => { if (document.hidden) halt(); else run(); });
mq.addEventListener('change', (e) => { reduce = e.matches; halt(); run(); if (reduce) still(); else draw(); });
let tid = 0;
addEventListener('resize', () => {
  // A phone's address bar sliding away changes the height alone, by a little: that is not a new screen.
  if (innerWidth === W && Math.abs(innerHeight - H) < 160) return;
  clearTimeout(tid); tid = setTimeout(() => { size(); retarget(); if (!raf) { if (reduce) still(); else draw(); } }, 150);
});
return { on, colors };
})();
