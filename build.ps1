param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64')]
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'

function Find-MSBuild {
    $candidates = @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\17\Community\MSBuild\Current\Bin\MSBuild.exe',
        'C:\Program Files\Microsoft Visual Studio\17\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }

    $cmd = Get-Command msbuild -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    throw 'MSBuild.exe was not found. Install Visual Studio with Desktop development for C++ or Build Tools.'
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$msbuild = Find-MSBuild

$projects = @(
    Join-Path $scriptDir 'saninet_autologin.vcxproj',
    Join-Path $scriptDir 'saninet_injector.vcxproj'
)

Write-Host "Using MSBuild: $msbuild"
Write-Host "Configuration: $Configuration | Platform: $Platform"

foreach ($project in $projects) {
    Write-Host "`n==> Building $(Split-Path $project -Leaf)"
    & $msbuild $project /t:Rebuild /p:Configuration=$Configuration /p:Platform=$Platform
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed: $project"
    }
}

$binDir = Join-Path $scriptDir "bin\$Configuration"
$dllPath = Join-Path $binDir 'saninet_autologin.dll'
$injectorPath = Join-Path $binDir 'saninet_injector.exe'

Write-Host "`nBuild complete."
Write-Host "DLL:      $dllPath"
Write-Host "Injector: $injectorPath"
