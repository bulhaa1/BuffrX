// Runs before the page paints: lets the style sheet hide what scroll reveals will show (only when there is script to show it), and
// puts the visitor's saved theme on the page at once, so a dark page never flashes before a light one.
document.documentElement.classList.add('js');
try {
  const saved = JSON.parse(localStorage.getItem('buffrx-site'));
  if (saved && ['clutch', 'overtime', 'respawn', 'afterglow', 'killcam', 'lobby', 'headshot', 'frostbite', 'toxic', 'nightshift', 'highscore', 'ghillie', 'sandstorm', 'arcade'].includes(saved.site)) document.documentElement.dataset.theme = saved.site;
} catch { /* no storage: the page keeps its first theme */ }
// The headline's arrival is started by fx.js; if that never runs, this lets the page show anyway.
setTimeout(() => document.documentElement.classList.add('title-go'), 2500);
