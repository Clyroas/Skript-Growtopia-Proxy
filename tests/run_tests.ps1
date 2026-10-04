# Runs the standalone compatibility tests.
#
# These only need a C++ compiler and the vendored nlohmann/json + a glm stub,
# so they work without Conan. Requires the Visual Studio developer environment.
#
#   powershell -ExecutionPolicy Bypass -File tests/run_tests.ps1
#
# Run from the repository root.

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

$failures = @()

function Invoke-Test {
    param(
        [string]$Name,
        [string]$Source,
        [string[]]$ExtraArgs = @()
    )

    Write-Host ""
    Write-Host "==============================================" -ForegroundColor Cyan
    Write-Host " $Name" -ForegroundColor Cyan
    Write-Host "==============================================" -ForegroundColor Cyan

    $exe = "tests\_$Name.exe"
    $obj = "tests\_obj_$Name"

    New-Item -ItemType Directory -Force -Path $obj | Out-Null

    $compileArgs = @(
        '/nologo', '/std:c++latest', '/EHsc', '/W3', '/wd4267', '/wd4101'
    ) + $ExtraArgs + @(
        "/Fe:$exe", "/Fo:$obj\", $Source
    )

    & cl.exe @compileArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "COMPILE FAILED: $Name" -ForegroundColor Red
        $script:failures += "$Name (compile)"
        return
    }

    & $exe
    if ($LASTEXITCODE -ne 0) {
        Write-Host "TEST FAILED: $Name" -ForegroundColor Red
        $script:failures += "$Name (run)"
    }
    else {
        Write-Host "PASSED: $Name" -ForegroundColor Green
    }

    Remove-Item -Recurse -Force $obj -ErrorAction SilentlyContinue
    Remove-Item -Force $exe -ErrorAction SilentlyContinue
}

# --- dependency-free tests (standard library only) ---
Invoke-Test -Name 'byte_stream'          -Source 'tests\byte_stream_test.cpp'
Invoke-Test -Name 'server_data_parser'   -Source 'tests\server_data_parser_test.cpp'
Invoke-Test -Name 'spawn_field'          -Source 'tests\spawn_field_test.cpp'
Invoke-Test -Name 'packet_variant'       -Source 'tests\packet_variant_test.cpp' -ExtraArgs @('/I', 'tests\stub')
Invoke-Test -Name 'declaration_handling' -Source 'tests\declaration_handling_test.cpp'

# --- item database tests (need vendored nlohmann/json) ---
$jsonInclude = '/Ilib\nlohmann_json\include'
Invoke-Test -Name 'item_database'       -Source 'tests\item_database_test.cpp'      -ExtraArgs @($jsonInclude)
Invoke-Test -Name 'item_database_edge'  -Source 'tests\item_database_edge_test.cpp' -ExtraArgs @($jsonInclude)

Write-Host ""
Write-Host "==============================================" -ForegroundColor Cyan
if ($failures.Count -eq 0) {
    Write-Host " ALL TESTS PASSED" -ForegroundColor Green
    Pop-Location
    exit 0
}

Write-Host " FAILURES:" -ForegroundColor Red
$failures | ForEach-Object { Write-Host "   - $_" -ForegroundColor Red }
Pop-Location
exit 1
