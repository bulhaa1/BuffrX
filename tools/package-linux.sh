#!/bin/bash
# Builds the Linux AppImage from the current engine and editor builds.
#
#   tools/package-linux.sh [version]
#
# The engine and UI are built by linux/CMakeLists.txt and src-tauri; the runtime staging comes from
# tools/prepare-linux-runtime.sh, which fetches the Ubuntu OBS packages without root.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="${1:-0.5.0}"
appdir="$root/work/BuffrX.AppDir"
out="$root/dist/BuffrX-${version}-x86_64.AppImage"
runtime="$root/work/linux-app/usr/lib/buffrx"

"$root/tools/prepare-linux-runtime.sh"

if [ ! -x "$root/build-linux/buffrx-engine" ]; then
  echo "the engine is not built; run: cmake -S linux -B build-linux && cmake --build build-linux -j8" >&2
  exit 1
fi
ui_binary=""
for candidate in "$root/work/cargo-target/release/buffrx-ui" "$root/src-tauri/target/release/buffrx-ui"; do
  if [ -x "$candidate" ]; then ui_binary="$candidate"; break; fi
done
if [ -z "$ui_binary" ]; then
  echo "the editor is not built; run cargo build --release in src-tauri" >&2
  exit 1
fi

rm -rf "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/lib" "$appdir/usr/share/icons/hicolor/256x256/apps" "$appdir/usr/share/applications"
install -m 0755 "$root/build-linux/buffrx-engine" "$appdir/usr/bin/buffrx-engine"
install -m 0755 "$ui_binary" "$appdir/usr/bin/buffrx-ui"
install -m 0755 "$root/build-linux/buffrx-pathshim.so" "$appdir/usr/lib/libbuffrx-pathshim.so"
cp -a "$runtime" "$appdir/usr/lib/buffrx"
cp "$root/linux/appimage/AppRun" "$appdir/AppRun"
chmod +x "$appdir/AppRun"
cp "$root/linux/appimage/buffrx.desktop" "$appdir/buffrx.desktop"
cp "$root/linux/appimage/buffrx.desktop" "$appdir/usr/share/applications/buffrx.desktop"
cp "$root/linux/appimage/buffrx.png" "$appdir/buffrx.png"
cp "$root/linux/appimage/buffrx.png" "$appdir/usr/share/icons/hicolor/256x256/apps/buffrx.png"
# GPL compliance: the product licence and the notices of the OBS/FFmpeg components bundled beside it.
mkdir -p "$appdir/licenses"
cp "$root/LICENSE" "$appdir/licenses/BuffrX-GPL-2.0.txt"
for package in libobs0t64 obs-studio libavfilter-dev libsrt1.5-gnutls libmbedtls14t64 libmbedx509-1t64 libcurl4-openssl-dev; do
  if [ -f "$root/work/linux-runtime/usr/share/doc/$package/copyright" ]; then
    cp "$root/work/linux-runtime/usr/share/doc/$package/copyright" "$appdir/licenses/$package-copyright.txt"
  fi
done
cat > "$appdir/licenses/README.txt" <<'TXT'
BuffrX is free software under the GNU General Public License, version 2 or later. The complete source
of this build is the repository the AppImage was made from (including src/linux for the port layers).

This AppImage bundles libobs (OBS Studio 30.0.2, GPL-2.0-or-later), its plugins, and FFmpeg libraries.
Their copyright notices are in this folder. The engine's OBS runtime and FFmpeg are dynamically linked
and remain separate programs under their own licences.

The editor UI is linked against the system's WebKitGTK and GTK 3; those are not bundled.
TXT

# Strip the product binaries: a 72 MB debug engine is not a download.
strip --strip-unneeded "$appdir/usr/bin/buffrx-engine" "$appdir/usr/bin/buffrx-ui" 2>/dev/null || true

python3 - "$appdir" "$root" <<'PY'
import os, re, shutil, subprocess, sys
appdir, root = sys.argv[1], sys.argv[2]
libdir = os.path.join(appdir, 'usr', 'lib')
os.makedirs(libdir, exist_ok=True)
block = re.compile(r'''^(
  ld-linux|libc\.|libm\.|libpthread|libdl\.|librt\.|libresolv|libutil|libanl|libnsl|
  libGL|libEGL|libGLX|libOpenGL|libdrm|libgbm|libva|libvdpau|libnvidia|libcuda|
  libX11|libxcb|libwayland|libXext|libXrandr|libXinerama|libXcomposite|libXfixes|libXrender|
  libXi\.|libSM|libICE|libXau|libXdmcp|libxkbcommon|
  libgtk-3|libgdk-3|libgdk_pixbuf|libglib-2|libgobject|libgio-2|libgmodule|libgthread|
  libpango|libcairo|libharfbuzz|libatk|libat-spi|libwebkit2gtk|libjavascriptcoregtk|libsoup|libgst|liborc|libwpe|libepoxy|libnice|libenchant|libgspell|libsecret|libwoff|libxslt|libyaml|libgudev|libmanette|libhyphen|
  libjson-glib|libxml2|libfontconfig|libfreetype|libpng|libjpeg|libtiff|libwebp|
  libsystemd|libdbus-1|libudev|libselinux|libapparmor|libcap|libmount|libblkid|libpulse|libasound|
  libz\.|libzstd|liblz4|liblzma|libbz2|libpcre|libffi|libexpat|libuuid|libgcc_s|libstdc
).*$''', re.X)
seen = set()

def dependencies(file):
    env = dict(os.environ)
    env['LD_LIBRARY_PATH'] = os.pathsep.join([
        os.path.join(appdir, 'usr', 'lib'),
        os.path.join(appdir, 'usr', 'lib', 'buffrx', 'libobs'),
        os.path.join(appdir, 'usr', 'lib', 'buffrx', 'obs-plugins'),
        os.path.join(root, 'work', 'linux-runtime', 'usr', 'lib', 'x86_64-linux-gnu'),
    ])
    try:
        text = subprocess.run(['ldd', file], capture_output=True, text=True, env=env).stdout
    except FileNotFoundError:
        return []
    found = []
    for line in text.splitlines():
        line = line.strip()
        match = re.match(r'^(\S+)\s+=>\s+(\S+)\s+\(0x', line)
        if not match:
            continue
        soname, path = match.group(1), match.group(2)
        if path.startswith('/'):
            found.append((soname, path))
    return found

def copy_library(soname, path):
    if soname in seen:
        return
    seen.add(soname)
    destination = os.path.join(libdir, soname)
    if not os.path.exists(destination):
        shutil.copy2(path, destination)
        os.chmod(destination, 0o755)
    for dependency, dep_path in dependencies(path):
        if block.match(dependency):
            continue
        copy_library(dependency, dep_path)

targets = [os.path.join(appdir, 'usr', 'bin', name) for name in ('buffrx-engine', 'buffrx-ui')]
targets.append(os.path.join(appdir, 'usr', 'lib', 'libbuffrx-pathshim.so'))
plugins = os.path.join(appdir, 'usr', 'lib', 'buffrx', 'obs-plugins')
if os.path.isdir(plugins):
    targets += [os.path.join(plugins, name) for name in os.listdir(plugins) if name.endswith('.so')]
for target in targets:
    for soname, path in dependencies(target):
        if block.match(soname):
            continue
        copy_library(soname, path)
print('bundled libraries:', len(seen))
PY

# The appimage tool is fetched once; it is a build tool, not part of the product.
tool="$root/work/tools/appimagetool-x86_64.AppImage"
if [ ! -x "$tool" ]; then
  mkdir -p "$root/work/tools"
  curl -fL --retry 3 -o "$tool" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
  chmod +x "$tool"
fi

mkdir -p "$root/dist"
rm -f "$out"
ARCH=x86_64 "$tool" --appimage-extract-and-run "$appdir" "$out"
echo "built $out"
