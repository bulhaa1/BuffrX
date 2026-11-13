#!/bin/bash
# Fetches the Linux build dependencies without root: pinned OBS/FFmpeg packages are downloaded with
# apt-get and unpacked under work/linux-runtime, Dear ImGui/GLFW/stb and the fonts under
# work/toolchain-linux, and a local Rust toolchain under work/cargo when cargo is not installed.
#
# It does not install system packages. The header packages the build needs (GTK 3, X11/Xrandr,
# FFmpeg, OpenSSL) are expected from the distribution; see README.md.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
apt="$root/work/apt-linux"
runtime="$root/work/linux-runtime"
tool="$root/work/toolchain-linux"
mkdir -p "$apt" "$runtime" "$tool" "$root/work/linux-ui/lib" "$root/work/linux-ui/pkgconfig"

# ---------------------------------------------------------------- OBS + FFmpeg development files
cd "$apt"
packages=(
  libobs-dev libobs0t64 obs-studio libsimde-dev
  libsrt1.5-gnutls libmbedtls14t64 libmbedx509-1t64
  libavfilter-dev libcurl4-openssl-dev
)
for package in "${packages[@]}"; do
  # apt-get download refuses to overwrite an existing .deb; reuse it instead.
  if ! ls "${package}"_*.deb >/dev/null 2>&1; then
    apt-get download "$package" >/dev/null
  fi
done
for deb in *.deb; do
  dpkg-deb -x "$deb" "$runtime"
done
# The -dev packages ship a symlink to the runtime SONAME, which is not part of the -dev package.
libdir="$runtime/usr/lib/x86_64-linux-gnu"
ln -sf libsrt-gnutls.so.1.5 "$libdir/libsrt.so.1.5"
for source in /usr/lib/x86_64-linux-gnu/libavfilter.so.9* /usr/lib/x86_64-linux-gnu/libcurl.so.4*; do
  [ -e "$source" ] && cp -P "$source" "$libdir/" 2>/dev/null || true
done
# Ubuntu's obs-ffmpeg hardcodes its muxer helper at /usr/lib/...; the engine puts a copy at /tmp so
# an AppImage can run it. The replacement is exactly as long as the original at most, NUL-padded.
python3 - "$libdir/obs-plugins/obs-ffmpeg.so" <<'PY'
import sys
path = sys.argv[1]
try:
    data = open(path, 'rb').read()
    old = b'/usr/lib/x86_64-linux-gnu/obs-plugins/obs-ffmpeg/obs-ffmpeg-mux'
    new = b'/tmp/buffrx-obs-ffmpeg-mux'
    if old in data:
        open(path, 'wb').write(data.replace(old, new + b'\0' * (len(old) - len(new))))
        print('patched the obs-ffmpeg muxer path')
except FileNotFoundError:
    pass
PY

# ---------------------------------------------------------------- Dear ImGui, GLFW, stb, fonts
cd "$tool"
if [ ! -d imgui-1.92.9b ]; then
  curl -fL --retry 3 -o imgui.tgz https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
  tar xf imgui.tgz
fi
if [ ! -d glfw-3.4 ]; then
  curl -fL --retry 3 -o glfw.tgz https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
  tar xf glfw.tgz
fi
if [ ! -f stb_image_write.h ]; then
  curl -fL --retry 3 -o stb_image_write.h https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h
fi
if [ ! -d jb/JetBrainsMono-2.304 ]; then
  mkdir -p jb
  curl -fL --retry 3 -o jetbrains.zip https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip
  unzip -q -o jetbrains.zip -d jb
fi

# ---------------------------------------------------------------- WebKit/GTK pkg-config for Tauri
pkg="$root/work/linux-ui/pkgconfig"
libs="$root/work/linux-ui/lib"
for library in webkit2gtk-4.1 javascriptcoregtk-4.1 soup-3.0; do
  source=$(ls /usr/lib/x86_64-linux-gnu/lib${library}.so.0 2>/dev/null | head -1)
  [ -n "$source" ] && ln -sf "$source" "$libs/lib${library}.so"
done
version=$(dpkg-query -W -f='${Version}' libwebkit2gtk-4.1-0 2>/dev/null || echo 2.52.6)
cat > "$pkg/webkit2gtk-4.1.pc" <<PC
prefix=/usr
exec_prefix=\${prefix}
libdir=$libs
includedir=/usr/include/webkitgtk-4.1

Name: webkit2gtk-4.1
Description: WebKitGTK for Tauri
Version: ${version%%-*}
Requires: javascriptcoregtk-4.1 libsoup-3.0 gtk+-3.0
Libs: -L\${libdir} -lwebkit2gtk-4.1
Cflags: -I\${includedir}
PC
cat > "$pkg/javascriptcoregtk-4.1.pc" <<PC
prefix=/usr
exec_prefix=\${prefix}
libdir=$libs
includedir=/usr/include/webkitgtk-4.1

Name: javascriptcoregtk-4.1
Description: JavaScriptCore for WebKitGTK
Version: ${version%%-*}
Requires: glib-2.0 gobject-2.0
Libs: -L\${libdir} -ljavascriptcoregtk-4.1
Cflags: -I\${includedir}
PC
cat > "$pkg/libsoup-3.0.pc" <<PC
prefix=/usr
exec_prefix=\${prefix}
libdir=$libs
includedir=/usr/include/libsoup-3.0

Name: libsoup-3.0
Description: Soup HTTP library
Version: 3.4.4
Requires: glib-2.0 gobject-2.0 gio-2.0
Libs: -L\${libdir} -lsoup-3.0
Cflags: -I\${includedir}
PC

# ---------------------------------------------------------------- Rust and the editor icon
if ! command -v cargo >/dev/null 2>&1; then
  if [ ! -x "$root/work/cargo/bin/cargo" ]; then
    curl -fL --retry 3 -o "$tool/rustup-init.sh" https://sh.rustup.rs
    RUSTUP_HOME="$root/work/rustup" CARGO_HOME="$root/work/cargo" sh "$tool/rustup-init.sh" -y --profile minimal --default-toolchain stable --no-modify-path
  fi
fi
if [ ! -f "$root/src-tauri/icons/icon.png" ]; then
  python3 - "$root/src-tauri/icons/icon.ico" "$root/src-tauri/icons/icon.png" <<'PY'
import sys
from PIL import Image
Image.open(sys.argv[1]).save(sys.argv[2])
PY
fi
if [ ! -d "$root/work/toolchain-linux/node-v22.14.0-linux-x64" ]; then
  cd "$tool"
  curl -fL --retry 3 -o node.tar.xz https://nodejs.org/dist/v22.14.0/node-v22.14.0-linux-x64.tar.xz
  tar xf node.tar.xz
fi
echo "linux build dependencies are staged under work/"
echo "next: cmake -S linux -B build-linux && cmake --build build-linux -j8"
