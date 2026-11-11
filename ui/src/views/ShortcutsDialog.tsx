// The editor's keyboard and wheel, as one sheet. It reads the same table the key handler does
// (lib/editor_keys.ts), so what is listed is what works.

import { Button, DialogBox } from '../components/ui'
import { SHORTCUTS } from '../lib/editor_keys'

export function ShortcutsDialog({ isOpen, onClose }: { isOpen: boolean; onClose: () => void }) {
  return (
    <DialogBox
      isOpen={isOpen}
      onClose={onClose}
      title="Keyboard"
      width={760}
      actions={<Button variant="ghost" onPress={onClose}>Close</Button>}
    >
      <div className="grid grid-cols-2 gap-x-8">
        {[0, 1].map((column) => (
          <div key={column} className="flex flex-col gap-5">
            {SHORTCUTS.filter((_, i) => i % 2 === column).map((group) => (
              <section key={group.group}>
                <h4 className="label mb-2">{group.group}</h4>
                <dl className="flex flex-col gap-1.5">
                  {group.items.map((row) => (
                    <div key={row.keys} className="flex items-baseline justify-between gap-3">
                      <dt className="min-w-0 text-[13px] leading-snug text-ink">{row.label}</dt>
                      <dd className="num shrink-0 whitespace-nowrap border border-line-strong bg-canvas px-1.5 text-[12px] leading-[18px] text-muted">{row.keys}</dd>
                    </div>
                  ))}
                </dl>
              </section>
            ))}
          </div>
        ))}
      </div>
    </DialogBox>
  )
}
