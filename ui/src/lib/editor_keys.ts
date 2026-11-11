// The editor's keyboard: one table from a key press to what it does, and the list the help sheet shows.
// The letters, the arrows and the Ctrl chords follow what video editors share (Premiere, Resolve, CapCut,
// Final Cut): Space plays, S / C / B cut at the playhead, Delete removes what is selected, I / O set the
// ends of a part, Up / Down step between clips, Ctrl+Z undoes. Pure, so the table is tested without a window.

export type EditorAction =
  | 'play'
  | 'split'
  | 'delete'
  | 'removeClip'
  | 'undo'
  | 'redo'
  | 'earlier'
  | 'later'
  | 'back1'
  | 'forward1'
  | 'back5'
  | 'forward5'
  | 'frameBack'
  | 'frameForward'
  | 'clipStart'
  | 'clipEnd'
  | 'previousClip'
  | 'nextClip'
  | 'sequenceStart'
  | 'sequenceEnd'
  | 'markIn'
  | 'markOut'
  | 'zoomIn'
  | 'zoomOut'
  | 'zoomFit'
  | 'resetTrim'
  | 'saveCopy'
  | 'saveShare'
  | 'browse'
  | 'help'
  | 'fullscreen'
  | 'exitFullscreen'

export interface KeyLike {
  key: string
  ctrlKey?: boolean
  metaKey?: boolean
  shiftKey?: boolean
  altKey?: boolean
}

/** What a key press means in the editor, or null when it means nothing here. */
export function editorAction(event: KeyLike): EditorAction | null {
  const mod = !!(event.ctrlKey || event.metaKey)
  const shift = !!event.shiftKey
  const alt = !!event.altKey
  const key = event.key.length === 1 ? event.key.toLowerCase() : event.key

  if (mod && !alt) {
    if (key === 'z') return shift ? 'redo' : 'undo'
    if (key === 'y') return 'redo'
    if (key === 's') return shift ? 'saveShare' : 'saveCopy'
    if (key === 'o') return 'browse'
    if (key === 'k' || key === 'b') return 'split'
    if (key === 'Home') return 'sequenceStart'
    if (key === 'End') return 'sequenceEnd'
    return null
  }
  if (mod) return null
  if (alt) {
    if (key === 'ArrowLeft') return 'earlier'
    if (key === 'ArrowRight') return 'later'
    return null
  }
  switch (key) {
    case ' ': return 'play'
    case 's': case 'c': case 'b': return shift ? null : 'split'
    case 'Delete': case 'Backspace': return shift ? 'removeClip' : 'delete'
    case 'ArrowLeft': return shift ? 'back5' : 'back1'
    case 'ArrowRight': return shift ? 'forward5' : 'forward1'
    case 'ArrowUp': return 'previousClip'
    case 'ArrowDown': return 'nextClip'
    case ',': return 'frameBack'
    case '.': return 'frameForward'
    case 'Home': return 'clipStart'
    case 'End': return 'clipEnd'
    case 'i': return 'markIn'
    case 'o': return 'markOut'
    case '=': case '+': return 'zoomIn'
    case '-': case '_': return 'zoomOut'
    case '\\': return 'zoomFit'
    case 'r': return 'resetTrim'
    case 'f': return 'fullscreen'
    case 'Escape': return 'exitFullscreen'
    case '?': case 'F1': return 'help'
    default: return null
  }
}

/** One row of the help sheet: the keys as they are written, what they do, and a press that proves it. */
export interface Shortcut {
  keys: string
  label: string
  action?: EditorAction
  press?: KeyLike
}

export const SHORTCUTS: { group: string; items: Shortcut[] }[] = [
  {
    group: 'Playback',
    items: [
      { keys: 'Space', label: 'Play or pause', action: 'play', press: { key: ' ' } },
      { keys: 'F / Esc', label: 'Full screen, and back', action: 'fullscreen', press: { key: 'f' } },
      { keys: 'Left / Right', label: 'Back or forward 1 s', action: 'back1', press: { key: 'ArrowLeft' } },
      { keys: 'Shift+Left / Right', label: 'Back or forward 5 s', action: 'back5', press: { key: 'ArrowLeft', shiftKey: true } },
      { keys: ', / .', label: 'One frame back or forward', action: 'frameBack', press: { key: ',' } },
      { keys: 'Up / Down', label: 'Previous or next clip', action: 'previousClip', press: { key: 'ArrowUp' } },
      { keys: 'Home / End', label: 'Clip start or end', action: 'clipStart', press: { key: 'Home' } },
      { keys: 'Ctrl+Home / End', label: 'Sequence start or end', action: 'sequenceStart', press: { key: 'Home', ctrlKey: true } },
    ],
  },
  {
    group: 'Edit',
    items: [
      { keys: 'S / C / B', label: 'Cut at the playhead', action: 'split', press: { key: 'c' } },
      { keys: 'Delete', label: 'Delete the part (the clip if alone)', action: 'delete', press: { key: 'Delete' } },
      { keys: 'Shift+Delete', label: 'Remove the clip', action: 'removeClip', press: { key: 'Delete', shiftKey: true } },
      { keys: 'I / O', label: 'Set the part start or end', action: 'markIn', press: { key: 'i' } },
      { keys: 'R', label: 'Reset the trim', action: 'resetTrim', press: { key: 'r' } },
      { keys: 'Alt+Left / Right', label: 'Move the clip earlier or later', action: 'earlier', press: { key: 'ArrowLeft', altKey: true } },
      { keys: 'Ctrl+Z', label: 'Undo', action: 'undo', press: { key: 'z', ctrlKey: true } },
      { keys: 'Ctrl+Y', label: 'Redo', action: 'redo', press: { key: 'y', ctrlKey: true } },
    ],
  },
  {
    group: 'Mouse wheel',
    items: [
      { keys: 'Scroll', label: 'Move the playhead' },
      { keys: 'Shift+Scroll', label: 'One frame at a time' },
      { keys: 'Ctrl+Scroll', label: 'Zoom around the playhead' },
    ],
  },
  {
    group: 'Timeline and files',
    items: [
      { keys: '+ / -', label: 'Zoom in or out', action: 'zoomIn', press: { key: '=' } },
      { keys: '\\', label: 'Fit the whole clip', action: 'zoomFit', press: { key: '\\' } },
      { keys: 'Ctrl+O', label: 'Browse for files', action: 'browse', press: { key: 'o', ctrlKey: true } },
      { keys: 'Ctrl+S', label: 'Save a copy', action: 'saveCopy', press: { key: 's', ctrlKey: true } },
      { keys: 'Ctrl+Shift+S', label: 'Save and share', action: 'saveShare', press: { key: 's', ctrlKey: true, shiftKey: true } },
      { keys: '?', label: 'This list', action: 'help', press: { key: '?', shiftKey: true } },
    ],
  },
]
