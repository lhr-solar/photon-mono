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
. (Join-Path $PSScriptRoot 'selection.ps1')

function Show-Use {
    Write-Output @'
Photon is two programs. Run the one you need, and name the use case.

  photon setup [server] [client] [station] [tools] [--ui desktop|web|both] [--pcan] [--build]
      Install what those pieces need to build on this machine.

  photon build [server] [client] [station] [tools] [--ui desktop|web|both] [--config Debug|Release]
      Build those pieces into build\. The server finds photon-backend,
      photon-index, the page in ui\, and assets beside itself.
        server   photon-server, photon-sshd, photon-index (no desktop UI libraries)
        client   photon-backend and the UI --ui names (desktop when omitted)
        station  server and client with the web page photon-server serves
        tools    photon-export, photon-observer
      Plain photon build is server and the desktop client.

  photon test [go] [native] [e2e] [--config Debug|Release] [--strict]
      go       go vet and the photon-server package tests
      native   the C++ tests (ctest) and the video decoder glue test
      e2e      tests\e2e against build\: replay, a whole station, the page
               in a headless browser, import and export. --strict fails
               on a missing piece instead of skipping it.
      No words runs all three.

  photon package DIR
      Copy build\ into one folder per bundle under DIR: server, station,
      client-desktop, tools, and symbols (the .pdb files).

  photon clean [--deps]
      Remove build\ and the photon-next build trees (.artifacts*).
      --deps also removes photon-next\.external (fetched libraries, emsdk).
      data\ is never touched.

  photon server [ingest PORT]
      Ingest only. The DAQ dials the CAN ingress on port 6100, or PORT.
      The video store and the fan-out stay up. The UI stays down.

  photon station [ingest PORT]
      Server and the web UI on this machine, reading this server's fan-out.

  photon client [FROM] [--ui desktop|web]
      The UI, reading FROM:
        (nothing)     the server on this machine, 127.0.0.1:6500
        HOST[:PORT]   a server's fan-out; the port defaults to 6500
        oracle        the public Oracle pipe, 150.136.85.207:6110
        pcan          a USB PCAN on this machine
        PATH          replay a VSD1 log or an on_track folder
      Video comes from the same host, TCP :6700 and UDP :6800.
      --ui picks the desktop window or the page; desktop when both are built.

Flags after that are passed through to photon-server or photon-backend (--password, -ingress, ...).
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

function Invoke-Step {
    param([string]$Title, [string]$File, [string]$Work, [string[]]$CmdArgs)
    if ($DryRun) {
        Show-Command $Title $File $Work $CmdArgs
        return
    }
    if ($Work) {
        Push-Location $Work
    }
    try {
        & $File @CmdArgs
        if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        if ($Work) {
            Pop-Location
        }
    }
}

# Copy what is inside From into To, so a second copy does not nest From inside To.
function Copy-Tree {
    param([string]$From, [string]$To)
    if ($DryRun) {
        Show-Command 'copy' $From $null @('->', $To)
        return
    }
    New-Item -ItemType Directory -Force -Path $To | Out-Null
    Copy-Item -Path (Join-Path $From '*') -Destination $To -Recurse -Force
}

function Test-Toolchain {
    param($Sel, [string]$Words)
    $need = @()
    if ($Sel.Server -or $Sel.Client -or $Sel.Tools) {
        $need += @('cmake', 'ninja', 'clang')
    }
    if ($Sel.Server -or $Sel.Tools) {
        $need += 'go'
    }
    if ($Sel.Desktop -or $Sel.Web) {
        $need += 'git'
    }
    if ($Sel.Web) {
        $need += 'python'
    }
    $missing = @($need | Where-Object { -not (Get-Command $_ -ErrorAction SilentlyContinue) })
    if ($missing.Count -gt 0 -and -not $DryRun) {
        Write-Output ('missing ' + ($missing -join ', ') + '. Run: photon setup ' + $Words)
        exit 1
    }
}

function Start-Build {
    try {
        $sel = Read-Selection $script:Rest
    } catch {
        Write-Output $_.Exception.Message
        exit 2
    }
    if ($sel.Rest.Count -gt 0) {
        Write-Output ('unknown build word: ' + ($sel.Rest -join ' '))
        Show-Use
        exit 2
    }
    $ship = Join-Path $Root 'build'
    Write-Output ('build ' + (Format-Selection $sel) + ', ' + $sel.Config + ', into ' + $ship)
    Test-Toolchain $sel ($script:Rest -join ' ')
    if (-not $DryRun) {
        New-Item -ItemType Directory -Force -Path $ship | Out-Null
    }

    $targets = @()
    $expect = @()
    if ($sel.Server) {
        $targets += 'photon-index'
        $expect += @('photon-server.exe', 'photon-sshd.exe', 'photon-index.exe', 'assets\dbc\carcan_telemcan.dbc')
    }
    if ($sel.Client) {
        $targets += 'photon-backend'
        $expect += @('photon-backend.exe', 'assets\dbc\carcan_telemcan.dbc')
    }
    if ($sel.Desktop) {
        $targets += 'photon-ui'
        $expect += 'photon-ui.exe'
    }
    if ($sel.Web) {
        $expect += @('ui\photon.html', 'ui\photon.js', 'ui\photon.wasm', 'ui\photon.data')
    }
    if ($sel.Tools) {
        $targets += 'photon-export'
        $expect += @('photon-export.exe', 'photon-observer.exe')
    }

    # Debug stays in .artifacts, the tree the photon-next README and editors use.
    $cache = Join-Path $UiDir '.artifacts'
    if ($sel.Config -ne 'Debug') {
        $cache = Join-Path $UiDir ('.artifacts-' + $sel.Config.ToLowerInvariant())
    }
    $cmakeArgs = @(
        '-S', $UiDir,
        '-B', $cache,
        '-G', 'Ninja',
        '-D', 'CMAKE_C_COMPILER=clang',
        '-D', 'CMAKE_CXX_COMPILER=clang++',
        '-D', ('CMAKE_BUILD_TYPE=' + $sel.Config),
        '-D', ('PHOTON_SHIP_DIR=' + ($ship -replace '\\', '/'))
    )
    if ($sel.Desktop) {
        $cmakeArgs += @('-D', 'PHOTON_NATIVE_UI=ON')
    } elseif (-not (Test-Path -LiteralPath (Join-Path $cache 'CMakeCache.txt'))) {
        # A new tree without the desktop UI never fetches SDL, ImGui, or WebView2.
        $cmakeArgs += @('-D', 'PHOTON_NATIVE_UI=OFF')
    }
    if ($targets.Count -gt 0) {
        Invoke-Step 'cmake' 'cmake' $null $cmakeArgs
        Invoke-Step 'cmake' 'cmake' $null (@('--build', $cache, '--target') + $targets)
    }
    if ($sel.Tools) {
        $export = Join-Path $cache 'tools\photon-export\photon-export.exe'
        if ($DryRun) {
            Show-Command 'copy' $export $null @('->', $ship)
        } else {
            Copy-Item -LiteralPath $export -Destination $ship -Force
        }
    }

    $goBuilds = @()
    if ($sel.Server) {
        $goBuilds += , @('photon-server.exe', './server')
        $goBuilds += , @('photon-sshd.exe', './cmd/photon-sshd')
    }
    if ($sel.Tools) {
        $goBuilds += , @('photon-observer.exe', './observer')
    }
    foreach ($go in $goBuilds) {
        Invoke-Step 'go' 'go' $ServerDir @('build', '-o', (Join-Path $ship $go[0]), $go[1])
    }

    if ($sel.Web) {
        Invoke-Step 'web' (Join-Path $UiDir 'web\scripts\build.ps1') $null @()
        $web = Join-Path $UiDir '.artifacts-web'
        $uiOut = Join-Path $ship 'ui'
        foreach ($name in @('photon.html', 'photon.js', 'photon.wasm', 'photon.data')) {
            if ($DryRun) {
                Show-Command 'copy' (Join-Path $web $name) $null @('->', $uiOut)
            } elseif (Test-Path -LiteralPath (Join-Path $web $name)) {
                New-Item -ItemType Directory -Force -Path $uiOut | Out-Null
                Copy-Item -LiteralPath (Join-Path $web $name) -Destination $uiOut -Force
            }
        }
        # The page fetches the car and track GLBs over HTTP, not from photon.data.
        Copy-Tree (Join-Path $web 'models') (Join-Path $uiOut 'models')
    }
    if ($sel.Server -or $sel.Client) {
        # photon-index takes its DBC from here; a server-only build links nothing that copies it.
        Copy-Tree (Join-Path $UiDir 'assets') (Join-Path $ship 'assets')
    }

    if ($DryRun) {
        return
    }
    $missing = @($expect | Select-Object -Unique | Where-Object { -not (Test-Path -LiteralPath (Join-Path $ship $_)) })
    if ($missing.Count -gt 0) {
        Write-Output ('build finished without ' + ($missing -join ', '))
        exit 1
    }
    Write-Output ('built ' + (Format-Selection $sel) + ' into ' + $ship)
}

function Find-Python {
    foreach ($name in @('python', 'py')) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd -and $cmd.Source -notlike '*\WindowsApps\python*') {
            return $name
        }
    }
    return $null
}

# photon test [go] [native] [e2e] [--config Debug|Release] [--strict]. No words runs all three.
function Start-Test {
    $run = @{ go = $false; native = $false; e2e = $false }
    $config = 'Debug'
    $strict = $false
    $i = 0
    while ($i -lt $script:Rest.Count) {
        $word = $script:Rest[$i].ToLowerInvariant()
        if ($word -in @('--config', '-config') -and $i + 1 -lt $script:Rest.Count) {
            $config = (Read-Selection @('--config', $script:Rest[$i + 1])).Config
            $i += 2
            continue
        }
        if ($word -eq '--strict') {
            $strict = $true
        } elseif ($run.ContainsKey($word)) {
            $run[$word] = $true
        } else {
            Write-Output ('unknown test word: ' + $script:Rest[$i])
            exit 2
        }
        $i += 1
    }
    if (-not ($run.go -or $run.native -or $run.e2e)) {
        $run.go = $true
        $run.native = $true
        $run.e2e = $true
    }

    if ($run.go) {
        Invoke-Step 'go' 'go' $ServerDir @('vet', './...')
        # cache is the unfinished one-second tally; its tests are its spec until it is written.
        $packages = @('./server', './observer', './sshd', './cmd/...')
        Invoke-Step 'go' 'go' $ServerDir (@('test', '-count=1') + $packages)
        Write-Output 'go: server/cache not run (unfinished; go test ./cache in photon-server)'
    }

    if ($run.native) {
        $cache = Join-Path $UiDir '.artifacts'
        if ($config -ne 'Debug') {
            $cache = Join-Path $UiDir ('.artifacts-' + $config.ToLowerInvariant())
        }
        if (-not (Test-Path -LiteralPath (Join-Path $cache 'CMakeCache.txt')) -and -not $DryRun) {
            Write-Output ($cache + ' is not configured. Run: photon build client --config ' + $config)
            exit 1
        }
        # photon build makes only the shipped targets; the test executables come from here.
        Invoke-Step 'cmake' 'cmake' $null @('--build', $cache)
        Invoke-Step 'ctest' 'ctest' $null @('--test-dir', $cache, '--output-on-failure')
        if (Get-Command node -ErrorAction SilentlyContinue) {
            Invoke-Step 'node' 'node' $null @((Join-Path $UiDir 'tests\test_video_webcodecs.cjs'))
        } else {
            Write-Output 'node: not on PATH, video decoder glue test not run'
        }
    }

    if ($run.e2e) {
        $py = Find-Python
        if (-not $py -and -not $DryRun) {
            Write-Output 'Python is not on PATH. Run: photon setup client --ui web'
            exit 1
        }
        if ($strict) {
            $env:PHOTON_E2E_STRICT = '1'
        }
        $env:PHOTON_BUILD_DIR = Join-Path $Root 'build'
        $e2e = Join-Path $Root 'tests\e2e'
        Invoke-Step 'e2e' $py $e2e @('-m', 'unittest', 'discover', '-s', $e2e, '-t', $e2e, '-v')
    }
}

function Start-Package {
    if ($script:Rest.Count -ne 1) {
        Write-Output 'photon package needs one destination folder'
        exit 2
    }
    $dest = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($script:Rest[0])
    $ship = Join-Path $Root 'build'
    $commit = (& git -C $Root rev-parse --short HEAD 2>$null)
    if (-not $commit) {
        $commit = 'unknown'
    }
    $bundles = [ordered]@{
        'server'         = @{
            Files = @('photon-server.exe', 'photon-sshd.exe', 'photon-index.exe', 'assets')
            Run   = @'
Run from this folder:
  set PHOTON_PASSWORD=<password>
  photon-server.exe -data-dir data -ui off

The DAQ dials TCP 6100. Clients read the CANP fan-out on 6500 and video on 6700.
photon-index.exe writes the TSDB to data\tsdb. photon-server.exe -h lists the flags.
'@
        }
        'station'        = @{
            Files = @('photon-server.exe', 'photon-sshd.exe', 'photon-index.exe', 'photon-backend.exe', 'assets', 'ui')
            Run   = @'
Run from this folder:
  set PHOTON_PASSWORD=<password>
  photon-server.exe -data-dir data

The server starts photon-index and the local UI, then opens
http://127.0.0.1:8080/photon.html, which asks for the password once.
'@
        }
        'client-desktop' = @{
            Files = @('photon-backend.exe', 'photon-ui.exe', 'assets')
            Run   = @'
Run from this folder, the backend first, then the window:
  photon-backend.exe --no-live-tail --assets-dir assets --daq-host <server> --password <password>
  photon-ui.exe

Other sources: --source pcan, or --source log_replay --log-path <log or folder>.
'@
        }
        'tools'          = @{
            Files = @('photon-export.exe', 'photon-observer.exe')
            Run   = @'
photon-export.exe --store <data\tsdb> --list-sessions
photon-export.exe --store <data\tsdb> --session <id> --out run.parquet --all-signals
photon-observer.exe watches a running server's status; -h lists its flags.
'@
        }
    }
    if (-not $DryRun) {
        New-Item -ItemType Directory -Force -Path $dest | Out-Null
    }
    foreach ($name in $bundles.Keys) {
        $bundle = $bundles[$name]
        $missing = @($bundle.Files | Where-Object { -not (Test-Path -LiteralPath (Join-Path $ship $_)) })
        if ($missing.Count -gt 0) {
            Write-Output ('skip ' + $name + ' (build\ has no ' + ($missing -join ', ') + ')')
            continue
        }
        $out = Join-Path $dest $name
        if ($DryRun) {
            Show-Command 'package' $name $null @('->', $out)
            continue
        }
        # A fresh folder, so a file dropped from a bundle does not linger in the next one.
        if (Test-Path -LiteralPath $out) {
            Remove-Item -LiteralPath $out -Recurse -Force
        }
        New-Item -ItemType Directory -Force -Path $out | Out-Null
        foreach ($file in $bundle.Files) {
            Copy-Item -LiteralPath (Join-Path $ship $file) -Destination $out -Recurse -Force
        }
        $readme = "Photon $name, commit $commit`r`n`r`n" + ($bundle.Run -replace "`r?`n", "`r`n") + "`r`n"
        [IO.File]::WriteAllText((Join-Path $out 'README.txt'), $readme)
        Write-Output ('packaged ' + $name + ' -> ' + $out)
    }
    $pdbs = @(Get-ChildItem -LiteralPath $ship -Filter '*.pdb' -File -ErrorAction SilentlyContinue)
    if ($pdbs.Count -gt 0 -and -not $DryRun) {
        $out = Join-Path $dest 'symbols'
        if (Test-Path -LiteralPath $out) {
            Remove-Item -LiteralPath $out -Recurse -Force
        }
        New-Item -ItemType Directory -Force -Path $out | Out-Null
        $pdbs | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $out }
        Write-Output ('packaged symbols -> ' + $out)
    }
}

function Start-Clean {
    $deps = $false
    foreach ($word in $script:Rest) {
        switch ($word.ToLowerInvariant()) {
            '--deps' { $deps = $true }
            default {
                Write-Output ('unknown clean word: ' + $word)
                exit 2
            }
        }
    }
    # Build output only. data\ holds telemetry and is never touched.
    $dirs = @(Join-Path $Root 'build')
    $dirs += @(Get-ChildItem -LiteralPath $UiDir -Directory -Filter '.artifacts*' | ForEach-Object { $_.FullName })
    if ($deps) {
        # Fetched libraries and emsdk; the next build fetches them again, setup reinstalls emsdk.
        $dirs += Join-Path $UiDir '.external'
    }
    $dirs = @($dirs | Where-Object { Test-Path -LiteralPath $_ })
    if ($dirs.Count -eq 0) {
        Write-Output 'nothing to clean'
        return
    }
    # Windows will not delete a running exe, so stop rather than leave half a tree.
    $running = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
            $path = $_.Path
            $path -and @($dirs | Where-Object { $path.StartsWith($_ + '\', [StringComparison]::OrdinalIgnoreCase) }).Count -gt 0
        })
    if ($running.Count -gt 0) {
        $names = $running | ForEach-Object { $_.ProcessName + ' (pid ' + $_.Id + ')' }
        Write-Output ('stop these first: ' + ($names -join ', '))
        exit 1
    }
    foreach ($dir in $dirs) {
        $bytes = (Get-ChildItem -LiteralPath $dir -Recurse -Force -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
        $size = '{0:N0} MB' -f ($bytes / 1MB)
        if ($DryRun) {
            Write-Output ('would remove ' + $dir + '  ' + $size)
            continue
        }
        Remove-Item -LiteralPath $dir -Recurse -Force
        Write-Output ('removed ' + $dir + '  ' + $size)
    }
}

function Start-Setup {
    $setupArgs = @($script:Rest)
    & (Join-Path $PSScriptRoot 'setup.ps1') -DryRun:$DryRun @setupArgs
    exit $LASTEXITCODE
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
        # photon-server serves the web page itself; photon-ui.exe is not part of a station.
        if (-not (Find-Backend) -and -not $DryRun) {
            Write-Output 'build\photon-backend.exe is missing. Run: photon build station'
            exit 1
        }
        if (-not (Find-Page) -and -not $DryRun) {
            Write-Output 'build\ui\photon.html is missing. Run: photon build station'
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
        Write-Output 'build\photon-server.exe is missing. Run: photon build server'
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
        [string]$LogPath,
        [string]$Ui
    )
    $backend = Find-Backend
    if (-not $backend) {
        if (-not $DryRun) {
            Write-Output 'build\photon-backend.exe is missing. Run: photon build client'
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
    if ($Ui -eq 'desktop') {
        if (-not $native -and -not $DryRun) {
            Write-Output 'build\photon-ui.exe is missing. Run: photon build client --ui desktop'
            exit 1
        }
        $page = $null
    } elseif ($Ui -eq 'web') {
        if (-not $page -and -not $DryRun) {
            Write-Output 'build\ui\photon.html is missing. Run: photon build client --ui web'
            exit 1
        }
        $native = $null
    }
    $pageProc = $null
    $uiProc = $null
    Write-Output ("password " + $password)
    if ($Source -eq 'daq_tcp') {
        Write-Output ("daq " + $DaqHost + ":" + $DaqPort + "  video " + $VideoHost + ":" + $VideoPort + "  udp " + $VideoUdpHost + ":" + $VideoUdpPort)
    }
    if ($DryRun) {
        Show-Command 'backend' $backend $null $backendArgs
        if ($Ui -eq 'desktop' -or ($native -and $Ui -ne 'web')) {
            Show-Command 'ui' (Join-Path $Root 'build\photon-ui.exe') $null @()
        } elseif ($page -or $Ui -eq 'web') {
            $page = Join-Path $Root 'build\ui\photon.html'
            $pageDir = Split-Path $page -Parent
            Show-Command 'page' 'python -m http.server 8080 --bind 127.0.0.1 --directory' $null @($pageDir)
        } else {
            Write-Output 'ui: not built (run photon build client, or photon build client --ui web for the page)'
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

# photon client [FROM] [--ui desktop|web] [backend flags]. FROM is read the way a person
# would say it: nothing, oracle, pcan, a log on disk, or a host.
function Start-Client {
    $ui = $null
    $words = @()
    $i = 0
    while ($i -lt $script:Rest.Count) {
        $word = $script:Rest[$i]
        if ($word.ToLowerInvariant() -in @('--ui', '-ui')) {
            if ($i + 1 -ge $script:Rest.Count -or $script:Rest[$i + 1].ToLowerInvariant() -notin @('desktop', 'web')) {
                Write-Output '--ui is desktop or web'
                exit 2
            }
            $ui = $script:Rest[$i + 1].ToLowerInvariant()
            $i += 2
            continue
        }
        $words += $word
        $i += 1
    }
    $script:Rest = $words
    $from = Take-Target
    $key = ''
    if ($from) {
        $key = $from.ToLowerInvariant()
    }
    if ($key -eq '') {
        Write-Output 'from this machine'
        Start-Ui -Source 'daq_tcp' -DaqHost '127.0.0.1' -DaqPort 6500 -VideoHost '127.0.0.1' -VideoPort 6700 -VideoUdpHost '127.0.0.1' -VideoUdpPort 6800 -Ui $ui
    } elseif ($key -eq 'oracle') {
        Write-Output 'from the Oracle pipe'
        Start-Ui -Source 'daq_tcp' -DaqHost '150.136.85.207' -DaqPort 6110 -VideoHost '150.136.85.207' -VideoPort 6700 -VideoUdpHost '10.66.0.1' -VideoUdpPort 6800 -Ui $ui
    } elseif ($key -eq 'pcan') {
        Write-Output 'from a USB PCAN'
        Start-Ui -Source 'pcan' -Ui $ui
    } elseif (Test-Path -LiteralPath $from) {
        Write-Output ('replaying ' + $from)
        Start-Ui -Source 'log_replay' -LogPath $from -Ui $ui
    } elseif ($from -match '[\\/]' -or $key -match '\.(log|zip)$') {
        Write-Output ('no log or folder at ' + $from)
        exit 2
    } else {
        $hp = Split-HostPort $from 6500
        Write-Output ('from ' + $hp.Host + ':' + $hp.Port)
        Start-Ui -Source 'daq_tcp' -DaqHost $hp.Host -DaqPort $hp.Port -VideoHost $hp.Host -VideoPort 6700 -VideoUdpHost $hp.Host -VideoUdpPort 6800 -Ui $ui
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
    'setup' {
        Start-Setup
    }
    'build' {
        Start-Build
    }
    'clean' {
        Start-Clean
    }
    'test' {
        Start-Test
    }
    'package' {
        Start-Package
    }
    'server' {
        Start-Server
    }
    'station' {
        Start-Server -WithUi
    }
    'client' {
        Start-Client
    }
    default {
        Write-Output ("unknown command: " + $command)
        Show-Use
        exit 2
    }
}
