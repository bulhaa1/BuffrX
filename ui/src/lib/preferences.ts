/** The export choices the editor remembers between clips and launches. */
export interface ExportDefaults {
  quality: number
  resolution: string
}

const KEY = 'buffrx.editor.export'

/** Reads a stored value; anything malformed falls back to the product's defaults. */
export function parseExportDefaults(text: string | null): ExportDefaults {
  if (!text) return { quality: 0, resolution: 'original' }
  try {
    const value = JSON.parse(text) as Partial<ExportDefaults>
    const quality = typeof value.quality === 'number' && Number.isFinite(value.quality) ? value.quality : 0
    const resolution =
      typeof value.resolution === 'string' && /^(original|\d{3,4})$/.test(value.resolution) ? value.resolution : 'original'
    return { quality, resolution }
  } catch {
    return { quality: 0, resolution: 'original' }
  }
}

export function loadExportDefaults(): ExportDefaults {
  try {
    return parseExportDefaults(localStorage.getItem(KEY))
  } catch {
    return parseExportDefaults(null)
  }
}

export function saveExportDefaults(value: ExportDefaults) {
  try {
    localStorage.setItem(KEY, JSON.stringify(value))
  } catch {
    // A private window or a full quota: the choice simply is not remembered this time.
  }
}
