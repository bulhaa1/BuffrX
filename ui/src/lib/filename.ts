// The name a saved copy is given in the export tab: one Windows file name, always .mp4, never a path.
// Pure, so every rule is tested without a window.

export const EXPORT_EXTENSION = '.mp4'

// Windows refuses these anywhere in a name, and a path separator would move the save out of the folder.
const INVALID_CHARACTERS = /[<>:"/\\|?*\u0000-\u001f]/
// These are devices, not files: Windows opens the device even when the name carries an extension.
const DEVICE_NAMES = /^(CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])$/i

export interface ExportName {
  /** The file name to save as, extension included; empty when `error` says why it cannot be used. */
  name: string
  /** Empty when the name can be saved; otherwise what to tell the person. */
  error: string
}

/** Validates a typed name and gives back the file name to save, adding `.mp4` when it is missing. */
export function exportFileName(value: string): ExportName {
  // Windows drops a name's leading and trailing spaces, so the name judged is the name saved.
  const typed = value.trim()
  if (!typed) return { name: '', error: 'Give the copy a name first.' }
  if (typed === '.' || typed === '..') return { name: '', error: 'A file name cannot be . or ..' }
  if (INVALID_CHARACTERS.test(typed)) {
    return { name: '', error: 'A file name cannot contain < > : " / \\ | ? *' }
  }
  // A trailing dot or space is dropped by Windows, so "clip." and "clip" would be the same file: say so rather
  // than save a name the person did not type.
  if (/[. ]$/.test(typed) && !/\.mp4$/i.test(typed)) {
    return { name: '', error: 'A file name cannot end with a dot or a space.' }
  }
  const named = /\.mp4$/i.test(typed) ? typed : typed + EXPORT_EXTENSION
  const stem = named.slice(0, named.length - EXPORT_EXTENSION.length).replace(/[. ]+$/, '')
  if (!stem) return { name: '', error: 'Give the copy a name first.' }
  // Windows reserves the part before the first dot, even with multiple extensions.
  if (DEVICE_NAMES.test(stem.split('.')[0].trimEnd())) {
    return { name: '', error: 'CON, PRN, AUX, NUL, COM1-9 and LPT1-9 are Windows device names. Choose another name.' }
  }
  if (typed.length > 200) return { name: '', error: 'That name is too long. Use 200 characters or fewer.' }
  return { name: named, error: '' }
}
