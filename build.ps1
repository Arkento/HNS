# ---------------------------------------------------------------------------
# NjuLmsCrawler build script
#
# Usage (from the project directory):
#   .\build.ps1                    # configure + build + run tests
#   .\build.ps1 -NoGui             # build and test the core only (no Qt WebEngine)
#   .\build.ps1 -Clean             # wipe the build directory first
#   .\build.ps1 -Generator VS      # use the Visual Studio generator instead of Ninja
#   .\build.ps1 -NoDeploy          # skip the windeployqt step
#
# Design notes:
#   * The Qt prefix is passed explicitly so no environment variable is needed.
#   * After a successful build the Qt runtime is deployed next to the exe with
#     windeployqt. Without it, double-clicking njulms_app.exe fails with a
#     missing Qt6Cored.dll error because Qt is not on the system PATH.
#   * This machine's Windows SDK lives at the NON-standard path
#     D:\Windows Kits\10, so the script detects the SDK version and forwards it
#     through CMAKE_SYSTEM_VERSION; without it CMake cannot find the headers.
#   * Ninja is the default because it is faster; the VS generator is kept as an
#     option.
#
# NOTE: this file is intentionally ASCII-only. Windows PowerShell 5.1 reads .ps1
# files using the ANSI code page, so non-ASCII text is decoded incorrectly,
# which both garbles messages and can break parsing entirely (observed:
# "Cannot bind argument to parameter 'Path' because it is null").
# ---------------------------------------------------------------------------
[CmdletBinding()]
param(
    [string]$QtPrefix = 'D:\Qt\6.8.3\msvc2022_64',
    [ValidateSet('Ninja', 'VS')]
    [string]$Generator = 'Ninja',
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Debug',
    [switch]$Clean,
    [switch]$NoTests,
    [switch]$NoGui,
    [switch]$NoDeploy,
    [switch]$WithVerifyTool
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$buildDir = Join-Path $root 'build'

function Find-VsInstall {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        Write-Host "vswhere.exe not found; falling back to known locations" -ForegroundColor DarkGray
        foreach ($candidate in @('D:\app\vs', 'C:\Program Files\Microsoft Visual Studio\2022\Community',
                                 'C:\Program Files\Microsoft Visual Studio\2022\Professional',
                                 'C:\Program Files\Microsoft Visual Studio\2022\Enterprise')) {
            if (Test-Path (Join-Path $candidate 'VC\Auxiliary\Build\vcvars64.bat')) {
                return $candidate
            }
        }
        return $null
    }
    $found = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                        -property installationPath -format value
    if ($found) {
        # vswhere may return several lines; take the first non-empty one
        return ($found | Where-Object { $_ -and $_.Trim() } | Select-Object -First 1)
    }
    return $null
}

function Find-WindowsSdkVersion {
    $candidates = @(
        'C:\Program Files (x86)\Windows Kits\10\Include',
        'C:\Program Files\Windows Kits\10\Include',
        'D:\Windows Kits\10\Include',
        'D:\Windows Kits\10\include'
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) {
            $v = Get-ChildItem $c -Directory -ErrorAction SilentlyContinue |
                 Where-Object { $_.Name -match '^10\.' } |
                 Sort-Object Name -Descending |
                 Select-Object -First 1
            if ($v) {
                Write-Host "Windows SDK    : $($v.Name)  ($c)" -ForegroundColor DarkGray
                return $v.Name
            }
        }
    }
    return $null
}

# --- Qt check ---------------------------------------------------------------
# Qt6Config.cmake is in lib/cmake/Qt6, but the module-specific config files
# (Qt6WebEngineWidgets, ...) live directly under lib/cmake. Keep both paths.
$qtCmakeDir = Join-Path $QtPrefix 'lib\cmake\Qt6'
$qtModulesDir = Join-Path $QtPrefix 'lib\cmake'
if (-not (Test-Path (Join-Path $qtCmakeDir 'Qt6Config.cmake'))) {
    Write-Host ""
    Write-Host "ERROR: Qt 6 not found at '$QtPrefix'" -ForegroundColor Red
    Write-Host ""
    Write-Host "Install Qt 6.8 LTS with these components:" -ForegroundColor Yellow
    Write-Host "  - Qt 6.8.x -> MSVC 2022 64-bit   (required)"
    Write-Host "  - Qt WebEngine, Qt WebChannel, Qt Positioning   (required for the GUI)"
    Write-Host ""
    Write-Host "Account-free install command:"
    Write-Host "  python -m pip install aqtinstall"
    Write-Host "  python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --modules qtwebengine qtwebchannel qtpositioning --outputdir D:\Qt"
    Write-Host ""
    Write-Host "Then either install to D:\Qt\6.8.3\msvc2022_64, or pass the real path:"
    Write-Host "  .\build.ps1 -QtPrefix 'D:\Qt\6.9.0\msvc2022_64'"
    Write-Host ""
    exit 1
}

if (-not $NoGui) {
    if (-not (Test-Path (Join-Path $qtModulesDir 'Qt6WebEngineWidgets'))) {
        Write-Host ""
        Write-Host "WARNING: Qt WebEngine is not installed at '$QtPrefix'." -ForegroundColor Yellow
        Write-Host "The GUI cannot be built. Build and test the core instead:"
        Write-Host "  .\build.ps1 -NoGui"
        Write-Host ""
        exit 1
    }
}

$vsPath = Find-VsInstall
if (-not $vsPath) {
    Write-Host "ERROR: Visual Studio C++ build tools not found." -ForegroundColor Red
    exit 1
}
Write-Host "Visual Studio  : $vsPath" -ForegroundColor DarkGray
Write-Host "Qt             : $QtPrefix" -ForegroundColor DarkGray

$sdkVersion = Find-WindowsSdkVersion

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir ..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

$cmakeArgs = @(
    '-S', $root
    '-B', $buildDir
    "-DCMAKE_PREFIX_PATH=$QtPrefix"
    "-DCMAKE_BUILD_TYPE=$Config"
    '-DNJULMS_WARNINGS_AS_ERRORS=OFF'
)
if ($NoTests)        { $cmakeArgs += '-DNJULMS_BUILD_TESTS=OFF' }
if ($NoGui)          { $cmakeArgs += '-DNJULMS_BUILD_GUI=OFF' }
if ($WithVerifyTool) { $cmakeArgs += '-DNJULMS_BUILD_VERIFY_TOOL=ON' }
if ($sdkVersion)     { $cmakeArgs += "-DCMAKE_SYSTEM_VERSION=$sdkVersion" }

if ($Generator -eq 'Ninja') {
    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) {
        Write-Host "ERROR: vcvars64.bat not found at $vcvars" -ForegroundColor Red
        exit 1
    }
    $cmakeArgs = @('-G', 'Ninja') + $cmakeArgs

    # Ninja needs the MSVC environment, so run cmake through a cmd that first
    # calls vcvars64.bat.
    $quoted = ($cmakeArgs | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }) -join ' '

    Write-Host ""
    Write-Host "=== Configure (Ninja) ===" -ForegroundColor Cyan
    cmd /c "call `"$vcvars`" >nul 2>&1 && cmake $quoted"
    if ($LASTEXITCODE -ne 0) { Write-Host "Configure FAILED" -ForegroundColor Red; exit 1 }

    Write-Host ""
    Write-Host "=== Build ===" -ForegroundColor Cyan
    cmd /c "call `"$vcvars`" >nul 2>&1 && cmake --build `"$buildDir`" --parallel"
    if ($LASTEXITCODE -ne 0) { Write-Host "Build FAILED" -ForegroundColor Red; exit 1 }
}
else {
    $cmakeArgs = @('-G', 'Visual Studio 18 2026', '-A', 'x64') + $cmakeArgs

    Write-Host ""
    Write-Host "=== Configure (Visual Studio) ===" -ForegroundColor Cyan
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { Write-Host "Configure FAILED" -ForegroundColor Red; exit 1 }

    Write-Host ""
    Write-Host "=== Build ===" -ForegroundColor Cyan
    & cmake --build $buildDir --config $Config --parallel
    if ($LASTEXITCODE -ne 0) { Write-Host "Build FAILED" -ForegroundColor Red; exit 1 }
}

if (-not $NoTests) {
    Write-Host ""
    Write-Host "=== Test ===" -ForegroundColor Cyan

    # The test executables link against Qt DLLs, so the Qt bin directory must be
    # on PATH. Do it only for this process tree instead of requiring the caller
    # to configure a system-wide PATH.
    $qtBin = Join-Path $QtPrefix 'bin'
    $env:PATH = "$qtBin;$env:PATH"

    # Qt tests do not need a real display; keep them headless and deterministic.
    $env:QT_QPA_PLATFORM = 'offscreen'

    $ctestArgs = @('--test-dir', $buildDir, '--output-on-failure')
    if ($Generator -eq 'VS') { $ctestArgs += @('-C', $Config) }
    & ctest @ctestArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host ""
        Write-Host "Some tests FAILED" -ForegroundColor Red
        exit 1
    }
}

# --- Deploy Qt runtime -------------------------------------------------------
# build\bin contains only the exe and pdb files; Qt is not on the system PATH,
# so the exe cannot start when launched from Explorer. windeployqt copies the
# Qt DLLs, platform/WebEngine plugins, resources and translations next to it.
if (-not $NoGui -and -not $NoDeploy) {
    $windeployqt = Join-Path $QtPrefix 'bin\windeployqt.exe'
    if (Test-Path $windeployqt) {
        Write-Host ""
        Write-Host "=== Deploy Qt runtime (windeployqt) ===" -ForegroundColor Cyan
        $appExe = Join-Path $buildDir 'bin\njulms_app.exe'
        $deployArgs = @()
        if ($Config -eq 'Debug') { $deployArgs += '--debug' }
        $deployArgs += $appExe
        & $windeployqt @deployArgs
        if ($LASTEXITCODE -ne 0) { Write-Host "windeployqt FAILED" -ForegroundColor Red; exit 1 }
    }
    else {
        Write-Host "WARNING: windeployqt.exe not found under '$QtPrefix\bin'." -ForegroundColor Yellow
        Write-Host "The app may fail to start unless Qt is on PATH." -ForegroundColor Yellow
    }
}
elseif (-not $NoGui -and $NoDeploy) {
    Write-Host "Skipping Qt deployment (-NoDeploy)." -ForegroundColor DarkGray
}

Write-Host ""
Write-Host "=== OK ===" -ForegroundColor Green
Write-Host "Build directory : $buildDir"
Write-Host "Binaries        : $buildDir\bin"
Write-Host ""
Write-Host "Next steps:"
Write-Host "  Run the app      : $buildDir\bin\njulms_app.exe"
Write-Host "  Live verification: .\verify.ps1    (after logging in)"
