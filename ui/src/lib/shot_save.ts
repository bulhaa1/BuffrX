// One screenshot save. The edit signature is read before anything yields, because the write can take long
// enough for the person to edit again: the signature read here belongs to the picture this save encodes,
// and a later edit is a different picture. Marking the later one saved would let a stale file be shared.

export async function saveScreenshot<T>({
  signature,
  write,
}: {
  /** Reads the edit signature on screen. Called before the write starts and once it has finished. */
  signature: () => string
  /** Encodes and writes the picture. */
  write: () => Promise<T>
}): Promise<{ written: T; saved: string; current: string }> {
  const saved = signature()
  const written = await write()
  return { written, saved, current: signature() }
}
