#!/bin/bash
# Stages the Linux OBS runtime, fonts and engine layout under work/linux-app.
# The packages in work/linux-runtime were downloaded with apt-get and unpacked without root.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
runtime="$root/work/linux-runtime"
stage="$root/work/linux-app/usr/lib/buffrx"
rm -rf "$stage"
mkdir -p "$stage"/{libobs,obs-plugins,data/libobs,data/obs-plugins,data/buffrx/fonts}

src="$runtime/usr/lib/x86_64-linux-gnu"
cp -P "$src"/libobs.so* "$src"/libobs-opengl.so* "$stage/libobs/"
ln -sf libobs.so.30 "$stage/libobs/libobs.so.0" 2>/dev/null || true
ln -sf libobs-opengl.so.30 "$stage/libobs/libobs-opengl.so" 2>/dev/null || true
ln -sf libobs.so.30 "$stage/libobs/libobs.so" 2>/dev/null || true

for module in linux-capture linux-pulseaudio obs-ffmpeg obs-x264 obs-outputs image-source obs-filters; do
  cp "$src/obs-plugins/$module.so" "$stage/obs-plugins/"
  if [ -d "$runtime/usr/share/obs/obs-plugins/$module" ]; then
    mkdir -p "$stage/data/obs-plugins/$module"
    cp -r "$runtime/usr/share/obs/obs-plugins/$module/." "$stage/data/obs-plugins/$module/"
  fi
done
# Ubuntu's obs-ffmpeg hardcodes its muxer helper at /usr/lib/...; the engine stages a copy in a
# private per-user folder and exports its path as BUFFRX_MUXER. Patch only the pristine package copy.
python3 - "$stage/obs-plugins/obs-ffmpeg.so" <<'PY'
import sys
path = sys.argv[1]
data = open(path, 'rb').read()
new = b'$BUFFRX_MUXER'
for old in (b'/usr/lib/x86_64-linux-gnu/obs-plugins/obs-ffmpeg/obs-ffmpeg-mux',
            b'/tmp/buffrx-obs-ffmpeg-mux'):
    if old in data:
        open(path, 'wb').write(data.replace(old, new + b'\0' * (len(old) - len(new))))
        print('patched the obs-ffmpeg muxer path')
        break
PY
cp "$src/obs-plugins/obs-ffmpeg/obs-ffmpeg-mux" "$stage/obs-ffmpeg-mux"
chmod +x "$stage/obs-ffmpeg-mux"
cp -r "$runtime/usr/share/obs/libobs/." "$stage/data/libobs/"

fonts="$root/work/toolchain-linux/jb/JetBrainsMono-2.304/ttf"
cp "$fonts/JetBrainsMonoNL-Regular.ttf" "$fonts/JetBrainsMonoNL-Medium.ttf" \
   "$fonts/JetBrainsMonoNL-Bold.ttf" "$fonts/JetBrainsMonoNL-ExtraBold.ttf" "$stage/data/buffrx/fonts/"
echo "staged into $stage"
