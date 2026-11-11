// A clip or screenshot thumbnail: the engine's cached one, or a screenshot itself, an icon while neither
// loads. The editor's file bin and library picker show these.
import clsx from 'clsx'
import { useEffect, useState } from 'react'
import { Icon } from './icons'
import type { MediaItem } from '../lib/api'
import { thumbnailUrl, mediaUrl } from '../lib/bridge'

export function Thumb({ item, className }: { item: MediaItem; className?: string }) {
  const [failed, setFailed] = useState(false)
  // F10: the picture fades in over the surface when it loads, instead of popping in (the motion tokens in index.css).
  const [loaded, setLoaded] = useState(false)
  // A thumbnail the engine made; for a screenshot without one, the image itself.
  const src = item.thumbnail ? thumbnailUrl(item.thumbnail) : item.kind === 'screenshot' ? mediaUrl(item.file) : ''
  useEffect(() => {
    setFailed(false)
    setLoaded(false)
  }, [src])
  return (
    <div className={clsx('relative overflow-hidden bg-canvas', className)}>
      {src && !failed ? (
        <img
          src={src}
          alt=""
          loading="lazy"
          decoding="async"
          draggable={false}
          // A picture the browser had already decoded can finish before React attaches onLoad: it is checked once here too.
          ref={(img) => {
            if (img?.complete && img.naturalWidth > 0) setLoaded(true)
          }}
          onLoad={() => setLoaded(true)}
          onError={() => setFailed(true)}
          className={clsx('h-full w-full object-cover transition-opacity [transition-duration:var(--bx-dur-base)] [transition-timing-function:var(--bx-ease-out)]', loaded ? 'opacity-100' : 'opacity-0')}
        />
      ) : (
        <div className="grid h-full w-full place-items-center text-line-strong">
          <Icon name={item.kind === 'clip' ? 'film' : 'image'} size={26} />
        </div>
      )}
    </div>
  )
}

