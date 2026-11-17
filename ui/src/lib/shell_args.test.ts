// The page and the editor's Rust side are one boundary with no compiler between them: `#[tauri::command]`
// turns `fn write_image(path: String, bytes_base64: String)` into a handler that reads `bytesBase64` from
// the page's object (tauri-macros `ArgumentCase::Camel` is the default). A snake_case key is not a typo the
// type checker can see; it arrives as "invalid args `bytesBase64` for command `write_image`: missing
// required key" when the user presses Save. This holds the two halves to each other the way test_themes
// holds the palettes and layout.test.ts holds the keys to their titles.

import { readFileSync, readdirSync } from 'node:fs'
import { test } from 'node:test'
import { strict as assert } from 'node:assert'

const rustSources = ['main.rs', 'media.rs', 'web_clip.rs', 'timeline_window.rs', 'pipe.rs'].map((name) => {
  try {
    return readFileSync(new URL(`../../../src-tauri/src/${name}`, import.meta.url), 'utf8')
  } catch {
    return ''
  }
})

/** One command's page-facing parameters: the ones the page passes, never one Tauri injects. */
type Command = { name: string; params: string[] }

/** Tauri fills these itself, so the page never sends them. */
const injected = /^(tauri::)?(State|WebviewWindow|AppHandle|Window)\b/

/** Splits on the commas that separate items, not the ones inside `<...>`, `(...)` or `[...]`. */
function splitTop(text: string): string[] {
  const parts: string[] = []
  let depth = 0
  let start = 0
  for (let at = 0; at < text.length; at++) {
    const char = text[at]
    if (char === '<' || char === '(' || char === '[') depth++
    else if (char === '>' || char === ')' || char === ']') depth--
    else if (char === ',' && depth === 0) {
      parts.push(text.slice(start, at))
      start = at + 1
    }
  }
  parts.push(text.slice(start))
  return parts.map((part) => part.trim()).filter(Boolean)
}

/** The identifier a Tauri command reads from the page for a Rust parameter of this name. */
const pageKey = (param: string) => param.replace(/_([a-z0-9])/g, (_, letter: string) => letter.toUpperCase())

const commands = new Map<string, Command>()
for (const source of rustSources) {
  const found = source.matchAll(/#\[tauri::command[^\]]*\]\s*(?:pub\s+)?(?:async\s+)?fn\s+(\w+)\s*\(([\s\S]*?)\)\s*(?:->[^{;]*)?\{/g)
  for (const [, name, raw] of found) {
    const params = splitTop(raw)
      .filter((entry) => !injected.test(entry.slice(entry.indexOf(':') + 1).trim()))
      .map((entry) => pageKey(entry.slice(0, entry.indexOf(':')).trim()))
      .filter(Boolean)
    commands.set(name, { name, params })
  }
}

/** Every `shell('command', { ... })` in the page, with the keys of its literal payload. */
function callSites(): { file: string; command: string; keys: string[] }[] {
  const root = new URL('../../src/', import.meta.url)
  const files = (readdirSync(root, { recursive: true }) as string[]).filter((name) => /\.tsx?$/.test(name))
  const sites: { file: string; command: string; keys: string[] }[] = []
  for (const file of files) {
    // A test reads other sources; only the page's own modules are call sites.
    if (/\.test\./.test(file)) continue
    const source = readFileSync(new URL(file.replace(/\\/g, '/'), root), 'utf8')
    const calls = source.matchAll(/\bshell(?:<[^>]*>)?\(\s*'(\w+)'\s*(?:,\s*\{)?/g)
    for (const call of calls) {
      const at = call.index! + call[0].length
      const keys: string[] = []
      if (call[0].endsWith('{')) {
        // Brace-matched, so a payload spread over lines (preview_proxy's) is read whole.
        let depth = 0
        let end = at - 1
        for (let from = at - 1; from < source.length; from++) {
          if (source[from] === '{') depth++
          else if (source[from] === '}' && --depth === 0) { end = from; break }
        }
        for (const entry of splitTop(source.slice(at, end))) keys.push(entry.includes(':') ? entry.slice(0, entry.indexOf(':')).trim() : entry)
      }
      sites.push({ file, command: call[1], keys })
    }
  }
  return sites
}

const sites = callSites()

test('the page was read, or this proves nothing', () => {
  assert.ok(commands.size > 10, `${commands.size} commands found`)
  assert.ok(sites.length > 10, `${sites.length} shell calls found`)
  assert.ok(commands.has('write_image'))
})

test('every key the page sends is a parameter the command reads', () => {
  const wrong: string[] = []
  for (const site of sites) {
    const command = commands.get(site.command)
    if (!command) continue  // a command another host answers (the panel's own WebView2)
    for (const key of site.keys) {
      if (!command.params.includes(key)) wrong.push(`${site.file}: shell('${site.command}') sends "${key}", the command reads ${JSON.stringify(command.params)}`)
    }
  }
  assert.deepEqual(wrong, [])
})

test('every parameter a command needs is sent', () => {
  const missing: string[] = []
  for (const site of sites) {
    const command = commands.get(site.command)
    if (!command) continue
    for (const param of command.params) {
      if (!site.keys.includes(param)) missing.push(`${site.file}: shell('${site.command}') omits "${param}"`)
    }
  }
  assert.deepEqual(missing, [])
})
