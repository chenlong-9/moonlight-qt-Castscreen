param(
    [string]$Workspace = (Resolve-Path "$PSScriptRoot\..\..").Path,
    [string]$AppBuild = "$Workspace\artifacts\moonlight-native\build-x64-release-nobatch\app",
    [string]$Qt = "$Workspace\artifacts\moonlight-native\Qt\6.12.0\msvc2022_64",
    [string]$VcVars,
    [switch]$PersistenceOnly,
    [switch]$SettingsOnly
)
$ErrorActionPreference = 'Stop'
$out = "$Workspace\artifacts\receiver-regression"
New-Item -ItemType Directory -Force $out | Out-Null
$make = Get-Content "$AppBuild\Makefile.Release"
function MakeValue([string]$name) {
    $line = $make | Where-Object { $_ -match "^$name\s+=" } | Select-Object -First 1
    if (!$line) { throw "Missing $name in app Makefile.Release" }
    return ($line -replace "^$name\s+=\s*", '')
}
# Read app compiler ABI/dependency flags; all outputs go to this isolated folder.
$flags = (MakeValue 'CXXFLAGS').Replace('$(DEFINES)', (MakeValue 'DEFINES'))
$flags = $flags -replace '/Fdrelease\\Moonlight.vc.pdb', ('/Fd"' + "$out\regression.pdb" + '"')
$includes = MakeValue 'INCPATH'
$includes = $includes -replace '-Irelease(?=\s|$)', ('-I"' + "$AppBuild\release" + '"')
$includes += ' -I"' + "$Workspace\app" + '"'
$libs = (MakeValue 'LIBS') -replace 'release\\Moonlight_resource.res', ''
# Use existing application dependency objects, replacing the three files whose
# private layout/root changes may still be under review. Never run app nmake.
Get-ChildItem "$AppBuild\release\*.obj" | Where-Object {
    $_.BaseName -notin @('main','computermanager','receiverservice','moc_computermanager','moc_receiverservice')
} | Copy-Item -Destination $out
if (!$VcVars) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path $vswhere)) { throw 'Provide -VcVars with the full path to vcvars64.bat' }
    $vs = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Out-String).Trim()
    $VcVars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
}
if (!(Test-Path $VcVars)) { throw "MSVC environment script missing: $VcVars" }
$commands = @('@echo off', ('call "' + $VcVars + '"'), 'if errorlevel 1 exit /b %errorlevel%')
$backend = "$Workspace\app\backend"
foreach ($name in @('computermanager','receiverservice')) {
    $commands += '"' + "$Qt\bin\moc.exe" + '" -D_WIN32 -DWIN32 -DWIN64 ' + $includes + ' "' + "$backend\$name.h" + '" -o "' + "$out\moc_$name.cpp" + '"'
    if ($name -eq 'computermanager') {
        $commands += '"' + "$Qt\bin\moc.exe" + '" -D_WIN32 -DWIN32 -DWIN64 ' + $includes + ' "' + "$backend\$name.cpp" + '" -o "' + "$out\computermanager.moc" + '"'
    }
    $commands += 'if errorlevel 1 exit /b %errorlevel%'
    $commands += 'cl /c ' + $flags + ' ' + $includes + ' -I"' + $out + '" "' + "$backend\$name.cpp" + '" /Fo"' + "$out\$name.obj" + '"'
    $commands += 'if errorlevel 1 exit /b %errorlevel%'
    $commands += 'cl /c ' + $flags + ' ' + $includes + ' "' + "$out\moc_$name.cpp" + '" /Fo"' + "$out\moc_$name.obj" + '"'
    $commands += 'if errorlevel 1 exit /b %errorlevel%'
}
$commands += 'cl /c ' + $flags + ' ' + $includes + ' "' + "$PSScriptRoot\main.cpp" + '" /Fo"' + "$out\regression-main.obj" + '"'
$commands += 'if errorlevel 1 exit /b %errorlevel%'
$commands += 'link /NOLOGO /SUBSYSTEM:CONSOLE /OUT:"' + "$out\receiver-regression.exe" + '" "' + "$out\*.obj" + '" ' + $libs
$commands += 'exit /b %errorlevel%'
$commands | Set-Content -Encoding ascii "$out\build.cmd"
Push-Location $out
try {
    & cmd.exe /d /c "$out\build.cmd"
    if ($LASTEXITCODE -ne 0) { throw "Regression build failed ($LASTEXITCODE)" }
    $env:PATH = "$Qt\bin;$Workspace\libs\windows\lib\x64;$env:PATH"
    $env:QT_PLUGIN_PATH = "$Qt\plugins"
    if ($SettingsOnly) {
        & "$out\receiver-regression.exe" --settings-only
    } elseif ($PersistenceOnly) {
        & "$out\receiver-regression.exe" --persistence-only
    } else {
        & "$out\receiver-regression.exe"
    }
    if ($LASTEXITCODE -ne 0) { throw "Regression tests failed ($LASTEXITCODE)" }
} finally { Pop-Location }

