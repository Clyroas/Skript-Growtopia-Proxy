<#
    Skript Growtopia Proxy - build bootstrap
    ========================================
    Installs Conan if needed, then configures and builds Skript.exe in Release mode.

    RUN THIS IN A NORMAL POWERSHELL TERMINAL, NOT inside a restricted/agent shell.
    Restricted shells, sandboxes and some security products block the temporary
    folders that both pip and Conan need to work.

    Usage:
        cd <repo>
        powershell -ExecutionPolicy Bypass -File build.ps1

    Optional:
        -BuildDir <path>   CMake build directory (default: build)
        -SkipPrereqs       do not install anything, just configure and build
        -Clean             delete the build directory first
        -AllowCmake4       allow CMake 4.x (see below) instead of warning

    Prerequisites this script checks for and explains how to get:
        * Python 3 on PATH          (only needed to install Conan)
        * Visual Studio 2022 with the "Desktop development with C++" workload
        * CMake 3.24+               (Visual Studio's bundled copy is used if present)
        * Conan 2.0.5+              (installed here with pip --user if missing)

    About CMake versions:
        lib/enet, lib/glm, lib/eventpp, lib/libressl and lib/nlohmann_json declare
        cmake_minimum_required below 3.5. CMake 4.x hard-errors on those, so without
        -AllowCmake4 this script passes -DCMAKE_POLICY_VERSION_MINIMUM=3.5 when it
        detects CMake 4.x. Installing CMake 3.31 gives a cleaner build:
            pip install --user cmake==3.31.6

    Nothing is installed system-wide and no admin rights are needed to build.
    Admin rights are only needed later, to edit the hosts file and bind port 443.
#>

[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [switch]$SkipPrereqs,
    [switch]$Clean,
    [switch]$AllowCmake4
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Write-Step  { param([string]$m) Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Write-Ok    { param([string]$m) Write-Host "  [ok]   $m" -ForegroundColor Green }
function Write-Warn2 { param([string]$m) Write-Host "  [warn] $m" -ForegroundColor Yellow }
function Write-Err   { param([string]$m) Write-Host "  [FAIL] $m" -ForegroundColor Red }

function Get-PythonCommand {
    foreach ($candidate in @('python', 'py')) {
        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($cmd) {
            # 'py' needs -3 to be safe; verify it actually runs
            try {
                if ($candidate -eq 'py') { & py -3 --version *> $null } else { & python --version *> $null }
                if ($LASTEXITCODE -eq 0) { return $candidate }
            } catch { }
        }
    }
    return $null
}

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $repoRoot
Write-Host "Repository: $repoRoot" -ForegroundColor White

# --------------------------------------------------------------------------
Write-Step 'Checking prerequisites'
# --------------------------------------------------------------------------

$python = Get-PythonCommand
if (-not $python) {
    Write-Err 'Python 3 was not found on PATH.'
    Write-Host '  Install it from https://www.python.org/downloads/ (tick "Add python.exe to PATH"), then re-run.'
    exit 1
}
$pyVersion = if ($python -eq 'py') { & py -3 --version } else { & python --version }
Write-Ok "$python -> $pyVersion"

if ($pyVersion -match '3\.(1[3-9]|[2-9][0-9])') {
    Write-Warn2 'Python 3.13+ may not yet have wheels for every Conan dependency.'
    Write-Warn2 'If Conan install fails, install Python 3.12 and re-run with: py -3.12'
}

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) { Write-Ok "cmake found: $($cmake.Source)" } else { Write-Warn2 'cmake not found - will install it' }

$conan = Get-Command conan -ErrorAction SilentlyContinue
if ($conan) { Write-Ok "conan found: $($conan.Source)" } else { Write-Warn2 'conan not found - will install it' }

# Visual Studio with the C++ toolset
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Err 'vswhere.exe not found - Visual Studio 2022 does not appear to be installed.'
    Write-Host '  Install "Visual Studio 2022" with the "Desktop development with C++" workload.'
    exit 1
}
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    Write-Err 'No Visual Studio installation with the C++ toolset was found.'
    Write-Host '  In the Visual Studio Installer, add the "Desktop development with C++" workload.'
    exit 1
}
Write-Ok "Visual Studio: $vsPath"

# Visual Studio ships a CMake and Ninja; prefer them so only Conan needs pip.
$vsCmake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$vsNinja = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
if (Test-Path $vsCmake) { Write-Ok "Visual Studio bundles CMake: $(& $vsCmake --version | Select-Object -First 1)" }
if (Test-Path $vsNinja) { Write-Ok 'Visual Studio bundles Ninja' }

# --------------------------------------------------------------------------
if (-not $SkipPrereqs) {
    Write-Step 'Installing Conan (current user only)'

    Write-Host '  pip install --user conan' -ForegroundColor DarkGray
    & $python -m pip install --user --upgrade 'conan>=2.0.5'
    if ($LASTEXITCODE -ne 0) {
        Write-Err 'pip install failed.'
        Write-Host ''
        Write-Host '  Common causes and fixes:' -ForegroundColor Yellow
        Write-Host '   * "Access is denied" on a ...\pip-*\... path: a sandbox, AppContainer or'
        Write-Host '     security product is blocking pip temp folders. Run this script in a'
        Write-Host '     normal, unrestricted PowerShell window (not inside a restricted shell).'
        Write-Host '   * No wheels for your Python version: install Python 3.12 and run'
        Write-Host '         py -3.12 -m pip install --user conan'
        Write-Host '   * Corporate proxy: set HTTP_PROXY/HTTPS_PROXY, then retry.'
        Write-Host ''
        Write-Host '  Alternative: install Conan with pipx or from your OS package manager,'
        Write-Host '  then re-run this script with -SkipPrereqs.'
        exit 1
    }

    # Newly installed console scripts are not on PATH for this process
    $userBase = & $python -c "import site; print(site.getusersitepackages())"
    $userScripts = Join-Path (Split-Path -Parent $userBase) 'Scripts'
    if (Test-Path $userScripts) {
        $env:PATH = "$userScripts;$env:PATH"
        Write-Ok "added to PATH for this session: $userScripts"
    }
    if (-not (Get-Command conan -ErrorAction SilentlyContinue)) {
        Write-Err 'conan still not resolvable after install.'
        Write-Host "  It is probably in: $userScripts"
        Write-Host '  Add that folder to PATH, open a new terminal, re-run with -SkipPrereqs'
        exit 1
    }
}

# Resolve cmake: PATH first, then the Visual Studio copy
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
$cmakeExe = if ($cmake) { $cmake.Source } elseif (Test-Path $vsCmake) { $vsCmake } else { $null }
if (-not $cmakeExe) {
    Write-Err 'No CMake found on PATH and none bundled with Visual Studio.'
    Write-Host '  Install it:  pip install --user cmake==3.31.6  then re-run.'
    exit 1
}

# Conan has to run cmake itself when it builds a dependency from source, and it
# looks the executable up on PATH. The copy bundled with Visual Studio is not on
# PATH, so expose it through a small shim unless a real cmake is already there.
$cmakeDir = Split-Path -Parent $cmakeExe
if (-not ($env:PATH -split ';' | Where-Object { $_ -and ($_.TrimEnd('\') -ieq $cmakeDir.TrimEnd('\')) })) {
    $shimDir = Join-Path $env:TEMP 'skript-cmake-shim'
    New-Item -ItemType Directory -Force -Path $shimDir | Out-Null
    $shimCmd = "@echo off`r`n`"$cmakeExe`" %*`r`n"
    Set-Content -Path (Join-Path $shimDir 'cmake.bat') -Value $shimCmd -Encoding ASCII -NoNewline
    $env:PATH = "$shimDir;$env:PATH"
    Write-Ok "put cmake on PATH via shim so Conan can build dependencies: $shimDir"
}

if (-not (Get-Command conan -ErrorAction SilentlyContinue)) {
    Write-Err 'conan was not found on PATH.'
    Write-Host '  Install it:  pip install --user conan   then re-run (or open a new terminal).'
    exit 1
}

$cmakeVersion = (& $cmakeExe --version | Select-Object -First 1)
Write-Ok "using $cmakeExe ($cmakeVersion)"
Write-Ok "using conan $(& conan --version)"

$cmakeMajor = 0
if ($cmakeVersion -match 'version\s+(\d+)\.') { $cmakeMajor = [int]$Matches[1] }
if ($cmakeMajor -ge 4) {
    Write-Warn2 'CMake 4.x detected. The vendored libraries (enet, glm, eventpp, libressl,'
    Write-Warn2 'nlohmann_json) declare cmake_minimum_required < 3.5, which CMake 4 rejects.'
    Write-Warn2 'Passing -DCMAKE_POLICY_VERSION_MINIMUM=3.5 to work around it.'
    Write-Warn2 'For a cleaner build, prefer CMake 3.31:  pip install --user cmake==3.31.6'
}

# --------------------------------------------------------------------------
Write-Step 'Configuring the Conan profile'
# --------------------------------------------------------------------------

# Conan and CMake write progress and diagnostic text to stderr. With
# $ErrorActionPreference='Stop' PowerShell would treat that as a terminating
# error, so relax it for the native tool calls; failures are still detected via
# $LASTEXITCODE below.
$ErrorActionPreference = 'Continue'

# Idempotent: creates the profile on first run, leaves an existing one alone.
# Conan printing "detect_api: Found msvc 18" on stderr is normal, not an error.
& conan profile detect --force 2>&1 | Select-Object -First 8 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }
& conan profile show -pr default 2>&1 | Select-Object -First 12 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkGray }

# Note: the CMake Conan provider generates its own host profile and takes
# compiler.cppstd from the project (C++23 here), so the default profile's
# cppstd value does not affect the build. It only matters if you run
# 'conan install' by hand, in which case add compiler.cppstd=20 or 23 to
# ~/.conan2/profiles/default yourself (Conan 2 removed 'conan profile update').

# --------------------------------------------------------------------------
Write-Step 'Configuring CMake'
# --------------------------------------------------------------------------

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "  removing $BuildDir" -ForegroundColor DarkGray
    Remove-Item -Recurse -Force $BuildDir
}

$configureArgs = @('-B', $BuildDir)
$cmakeIsV4 = ($cmakeMajor -ge 4)
if ($cmakeIsV4 -and -not $AllowCmake4) {
    # Belt and braces: also allow sub-projects that declare ancient minimums
    $configureArgs += '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'
}

Write-Host "  $cmakeExe $($configureArgs -join ' ')" -ForegroundColor DarkGray
& $cmakeExe @configureArgs
if ($LASTEXITCODE -ne 0) {
    Write-Err 'CMake configure failed.'
    Write-Host ''
    Write-Host '  If the error mentions cmake_minimum_required / compatibility with CMake < 3.5:' -ForegroundColor Yellow
    Write-Host '    your CMake is 4.x and the workaround did not apply. Install 3.31:'
    Write-Host '      pip install --user cmake==3.31.6'
    Write-Host '  If it mentions conan: ensure "conan --version" works, then re-run.'
    Write-Host '  If it mentions a missing package (fmt/spdlog/glm/...): the Conan install'
    Write-Host '    step did not complete. Delete the build folder and re-run.'
    exit 1
}

# --------------------------------------------------------------------------
Write-Step 'Building (Release)'
# --------------------------------------------------------------------------

& $cmakeExe --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Err 'Build failed - see the compiler output above.'
    exit 1
}

# --------------------------------------------------------------------------
Write-Step 'Locating the binary'
# --------------------------------------------------------------------------

$exeCandidates = @(
    (Join-Path $BuildDir 'src\Release\Skript.exe'),
    (Join-Path $BuildDir 'src\Skript.exe')
)
$exe = $exeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

if (-not $exe) {
    Write-Err 'Build reported success but Skript.exe was not found.'
    Write-Host '  Searched:' -ForegroundColor Yellow
    $exeCandidates | ForEach-Object { Write-Host "    $_" }
    exit 1
}

$exe = (Resolve-Path $exe).Path
Write-Ok "built: $exe"
Write-Ok ("size: {0:N1} MB" -f ((Get-Item $exe).Length / 1MB))

# config.json is read from the current working directory
$runDir = Split-Path -Parent $exe
$rootConfig = Join-Path $repoRoot 'config.json'
if ((Test-Path $rootConfig) -and -not (Test-Path (Join-Path $runDir 'config.json'))) {
    Copy-Item $rootConfig (Join-Path $runDir 'config.json')
    Write-Ok "copied config.json next to the binary (it is read from the working directory)"
}

Write-Host ''
Write-Host '====================================================' -ForegroundColor Green
Write-Host ' BUILD SUCCEEDED' -ForegroundColor Green
Write-Host '====================================================' -ForegroundColor Green
Write-Host ''
Write-Host ' Next steps:' -ForegroundColor White
Write-Host '   1. Add these lines to C:\Windows\System32\drivers\etc\hosts (needs admin):'
Write-Host '        127.0.0.1 www.growtopia1.com'
Write-Host '        127.0.0.1 www.growtopia2.com'
Write-Host '   2. Launch Skript.exe AS ADMINISTRATOR (it binds port 443):'
Write-Host "        Start-Process '$exe' -Verb RunAs"
Write-Host '   3. Start Growtopia. The proxy log should show:'
Write-Host '        Growtopia client declared version=... protocol=...'
Write-Host '        Proxied server_data.php -> ...'
Write-Host ''
Write-Host ' To undo the hosts change, remove those two lines.' -ForegroundColor DarkGray
Write-Host ''
