/** Share only the saved library copy that matches the edit currently on screen. */
export function canShareSaved(saved: string, current: string, inLibrary: boolean): boolean {
  return inLibrary && saved === current
}
