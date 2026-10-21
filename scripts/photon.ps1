# Run one Photon piece, and say what that run is for.
# The repo is the server (photon-server) and the UI (photon-next).
param(
    [switch]$DryRun,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arg
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ServerDir = Join-Path $Root 'photon-server'
$UiDir = Join-Path $Root 'photon-next'

function Show-Use {
    Write-Output @'
Photon is two programs. Run the one you need, and name the use case.

  photon build
      Build the server and the UI into build\. The server finds
      photon-backend, photon-index, photon-ui, and assets beside itself.

  photon server
      Ingest only. The DAQ dials the CAN ingress on port 6100.
      The video store and the fan-out stay up. The UI stays down.

  photon server ingest PORT
      Same server. The DAQ dials PORT instead of 6100.
        photon server ingest 6100

  photon ui local
      UI on this machine, reading the server here.
      CAN 127.0.0.1:6500, video TCP :6700, video UDP :6800.

  photon ui daq HOST[:PORT]
      UI ingesting CANP from that fan-out. The port defaults to 6500.
      Video uses the same host, TCP :6700 and UDP :6800.
      The public Oracle fan-out is port 6110:
        photon ui daq 150.136.85.207:6110

  photon ui oracle
      UI on the public Oracle pipe.
      CAN 150.136.85.207:6110, video TCP :6700, video UDP 10.66.0.1:6800.

  photon ui pcan
      UI reading a USB PCAN on this machine.

  photon ui log PATH
      UI replaying a VSD1 log or an on_track folder.

  photon station
      Server and UI on this machine. The UI reads this server's fan-out.
      photon station ingest PORT moves the DAQ's ingress port the same way.

Flags after the use case are passed through (--password, --video-port, -ingress, ...).
PHOTON_PASSWORD defaults to laptop. PHOTON_DATA_DIR defaults to <repo>\data.
'@
}

function Split-HostPort {
    param([string]$Spec, [int]$DefaultPort)
    if ($Spec -match '^(?<host>.+):(?<port>\d+)$') {
        return @{ Host = $Matches.host; Port = [int]$Matches.port }
    }
    return @{ Host = $Spec; Port = $DefaultPort }
}

function Take-Target {
    if ($script:Rest.Count -gt 0 -and $script:Rest[0] -notlike '-*') {
        $target = $script:Rest[0]
        if ($script:Rest.Count -eq 1) {
            $script:Rest = @()
        } else {
            $script:Rest = $script:Rest[1..($script:Rest.Count - 1)]
        }
        return $target
    }
    return $null
}

function Get-Password {
    $secret = [Environment]::GetEnvironmentVariable('PHOTON_PASSWORD')
    if ([string]::IsNullOrWhiteSpace($secret)) {
        return 'laptop'
    }
    return $secret.Trim()
}

function Get-DataDir {
    $dir = [Environment]::GetEnvironmentVariable('PHOTON_DATA_DIR')
    if ([string]::IsNullOrWhiteSpace($dir)) {
        return (Join-Path $Root 'data')
    }
    return $dir.Trim()
}

function Find-Ship {
    param([string]$Name)
    $path = Join-Path $Root (Join-Path 'build' $Name)
    if (Test-Path -LiteralPath $path) {
        return (Resolve-Path -LiteralPath $path).Path
    }
    return $null
}

function Find-Backend {
    return Find-Ship 'photon-backend.exe'
}

function Find-NativeUi {
    return Find-Ship 'photon-ui.exe'
}

function Find-Page {
    $path = Join-Path $Root 'build\ui\photon.html'
    if (Test-Path -LiteralPath $path) {
        return (Resolve-Path -LiteralPath $path).Path
    }
    return $null
}

function Show-Command {
    param([string]$Title, [string]$File, [string]$Work, [string[]]$CmdArgs)
    Write-Output ($Title + ':')
    if ($Work) {
        Write-Output ('  cd ' + $Work)
    }
    $rendered = @($File) + @($CmdArgs)
    Write-Output ('  ' + ($rendered -join ' '))
}

function Start-Logged {
    param([string]$File, [string]$Work, [string[]]$CmdArgs)
    if ($DryRun) {
        Show-Command 'run' $File $Work $CmdArgs
        return
    }
    if ($Work) {
        Push-Location $Work
    }
    try {
        & $File @CmdArgs
        if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    } finally {
        if ($Work) {
            Pop-Location
        }
    }
}

function Start-Build {
    $ship = Join-Path $Root 'build'
    $cache = Join-Path $UiDir '.artifacts'
    $vcpkgToolchain = $null
    if ($env:VCPKG_ROOT) {
        $vcpkgToolchain = Join-Path $env:VCPKG_ROOT 'scripts\buildsystems\vcpkg.cmake'
        if (-not (Test-Path $vcpkgToolchain)) {
            throw "VCPKG_ROOT is set but vcpkg.cmake was not found at $vcpkgToolchain"
        }
        $cache = Join-Path $UiDir '.artifacts-vcpkg'
    }
    if (-not $DryRun) {
        New-Item -ItemType Directory -Force -Path $ship | Out-Null
    }
    $cmakeArgs = @(
        '-S', $UiDir,
        '-B', $cache,
        '-G', 'Ninja',
        '-D', 'CMAKE_C_COMPILER=clang',
        '-D', 'CMAKE_CXX_COMPILER=clang++',
        '-D', ("PHOTON_SHIP_DIR=" + ($ship -replace '\\', '/'))
    )
    if ($vcpkgToolchain) {
        $cmakeArgs += @('-D', ("CMAKE_TOOLCHAIN_FILE=" + ($vcpkgToolchain -replace '\\', '/')))
        $cmakeArgs += @('-D', 'PHOTON_REQUIRE_ARRAY_SIM=ON')
    }
    $buildArgs = @('--build', $cache, '--target', 'photon-backend', 'photon-index', 'photon-ui')
    if ($DryRun) {
        Show-Command 'cmake' 'cmake' $null $cmakeArgs
        Show-Command 'cmake' 'cmake' $null $buildArgs
        Show-Command 'go' 'go build -o' $ServerDir @((Join-Path $ship 'photon-server.exe'), './server')
        Show-Command 'go' 'go build -o' $ServerDir @((Join-Path $ship 'photon-sshd.exe'), './cmd/photon-sshd')
        return
    }
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & cmake @buildArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Push-Location $ServerDir
    try {
        & go build -o (Join-Path $ship 'photon-server.exe') ./server
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & go build -o (Join-Path $ship 'photon-sshd.exe') ./cmd/photon-sshd
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        Pop-Location
    }
    $page = Join-Path $UiDir '.artifacts-web\photon.html'
    if (Test-Path -LiteralPath $page) {
        $uiOut = Join-Path $ship 'ui'
        New-Item -ItemType Directory -Force -Path $uiOut | Out-Null
        $webDir = Split-Path $page -Parent
        foreach ($name in @('photon.html', 'photon.js', 'photon.wasm', 'photon.data')) {
            $src = Join-Path $webDir $name
            if (Test-Path -LiteralPath $src) {
                Copy-Item -LiteralPath $src -Destination (Join-Path $uiOut $name) -Force
            }
        }
    }
    Write-Output ("built " + $ship)
}

function Start-Server {
    param([switch]$WithUi)
    $password = Get-Password
    $dataDir = Get-DataDir
    $ship = Join-Path $Root 'build'
    $exe = Find-Ship 'photon-server.exe'
    $ingress = $null
    if ($script:Rest.Count -gt 0 -and $script:Rest[0].ToLowerInvariant() -eq 'ingest') {
        if ($script:Rest.Count -lt 2 -or $script:Rest[1] -notmatch '^\d+$') {
            Write-Output 'photon server ingest needs a port'
            exit 2
        }
        $ingress = $script:Rest[1]
        if ($script:Rest.Count -le 2) {
            $script:Rest = @()
        } else {
            $script:Rest = @($script:Rest[2..($script:Rest.Count - 1)])
        }
    }
    $serverArgs = @('-data-dir', $dataDir, '-password', $password)
    if ($ingress) {
        $serverArgs += @('-ingress', $ingress)
    }
    if ($WithUi) {
        if (-not (Find-Backend) -and -not $DryRun) {
            Write-Output 'build\photon-backend.exe is missing. Run: photon build'
            exit 1
        }
    } else {
        $serverArgs += @('-ui', 'off')
    }
    if ($Rest) {
        $serverArgs += $Rest
    }
    if (-not $DryRun) {
        New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
    }
    $shownPort = '6100'
    if ($ingress) {
        $shownPort = $ingress
    }
    Write-Output ("ingest " + $shownPort + "  password " + $password + "  data " + $dataDir)
    if (-not $exe) {
        if ($DryRun) {
            Show-Command 'run' (Join-Path $ship 'photon-server.exe') $ship $serverArgs
            return
        }
        Write-Output 'build\photon-server.exe is missing. Run: photon build'
        exit 1
    }
    Start-Logged $exe $ship $serverArgs
}

function Start-Ui {
    param(
        [string]$Source,
        [string]$DaqHost,
        [int]$DaqPort,
        [string]$VideoHost,
        [int]$VideoPort,
        [string]$VideoUdpHost,
        [int]$VideoUdpPort,
        [string]$LogPath
    )
    $backend = Find-Backend
    if (-not $backend) {
        if (-not $DryRun) {
            Write-Output 'build\photon-backend.exe is missing. Run: photon build'
            exit 1
        }
        $backend = Join-Path $Root 'build\photon-backend.exe'
    }
    $password = Get-Password
    $assets = Join-Path $Root 'build\assets'
    if (-not (Test-Path -LiteralPath $assets)) {
        $assets = Join-Path $UiDir 'assets'
    }
    $backendArgs = @(
        '--no-live-tail',
        '--source', $Source,
        '--ws-port', '8765',
        '--password', $password,
        '--assets-dir', $assets
    )
    if ($Source -eq 'daq_tcp') {
        $backendArgs += @(
            '--daq-host', $DaqHost,
            '--daq-port', "$DaqPort",
            '--video-host', $VideoHost,
            '--video-port', "$VideoPort",
            '--video-udp-port', "$VideoUdpPort",
            '--video-udp-host', $VideoUdpHost
        )
    } elseif ($Source -eq 'log_replay') {
        $backendArgs += @('--log-path', $LogPath)
    }
    if ($Rest) {
        $backendArgs += $Rest
    }

    $native = Find-NativeUi
    $page = Find-Page
    $pageProc = $null
    $uiProc = $null
    Write-Output ("password " + $password)
    if ($Source -eq 'daq_tcp') {
        Write-Output ("daq " + $DaqHost + ":" + $DaqPort + "  video " + $VideoHost + ":" + $VideoPort + "  udp " + $VideoUdpHost + ":" + $VideoUdpPort)
    }
    if ($DryRun) {
        Show-Command 'backend' $backend $null $backendArgs
        if ($native) {
            Show-Command 'ui' $native $null @()
        } elseif ($page) {
            $pageDir = Split-Path $page -Parent
            Show-Command 'page' 'python -m http.server 8080 --bind 127.0.0.1 --directory' $null @($pageDir)
        } else {
            Write-Output 'ui: not built (run photon build, and photon-next\web\scripts\build.ps1 for the page)'
        }
        return
    }

    if ($native) {
        $uiProc = Start-Process -FilePath $native -WorkingDirectory (Split-Path $native -Parent) -PassThru
        Write-Output ("ui " + $native)
    } elseif ($page) {
        $pageDir = Split-Path $page -Parent
        $py = $null
        foreach ($name in @('py', 'python')) {
            if (Get-Command $name -ErrorAction SilentlyContinue) {
                $py = $name
                break
            }
        }
        if ($py) {
            $pyArgs = @('-m', 'http.server', '8080', '--bind', '127.0.0.1', '--directory', $pageDir)
            if ($py -eq 'py') {
                $pyArgs = @('-3') + $pyArgs
            }
            $pageProc = Start-Process -FilePath $py -ArgumentList $pyArgs -WindowStyle Minimized -PassThru
            Write-Output 'page http://127.0.0.1:8080/photon.html'
        } else {
            Write-Output ("page files are in " + $pageDir + " and Python is not on PATH")
        }
    } else {
        Write-Output 'ui binary and photon.html were not built; the daemon will still listen on ws://127.0.0.1:8765'
    }

    try {
        & $backend @backendArgs
    } finally {
        if ($uiProc -and -not $uiProc.HasExited) {
            Stop-Process -Id $uiProc.Id -Force -ErrorAction SilentlyContinue
        }
        if ($pageProc -and -not $pageProc.HasExited) {
            Stop-Process -Id $pageProc.Id -Force -ErrorAction SilentlyContinue
        }
    }
}

if (-not $Arg -or $Arg.Count -eq 0 -or $Arg[0] -in @('help', '-h', '--help')) {
    Show-Use
    exit 0
}

$command = $Arg[0].ToLowerInvariant()
if ($Arg.Count -gt 1) {
    $script:Rest = @($Arg[1..($Arg.Count - 1)])
} else {
    $script:Rest = @()
}

switch ($command) {
    'build' {
        Start-Build
    }
    'server' {
        Start-Server
    }
    'station' {
        Start-Server -WithUi
    }
    'ui' {
        if ($Rest.Count -eq 0) {
            Show-Use
            exit 2
        }
        $use = $Rest[0].ToLowerInvariant()
        if ($Rest.Count -le 1) {
            $script:Rest = @()
        } else {
            $script:Rest = @($Rest[1..($Rest.Count - 1)])
        }
        switch ($use) {
            'local' {
                Start-Ui -Source 'daq_tcp' -DaqHost '127.0.0.1' -DaqPort 6500 -VideoHost '127.0.0.1' -VideoPort 6700 -VideoUdpHost '127.0.0.1' -VideoUdpPort 6800
            }
            'oracle' {
                Start-Ui -Source 'daq_tcp' -DaqHost '150.136.85.207' -DaqPort 6110 -VideoHost '150.136.85.207' -VideoPort 6700 -VideoUdpHost '10.66.0.1' -VideoUdpPort 6800
            }
            'daq' {
                $target = Take-Target
                if (-not $target) {
                    Write-Output 'photon ui daq needs HOST or HOST:PORT'
                    exit 2
                }
                $hp = Split-HostPort $target 6500
                Start-Ui -Source 'daq_tcp' -DaqHost $hp.Host -DaqPort $hp.Port -VideoHost $hp.Host -VideoPort 6700 -VideoUdpHost $hp.Host -VideoUdpPort 6800
            }
            'pcan' {
                Start-Ui -Source 'pcan'
            }
            'log' {
                $path = Take-Target
                if (-not $path) {
                    Write-Output 'photon ui log needs a VSD1 log or on_track folder'
                    exit 2
                }
                Start-Ui -Source 'log_replay' -LogPath $path
            }
            default {
                Write-Output ("unknown UI use case: " + $use)
                Show-Use
                exit 2
            }
        }
    }
    default {
        Write-Output ("unknown command: " + $command)
        Show-Use
        exit 2
    }
}
