// Reading one Timeline chunk over the `media` scheme. The scheme hands a large file out in 8 MiB
// pieces; asking by `?piece=<offset>` keeps the request simple for CORS, where a Range header would
// trigger an OPTIONS preflight the scheme does not answer. This keeps asking until the file's own
// total is in hand.

const CONTENT_RANGE = /^bytes (\d+)-(\d+)\/(\d+)$/

const join = (pieces: Uint8Array[], total: number): Uint8Array => {
  const whole = new Uint8Array(total)
  let at = 0
  for (const piece of pieces) {
    whole.set(piece, at)
    at += piece.length
  }
  return whole
}

/**
 * The file at `url`, starting at `from`: a 200 is taken as it stands, a 206 is followed piece by
 * piece. `from` is what a live chunk asks for, so a tick pays only for the bytes flushed since the
 * last one instead of the whole file again.
 */
export const fetchChunkBytes = async (
  url: string,
  signal?: AbortSignal,
  fetchImpl: typeof fetch = fetch,
  from = 0,
): Promise<Uint8Array> => {
  const pieces: Uint8Array[] = []
  let received = from
  for (;;) {
    const pieceUrl = `${url}${url.includes('?') ? '&' : '?'}piece=${received}`
    const reply = await fetchImpl(pieceUrl, { signal })
    if (reply.status === 200) {
      const whole = new Uint8Array(await reply.arrayBuffer())
      return whole.subarray(from)
    }
    if (reply.status !== 206) throw new Error(`${url} answered ${reply.status}`)
    const range = CONTENT_RANGE.exec(reply.headers.get('Content-Range')?.trim() ?? '')
    if (!range) throw new Error(`${url} answered 206 without a usable Content-Range`)
    const start = Number(range[1])
    const total = Number(range[3])
    if (start !== received) {
      throw new Error(`${url} answered from ${start} while ${received} bytes were read`)
    }
    const piece = new Uint8Array(await reply.arrayBuffer())
    if (piece.length === 0) throw new Error(`${url} stopped at ${received} of ${total} bytes`)
    pieces.push(piece)
    received += piece.length
    if (received >= total) return join(pieces, received - from)
  }
}
