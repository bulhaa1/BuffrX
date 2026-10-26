// The link page. It reads the link after # (which the browser never sends anywhere), checks it is a
// BuffrX link, says how long it has left, and hands it to BuffrX with buffrx://. No request is made:
// the page's own CSP forbids every connection.
'use strict';

// Where "Get BuffrX" points. Set it to the BuffrX download page before publishing.
const DOWNLOAD_URL = 'https://github.com/bulhaa1'; // the site's Get button points here until a release exists

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
const $ = (id) => document.getElementById(id);

function decode(text) {
  const out = [];
  let buffer = 0;
  let bits = 0;
  for (const c of text) {
    const value = B64.indexOf(c);
    if (value < 0) return null;
    buffer = ((buffer << 6) | value) & 0xffffff;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push((buffer >> bits) & 255);
    }
  }
  return new Uint8Array(out);
}

function left(seconds) {
  const minutes = Math.max(1, Math.floor(seconds / 60));
  return minutes < 60 ? `${minutes} min` : `${Math.round(minutes / 60)} h`;
}

// The state line: a dot in the tone's colour (ok, warn, bad) and the sentence.
function say(text, tone) {
  $('status').dataset.tone = tone;
  $('status-text').textContent = text;
}

function main() {
  $('download').href = DOWNLOAD_URL;
  let payload = location.hash.slice(1);
  if (payload.endsWith('/')) payload = payload.slice(0, -1);
  const bytes = payload.length <= 400 ? decode(payload) : null;
  // Version 1: version, endpoint (32), share (16), token (16), expires (4), relay length (1), relay.
  if (!bytes || bytes.length < 70 || bytes[0] !== 1 || bytes.length !== 70 + bytes[69]) {
    say('This link is incomplete. Ask your friend to copy it again.', 'bad');
    return;
  }
  const expires = ((bytes[65] << 24) >>> 0) + (bytes[66] << 16) + (bytes[67] << 8) + bytes[68];
  const remaining = expires - Math.floor(Date.now() / 1000);
  if (remaining <= 0) {
    say('This link has expired. Ask your friend for a new one.', 'bad');
    return;
  }
  $('left').textContent = `${left(remaining)} left`;
  $('left').hidden = false;
  // A phone or a Mac opens the link in Discord too, and cannot run BuffrX: say so instead of a dead button.
  if (!/Windows/.test(navigator.userAgent)) {
    say(`BuffrX runs on Windows PCs. Open this link on one (it works while your friend's PC is on).`, 'warn');
    $('get').hidden = false;
    return;
  }
  const target = 'buffrx://l/' + payload;
  $('open').href = target;
  $('open').hidden = false;
  say(`Opening it in BuffrX… (the link works while your friend's PC is on)`, 'ok');
  // Browsers may ask first; nothing happens at all when BuffrX is not installed, so the offer follows.
  location.href = target;
  setTimeout(() => { $('get').hidden = false; }, 2500);
}

main();
