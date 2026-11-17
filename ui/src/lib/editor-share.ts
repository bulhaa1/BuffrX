/** Share only the saved library copy that matches the edit currently on screen. */
export function canShareSaved(saved: string, current: string, inLibrary: boolean): boolean {
  return inLibrary && saved === current
}

/** The Share sheet belongs to the panel: leave the editor for it only once the engine took the file.
 *  A refused share (not in the library, engine gone) keeps the editor where the person still is. */
export async function shareThenLeave(share: () => Promise<unknown>, leave: () => void): Promise<void> {
  await share()
  leave()
}
