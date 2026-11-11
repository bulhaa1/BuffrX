// The short name a clip or screenshot goes by in the editor. BuffrX files are named "Replay 2026-11-03 22-34-28 (1).mp4":
// the kind is the title and the date and time are the second line, so a bin of them reads at a glance.

const STAMP = /(\d{4})-(\d{2})-(\d{2})[ _T-](\d{2})-(\d{2})-(\d{2})/

/** The moment a file's name says it was made, or null when the name carries none. */
export function stampOf(name: string): Date | null {
  const match = STAMP.exec(name)
  if (!match) return null
  const [year, month, day, hour, minute, second] = match.slice(1).map(Number)
  const date = new Date(year, month - 1, day, hour, minute, second)
  return Number.isNaN(date.getTime()) ? null : date
}

/** "Replay 2026-11-03 22-34-28 (1).mp4" is "Replay"; a name without a stamp is itself, minus its extension. */
export function simpleTitle(name: string): string {
  const stem = name.replace(/\.[A-Za-z0-9]{2,4}$/, '')
  const match = STAMP.exec(stem)
  const title = (match ? stem.slice(0, match.index) : stem).replace(/[\s_-]+$/, '').trim()
  return title || stem
}

/** "Nov 3, 10:34 PM", in the person's own locale. */
export function stampText(date: Date): string {
  return date.toLocaleString([], { month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit' })
}
