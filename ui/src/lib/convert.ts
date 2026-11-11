// snake_case <-> camelCase, the one translation between the engine and the interface (AGENTS.md:
// convert once, in the bridge layer). Pure, so it is tested on its own.

export type Json = Record<string, unknown>

const camel = (key: string) => key.replace(/_([a-z0-9])/g, (_, c: string) => c.toUpperCase())
const snake = (key: string) => key.replace(/[A-Z]/g, (c) => `_${c.toLowerCase()}`)
// Values that are maps keyed by someone else's names (libobs settings, executables) are carried
// through untouched.
const OPAQUE = new Set(['encoder_options', 'encoderOptions', 'problems', 'patch'])

function convert(value: unknown, rename: (key: string) => string): unknown {
  if (Array.isArray(value)) return value.map((item) => convert(item, rename))
  if (value && typeof value === 'object') {
    const out: Json = {}
    for (const [key, inner] of Object.entries(value as Json)) {
      out[rename(key)] = OPAQUE.has(key) ? inner : convert(inner, rename)
    }
    return out
  }
  return value
}

export const fromEngine = <T>(value: unknown) => convert(value, camel) as T
export const toEngine = (value: unknown) => convert(value, snake) as Json
