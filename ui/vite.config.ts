import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

export default defineConfig({
  plugins: [react(), tailwindcss()],
  // Three pages: the editor, the small clip viewer, and the Timeline preview. The last two must not load
  // the editor's bundle.
  build: {
    rollupOptions: {
      input: { main: 'index.html', timeline: 'timeline.html' },
    },
  },
})
