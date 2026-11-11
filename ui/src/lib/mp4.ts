// The Timeline preview's MP4 join. A session is one file per keyframe-aligned chunk, and every chunk is
// a whole fragmented MP4: ftyp + moov, then moof + mdat repeated. MediaSource wants one init segment and
// the media segments appended after it, so this reads the boxes, hands back ftyp+moov, names the codec
// and copies every moof+mdat with its decode time moved onto the session's own clock.
//
// Only two fields are rewritten: `mfhd`'s sequence number (MediaSource wants them unique) and each
// track's `tfdt` (where a segment decodes). Nothing else needs it: the engine writes
// `movflags=frag_keyframe+empty_moov+default_base_moof`, so a `trun`'s data offset is relative to its
// own moof and a moof copied together with its mdat still points at the right bytes. A `tfhd` carrying
// `base-data-offset` is not that shape and is refused, exactly as Chromium refuses it
// (CHUNK_DEMUXER_ERROR_APPEND_FAILED). Evidence: docs/timeline-spike-progress.md.

/** One `moof`+`mdat` pair, ready to append after the init segment. */
export interface MoofSegment {
  bytes: Uint8Array
  /** Where this segment starts decoding, after the shift, in milliseconds. */
  decodeStartMs: number
  /** How much media its sample durations add up to, in milliseconds. */
  durationMs: number
}

/** A box's place in the file: `body` is past the header (and past a full box's version and flags). */
interface Box {
  type: string
  start: number
  body: number
  end: number
}

/** The init segment a player appends first: ftyp and moov, which hold the sample tables of nothing. */
export function initBytes(file: Uint8Array): Uint8Array {
  const top = boxes(file, 0, file.length)
  const moov = top.find((box) => box.type === 'moov')
  if (!moov) throw new Error('mp4: no moov box: this is not a fragmented MP4')
  const ftyp = top.find((box) => box.type === 'ftyp')
  if (!ftyp) return file.slice(moov.start, moov.end)
  const init = new Uint8Array(ftyp.end - ftyp.start + moov.end - moov.start)
  init.set(file.subarray(ftyp.start, ftyp.end), 0)
  init.set(file.subarray(moov.start, moov.end), ftyp.end - ftyp.start)
  return init
}

/**
 * The codec string MediaSource is asked about, from the video track's own configuration box:
 * `avc1.640033` from `avcC`, `hvc1.1.6.L93.B0` from `hvcC`, `av01.0.00M.08` from `av1C`.
 */
export function codecString(init: Uint8Array): string {
  const top = boxes(init, 0, init.length)
  const moov = top.find((box) => box.type === 'moov')
  if (!moov) throw new Error('mp4: no moov box: this is not a fragmented MP4')
  for (const track of children(init, moov)) {
    if (track.type !== 'trak') continue
    const handler = handlerOf(init, track)
    if (handler !== 'vide') continue
    const entry = sampleEntry(init, track)
    if (!entry) throw new Error('mp4: the video track has no sample description')
    const config = children(init, entry, VISUAL_ENTRY_FIXED).find((child) => child.type === 'avcC' || child.type === 'hvcC' || child.type === 'av1C')
    if (!config) throw new Error(`mp4: ${entry.type} has no avcC, hvcC or av1C box`)
    if (config.type === 'avcC') return avc(entry.type, config, init)
    if (config.type === 'hvcC') return hevc(entry.type, config, init)
    return av1(entry.type, config, init)
  }
  throw new Error('mp4: no video track')
}

/**
 * Every moof+mdat pair in one chunk file, copied, with its decode time moved by `shiftMs` and its
 * `mfhd` sequence number set from `sequenceStart`. `shiftMs` is where this chunk sits in the session:
 * the durations of the chunks before it, in milliseconds.
 */
export function segments(file: Uint8Array, shiftMs: number, sequenceStart: number, skip = 0): MoofSegment[] {
  const top = boxes(file, 0, file.length)
  const moov = top.find((box) => box.type === 'moov')
  if (!moov) throw new Error('mp4: no moov box: this is not a fragmented MP4')
  const tracks = trackTimescales(file, moov)
  const out: MoofSegment[] = []
  let sequence = sequenceStart
  let seen = 0
  for (let i = 0; i < top.length; i++) {
    const moof = top[i]
    if (moof.type !== 'moof') continue
    const mdat = top[i + 1]
    if (!mdat || mdat.type !== 'mdat') throw new Error('mp4: a moof box without its mdat beside it')
    if (seen++ < skip) continue
    const copy = file.slice(moof.start, mdat.end)
    const view = new DataView(copy.buffer, copy.byteOffset, copy.byteLength)
    // Offsets inside the moof, relative to the copy, which starts at the moof.
    const at = (box: Box) => ({ body: box.body - moof.start })
    const fields = children(file, moof)
    const mfhd = fields.find((box) => box.type === 'mfhd')
    if (mfhd) {
      view.setUint32(at(mfhd).body + 4, sequence, false)
      sequence += 1
    }
    let startMs = Number.POSITIVE_INFINITY
    let durationMs = 0
    let pictureMs = -1
    for (const traf of fields) {
      if (traf.type !== 'traf') continue
      const inner = children(file, traf)
      const tfhd = inner.find((box) => box.type === 'tfhd')
      if (!tfhd) throw new Error('mp4: a traf with no tfhd')
      const tfhdFlags = flagsOf(file, tfhd)
      if (tfhdFlags & 0x1) {
        // MediaSource refuses an absolute media offset: the engine must write default_base_moof.
        throw new Error('mp4: this fragment carries a tfhd base-data-offset, which MediaSource refuses')
      }
      const full = new DataView(file.buffer, file.byteOffset, file.byteLength)
      const trackId = full.getUint32(tfhd.body + 4, false)
      const track = tracks.get(trackId)
      if (!track) throw new Error(`mp4: no track ${trackId} in the moov box`)
      const tfdt = inner.find((box) => box.type === 'tfdt')
      if (tfdt) {
        const delta = Math.round((shiftMs * track.timescale) / 1000)
        const version = file[tfdt.body]
        if (version === 1) {
          const time = full.getBigUint64(tfdt.body + 4, false)
          view.setBigUint64(at(tfdt).body + 4, time + BigInt(delta), false)
          startMs = Math.min(startMs, Math.round((Number(time + BigInt(delta)) * 1000) / track.timescale))
        } else {
          const moved = full.getUint32(tfdt.body + 4, false) + delta
          if (moved > 0xffffffff) throw new Error('mp4: the shifted decode time no longer fits its 32 bits')
          view.setUint32(at(tfdt).body + 4, moved, false)
          startMs = Math.min(startMs, Math.round((moved * 1000) / track.timescale))
        }
      }
      const length = durationOf(file, inner, track.timescale)
      if (track.video) pictureMs = Math.max(pictureMs, length)
      else durationMs = Math.max(durationMs, length)
    }
    out.push({ bytes: copy, decodeStartMs: Number.isFinite(startMs) ? startMs : 0, durationMs: pictureMs >= 0 ? pictureMs : durationMs })
  }
  return out
}

/** How long a traf's samples run, from its trun sample durations or its tfhd's default. */
function durationOf(file: Uint8Array, fields: Box[], timescale: number): number {
  const trun = fields.find((box) => box.type === 'trun')
  if (!trun) return 0
  const view = new DataView(file.buffer, file.byteOffset, file.byteLength)
  const flags = flagsOf(file, trun)
  const count = view.getUint32(trun.body + 4, false)
  if (!(flags & 0x100)) {
    // No per-sample duration: the tfhd's default is used for every sample.
    const tfhd = fields.find((box) => box.type === 'tfhd')
    const tfhdFlags = tfhd ? flagsOf(file, tfhd) : 0
    if (!tfhd || !(tfhdFlags & 0x8)) return 0
    return Math.round((count * view.getUint32(tfhd.body + 8 + extraTfhdLength(tfhdFlags), false) * 1000) / timescale)
  }
  let at = trun.body + 8 + (flags & 0x1 ? 4 : 0) + (flags & 0x4 ? 4 : 0)
  let ticks = 0
  for (let i = 0; i < count; i++) {
    ticks += view.getUint32(at, false)
    at += 4 + (flags & 0x200 ? 4 : 0) + (flags & 0x400 ? 4 : 0) + (flags & 0x800 ? 4 : 0)
  }
  return Math.round((ticks * 1000) / timescale)
}

/** Where a tfhd's default sample duration sits, after the fields its own flags put before it. */
function extraTfhdLength(flags: number): number {
  return (flags & 0x1 ? 8 : 0) + (flags & 0x2 ? 4 : 0)
}

function avc(type: string, config: Box, file: Uint8Array): string {
  if (config.end < config.body + 4) throw new Error('mp4: a truncated avcC box')
  const hex = (n: number) => n.toString(16).toUpperCase().padStart(2, '0')
  return `${type}.${hex(file[config.body + 1])}${hex(file[config.body + 2])}${hex(file[config.body + 3])}`
}

function hevc(type: string, config: Box, file: Uint8Array): string {
  if (config.end < config.body + 13) throw new Error('mp4: a truncated hvcC box')
  const view = new DataView(file.buffer, file.byteOffset, file.byteLength)
  const byte = file[config.body + 1]
  const space = ['', 'A', 'B', 'C'][byte >> 6]
  const tier = (byte >> 5) & 0x1 ? 'H' : 'L'
  const profile = (`${space}${byte & 0x1f}`).trim()
  const compatibility = reverseBits32(view.getUint32(config.body + 2, false)).toString(16).toUpperCase()
  const level = file[config.body + 12]
  const constraints: string[] = []
  for (let i = 0; i < 6; i++) constraints.push(file[config.body + 6 + i].toString(16).toUpperCase().padStart(2, '0'))
  while (constraints.length && constraints[constraints.length - 1] === '00') constraints.pop()
  const suffix = constraints.length ? `.${constraints.join('.')}` : ''
  return `${type}.${profile}.${compatibility}.${tier}${level}${suffix}`
}

function av1(type: string, config: Box, file: Uint8Array): string {
  if (config.end < config.body + 3) throw new Error('mp4: a truncated av1C box')
  const profile = file[config.body + 1] >> 5
  const level = file[config.body + 1] & 0x1f
  const detail = file[config.body + 2]
  const tier = detail & 0x80 ? 'H' : 'M'
  const depth = detail & 0x40 ? 10 : detail & 0x20 ? 12 : 8
  return `${type}.${profile}.${String(level).padStart(2, '0')}${tier}.${String(depth).padStart(2, '0')}`
}

function reverseBits32(value: number): number {
  let out = 0
  for (let i = 0; i < 32; i++) out = (out << 1) | ((value >>> i) & 1)
  return out >>> 0
}

/** Every box between `from` and `to`: 32-bit sizes, 64-bit `largesize`, or the rest of the region. */
function boxes(bytes: Uint8Array, from: number, to: number): Box[] {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  const out: Box[] = []
  let at = from
  while (at + 8 <= to) {
    let size = view.getUint32(at, false)
    const type = String.fromCharCode(bytes[at + 4], bytes[at + 5], bytes[at + 6], bytes[at + 7])
    let header = 8
    if (size === 1) {
      if (at + 16 > to) throw new Error(`mp4: a truncated ${type} box`)
      const large = view.getBigUint64(at + 8, false)
      if (large > BigInt(Number.MAX_SAFE_INTEGER)) throw new Error(`mp4: the ${type} box is larger than can be read`)
      size = Number(large)
      header = 16
    } else if (size === 0) {
      size = to - at // to the end of its region, as the format allows
    }
    if (size < header || at + size > to) throw new Error(`mp4: the ${type} box does not fit its file`)
    out.push({ type, start: at, body: at + header, end: at + size })
    at += size
  }
  return out
}

/** The boxes a container box holds, after `fixed` bytes of its own fields. */
function children(bytes: Uint8Array, box: Box, fixed = 0): Box[] {
  return boxes(bytes, box.body + fixed, box.end)
}

/** A VisualSampleEntry's fixed fields, between its 8-byte box header and its own boxes. */
const VISUAL_ENTRY_FIXED = 78

/** The video track's first sample description, in its stbl's stsd box. */
function sampleEntry(bytes: Uint8Array, trak: Box): Box | undefined {
  for (const box of children(bytes, trak)) {
    if (box.type !== 'mdia') continue
    for (const inner of children(bytes, box)) {
      if (inner.type !== 'minf') continue
      for (const third of children(bytes, inner)) {
        if (third.type !== 'stbl') continue
        const stsd = children(bytes, third).find((entry) => entry.type === 'stsd')
        if (!stsd) continue
        const entries = children(bytes, stsd, 8)
        if (entries.length) return entries[0]
      }
    }
  }
  return undefined
}

/** A trak's `hdlr` handler type: `vide`, `soun`, or something else. */
function handlerOf(bytes: Uint8Array, trak: Box): string {
  for (const box of children(bytes, trak)) {
    if (box.type !== 'mdia') continue
    const hdlr = children(bytes, box).find((entry) => entry.type === 'hdlr')
    if (hdlr && hdlr.end >= hdlr.body + 12) return String.fromCharCode(...bytes.subarray(hdlr.body + 8, hdlr.body + 12))
  }
  return ''
}

/** track_ID -> its media timescale, and whether it is the picture. */
function trackTimescales(bytes: Uint8Array, moov: Box): Map<number, { timescale: number; video: boolean }> {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  const out = new Map<number, { timescale: number; video: boolean }>()
  for (const trak of children(bytes, moov)) {
    if (trak.type !== 'trak') continue
    const fields = children(bytes, trak)
    const tkhd = fields.find((box) => box.type === 'tkhd')
    if (!tkhd) continue
    const idAt = tkhd.body + 4 + (bytes[tkhd.body] === 1 ? 16 : 8)
    if (idAt + 4 > tkhd.end) continue
    const id = view.getUint32(idAt, false)
    const mdia = fields.find((box) => box.type === 'mdia')
    if (!mdia) continue
    const mdhd = children(bytes, mdia).find((box) => box.type === 'mdhd')
    if (!mdhd) continue
    const timescaleAt = mdhd.body + 4 + (bytes[mdhd.body] === 1 ? 16 : 8)
    if (timescaleAt + 4 > mdhd.end) continue
    out.set(id, { timescale: view.getUint32(timescaleAt, false), video: handlerOf(bytes, trak) === 'vide' })
  }
  return out
}

/** A full box's flags: the three bytes after its version. */
function flagsOf(bytes: Uint8Array, box: Box): number {
  return (bytes[box.body + 1] << 16) | (bytes[box.body + 2] << 8) | bytes[box.body + 3]
}
