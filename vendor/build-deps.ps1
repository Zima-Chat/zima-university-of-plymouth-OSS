<#
build-deps.ps1 -- build vendored dependencies from source as static (/MT) libs.

Builds, from the pinned submodule sources in vendor\src\:
    libsodium  -> vendor\libsodium\{include, lib\x64\<Config>\libsodium.lib}
    OpenSSL    -> vendor\openssl\{include\openssl, lib\x64\<Config>\{libssl,libcrypto}.lib}
    curl       -> vendor\curl\{include\curl, lib\x64\<Config>\libcurl.lib}

All three are built with the static CRT (/MT, /MTd) so the final EXEs have no
runtime-DLL dependency. curl is built against the OpenSSL we just built.

Requirements (verified present on this machine):
    - Visual Studio 2022 (cl, link, nmake, msbuild)   -- located via vswhere
    - Strawberry Perl  (C:\Strawberry\perl\bin\perl.exe)  -- for OpenSSL Configure
    - NASM             (C:\Strawberry\c\bin\nasm.exe)      -- for OpenSSL asm
    - CMake 3.25+                                          -- for curl

Usage (from anywhere):
    powershell -ExecutionPolicy Bypass -File vendor\build-deps.ps1            # Release
    powershell -ExecutionPolicy Bypass -File vendor\build-deps.ps1 -Config Both
    powershell -ExecutionPolicy Bypass -File vendor\build-deps.ps1 -Clean
#>
[CmdletBinding()]
param(
    [ValidateSet("Release","Debug","Both")]
    [string]$Config = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root  = $PSScriptRoot                    # vendor\
$src   = Join-Path $root "src"
$build = Join-Path $root "_build"

# Pinned versions (must match the submodule tags).
$SODIUM_VERSION = "1.0.20"
$SODIUM_VER_MAJOR = 26
$SODIUM_VER_MINOR = 2

# Tool locations.
$PERL     = "C:\Strawberry\perl\bin\perl.exe"
$NASM_DIR = "C:\Strawberry\c\bin"

function EnsureDir($p) { if (-not (Test-Path $p)) { New-Item -ItemType Directory -Force $p | Out-Null } }
function Section($t)   { Write-Host ""; Write-Host "==================== $t ====================" -ForegroundColor Cyan }

if ($Clean) {
    Section "CLEAN"
    foreach ($d in $build,"$root\libsodium","$root\openssl","$root\curl") {
        if (Test-Path $d) { Remove-Item $d -Recurse -Force; Write-Host "removed $d" }
    }
    Write-Host "Clean complete."
    return
}

$configs = if ($Config -eq "Both") { @("Release","Debug") } else { @($Config) }

# ---------------------------------------------------------------------------
# Import the VS x64 developer environment into this PowerShell session so
# cl / link / nmake / msbuild are on PATH.
# ---------------------------------------------------------------------------
function Import-VcVars {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found -- is Visual Studio installed?" }
    $vsPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $vsPath) { throw "No VS install with the C++ x64 toolset found." }
    $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found at $vcvars" }

    Write-Host "Importing VS env from: $vcvars"
    # Run vcvars in a child cmd, dump the resulting environment, mirror it here.
    $tmpFile = [System.IO.Path]::GetTempFileName()
    cmd /c "`"$vcvars`" >nul 2>&1 && set > `"$tmpFile`""
    Get-Content $tmpFile | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] }
    }
    Remove-Item $tmpFile -Force
}

Import-VcVars
# Native Perl + NASM ahead of anything else (avoid cygwin/git perl).
$env:PATH = "$NASM_DIR;C:\Strawberry\perl\bin;$env:PATH"

foreach ($t in @($PERL,"$NASM_DIR\nasm.exe")) {
    if (-not (Test-Path $t)) { throw "Required tool missing: $t" }
}
Write-Host "Toolchain ready. Building config(s): $($configs -join ', ')"

# ===========================================================================
# 1. libsodium  (build its bundled VS2022 static-LIB project)
#    Config map: Release -> ReleaseLIB (/MT), Debug -> DebugLIB (/MTd)
# ===========================================================================
function Build-Sodium($cfg) {
    Section "libsodium ($cfg)"
    $sodiumSrc = "$src\libsodium"
    $incSrc    = "$sodiumSrc\src\libsodium\include"

    # Generate the version.h that a git checkout lacks (the release tarball ships it).
    $verIn  = "$incSrc\sodium\version.h.in"
    $verOut = "$incSrc\sodium\version.h"
    if (-not (Test-Path $verOut)) {
        Write-Host "  generating version.h ($SODIUM_VERSION)"
        (Get-Content $verIn -Raw) `
            -replace '@VERSION@', $SODIUM_VERSION `
            -replace '@SODIUM_LIBRARY_VERSION_MAJOR@', "$SODIUM_VER_MAJOR" `
            -replace '@SODIUM_LIBRARY_VERSION_MINOR@', "$SODIUM_VER_MINOR" `
            -replace '@SODIUM_LIBRARY_MINIMAL_DEF@', '' `
            | Set-Content $verOut -Encoding ASCII
    }

    $cfgName = if ($cfg -eq "Debug") { "DebugLIB" } else { "ReleaseLIB" }
    $vcxproj = "$sodiumSrc\builds\msvc\vs2022\libsodium\libsodium.vcxproj"
    & msbuild $vcxproj /p:Configuration=$cfgName /p:Platform=x64 /m /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "libsodium build failed ($cfg)" }

    # Locate the produced static lib and install it.
    $lib = Get-ChildItem "$sodiumSrc\bin" -Recurse -Filter "libsodium.lib" |
           Where-Object { $_.FullName -match "\\x64\\" -and $_.FullName -match "static" } |
           Select-Object -First 1
    if (-not $lib) {
        $lib = Get-ChildItem "$sodiumSrc\bin" -Recurse -Filter "libsodium.lib" | Select-Object -First 1
    }
    if (-not $lib) { throw "libsodium.lib not found after build" }

    $libDst = "$root\libsodium\lib\x64\$cfg"
    EnsureDir $libDst
    Copy-Item $lib.FullName "$libDst\libsodium.lib" -Force
    Write-Host "  installed $($lib.FullName)  ->  $libDst\libsodium.lib"

    # Headers (config-independent; copy once).
    $incDst = "$root\libsodium\include"
    EnsureDir "$incDst\sodium"
    Copy-Item "$incSrc\sodium.h" "$incDst\" -Force
    Copy-Item "$incSrc\sodium\*" "$incDst\sodium\" -Recurse -Force
}

# ===========================================================================
# 2. OpenSSL  (out-of-tree static build, static CRT, no apps/tests/docs)
# ===========================================================================
function Build-OpenSSL($cfg) {
    Section "OpenSSL ($cfg)"
    $osslSrc = "$src\openssl"
    $bld     = "$build\openssl-$cfg"
    $inst    = "$bld\_install"
    EnsureDir $bld

    Push-Location $bld
    try {
        $cfgFlag = if ($cfg -eq "Debug") { "--debug" } else { "--release" }
        Write-Host "  Configure VC-WIN64A $cfgFlag (static, static-CRT)"
        & $PERL "$osslSrc\Configure" VC-WIN64A `
            no-shared no-module no-tests no-apps no-docs `
            $cfgFlag --prefix="$inst" --openssldir="$inst\ssl"
        if ($LASTEXITCODE -ne 0) { throw "OpenSSL Configure failed ($cfg)" }

        # Force the static CRT: rewrite /MD(d) -> /MT(d) in the generated makefile.
        Write-Host "  patching makefile: /MD -> /MT"
        $mk = Get-Content "makefile" -Raw
        $mk = $mk -replace '/MDd', '/MTd' -replace '/MD', '/MT'
        Set-Content "makefile" $mk -Encoding ASCII

        Write-Host "  nmake (this is the slow one) ..."
        & nmake /nologo
        if ($LASTEXITCODE -ne 0) { throw "OpenSSL nmake failed ($cfg)" }

        & nmake /nologo install_dev
        if ($LASTEXITCODE -ne 0) { throw "OpenSSL install_dev failed ($cfg)" }
    }
    finally { Pop-Location }

    # Install libs (per-config) + headers (once).
    $libDst = "$root\openssl\lib\x64\$cfg"
    EnsureDir $libDst
    foreach ($name in "libssl.lib","libcrypto.lib") {
        $found = Get-ChildItem "$inst\lib" -Recurse -Filter $name | Select-Object -First 1
        if (-not $found) { throw "$name not found in OpenSSL install ($cfg)" }
        Copy-Item $found.FullName "$libDst\$name" -Force
        Write-Host "  installed $name -> $libDst"
    }
    $incDst = "$root\openssl\include\openssl"
    EnsureDir $incDst
    Copy-Item "$inst\include\openssl\*" $incDst -Recurse -Force
}

# ===========================================================================
# 3. curl  (CMake, static, static CRT, OpenSSL backend = our build)
# ===========================================================================
function Build-Curl($cfg) {
    Section "curl ($cfg)"
    $curlSrc  = "$src\curl"
    $bld      = "$build\curl-$cfg"
    $osslRoot = "$build\openssl-$cfg\_install"
    if (-not (Test-Path "$osslRoot\lib")) { throw "OpenSSL ($cfg) must be built before curl" }

    & cmake -S $curlSrc -B $bld -G "Visual Studio 17 2022" -A x64 `
        -DBUILD_SHARED_LIBS=OFF `
        -DCURL_STATIC_CRT=ON `
        -DBUILD_CURL_EXE=OFF `
        -DBUILD_TESTING=OFF `
        -DCURL_USE_OPENSSL=ON `
        -DOPENSSL_ROOT_DIR="$osslRoot" `
        -DOPENSSL_USE_STATIC_LIBS=ON `
        -DCURL_USE_SCHANNEL=OFF `
        -DCURL_USE_LIBPSL=OFF `
        -DCURL_USE_LIBSSH2=OFF `
        -DUSE_NGHTTP2=OFF `
        -DUSE_LIBIDN2=OFF `
        -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF `
        -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON
    if ($LASTEXITCODE -ne 0) { throw "curl CMake configure failed ($cfg)" }

    & cmake --build $bld --config $cfg --target libcurl_static --parallel
    if ($LASTEXITCODE -ne 0) { throw "curl build failed ($cfg)" }

    # The final static archive lands in lib\<cfg>\; ignore the libcurl_object
    # intermediate and any import lib.
    $lib = Get-ChildItem "$bld\lib\$cfg" -Filter "libcurl*.lib" -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -notmatch "object|imp" } | Select-Object -First 1
    if (-not $lib) { throw "libcurl static lib not found after build ($cfg)" }

    $libDst = "$root\curl\lib\x64\$cfg"
    EnsureDir $libDst
    Copy-Item $lib.FullName "$libDst\libcurl.lib" -Force
    Write-Host "  installed $($lib.Name) -> $libDst\libcurl.lib"

    # Public headers (config-independent; copy once).
    $incDst = "$root\curl\include\curl"
    EnsureDir $incDst
    Copy-Item "$curlSrc\include\curl\*" $incDst -Recurse -Force
}

# ===========================================================================
# Drive the build. OpenSSL must precede curl for each config.
# ===========================================================================
foreach ($cfg in $configs) {
    Build-Sodium  $cfg
    Build-OpenSSL $cfg
    Build-Curl    $cfg
}

Section "DONE"
Write-Host "Installed static libs:"
Get-ChildItem $root -Recurse -Filter "*.lib" |
    Where-Object { $_.FullName -notmatch "\\_build\\" -and $_.FullName -notmatch "\\src\\" } |
    ForEach-Object { Write-Host "  $($_.FullName.Substring($root.Length + 1))" }
Write-Host ""
Write-Host "Now open 'Zima CLI.sln' and build, or run:"
Write-Host "  msbuild `"Zima CLI.sln`" /p:Configuration=Release /p:Platform=x64"
