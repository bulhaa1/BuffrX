// The Timeline leases the editor holds while a session's footage is open: which sessions are pinned, the
// chunk files each one was built from, and the sessions whose last renewal failed. The engine calls are
// handed in, and one session's pins and releases run in the order they were asked: a pin that was already
// on its way when the person removed the session cannot outlive the release queued behind it. Proving a
// failed renewal's footage is still there is not one of those ordered calls: it runs beside them, so a slow
// or stalled source read can never hold back a release, and a closing window can go without waiting for it.
// Two proofs can be outstanding at once, so each carries the attempt and the link it was sent for.

/**
 * How long a closing window waits for its releases. A pin still on its way keeps the release queued behind
 * it for as long as that request lives (the pipe's own timeout is a minute), and a person who closed the
 * editor must not wait for that: long enough for an unpin already on its way to land, short enough to read
 * as a close.
 */
export const CLOSE_RELEASE_MS = 1000

/** The lease state a renewal pass reads and writes: the editor's own sets and maps. */
export interface LeaseBook {
  /** Sessions this window wants pinned. */
  leases: Set<string>
  /** The chunk files each opened session was built from, lower-case. */
  files: Map<string, string[]>
  /** Sessions whose last renewal failed: their footage is not protected until one succeeds. */
  failures: Set<string>
  /** Bumped whenever a session is leased again, so a reply for an earlier lease cannot touch the newer one. */
  generations: Map<string, number>
  /**
   * Bumped for every pin attempt of a session. A proof runs beside the ordering, so two of them can be
   * outstanding at once: only the one for the latest attempt may write, or an older success would clear a
   * newer renewal's failure and an older failure would hide a newer verification.
   */
  attempts: Map<string, number>
  /** Bumped whenever the link changes state: a proof taken over another connection proves nothing about this one. */
  connection: number
  /** Sessions the engine is believed to hold a pin for: a pin reached it and no release has since. */
  pinned: Set<string>
  /** Sessions whose release is still owed: a pin may have landed after the session was dropped. */
  releases: Set<string>
  /** One ordered chain of engine calls per session, so a pin and the release after it cannot overtake. */
  chains: Map<string, Promise<void>>
  /** The window is closing: nothing is pinned any more, and every session is owed its release. */
  closed: boolean
  /** The connection is known to be down: a reply that lands now cannot clear a failure it never verified. */
  offline: boolean
}

export interface LeasePorts {
  pin: (id: string) => Promise<unknown>
  unpin: (id: string) => Promise<unknown>
  /** The session as the engine has it now; proves a failed lease's footage is still there. */
  source: (id: string) => Promise<{ session: string; chunks: { file: string }[] }>
}

/** Records the link's state, and moves its epoch on every change: a proof from the other connection is stale. */
export function setConnection(book: LeaseBook, online: boolean): void {
  if (book.offline === !online) return
  book.offline = !online
  book.connection += 1
}

export function leaseSession(book: LeaseBook, id: string): void {
  book.leases.add(id)
  book.generations.set(id, (book.generations.get(id) ?? 0) + 1)
}

/** A removed session keeps no lease, no failure and no warning: nothing about it is still waiting to be saved. */
export function dropSession(book: LeaseBook, id: string): void {
  book.leases.delete(id)
  book.files.delete(id)
  book.failures.delete(id)
  book.releases.add(id)
  book.generations.set(id, (book.generations.get(id) ?? 0) + 1)
}

/** Runs `work` after every engine call already queued for this session, and never breaks the chain. */
function queued(book: LeaseBook, id: string, work: () => Promise<void>): Promise<void> {
  const tail = book.chains.get(id) ?? Promise.resolve()
  const outcome = tail.then(work, work)
  const link = outcome.then(() => undefined, () => undefined)
  book.chains.set(id, link)
  void link.then(() => { if (book.chains.get(id) === link) book.chains.delete(id) })
  return outcome
}

/**
 * Closes the book: every session it may have pinned is owed a release, including one whose pin reached the
 * engine while its reply never did. Nothing is pinned afterwards, and the releases queued behind an in-flight
 * pin run in its session's own order.
 */
export function closeLeases(book: LeaseBook): string[] {
  const ids = new Set([...book.leases, ...book.pinned, ...book.releases])
  book.closed = true
  book.leases.clear()
  book.files.clear()
  // Nothing is left to warn about: every session is on its way out, and a failure kept here would be a
  // warning with no footage and no editor to show it in.
  book.failures.clear()
  for (const id of ids) book.releases.add(id)
  return [...ids]
}

/** Whether a lease owes the engine a proof that its footage is still there: only a failed renewal does. */
function proofDue(book: LeaseBook, id: string): boolean {
  return !book.offline && book.failures.has(id) && book.files.has(id)
}

/**
 * What one proof answers about: the lease, the pin attempt that asked for it, and the link it was read over.
 * Any of them moving makes the reply stale in the same way a removed session does.
 */
interface LeaseProof {
  generation: number
  attempt: number
  connection: number
}

/**
 * Proves a failed renewal's footage is still there, after the pin and outside the session's ordering.
 * The reply lands in the book only while what it answers about is still current: a removal, a re-open, a
 * close, a later renewal attempt or a link change in the meantime makes it write nothing, so it can neither
 * resurrect a warning nor clear one that a newer renewal earned.
 */
async function verifyLease(book: LeaseBook, id: string, ports: LeasePorts, proof: LeaseProof): Promise<void> {
  const files = book.files.get(id)
  if (!files) return
  const live = () => !book.closed && book.leases.has(id)
    && (book.generations.get(id) ?? 0) === proof.generation
    && (book.attempts.get(id) ?? 0) === proof.attempt
    && book.connection === proof.connection
  try {
    const source = await ports.source(id)
    const present = new Set(source.chunks.map((chunk) => chunk.file.toLowerCase()))
    if (source.session !== id || files.some((file) => !present.has(file)))
      throw new Error('Timeline footage is no longer available')
    if (live() && !book.offline) book.failures.delete(id)
  } catch (error) {
    if (live()) book.failures.add(id)
    throw error
  }
}

/**
 * One step towards what the book asks of the engine right now: pin a session that is still open, release
 * one that is gone. It reads the book when it runs, not when it was queued, so a session removed and
 * opened again in between is pinned, never left with the release of its earlier self. A pin that has to
 * be proved returns that proof, for the caller to run outside the ordering.
 */
async function pinOrRelease(book: LeaseBook, id: string, ports: LeasePorts): Promise<(() => Promise<void>) | undefined> {
  if (book.closed || !book.leases.has(id)) {
    if (!book.pinned.has(id) && !book.releases.has(id)) return undefined
    await ports.unpin(id)
    book.pinned.delete(id)
    book.releases.delete(id)
    return undefined
  }
  const generation = book.generations.get(id) ?? 0
  const live = () => !book.closed && book.leases.has(id) && (book.generations.get(id) ?? 0) === generation
  // Every attempt gets its own token, taken before the call: a proof still outstanding from an earlier one is
  // older news from here on, whichever way this attempt goes.
  const attempt = (book.attempts.get(id) ?? 0) + 1
  book.attempts.set(id, attempt)
  const connection = book.connection
  try {
    await ports.pin(id)
    // The engine holds it now even if the session was removed meanwhile; the release queued behind this
    // call is what takes it away, so record the truth rather than pretend no pin exists.
    book.pinned.add(id)
    book.releases.delete(id)
  } catch (error) {
    if (live()) book.failures.add(id)
    throw error
  }
  if (!live() || !proofDue(book, id)) return undefined
  return () => verifyLease(book, id, ports, { generation, attempt, connection })
}

/**
 * Pins a lease, releases a dropped one, or does nothing, in the session's own order. A rejected engine
 * call is reported to the caller and kept in the book: a failed pin records a failure, a failed release
 * stays owed and the next renewal pass asks again. The reply waits for the proof a failed renewal needs,
 * but the session's ordering does not: the next call can run while a source read is still outstanding.
 */
export function syncSession(book: LeaseBook, id: string, ports: LeasePorts): Promise<void> {
  let proof: (() => Promise<void>) | undefined
  const step = queued(book, id, async () => { proof = await pinOrRelease(book, id, ports) })
  return step.then(() => proof?.())
}

/**
 * Closes the book and asks the engine for every release it owes, waiting at most `deadlineMs` for them.
 * Resolves `true` when the engine was told every session is gone, `false` when a release was still owed
 * at the deadline because a pin or its reply was still outstanding: that release stays queued and is sent
 * if this page lives long enough, and the engine's own ten-minute lease expires it otherwise. The
 * remaining window is the engine's, not this page's: a pin nobody here released keeps the session's
 * footage against the retention sweep and answers "open in the editor" to an erase until it expires.
 */
export async function releaseLeases(book: LeaseBook, ports: LeasePorts, deadlineMs = CLOSE_RELEASE_MS): Promise<boolean> {
  const sending = Promise.all(closeLeases(book).map((id) => syncSession(book, id, ports).then(() => undefined, () => undefined)))
  let timer: ReturnType<typeof setTimeout> | undefined
  await Promise.race([sending, new Promise<void>((resolve) => { timer = setTimeout(resolve, deadlineMs) })])
  if (timer !== undefined) clearTimeout(timer)
  return book.releases.size === 0 && book.pinned.size === 0
}

/**
 * One renewal pass over every lease, and every release still owed. Each session settles on its own and
 * `settled` runs then, so a slow request cannot hold back another session's failure: the warning appears
 * when it is known, not when the last reply lands.
 */
export async function renewLeases(book: LeaseBook, ports: LeasePorts, settled?: () => void): Promise<void> {
  const ids = new Set([...book.leases, ...book.pinned, ...book.releases])
  await Promise.all([...ids].map(async (id) => {
    try {
      await syncSession(book, id, ports)
    } catch {
      // The book carries the outcome: a failure stays on its session, a release stays owed.
    }
    settled?.()
  }))
}
