# Builds librashader's C API for Windows under the bare name PPSSPP's loader looks for, so it can be
# shipped next to PPSSPPWindows*.exe. macOS/Linux: scripts/build-librashader.sh. Android:
# android/build-librashader.sh (keep the pinned tag below in sync with those).
#
#   scripts/build-librashader.ps1 [-Platform x64|ARM64] [-Out DIR]
#
# Defaults to x64 and <repo>\build-librashader. Env: LIBRASHADER_TAG, LIBRASHADER_SRC.
# ARM64 is cross-compiled from an x64 host, so the VS "C++ ARM64 build tools" component is required.
param(
	[ValidateSet('x64', 'ARM64')][string]$Platform = 'x64',
	[string]$Out = ''
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$tag = if ($env:LIBRASHADER_TAG) { $env:LIBRASHADER_TAG } else { 'librashader-v0.12.0' }
$src = if ($env:LIBRASHADER_SRC) { $env:LIBRASHADER_SRC } else { Join-Path $repo 'build\librashader-src' }
if (-not $Out) { $Out = Join-Path $repo 'build-librashader' }

# runtime-d3d11 is included so one DLL serves PPSSPP's Vulkan, OpenGL and D3D11 backends.
$features = 'runtime-vulkan,runtime-opengl,runtime-d3d11'
$target = if ($Platform -eq 'ARM64') { 'aarch64-pc-windows-msvc' } else { 'x86_64-pc-windows-msvc' }
$vcArch = if ($Platform -eq 'ARM64') { 'x64_arm64' } else { 'x64' }

function Invoke-Checked([string]$what, [scriptblock]$body) {
	# git, rustup, cargo and dumpbin all write progress to stderr, which Windows PowerShell turns into
	# a terminating error under $ErrorActionPreference = 'Stop' as soon as the stream is redirected (to
	# a log, say). The exit code is the real check. This assignment is function-scoped, so it lasts
	# only for the call.
	$ErrorActionPreference = 'Continue'
	& $body
	if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

# cargo shells out to MSVC's linker, so the build has to run inside a developer environment for the
# target architecture. Run it from a generated .bat rather than fighting cmd/PowerShell quoting.
function Invoke-VcvarsBat([string]$what, [string[]]$lines) {
	$bat = Join-Path ([System.IO.Path]::GetTempPath()) "ppsspp-librashader-$Platform.bat"
	@("@echo off", "call `"$vcvars`" $vcArch >nul || exit /b 1") + $lines | Set-Content -Encoding ASCII $bat
	Invoke-Checked $what { & cmd /c $bat }
	Remove-Item $bat -Force
}

foreach ($tool in 'git', 'cargo') {
	if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
		throw "'$tool' not found on PATH (Rust stable >= 1.88 is required; install it with rustup)"
	}
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found at $vswhere (is Visual Studio installed?)" }
$vsPath = Invoke-Checked 'vswhere' { & $vswhere -latest -property installationPath }
if (-not $vsPath) { throw 'vswhere found no Visual Studio installation' }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path $vcvars)) { throw "vcvarsall.bat not found at $vcvars (C++ build tools missing?)" }

# vcvarsall accepts x64_arm64 and exits 0 even with no ARM64 toolset installed, leaving the x64 linker
# on PATH - so the absence has to be caught here rather than as a puzzling link error minutes later.
if ($Platform -eq 'ARM64') {
	$crossTools = Get-ChildItem (Join-Path $vsPath 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
		Where-Object { Test-Path (Join-Path $_.FullName 'bin\Hostx64\arm64\link.exe') }
	if (-not $crossTools) {
		throw "no x64-hosted ARM64 toolset in $vsPath (install the Visual Studio 'C++ ARM64/ARM64EC build tools' component)"
	}
}

if (Get-Command rustup -ErrorAction SilentlyContinue) {
	Invoke-Checked "rustup target add $target" { & rustup target add $target }
}

if (-not (Test-Path (Join-Path $src '.git'))) {
	Invoke-Checked 'git clone' { & git clone --depth 1 --branch $tag https://github.com/SnowflakePowered/librashader.git $src }
} else {
	Invoke-Checked 'git fetch' { & git -C $src fetch --depth 1 origin "refs/tags/${tag}:refs/tags/${tag}" }
	Invoke-Checked 'git checkout' { & git -C $src checkout -q $tag }
}

Write-Host "== librashader.dll $Platform ($tag, features: $features)"
Invoke-VcvarsBat 'cargo build' @(
	"cd /d `"$src`" || exit /b 1",
	"cargo build -p librashader-capi --release --target $target --no-default-features --features $features || exit /b 1"
)

# No install-name/soname step on Windows: the import name lives in the importing module, and PPSSPP
# resolves the library by this bare filename (Common/GPU/Librashader/LibrashaderLoader.cpp).
$artifact = Join-Path $src "target\$target\release\librashader_capi.dll"
if (-not (Test-Path $artifact)) { throw "cargo did not produce $artifact" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$dll = Join-Path $Out 'librashader.dll'
Copy-Item $artifact $dll -Force

# A DLL missing a runtime still loads and reports success - every chain creation on that backend then
# fails instead - so check the entry points we actually call. The machine type is checked too, so a
# build that quietly targeted the host instead of $Platform can't ship as the wrong architecture.
$dumpTxt = Join-Path ([System.IO.Path]::GetTempPath()) "ppsspp-librashader-dumpbin-$Platform.txt"
Invoke-VcvarsBat 'dumpbin' @("dumpbin /headers /exports `"$dll`" > `"$dumpTxt`" || exit /b 1")
$dump = Get-Content $dumpTxt
Remove-Item $dumpTxt -Force
$machine = if ($Platform -eq 'ARM64') { 'machine (ARM64)' } else { 'machine (x64)' }
if (-not ($dump | Select-String -SimpleMatch $machine -Quiet)) {
	throw "librashader.dll is not $Platform - its PE header does not say '$machine'"
}
foreach ($sym in 'libra_vk_filter_chain_create', 'libra_gl_filter_chain_create', 'libra_d3d11_filter_chain_create') {
	if (-not ($dump | Select-String -SimpleMatch $sym -Quiet)) { throw "librashader.dll is missing $sym" }
}
$n = ($dump | Select-String -SimpleMatch ' libra_').Count
if ($n -lt 40) { throw "only $n libra_* exports in librashader.dll" }

Write-Host ("   ok: $dll ($n exports, {0:N1} MB)" -f ((Get-Item $dll).Length / 1MB))
