// The interface's icon set: hand-drawn 24-unit strokes, one component, no icon library.

const PATHS: Record<string, string> = {
  save: 'M12 4v10m0 0-4-4m4 4 4-4M5 17v2a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-2',
  bolt: 'M13 3 5 13h6l-1 8 8-10h-6l1-8Z',
  record: 'M12 12m-6 0a6 6 0 1 0 12 0a6 6 0 1 0-12 0',
  stop: 'M7 7h10v10H7z',
  camera: 'M4 8a2 2 0 0 1 2-2h2l1.5-2h5L16 6h2a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V8Zm8 1a3.5 3.5 0 1 0 0 7 3.5 3.5 0 0 0 0-7Z',
  settings: 'M12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6Zm7.4 3a7.4 7.4 0 0 0-.1-1.2l2-1.6-2-3.4-2.4 1a7.6 7.6 0 0 0-2-1.2L14.5 3h-4l-.4 2.6a7.6 7.6 0 0 0-2 1.2l-2.4-1-2 3.4 2 1.6a7.4 7.4 0 0 0 0 2.4l-2 1.6 2 3.4 2.4-1a7.6 7.6 0 0 0 2 1.2l.4 2.6h4l.4-2.6a7.6 7.6 0 0 0 2-1.2l2.4 1 2-3.4-2-1.6c.1-.4.1-.8.1-1.2Z',
  scissors: 'M6 9a3 3 0 1 0 0-6 3 3 0 0 0 0 6Zm0 12a3 3 0 1 0 0-6 3 3 0 0 0 0 6ZM8.2 7.8 20 18M8.2 16.2 20 6',
  folder: 'M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7Z',
  minus: 'M6 12h12',
  square: 'M6 6h12v12H6z',
  close: 'M6 6l12 12M18 6 6 18',
  compact: 'M4 14h6v6M20 10h-6V4M14 10l7-7M3 21l7-7',
  expand: 'M15 3h6v6M9 21H3v-6M21 3l-7 7M3 21l7-7',
  grid: 'M4 4h7v7H4zM13 4h7v7h-7zM4 13h7v7H4zM13 13h7v7h-7z',
  pin: 'M9 4h6l-1 6 3 3H7l3-3-1-6Zm3 9v7',
  play: 'M8 5v14l11-7L8 5Z',
  pause: 'M8 5h3v14H8zM13 5h3v14h-3z',
  dots: 'M5 12h.01M12 12h.01M19 12h.01',
  mic: 'M12 3a3 3 0 0 0-3 3v6a3 3 0 0 0 6 0V6a3 3 0 0 0-3-3Zm-6 9a6 6 0 0 0 12 0M12 18v3',
  micOff: 'M9 9v3a3 3 0 0 0 5.1 2.1M15 9.3V6a3 3 0 0 0-5.9-.8M6 12a6 6 0 0 0 10 4.5M18 12a6 6 0 0 1-.4 2.1M12 18v3M4 4l16 16',
  speaker: 'M4 9v6h4l5 4V5L8 9H4Zm12.5-.5a5 5 0 0 1 0 7M19 6a8.5 8.5 0 0 1 0 12',
  speakerOff: 'M4 9v6h4l5 4V5L8 9H4Zm12 1 5 5m0-5-5 5',
  check: 'M5 12.5 10 17 19 7',
  alert: 'M12 8v5m0 3.5h.01M10.3 3.9 2.6 17.5A2 2 0 0 0 4.3 20.5h15.4a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0Z',
  info: 'M12 11v6m0-9.5h.01M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18Z',
  copy: 'M9 9h10v12H9zM5 15V3h10',
  share: 'M12 15V3m-5 5 5-5 5 5M5 13v7h14v-7',
  trash: 'M4 7h16M10 11v6m4-6v6M6 7l1 13h10l1-13M9 7V4h6v3',
  edit: 'M4 20h4L19 9l-4-4L4 16v4Zm9-13 4 4',
  external: 'M14 4h6v6M20 4l-9 9M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5',
  chevronLeft: 'M15 5l-7 7 7 7',
  chevronRight: 'M9 5l7 7-7 7',
  chevronDown: 'M5 9l7 7 7-7',
  palette: 'M12 3a9 9 0 1 0 0 18c1 0 1.6-.8 1.6-1.7 0-.4-.2-.8-.4-1.1-.3-.3-.4-.7-.4-1.1 0-.9.7-1.6 1.6-1.6H16a5 5 0 0 0 5-5c0-4.1-4-7.5-9-7.5ZM7.5 12h.01M10 7.5h.01M15 8h.01',
  game: 'M7 9v4m-2-2h4m6 1h.01M18 10h.01M7 5h10a5 5 0 0 1 5 5v2.5a4.5 4.5 0 0 1-8 2.8l-.8-1h-2.4l-.8 1a4.5 4.5 0 0 1-8-2.8V10a5 5 0 0 1 5-5Z',
  monitor: 'M3 5h18v11H3zM8 20h8M12 16v4',
  window: 'M3 5h18v14H3zM3 9h18',
  film: 'M4 4h16v16H4zM8 4v16M16 4v16M4 8h4M4 12h4M4 16h4M16 8h4M16 12h4M16 16h4',
  image: 'M4 5h16v14H4zM4 16l5-5 4 4 2-2 5 5M15 9h.01',
  plus: 'M12 5v14M5 12h14',
  crop: 'M6 2v14a2 2 0 0 0 2 2h14M18 22V8a2 2 0 0 0-2-2H2',
  refresh: 'M20 11a8 8 0 1 0-2.3 5.7M20 4v7h-7',
  power: 'M12 3v8M6.3 7.3a8 8 0 1 0 11.4 0',
  search: 'M11 18a7 7 0 1 0 0-14 7 7 0 0 0 0 14Zm5-2 4 4',
  keyboard: 'M3 7h18v10H3zM7 11h.01M11 11h.01M15 11h.01M8 14h8',
  sliders: 'M4 6h10M18 6h2M4 12h4M12 12h8M4 18h12M20 18h0M14 4v4M8 10v4M16 16v4',
  layers: 'M12 3 3 8l9 5 9-5-9-5Zm-9 9 9 5 9-5M3 16l9 5 9-5',
  hdd: 'M4 14h16v5H4zM4 14l2.5-9h11L20 14M8 17h.01',
  bell: 'M6 16V11a6 6 0 1 1 12 0v5l2 2H4l2-2Zm4 4h4',
  cpu: 'M8 8h8v8H8zM9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3',
  rewind: 'M11 19 2 12l9-7v14Zm11 0-9-7 9-7v14Z',
  forward: 'M13 5v14l9-7-9-7Zm-11 0v14l9-7-9-7Z',
  sun: 'M12 16a4 4 0 1 0 0-8 4 4 0 0 0 0 8ZM12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4',
  contrast: 'M12 21a9 9 0 1 0 0-18v18Z',
  drop: 'M12 3s-6 6.5-6 11a6 6 0 0 0 12 0c0-4.5-6-11-6-11Z',
  undo: 'M9 14 4 9l5-5M4 9h11a5 5 0 0 1 0 10h-3',
  gauge: 'M3.5 17a9 9 0 1 1 17 0M12 14l4-4',
  rotateCw: 'M20 11a8 8 0 1 0-2.3 5.7M20 4v7h-7',
  rotateCcw: 'M4 11a8 8 0 1 1 2.3 5.7M4 4v7h7',
  flipH: 'M12 3v18M8 7 3 12l5 5V7ZM16 7l5 5-5 5V7Z',
  flipV: 'M3 12h18M7 8l5-5 5 5H7ZM7 16l5 5 5-5H7Z',
  arrow: 'M5 19 19 5M9 5h10v10',
  text: 'M5 6V4h14v2M12 4v16M9 20h6',
  blur: 'M4 4h16v16H4zM4 12h16M12 4v16',
  skipBack: 'M6 5v14M19 5v14L9 12l10-7Z',
  skipForward: 'M18 5v14M5 5v14l10-7L5 5Z',
  zoomIn: 'M11 18a7 7 0 1 0 0-14 7 7 0 0 0 0 14Zm5-2 4 4M8.5 11h5M11 8.5v5',
  zoomOut: 'M11 18a7 7 0 1 0 0-14 7 7 0 0 0 0 14Zm5-2 4 4M8.5 11h5',
}

export type IconName = keyof typeof PATHS

export function Icon({ name, size = 16, className = '', stroke = 1.7, fill = false }: {
  name: IconName
  size?: number
  className?: string
  stroke?: number
  fill?: boolean
}) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill={fill ? 'currentColor' : 'none'}
      stroke={fill ? 'none' : 'currentColor'}
      strokeWidth={stroke}
      strokeLinecap="round"
      strokeLinejoin="round"
      className={className}
      aria-hidden="true"
    >
      <path d={PATHS[name]} />
    </svg>
  )
}

/** The BuffrX mark (brand/buffrx-mark.svg), in the text colour so every theme paints it. `size` is the slot
 *  the titlebar reserves; the mark is 0.8 of it wide, like the native interface's. */
export function Mark({ size = 18 }: { size?: number }) {
  return (
    <svg aria-hidden="true" className="shrink-0 text-ink" width={size} height={size} viewBox="-18 35 360 360">
      <path fill="currentColor" d="M3.2 93L42.2 143.4L230 143.4C254.3 148.8 262 168.5 240 184.4C233.3 189.3 225.7 190.8 218 192.8L118.3 192.8L152.2 235.4L234 235.4C243.6 237.4 251.8 238.8 260 244.6C265.3 248.3 266.8 254.1 267.6 260C269.4 274.1 259.7 282.4 247 285.3L96 285.3L95.5 168L42.8 201.5L42.8 336.4L257 336.4C300.7 328.5 320.5 313.6 320.5 266C320.5 250 318.2 235.3 308 222.6C301.7 214.8 292 211.9 284.6 206C289.1 196.6 298.7 189.6 302.3 179C309.6 157.2 304.8 115.4 281 103.7C270.7 98.6 260.6 96.2 250 93L3.2 93Z" />
    </svg>
  )
}
