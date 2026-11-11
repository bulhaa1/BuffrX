// The editor process's media work: short-lived engine runs (--media-info, --media-peaks) whose last JSON
// line is the answer (src/engine/media_export.cpp).

import { shell } from './bridge'
import { createMediaWork } from './media_queue'

/** One engine run. `id` names the process, so a read the editor has left can be ended before it finishes. */
export async function runEngine(args: string[], id?: string): Promise<unknown> {
  const text = await shell<string>('media_run', { args, id: id ?? null })
  const line = text.trim().split(/\r?\n/).filter((l) => l.startsWith('{')).pop()
  if (!line) throw new Error('the engine gave no answer')
  return JSON.parse(line)
}

/** Ends a read by the name its run was given; the process is killed by the editor, not left to finish. */
export function cancelEngine(id: string) {
  void shell('media_read_cancel', { id }).catch(() => undefined)
}

/** The editor's keyframe and waveform reads: one engine process per file, however often a clip is visited. */
export const mediaWork = createMediaWork(runEngine, {}, cancelEngine)
