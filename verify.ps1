# ---------------------------------------------------------------------------
# Live-site verification script (stage 2: V1 / V2 / V3 / V5 / V6)
#
# Usage (from the project directory):
#   .\verify.ps1                    # re-test using the app's own browser profile
#   .\verify.ps1 -CourseId 11311    # probe a specific course
#
# Prerequisites:
#   * Already built with -DNJULMS_BUILD_VERIFY_TOOL=ON
#   * A valid session. If it expired, run .\build\bin\njulms_app.exe first,
#     log in manually, then re-run this script.
#
# What this script does:
#   1. Copies the app's browser profile (which holds the login session) into a
#      temporary directory under build/, so the verification tool is not blocked
#      by a running Edge instance holding the profile lock.
#   2. Runs njulms_verify.exe against that temporary profile.
#   3. Deletes the temporary profile on EVERY exit path - it is equivalent to an
#      account credential and must not be left in the build tree.
#
# NOTE: this file is intentionally ASCII-only. Windows PowerShell 5.1 reads
# .ps1 files using the ANSI code page, so non-ASCII comments would be mangled
# and could even break parsing.
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$CourseId = '',
    [string]$ProfileSource = '',
    [int]$TimeoutSeconds = 240
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$tool = Join-Path $root 'build\bin\njulms_verify.exe'

if (-not (Test-Path $tool)) {
    Write-Host "ERROR: $tool not found." -ForegroundColor Red
    Write-Host "Build it first with the verify tool enabled:" -ForegroundColor Yellow
    Write-Host "  cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/msvc2022_64 -DNJULMS_BUILD_GUI=ON -DNJULMS_BUILD_VERIFY_TOOL=ON"
    Write-Host "  cmake --build build --parallel --target njulms_verify"
    exit 1
}

if ([string]::IsNullOrWhiteSpace($ProfileSource)) {
    # Default: the app's own persistent profile (build\bin\profile).
    # Log in once with build\bin\njulms_app.exe, then re-run this script.
    $ProfileSource = Join-Path $root 'build\bin\profile'
}
if (-not (Test-Path $ProfileSource)) {
    Write-Host "ERROR: profile not found at '$ProfileSource'" -ForegroundColor Red
    Write-Host "Log in once with build\bin\njulms_app.exe first (it creates the profile)," -ForegroundColor Yellow
    Write-Host "or pass another path with -ProfileSource '<path>'." -ForegroundColor Yellow
    exit 1
}

$tempProfile = Join-Path $root 'build\_verify_profile'
$outFile = Join-Path $root 'build\verify_out.txt'
$errFile = Join-Path $root 'build\verify_err.txt'

Write-Host "profile source : $ProfileSource"
Write-Host "temp copy      : $tempProfile"
Write-Host ""

try {
    if (Test-Path $tempProfile) { Remove-Item -Recurse -Force $tempProfile }
    Write-Host "copying profile (contains session cookies - stays local)..." -ForegroundColor DarkGray
    # robocopy exit codes 0-7 mean success
    $null = robocopy $ProfileSource $tempProfile /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1
    if ($LASTEXITCODE -ge 8) {
        throw "robocopy failed with exit code $LASTEXITCODE"
    }

    $env:NJULMS_PROFILE_DIR = $tempProfile
    # Silence this project's own log noise; we only want the tool's own output.
    $env:QT_LOGGING_RULES = 'njulms.*=false'

    Write-Host ""
    Write-Host "=== running njulms_verify (timeout ${TimeoutSeconds}s) ===" -ForegroundColor Cyan

    $toolArgs = @()
    if (-not [string]::IsNullOrWhiteSpace($CourseId)) { $toolArgs += $CourseId }

    # Windows PowerShell 5.1 rejects an empty -ArgumentList, so only pass it
    # when we actually have an argument.
    $spParams = @{
        FilePath               = $tool
        NoNewWindow            = $true
        PassThru               = $true
        RedirectStandardOutput = $outFile
        RedirectStandardError  = $errFile
    }
    if ($toolArgs.Count -gt 0) { $spParams['ArgumentList'] = $toolArgs }

    $proc = Start-Process @spParams

    if (-not $proc.WaitForExit($TimeoutSeconds * 1000)) {
        Write-Host "TIMEOUT after ${TimeoutSeconds}s - stopping the tool" -ForegroundColor Yellow
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }

    # The tool writes UTF-8; read it as UTF-8 explicitly, otherwise Windows
    # PowerShell 5.1 decodes it with the ANSI code page and CJK text is mangled.
    if (Test-Path $outFile) {
        Get-Content $outFile -Encoding UTF8 -ErrorAction SilentlyContinue
    }

    $err = @()
    if (Test-Path $errFile) {
        $err = Get-Content $errFile -Encoding UTF8 -ErrorAction SilentlyContinue
    }
    if ($err) {
        Write-Host ""
        Write-Host "--- stderr (filtered to this program's log lines) ---" -ForegroundColor DarkGray
        $err | Where-Object { $_ -match '^\d{4}-\d{2}-\d{2}T' } | Select-Object -First 40
    }
}
finally {
    if (Test-Path $tempProfile) {
        Remove-Item -Recurse -Force $tempProfile -ErrorAction SilentlyContinue
        Write-Host ""
        Write-Host "temporary profile removed." -ForegroundColor DarkGray
    }
}

Write-Host ""
Write-Host "=== how to read the result ===" -ForegroundColor Cyan
Write-Host "  V1 pass    : 'RESULT V1: PASS' with a real course count"
Write-Host "  Session bad: 'RESULT V1: PARTIAL' + 'error code: 1' => log in first"
Write-Host "  V3         : each page_size line shows items / reportedTotal / pages;"
Write-Host "               if items always equals reportedTotal, page_size is honoured"
Write-Host "  V5/V6      : need a valid session; paste the output back to update the report"
