# Builds the peer-to-peer live-link spike (docs/share-spike.md) with the pinned toolchain, into
# build-share-spike/. Run tools/bootstrap-toolchain.ps1 -ShareSpike first. Nothing here is staged or
# packaged.
#   powershell -File tools/build-share-spike.ps1

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$paths = Get-Content -LiteralPath (Join-Path $root 'work\paths.json') -Raw | ConvertFrom-Json
$tools = Get-Content -LiteralPath (Join-Path $root 'work\toolchain.json') -Raw | ConvertFrom-Json
if (-not $tools.libdatachannel_root) { throw 'work/toolchain.json has no libdatachannel_root: run tools/bootstrap-toolchain.ps1 -ShareSpike' }

# libdatachannel builds its submodules from deps/<name>; its release archive leaves those empty, so the
# pinned sources go there once.
foreach ($dep in @(@('libjuice', $tools.libjuice_root), @('usrsctp', $tools.usrsctp_root), @('plog', $tools.plog_root))) {
  $target = Join-Path $tools.libdatachannel_root ('deps\' + $dep[0])
  if (-not (Test-Path (Join-Path $target 'CMakeLists.txt'))) {
    New-Item -ItemType Directory -Force $target | Out-Null
    Copy-Item -Path (Join-Path $dep[1] '*') -Destination $target -Recurse -Force
    Write-Host "$($dep[0]) placed in libdatachannel/deps"
  }
}

$build = Join-Path $root 'build-share-spike'
$common = @(
  '-G', 'Ninja',
  "-DCMAKE_MAKE_PROGRAM=$($paths.ninja)",
  "-DCMAKE_C_COMPILER=$($paths.clang)",
  "-DCMAKE_CXX_COMPILER=$($paths.clangxx)",
  '-DCMAKE_SYSTEM_NAME=Windows',
  '-DCMAKE_BUILD_TYPE=Release'
)

# Mbed TLS first, installed into the build folder: the libraries only, static.
$mbedInstall = Join-Path $build 'mbedtls-install'
if (-not (Test-Path (Join-Path $mbedInstall 'include\mbedtls\ssl.h'))) {
  $mbedBuild = Join-Path $build 'mbedtls'
  & $paths.cmake -S $tools.mbedtls_root -B $mbedBuild @common "-DCMAKE_INSTALL_PREFIX=$mbedInstall" `
    -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DMBEDTLS_FATAL_WARNINGS=OFF `
    "-DMBEDTLS_USER_CONFIG_FILE=$((Join-Path $root 'src\share-spike\mbedtls_user_config.h') -replace '\\', '/')" `
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON
  if ($LASTEXITCODE -ne 0) { throw 'Mbed TLS configure failed' }
  & $paths.cmake --build $mbedBuild --target install
  if ($LASTEXITCODE -ne 0) { throw 'Mbed TLS build failed' }
}

& $paths.cmake -S (Join-Path $root 'src\share-spike') -B $build @common `
  "-DCMAKE_PREFIX_PATH=$($mbedInstall -replace '\\', '/')" `
  "-DBUFFRX_LIBDATACHANNEL_ROOT=$($tools.libdatachannel_root -replace '\\', '/')"
if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
& $paths.cmake --build $build --target buffrx-share-spike-tool
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
Write-Output "SPIKE_BUILT $(Join-Path $build 'buffrx-share-spike-tool.exe')"
