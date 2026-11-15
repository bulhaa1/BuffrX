// BuffrX landing page: the games the page shows, and the pictures each one has. Every picture is the maker's own capture
// (see games/SOURCES.md); no publisher images. A game has one or more pictures so a
// page that shows two clips of one game never shows the same frame twice: `shot(id, cls, n)` takes the n-th. Nothing here makes a request
// but the page's own pictures.
const BXGames = (() => {
'use strict';
const LIST = {
  rivals: { name: 'Marvel Rivals', imgs: ['games/rivals.jpg'] },
  cs2: { name: 'Counter-Strike 2', imgs: ['games/cs2.jpg', 'games/cs2-2.jpg'] },
  valorant: { name: 'Valorant', imgs: ['games/valorant.jpg'] },
  finals: { name: 'The Finals', imgs: ['games/finals.jpg'] }, // names pixelated
  acbf: { name: 'AC Black Flag', imgs: ['games/acbf.jpg'] },
  acm: { name: 'AC Mirage', imgs: ['games/acm.jpg'] },
  msfs: { name: 'Flight Sim 2024', imgs: ['games/msfs.jpg'] },
  forza: { name: 'Forza Horizon 6', imgs: ['games/forza.jpg'] },
};
const ORDER = ['rivals', 'cs2', 'valorant', 'finals', 'acbf', 'acm', 'msfs', 'forza'];
const name = (id) => LIST[id].name;
const src = (id, n = 0) => LIST[id].imgs[n % LIST[id].imgs.length];
// A small picture of the game: its n-th frame.
const shot = (id, cls = '', n = 0) => `<div class="gshot ${cls}"><img src="${src(id, n)}" alt="" loading="lazy" decoding="async"></div>`;
return { LIST, ORDER, name, src, shot };
})();
