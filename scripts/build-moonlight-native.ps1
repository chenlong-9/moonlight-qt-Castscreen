<#
.SYNOPSIS
Build the unmodified Moonlight qmake project with a workspace-local Qt SDK.
.DESCRIPTION
No system installation, persistent environment changes, clean, signing, MSI, or Sunshine actions.
Prepare downloads only with -Prepare. Default requires VS2022; use -AllowNewerMsvc
explicitly for a compatibility experiment with a newer installed Visual Studio.
A Python with pip is needed only for -Prepare; pass its full path with -Python.
.EXAMPLE
powershell -File .\scripts\build-moonlight-native.ps1 -Prepare -Python C:\Python312\python.exe
.EXAMPLE
powershell -File .\scripts\build-moonlight-native.ps1 -AllowNewerMsvc
#>
[CmdletBinding()]
param(
    [string]$WorkspaceRoot,
    [string]$QtRoot,
    [string]$BuildDirectory,
    [string]$Python = 'python',
    [string]$VisualStudioPath,
    [ValidateSet('release','debug')][string]$Configuration = 'release',
    [switch]$Prepare,
    [switch]$PrepareOnly,
    [switch]$CheckOnly,
    [switch]$NoBatch,
    [switch]$AllowNewerMsvc
)
$ErrorActionPreference = 'Stop'
if (!$WorkspaceRoot) { $WorkspaceRoot = Split-Path $PSScriptRoot -Parent }
$WorkspaceRoot = [IO.Path]::GetFullPath($WorkspaceRoot)
$source = $WorkspaceRoot
$artifacts = Join-Path $WorkspaceRoot 'artifacts\moonlight-native'
$downloads = Join-Path $artifacts 'downloads'
$logs = Join-Path $artifacts 'logs'
if (!$QtRoot) { $QtRoot = Join-Path $artifacts 'Qt\6.12.0\msvc2022_64' }
if (!$BuildDirectory) { $BuildDirectory = Join-Path (Split-Path $source -Parent) ('build-moonlight-qt-' + $Configuration) }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$QtRoot = [IO.Path]::GetFullPath($QtRoot)
if (!(Test-Path (Join-Path $source 'moonlight-qt.pro'))) { throw "Missing Moonlight project: $source" }
foreach ($dir in @($artifacts,$downloads,$logs)) { New-Item -ItemType Directory -Force $dir | Out-Null }
function Assert-Exit([string]$Action) { if ($LASTEXITCODE -ne 0) { throw "$Action failed (exit $LASTEXITCODE)" } }
if ($Prepare) {
    $packages = Join-Path $artifacts 'tools\python-packages'
    $oldPythonPath = $env:PYTHONPATH
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $env:PYTHONPATH = $packages
        $ErrorActionPreference = 'Continue' # Windows PowerShell 5 treats native stderr as ErrorRecords.
        & $Python -m pip install --disable-pip-version-check --upgrade --target $packages 'git+https://github.com/miurahr/aqtinstall.git@073e34d7c2ab4ae6961ed7cca690b3abd5ba5a7e'
        $ErrorActionPreference = 'Stop'
        Assert-Exit 'Workspace-local CI-pinned aqt installation'
        if (!(Test-Path (Join-Path $QtRoot 'bin\qmake.exe'))) {
            $ErrorActionPreference = 'Continue'
            & $Python -m aqt install-qt windows desktop 6.12.0 win64_msvc2022_64 -O (Join-Path $artifacts 'Qt') --archives qtbase qtsvg qtdeclarative qttools qttranslations -k -d $downloads --timeout 60
            $ErrorActionPreference = 'Stop'
            Assert-Exit 'Qt SDK download'
        }
        $qtArchives = @(Get-ChildItem -LiteralPath $downloads -Filter '*.7z' | ForEach-Object {
            [ordered]@{ name=$_.Name; bytes=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
        })
        $qtRecord = [ordered]@{ qtVersion='6.12.0'; target='win64_msvc2022_64'; archives=$qtArchives; aqtCommit='073e34d7c2ab4ae6961ed7cca690b3abd5ba5a7e'; prepared=(Get-Date).ToString('o') }
        $qtRecord | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $artifacts 'qt-sdk-lock.json') -Encoding utf8
    } finally { $env:PYTHONPATH = $oldPythonPath; $ErrorActionPreference = $oldPreference }
    $zip = Join-Path $downloads 'windows-x64-v20.1.zip'
    $expected = '5FC9F15FC61D8346EC44981127A39F3FE546AFEC1D7BC7858218EBA2E3B0C646'
    if (!(Test-Path $zip)) {
        & curl.exe --fail --location --retry 2 --output $zip 'https://github.com/moonlight-stream/moonlight-qt-deps/releases/download/v20.1/windows-x64.zip'
        Assert-Exit 'Moonlight v20.1 dependency download'
    }
    if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $expected) { throw 'v20.1 dependency archive SHA256 mismatch' }
    $deps = Join-Path $source 'libs\windows'
    # Do not silently replace an existing user dependency tree.
    if (!(Test-Path $deps)) {
        Expand-Archive -LiteralPath $zip -DestinationPath $deps
        Set-Content -LiteralPath (Join-Path $deps '.native-v20.1-x64.sha256') -Value $expected -Encoding ascii
    } elseif (!(Test-Path (Join-Path $deps '.native-v20.1-x64.sha256'))) {
        Write-Warning 'Existing libs/windows preserved; its provenance must be verified separately.'
    }
}
if ($PrepareOnly) { return }
$qmake = Join-Path $QtRoot 'bin\qmake.exe'
if (!(Test-Path $qmake)) { throw "Qt6.12.0 msvc2022_64 SDK missing: $qmake. Run with -Prepare -Python <python.exe>." }
$qtVersion = (& $qmake -query QT_VERSION | Out-String).Trim()
Assert-Exit 'qmake version query'
if ($qtVersion -ne '6.12.0') { throw "Expected Qt 6.12.0; found $qtVersion" }
foreach ($relative in @('lib\Qt6Quick.lib','lib\Qt6QuickControls2.lib','lib\Qt6Svg.lib')) {
    if (!(Test-Path (Join-Path $QtRoot $relative))) { throw "Incomplete Qt SDK: $relative" }
}
foreach ($relative in @('libs\windows\lib\x64\SDL2.lib','libs\windows\lib\x64\avcodec.lib','libs\windows\lib\x64\libssl.lib','moonlight-common-c\moonlight-common-c\src\Limelight.h','qmdnsengine\qmdnsengine\src\include\qmdnsengine\server.h','app\SDL_GameControllerDB\gamecontrollerdb.txt')) {
    if (!(Test-Path (Join-Path $source $relative))) { throw "Missing Moonlight dependency/submodule: $relative" }
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path $vswhere)) { throw 'vswhere not found; install/provide VS2022 C++ tools outside this script.' }
$vsArgs = @('-latest','-products','*','-property','installationPath')
if (!$AllowNewerMsvc) { $vsArgs += @('-version','[17.0,18.0)') }
$vs = (& $vswhere @vsArgs | Out-String).Trim()
Assert-Exit 'Visual Studio discovery'
if ($VisualStudioPath) {
    if (!$AllowNewerMsvc) { throw 'Explicit VisualStudioPath requires -AllowNewerMsvc (non-equivalent toolchain experiment).' }
    $vs = [IO.Path]::GetFullPath($VisualStudioPath)
    Write-Warning 'Using explicit VS path; installer completeness/component registration is not guaranteed.'
}
if (!$vs) { throw 'VS2022 C++ tools not found. No global installation attempted. -AllowNewerMsvc permits a non-identical compatibility experiment.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
if (!(Test-Path $vcvars)) { throw "Missing vcvarsall: $vcvars" }
$toolVersions = Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending
$compiler = $toolVersions | ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe' } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (!$compiler) { throw "x64 cl.exe not found under $vs" }
if ($AllowNewerMsvc) { Write-Warning 'Newer MSVC allowed: this run does not establish an exact VS2022 reproduction.' }
Write-Host "Qt: $QtRoot ($qtVersion); Visual Studio: $vs; output: $BuildDirectory"
if ($CheckOnly) { return }
if ($BuildDirectory -eq $source -or $BuildDirectory.StartsWith($source + '\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Use an out-of-source build directory outside the Moonlight checkout.'
}
New-Item -ItemType Directory -Force $BuildDirectory | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$batch = Join-Path $BuildDirectory ("invoke-native-$stamp.cmd")
$log = Join-Path $logs ("build-$stamp.log")
$compilerProbe = Join-Path $BuildDirectory 'compiler-probe.c'
Set-Content -LiteralPath $compilerProbe -Value 'int moonlight_compiler_probe(void) { return 0; }' -Encoding ascii
@(
    '@echo off',
    'setlocal',
    ('call "' + $vcvars + '" x64'),
    'if errorlevel 1 exit /b %errorlevel%',
    ('set "PATH=' + (Join-Path $QtRoot 'bin') + ';%PATH%"'),
    ('cd /d "' + $BuildDirectory + '"'),
    ('cl.exe /Bv /c /Fo"' + (Join-Path $BuildDirectory 'compiler-probe.obj') + '" "' + (Join-Path $BuildDirectory 'compiler-probe.c') + '" 2>&1'),
    'if errorlevel 1 exit /b %errorlevel%',
    ('"' + $qmake + '" "' + (Join-Path $source 'moonlight-qt.pro') + '"' + $(if ($NoBatch) { ' -recursive CONFIG+=no_batch' } else { '' })),
    'if errorlevel 1 exit /b %errorlevel%',
    ('nmake.exe /NOLOGO ' + $Configuration),
    'exit /b %errorlevel%'
) | Set-Content -LiteralPath $batch -Encoding ascii
$ErrorActionPreference = 'Continue'
& $env:ComSpec /d /c ('"' + $batch + '"') 2>&1 | ForEach-Object { $line = $_.ToString(); Add-Content -LiteralPath $log -Value $line -Encoding utf8; Write-Output $line }
$buildExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
$result = [ordered]@{
    timestamp = (Get-Date).ToString('o'); qtVersion = $qtVersion; qtRoot = $QtRoot
    visualStudio = $vs; allowNewerMsvc = [bool]$AllowNewerMsvc
    configuration = $Configuration; buildDirectory = $BuildDirectory; noBatch = [bool]$NoBatch; compiler = $compiler
    exitCode = $buildExit; log = $log
    executable = (Join-Path $BuildDirectory "app\$Configuration\Moonlight.exe")
}
$result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $logs "build-$stamp.json") -Encoding utf8
if ($buildExit -ne 0) { throw "Native compilation failed ($buildExit). Evidence: $log" }
if (!(Test-Path $result.executable)) { throw "Build returned zero but executable missing: $($result.executable)" }
Write-Host "Compiled executable: $($result.executable). Not packaged or runtime-tested. Log: $log"

