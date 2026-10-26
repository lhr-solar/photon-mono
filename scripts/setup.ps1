# Install what photon build needs on a fresh Windows machine, for the pieces named.
# Safe to rerun: each step checks first and skips what is already there.
#
#   setup.ps1 server               CMake, Ninja, clang, C++ build tools, Go
#   setup.ps1 client               CMake, Ninja, clang, C++ build tools, Git
#   setup.ps1 client --ui web      the same, plus Python and the pinned emsdk
#   setup.ps1 station              server and client with the web page
#   setup.ps1 ... --pcan           the PEAK PCAN driver, for photon client pcan
#   setup.ps1 ... --build          then run photon build with the same words
#
# No component words means server and the desktop client, the same as photon build.
param(
    [switch]$DryRun,
    [switch]$Pcan,
    [switch]$Build,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arg
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$UiDir = Join-Path $Root 'photon-next'
$LlvmBin = Join-Path $env:ProgramFiles 'LLVM\bin'
# Keep in step with photon-next\web\scripts\setup-emsdk.ps1.
$EmsdkVersion = '6.0.13'
# winget's "already installed, nothing newer" result.
$AlreadyInstalled = -1978335189

. (Join-Path $PSScriptRoot 'selection.ps1')

$words = @()
foreach ($word in @($Arg | Where-Object { $_ })) {
    switch ($word.ToLowerInvariant()) {
        '--pcan' { $Pcan = $true }
        '--build' { $Build = $true }
        '--dry-run' { $DryRun = $true }
        default { $words += $word }
    }
}
try {
    $sel = Read-Selection $words
} catch {
    Write-Output $_.Exception.Message
    exit 2
}
if ($sel.Rest.Count -gt 0) {
    Write-Output ('unknown setup word: ' + ($sel.Rest -join ' '))
    exit 2
}
$native = $sel.Server -or $sel.Client -or $sel.Tools

function Update-SessionPath {
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}

function Test-Tool {
    param([string]$Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    # The WindowsApps python is a Store redirect, not an interpreter.
    return ($cmd -and $cmd.Source -notlike '*\WindowsApps\python*')
}

function Install-Package {
    param([string]$Id, [string]$Label, [bool]$Present, [string]$Override)
    if ($Present) {
        Write-Output ("have    " + $Label)
        return
    }
    if ($DryRun) {
        Write-Output ("install " + $Label + "  (winget " + $Id + ")")
        return
    }
    Write-Output ("install " + $Label)
    $wingetArgs = @('install', '--id', $Id, '-e', '--accept-package-agreements', '--accept-source-agreements')
    if ($Override) {
        $wingetArgs += @('--override', $Override)
    }
    & winget @wingetArgs
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne $AlreadyInstalled) {
        throw ("winget install " + $Id + " failed with " + $LASTEXITCODE)
    }
}

function Test-VcTools {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        return $false
    }
    $found = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Lib'
    return ($found -and (Test-Path -LiteralPath $sdk))
}

function Add-UserPath {
    param([string]$Dir)
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $parts = @()
    if ($userPath) {
        $parts = $userPath.Split(';') | Where-Object { $_ }
    }
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine').Split(';')
    if (($parts + $machine) -contains $Dir) {
        Write-Output ("have    " + $Dir + " on PATH")
        return
    }
    if ($DryRun) {
        Write-Output ("path    add " + $Dir)
        return
    }
    Write-Output ("path    add " + $Dir)
    [Environment]::SetEnvironmentVariable('Path', (($parts + $Dir) -join ';'), 'User')
}

function Install-Emsdk {
    $versionFile = Join-Path $UiDir '.external\emsdk\upstream\emscripten\emscripten-version.txt'
    $have = $null
    if (Test-Path -LiteralPath $versionFile) {
        $have = (Get-Content -LiteralPath $versionFile -Raw).Trim().Trim('"')
    }
    if ($have -eq $EmsdkVersion) {
        Write-Output ("have    emsdk " + $EmsdkVersion)
        return
    }
    $label = 'emsdk ' + $EmsdkVersion
    if ($have) {
        $label += ' (have ' + $have + ')'
    }
    if ($DryRun) {
        Write-Output ('install ' + $label + '  (photon-next\web\scripts\setup-emsdk.ps1)')
        return
    }
    Write-Output ('install ' + $label)
    & (Join-Path $UiDir 'web\scripts\setup-emsdk.ps1') -Version $EmsdkVersion
}

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw 'winget is missing. Install "App Installer" from the Microsoft Store, then rerun.'
}

Write-Output ('setup for ' + (Format-Selection $sel))
Update-SessionPath
if ($native) {
    Install-Package 'Kitware.CMake' 'CMake' (Test-Tool 'cmake')
    Install-Package 'Ninja-build.Ninja' 'Ninja' (Test-Tool 'ninja')
    Install-Package 'LLVM.LLVM' 'LLVM (clang)' ((Test-Tool 'clang') -or (Test-Path -LiteralPath (Join-Path $LlvmBin 'clang.exe')))
    # clang links against the MSVC libraries and the Windows SDK.
    Install-Package 'Microsoft.VisualStudio.BuildTools' 'Visual Studio Build Tools (C++ and Windows SDK)' (Test-VcTools) `
        '--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
}
if ($sel.Server -or $sel.Tools) {
    Install-Package 'GoLang.Go' 'Go' (Test-Tool 'go')
}
if ($sel.Client) {
    # CMake fetches ImGui, ImPlot, and tinygltf with git; emsdk is a git clone.
    Install-Package 'Git.Git' 'Git' (Test-Tool 'git')
}
if ($sel.Web) {
    Install-Package 'Python.Python.3.13' 'Python' (Test-Tool 'python') `
        '/quiet InstallAllUsers=0 PrependPath=1 Include_test=0'
}
if ($Pcan) {
    $pcanDll = Join-Path $env:SystemRoot 'System32\PCANBasic.dll'
    Install-Package 'PEAKSystem.PEAKDrivers' 'PEAK PCAN driver' (Test-Path -LiteralPath $pcanDll)
}

if ($native) {
    # The LLVM installer does not add itself to PATH.
    Add-UserPath $LlvmBin
}
Update-SessionPath

if ($sel.Web) {
    Install-Emsdk
}

if ($native -and -not $DryRun -and -not (Test-VcTools)) {
    Write-Output ''
    Write-Output 'Visual Studio is installed without the C++ build tools. Open "Visual Studio Installer",'
    Write-Output 'choose Modify, and add "Desktop development with C++".'
    exit 1
}

if ($Build -and -not $DryRun) {
    & (Join-Path $PSScriptRoot 'photon.ps1') build @words
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Output ''
Write-Output 'Setup done. Restart VS Code and any open terminals so they pick up the new PATH.'
if (-not $Build) {
    Write-Output ('Then run: photon build ' + ($words -join ' '))
}
