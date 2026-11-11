// The fourteen theme ids, the same table as src/engine/themes.h (the colours live in themes.css, which
// the engine tests hold to that table). Clutch is the main theme.
export const THEME_IDS = [
  'clutch',
  'overtime',
  'respawn',
  'afterglow',
  'killcam',
  'lobby',
  'headshot',
  'frostbite',
  'toxic',
  'nightshift',
  'highscore',
  'ghillie',
  'sandstorm',
  'arcade',
] as const

export type ThemeId = (typeof THEME_IDS)[number]

/** The theme to paint for a stored id: an older cockpit (auto, green, red, blue) or an unknown id is the main one. */
export function themeId(stored: string | undefined): ThemeId {
  return THEME_IDS.find((id) => id === stored) ?? 'clutch'
}
