// How numbers read in the interface. Rewind's conventions: "0:42", "12:46 PM" today / "Oct 12" before,
// sizes in lowercase kb / mb / gb.

export function duration(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds <= 0) return '0:00'
  const total = Math.round(seconds)
  const hours = Math.floor(total / 3600)
  const minutes = Math.floor((total % 3600) / 60)
  const rest = total % 60
  return hours > 0
    ? `${hours}:${String(minutes).padStart(2, '0')}:${String(rest).padStart(2, '0')}`
    : `${minutes}:${String(rest).padStart(2, '0')}`
}

/** A playhead's position to the hundredth: "0:07.25". */
export function timecode(seconds: number): string {
  const hundredths = Math.max(0, Math.round((Number.isFinite(seconds) ? seconds : 0) * 100))
  const whole = Math.floor(hundredths / 100)
  return `${duration(whole)}.${String(hundredths % 100).padStart(2, '0')}`
}

/** Seconds as a length a person says: "45s", "2m", "1m 30s". */
export function span(seconds: number): string {
  const s = Math.max(0, Math.round(seconds))
  if (s < 60) return `${s}s`
  const minutes = Math.floor(s / 60)
  const rest = s % 60
  return rest ? `${minutes}m ${rest}s` : `${minutes}m`
}

export function bytes(value: number): string {
  if (!Number.isFinite(value) || value <= 0) return '0 kb'
  if (value < 1024 * 1024) return `${Math.max(1, Math.round(value / 1024))} kb`
  if (value < 1024 * 1024 * 1024) return `${Math.round(value / (1024 * 1024))} mb`
  return `${(value / (1024 * 1024 * 1024)).toFixed(value < 10 * 1024 ** 3 ? 2 : 1)} gb`
}

/** The day a file was saved, as a heading: "Today", "Yesterday", "Oct 12". */
export function day(unixSeconds: number): string {
  if (!unixSeconds) return ''
  const date = new Date(unixSeconds * 1000)
  const now = new Date()
  if (date.toDateString() === now.toDateString()) return 'Today'
  const yesterday = new Date(now)
  yesterday.setDate(now.getDate() - 1)
  if (date.toDateString() === yesterday.toDateString()) return 'Yesterday'
  return date.toLocaleDateString([], { month: 'short', day: 'numeric' })
}

export function when(unixSeconds: number): string {
  if (!unixSeconds) return ''
  const date = day(unixSeconds)
  // Today's files are named by the clock, everything older by its day.
  return date === 'Today'
    ? new Date(unixSeconds * 1000).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })
    : date
}

export function folderOf(path: string): string {
  const parts = path.split(/[\\/]/)
  return parts.length > 1 ? parts.slice(0, -1).join('\\') : ''
}

export function fileName(path: string): string {
  return path.split(/[\\/]/).pop() ?? path
}

export function stem(path: string): string {
  const name = fileName(path)
  const dot = name.lastIndexOf('.')
  return dot > 0 ? name.slice(0, dot) : name
}
