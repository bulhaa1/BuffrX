// Audio (and video) sample bytes per track in an MP4, plain or fragmented: silence in an AAC track is a
// few bytes a frame, real sound is hundreds, so this tells the two apart without decoding.
const fs = require('fs')

function boxes(buf, start, end) {
  const out = []
  let at = start
  while (at + 8 <= end) {
    let size = buf.readUInt32BE(at)
    const type = buf.toString('latin1', at + 4, at + 8)
    let header = 8
    if (size === 1) {
      size = Number(buf.readBigUInt64BE(at + 8))
      header = 16
    }
    if (size < header) break
    out.push({ type, body: at + header, end: at + size })
    at += size
  }
  return out
}

const find = (list, type) => list.find((b) => b.type === type)

function analyse(path) {
  const buf = fs.readFileSync(path)
  const top = boxes(buf, 0, buf.length)
  const stats = new Map()
  const moov = find(top, 'moov')
  if (moov) {
    for (const trak of boxes(buf, moov.body, moov.end).filter((b) => b.type === 'trak')) {
      const level = boxes(buf, trak.body, trak.end)
      const tkhd = find(level, 'tkhd')
      const id = tkhd ? buf.readUInt32BE(tkhd.body + (buf[tkhd.body] === 1 ? 20 : 12)) : 0
      const mdia = find(level, 'mdia')
      if (!mdia) continue
      const inner = boxes(buf, mdia.body, mdia.end)
      const hdlr = find(inner, 'hdlr')
      const handler = hdlr ? buf.toString('latin1', hdlr.body + 8, hdlr.body + 12) : '?'
      const minf = find(inner, 'minf')
      const stbl = minf ? find(boxes(buf, minf.body, minf.end), 'stbl') : null
      const stsz = stbl ? find(boxes(buf, stbl.body, stbl.end), 'stsz') : null
      let count = 0
      let total = 0
      let min = Infinity
      let max = 0
      if (stsz) {
        const fixed = buf.readUInt32BE(stsz.body + 4)
        count = buf.readUInt32BE(stsz.body + 8)
        for (let i = 0; i < count; i++) {
          const v = fixed || buf.readUInt32BE(stsz.body + 12 + i * 4)
          total += v
          if (v < min) min = v
          if (v > max) max = v
        }
      }
      stats.set(id, { handler, count, total, min, max })
    }
  }
  for (const moof of top.filter((b) => b.type === 'moof')) {
    for (const traf of boxes(buf, moof.body, moof.end).filter((b) => b.type === 'traf')) {
      const level = boxes(buf, traf.body, traf.end)
      const tfhd = find(level, 'tfhd')
      if (!tfhd) continue
      const flags = tfhd.body === undefined ? 0 : buf.readUInt32BE(tfhd.body) & 0xffffff
      const id = buf.readUInt32BE(tfhd.body + 4)
      const entry = stats.get(id)
      if (!entry) continue
      for (const trun of level.filter((b) => b.type === 'trun')) {
        const trunFlags = buf.readUInt32BE(trun.body) & 0xffffff
        const samples = buf.readUInt32BE(trun.body + 4)
        entry.count += samples
        if ((trunFlags & 0x200) === 0) continue // no sample sizes: a single size in tfhd, unused here
        let at = trun.body + 8
        if (trunFlags & 0x1) at += 4 // data offset
        if (trunFlags & 0x4) at += 4 // first sample flags
        for (let i = 0; i < samples; i++) {
          const size = buf.readUInt32BE(at)
          entry.total += size
          if (size < entry.min) entry.min = size
          if (size > entry.max) entry.max = size
          at += 4
          if (trunFlags & 0x100) at += 4
          if (trunFlags & 0x200) at += 0
          if (trunFlags & 0x400) at += 4
          if (trunFlags & 0x800) at += 4
        }
      }
      void flags
    }
  }
  const lines = []
  for (const [, v] of stats) {
    lines.push(`${v.handler} samples=${v.count} bytes=${v.total} avg=${v.count ? (v.total / v.count).toFixed(1) : 0} min=${v.min} max=${v.max}`)
  }
  return lines
}

for (const path of process.argv.slice(2)) {
  console.log(path)
  for (const line of analyse(path)) console.log('  ' + line)
}
