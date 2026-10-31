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
Go owns recordings and replay. Photon Next is the display client.

  photon build
      Build the Go server, UI, PCAN adapter,
      and one assets tree into build\.

  photon server
      Ingest on 6100 and serve the authenticated UI and /ws on 8080.

  photon server ingest PORT
      Same server. The DAQ dials PORT instead of 6100.
        photon server ingest 6100

  photon ui local
      UI on this machine, reading the server here.
      Connect directly to ws://127.0.0.1:8080/ws.

  photon ui daq HOST[:PORT]
      Connect directly to that Go server's HTTP/WebSocket port (8080).

  photon ui oracle
      Open the public website in your browser.

  photon ui pcan
      Start a local Go server and the small USB PCAN adapter.
      A local InfluxDB instance and server token are required for plots.

  photon ui log RECORDING_ID
      Open an existing recording from the running local Go server.

  photon station
      Server and UI on this machine. The UI reads this server's fan-out.
      photon station ingest PORT moves the DAQ's ingress port the same way.

Flags after the use case are passed to the Go server or display client.
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

# Start-Process joins argument arrays; quote each Windows argument explicitly.
function Join-ProcessArgs {
    param([string[]]$Values)
    $quoted = foreach ($value in $Values) {
        $escaped = [regex]::Replace($value, '(\\*)"', '$1$1\"')
        $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
        '"' + $escaped + '"'
    }
    return ($quoted -join ' ')
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
    $buildArgs = @('--build', $cache, '--target', 'photon-ui', 'photon-pcan-adapter', 'photon-tsdb-export')
    if ($DryRun) {
        Show-Command 'cmake' 'cmake' $null $cmakeArgs
        Show-Command 'cmake' 'cmake' $null $buildArgs
        Show-Command 'go' 'go build -o' $ServerDir @((Join-Path $ship 'photon-server.exe'), './server')
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
    } finally {
        Pop-Location
    }
    Copy-Item -LiteralPath (Join-Path $UiDir 'assets') -Destination $ship -Recurse -Force
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
    $stationUi = $null
    if ($WithUi -and -not $DryRun) {
        $env:PHOTON_PASSWORD = $password
        if ($native = Find-NativeUi) { $stationUi = Start-Process -FilePath $native -ArgumentList @('--ws','ws://127.0.0.1:8080/ws') -WorkingDirectory $ship -PassThru }
    }
    try { Start-Logged $exe $ship $serverArgs } finally {
        if ($stationUi -and -not $stationUi.HasExited) { Stop-Process -Id $stationUi.Id -ErrorAction SilentlyContinue }
    }
}

function Start-Ui {
    param([string]$Source, [string]$DaqHost='127.0.0.1', [int]$DaqPort=8080,
          [string]$VideoHost, [int]$VideoPort, [string]$VideoUdpHost, [int]$VideoUdpPort,
          [string]$LogPath, [switch]$Browser)
    $ship=Join-Path $Root 'build'
    $password=Get-Password
    $env:PHOTON_PASSWORD=$password
    $serverProc=$null
    $adapterProc=$null
    $recordingId=$null
    if ($Source -eq 'log_replay') {
        if ($LogPath -notmatch '^[a-fA-F0-9]{32}$') { throw 'Use a recording ID from the Go recording catalog' }
        $recordingId=$LogPath
    }
    $ownedLocal=$Source -eq 'pcan'
    if ($ownedLocal) {
        $server=Join-Path $ship 'photon-server.exe'
        $serverArgs=@('-host','127.0.0.1','-data-dir',(Get-DataDir))
        if ($DryRun) { Show-Command 'local Go server' $server $ship $serverArgs }
        else {
            if (-not (Test-Path -LiteralPath $server)) { throw 'Run photon build first' }
            $serverProc=Start-Process -FilePath $server -WorkingDirectory $ship -ArgumentList (Join-ProcessArgs $serverArgs) -WindowStyle Hidden -PassThru
        }
        $DaqHost='127.0.0.1'; $DaqPort=8080
    }
    try {
        if ($Source -eq 'pcan') {
            $adapter=Join-Path $ship 'photon-pcan-adapter.exe'
            if ($DryRun) { Show-Command 'PCAN adapter' $adapter $ship @('--http-port','8080') }
            else { $adapterProc=Start-Process -FilePath $adapter -WorkingDirectory $ship -ArgumentList @('--http-port','8080') -WindowStyle Hidden -PassThru }
        }
        $url='ws://' + $DaqHost + ':' + $DaqPort + '/ws'
        $native=Find-NativeUi
        $nativeArgs=@('--ws',$url)
        if ($Source -eq 'pcan') { $nativeArgs+='--pcan' }
        if ($recordingId) { $nativeArgs+=@('--recording',$recordingId) }
        if ($Rest) { $nativeArgs+=$Rest }
        if ($Browser) {
            $pageUrl='https://150.136.85.207.sslip.io/photon.html'
            if ($DryRun) { Write-Output ('browser: '+$pageUrl) } else { Start-Process $pageUrl }
        } elseif ($native) {
            Start-Logged $native $ship $nativeArgs
        } elseif ($DryRun) {
            Show-Command 'ui' (Join-Path $ship 'photon-ui.exe') $ship $nativeArgs
        } else {
            Write-Output ('Open http://' + $DaqHost + ':' + $DaqPort + '/photon.html')
            if ($ownedLocal) { throw 'Build photon-ui before using the local PCAN/log launcher' }
        }
    } finally {
        foreach ($helper in @($adapterProc,$serverProc)) {
            if ($helper -and -not $helper.HasExited) { Stop-Process -Id $helper.Id -ErrorAction SilentlyContinue }
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
                Start-Ui -Source 'daq_tcp' -DaqHost '127.0.0.1' -DaqPort 8080
            }
            'oracle' {
                Start-Ui -Source 'daq_tcp' -Browser
            }
            'daq' {
                $target = Take-Target
                if (-not $target) {
                    Write-Output 'photon ui daq needs HOST or HOST:PORT'
                    exit 2
                }
                $hp = Split-HostPort $target 8080
                Start-Ui -Source 'daq_tcp' -DaqHost $hp.Host -DaqPort $hp.Port -VideoHost $hp.Host -VideoPort 6700 -VideoUdpHost $hp.Host -VideoUdpPort 6800
            }
            'pcan' {
                Start-Ui -Source 'pcan'
            }
            'log' {
                $path = Take-Target
                if (-not $path) {
                    Write-Output 'photon ui log needs a recording ID from the Go catalog'
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
