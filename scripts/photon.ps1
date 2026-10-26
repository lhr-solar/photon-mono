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
# Build trees (CMake presets put theirs here) and the runnable bundles assembled from them.
$BuildRoot = Join-Path $Root 'out\build'
$GoOut = Join-Path $BuildRoot 'go'
$WebTree = Join-Path $BuildRoot 'web-release'
$Dist = Join-Path $Root 'out\dist'
. (Join-Path $PSScriptRoot 'selection.ps1')

function Show-Use {
    Write-Output @'
Photon is two programs. Run the one you need, and name the use case.

  photon setup [server] [client] [station] [tools] [--ui desktop|web|both] [--pcan] [--build]
      Install what those pieces need to build on this machine.

  photon build [server] [client] [station] [tools] [--ui desktop|web|both] [--config Debug|Release]
      Build those pieces, then assemble one runnable bundle each under out\dist:
        server   photon-server, photon-sshd, photon-index (no desktop UI libraries)
        client   photon-backend and the UI --ui names (desktop when omitted)
        station  server and client with the web page photon-server serves
        tools    photon-export, photon-observer
      symbols holds the .pdb files. Build trees are out\build\<preset>.
      Plain photon build is server and the desktop client.

  photon test [go] [native] [e2e] [--config Debug|Release] [--strict]
      go       go vet and the photon-server package tests
      native   ctest in each native tree for --config, and the video decoder glue test
      e2e      tests\e2e against out\dist: replay, a whole station, the page
               in a headless browser, import and export. --strict fails
               on a missing piece instead of skipping it.
      No words runs all three.

  photon clean [--deps]
      Remove out\ (build trees and bundles), and the old build\ and .artifacts* trees.
      --deps also removes photon-next\.external (fetched libraries, emsdk).
      data\ is never touched.

  photon server [ingest PORT]
      Ingest only, from out\dist\server. The DAQ dials the CAN ingress on port 6100, or PORT.
      The video store and the fan-out stay up. The UI stays down.

  photon station [ingest PORT]
      Server and the web UI on this machine, from out\dist\station.

  photon client [FROM] [--ui desktop|web]
      The UI from out\dist\client, reading FROM:
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

function Find-InBundle {
    param([string]$Bundle, [string]$Name)
    $path = Join-Path $Dist (Join-Path $Bundle $Name)
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

# Processes whose exe lives under one of these folders. Windows will not delete a running
# exe, so clean and the bundle swap stop on these instead of leaving half a folder.
function Get-RunningUnder {
    param([string[]]$Dirs)
    return @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
            $path = $_.Path
            $path -and @($Dirs | Where-Object { $path.StartsWith($_ + '\', [StringComparison]::OrdinalIgnoreCase) }).Count -gt 0
        })
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

# The native preset for a build: desktop only when photon-ui is wanted, so a server
# or web-only client never fetches SDL, ImGui, or WebView2.
function Get-NativePreset {
    param([bool]$Desktop, [string]$Config)
    $kind = 'headless'
    if ($Desktop) {
        $kind = 'desktop'
    }
    return $kind + '-' + $Config.ToLowerInvariant()
}

# What each bundle under out\dist holds: install components from the native tree,
# Go programs from out\build\go, and the page (the web component) from the web tree.
function Get-Bundles {
    param($Sel)
    $bundles = [ordered]@{}
    $dbc = 'assets\dbc\carcan_telemcan.dbc'
    if ($Sel.Server) {
        $bundles['server'] = @{
            Native = @('server'); Go = @('photon-server.exe', 'photon-sshd.exe'); Web = $false
            Expect = @('photon-server.exe', 'photon-sshd.exe', 'photon-index.exe', $dbc)
        }
    }
    if ($Sel.Client) {
        $native = @('client')
        $expect = @('photon-backend.exe', $dbc, 'assets\tracks')
        if ($Sel.Desktop) {
            $native += 'desktop'
            $expect += @('photon-ui.exe', 'assets\fonts', 'assets\maps3d.html')
        }
        if ($Sel.Web) {
            $expect += @('ui\photon.html', 'ui\photon.wasm')
        }
        $bundles['client'] = @{ Native = $native; Go = @(); Web = $Sel.Web; Expect = $expect }
    }
    if ($Sel.Server -and $Sel.Client -and $Sel.Web) {
        $bundles['station'] = @{
            Native = @('server', 'client'); Go = @('photon-server.exe', 'photon-sshd.exe'); Web = $true
            Expect = @('photon-server.exe', 'photon-index.exe', 'photon-backend.exe', $dbc, 'assets\tracks', 'ui\photon.html', 'ui\models')
        }
    }
    if ($Sel.Tools) {
        $bundles['tools'] = @{
            Native = @('tools'); Go = @('photon-observer.exe'); Web = $false
            Expect = @('photon-export.exe', 'photon-observer.exe')
        }
    }
    # Only the programs linked with /DEBUG leave a .pdb; an empty bundle is dropped.
    $bundles['symbols'] = @{ Native = @('symbols'); Go = @(); Web = $false; Expect = @(); Optional = $true }
    return $bundles
}

function Get-BundleReadme {
    param([string]$Name, [string]$Folder)
    $desktop = Test-Path -LiteralPath (Join-Path $Folder 'photon-ui.exe')
    $web = Test-Path -LiteralPath (Join-Path $Folder 'ui\photon.html')
    switch ($Name) {
        'server' {
            return @'
Run from this folder:
  set PHOTON_PASSWORD=<password>
  photon-server.exe -data-dir data -ui off

The DAQ dials TCP 6100. Clients read the CANP fan-out on 6500 and video on 6700.
photon-index.exe writes the TSDB to data\tsdb. photon-server.exe -h lists the flags.
'@
        }
        'station' {
            return @'
Run from this folder:
  set PHOTON_PASSWORD=<password>
  photon-server.exe -data-dir data

The server starts photon-index and the local UI, then opens
http://127.0.0.1:8080/photon.html, which asks for the password once.
'@
        }
        'client' {
            $text = "Run from this folder, the backend first:`n" +
                "  photon-backend.exe --no-live-tail --assets-dir assets --daq-host <server> --password <password>`n" +
                "Other sources: --source pcan, or --source log_replay --log-path <log or folder>.`n`nThen a UI:`n"
            if ($desktop) {
                $text += "  photon-ui.exe`n"
            }
            if ($web) {
                $text += "  python -m http.server 8080 --bind 127.0.0.1 --directory ui`n" +
                    "  and open http://127.0.0.1:8080/photon.html`n"
            }
            return $text
        }
        'tools' {
            return @'
photon-export.exe --store <data\tsdb> --list-sessions
photon-export.exe --store <data\tsdb> --session <id> --out run.parquet --all-signals
photon-observer.exe watches a running server's status; -h lists its flags.
'@
        }
        default {
            return "Debug symbols for the .exe files built from the same commit.`n"
        }
    }
}

function Get-CommitLabel {
    $commit = (& git -C $Root rev-parse --short HEAD 2>$null)
    if (-not $commit) {
        return 'commit unknown'
    }
    $dirty = (& git -C $Root status --porcelain 2>$null)
    if ($dirty) {
        return 'commit ' + $commit + ' with local changes'
    }
    return 'commit ' + $commit
}

# Install into a fresh staging folder, check it, and only then replace the old bundle,
# so a failed build never leaves a bundle that looks complete.
function Install-Bundle {
    param([string]$Name, $Bundle, [string]$Tree, [string]$Label)
    $staging = Join-Path $Dist ('.staging\' + $Name)
    $final = Join-Path $Dist $Name
    if ($DryRun) {
        foreach ($component in $Bundle.Native) {
            Show-Command 'install' 'cmake' $null @('--install', $Tree, '--component', $component, '--prefix', $staging)
        }
        if ($Bundle.Web) {
            Show-Command 'install' 'cmake' $null @('--install', $WebTree, '--component', 'web', '--prefix', $staging)
        }
        foreach ($go in $Bundle.Go) {
            Show-Command 'copy' (Join-Path $GoOut $go) $null @('->', $staging)
        }
        Show-Command 'swap' $staging $null @('->', $final)
        return
    }
    if (Test-Path -LiteralPath $staging) {
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $staging | Out-Null
    $installs = @($Bundle.Native | ForEach-Object { , @($Tree, $_) })
    if ($Bundle.Web) {
        $installs += , @($WebTree, 'web')
    }
    foreach ($install in $installs) {
        $log = & cmake --install $install[0] --component $install[1] --prefix $staging 2>&1
        if ($LASTEXITCODE -ne 0) {
            $log | Write-Output
            Write-Output ('installing ' + $install[1] + ' into the ' + $Name + ' bundle failed')
            exit 1
        }
    }
    foreach ($go in $Bundle.Go) {
        Copy-Item -LiteralPath (Join-Path $GoOut $go) -Destination $staging -Force
    }
    if ($Bundle.Optional -and -not (Get-ChildItem -LiteralPath $staging -Force)) {
        # Nothing for this build, so an older one would only mislead.
        Remove-Item -LiteralPath $staging -Force
        if (Test-Path -LiteralPath $final) {
            Remove-Item -LiteralPath $final -Recurse -Force
        }
        return
    }
    $missing = @($Bundle.Expect | Where-Object { -not (Test-Path -LiteralPath (Join-Path $staging $_)) })
    if ($missing.Count -gt 0) {
        Write-Output ('the ' + $Name + ' bundle is missing ' + ($missing -join ', ') + '; left in ' + $staging)
        exit 1
    }
    $readme = 'Photon ' + $Name + ', ' + $Label + "`n`n" + (Get-BundleReadme $Name $staging)
    [IO.File]::WriteAllText((Join-Path $staging 'README.txt'), ($readme -replace "`r?`n", "`r`n"))
    if (Test-Path -LiteralPath $final) {
        Remove-Item -LiteralPath $final -Recurse -Force
    }
    Move-Item -LiteralPath $staging -Destination $final
    Write-Output ('bundle ' + $final)
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
    $preset = Get-NativePreset $sel.Desktop $sel.Config
    $tree = Join-Path $BuildRoot $preset
    $bundles = Get-Bundles $sel
    Write-Output ('build ' + (Format-Selection $sel) + ', ' + $sel.Config + ', preset ' + $preset)
    Test-Toolchain $sel ($script:Rest -join ' ')

    # Fail before building, not after, if a bundle about to be replaced is running.
    $busy = Get-RunningUnder @($bundles.Keys | ForEach-Object { Join-Path $Dist $_ })
    if ($busy.Count -gt 0 -and -not $DryRun) {
        $names = $busy | ForEach-Object { $_.ProcessName + ' (pid ' + $_.Id + ')' }
        Write-Output ('stop these first, they run from out\dist: ' + ($names -join ', '))
        exit 1
    }

    $targets = @()
    if ($sel.Server) {
        $targets += 'photon-index'
    }
    if ($sel.Client) {
        $targets += 'photon-backend'
    }
    if ($sel.Desktop) {
        $targets += 'photon-ui'
    }
    if ($sel.Tools) {
        $targets += 'photon-export'
    }
    Invoke-Step 'cmake' 'cmake' $UiDir @('--preset', $preset)
    Invoke-Step 'cmake' 'cmake' $UiDir (@('--build', '--preset', $preset, '--target') + $targets)

    $goBuilds = @()
    if ($sel.Server) {
        $goBuilds += , @('photon-server.exe', './server')
        $goBuilds += , @('photon-sshd.exe', './cmd/photon-sshd')
    }
    if ($sel.Tools) {
        $goBuilds += , @('photon-observer.exe', './observer')
    }
    foreach ($go in $goBuilds) {
        Invoke-Step 'go' 'go' $ServerDir @('build', '-o', (Join-Path $GoOut $go[0]), $go[1])
    }

    if ($sel.Web) {
        Invoke-Step 'web' (Join-Path $UiDir 'web\scripts\build.ps1') $null @()
    }

    $label = (Get-CommitLabel) + ', ' + $sel.Config
    foreach ($name in $bundles.Keys) {
        Install-Bundle $name $bundles[$name] $tree $label
    }
    if (-not $DryRun) {
        $staging = Join-Path $Dist '.staging'
        if ((Test-Path -LiteralPath $staging) -and -not (Get-ChildItem -LiteralPath $staging -Force)) {
            Remove-Item -LiteralPath $staging -Force
        }
        Write-Output ('built ' + (Format-Selection $sel) + ' into ' + $Dist)
    }
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
            try {
                $config = (Read-Selection @('--config', $script:Rest[$i + 1])).Config
            } catch {
                Write-Output $_.Exception.Message
                exit 2
            }
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
        $presets = @(@($true, $false) | ForEach-Object { Get-NativePreset $_ $config } |
                Where-Object { Test-Path -LiteralPath (Join-Path $BuildRoot (Join-Path $_ 'CMakeCache.txt')) })
        if ($presets.Count -eq 0 -and -not $DryRun) {
            Write-Output ('no native build tree for ' + $config + '. Run: photon build --config ' + $config)
            exit 1
        }
        foreach ($preset in $presets) {
            # photon build makes only the shipped targets; the test executables come from here.
            Invoke-Step 'cmake' 'cmake' $UiDir @('--build', '--preset', $preset)
            Invoke-Step 'ctest' 'ctest' $UiDir @('--preset', $preset)
        }
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
        $env:PHOTON_DIST_DIR = $Dist
        $e2e = Join-Path $Root 'tests\e2e'
        Invoke-Step 'e2e' $py $e2e @('-m', 'unittest', 'discover', '-s', $e2e, '-t', $e2e, '-v')
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
    # build\ and photon-next\.artifacts* are where builds went before out\.
    $dirs = @((Join-Path $Root 'out'), (Join-Path $Root 'build'))
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
    $running = Get-RunningUnder $dirs
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
    # A station is the server with the local UI beside it; it has its own bundle.
    $bundle = 'server'
    if ($WithUi) {
        $bundle = 'station'
    }
    $folder = Join-Path $Dist $bundle
    $exe = Find-InBundle $bundle 'photon-server.exe'
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
    if (-not $WithUi) {
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
            Show-Command 'run' (Join-Path $folder 'photon-server.exe') $folder $serverArgs
            return
        }
        Write-Output ('out\dist\' + $bundle + ' is not built. Run: photon build ' + $bundle)
        exit 1
    }
    Start-Logged $exe $folder $serverArgs
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
    $folder = Join-Path $Dist 'client'
    $backend = Find-InBundle 'client' 'photon-backend.exe'
    if (-not $backend) {
        if (-not $DryRun) {
            Write-Output 'out\dist\client is not built. Run: photon build client'
            exit 1
        }
        $backend = Join-Path $folder 'photon-backend.exe'
    }
    $password = Get-Password
    $backendArgs = @(
        '--no-live-tail',
        '--source', $Source,
        '--ws-port', '8765',
        '--password', $password,
        '--assets-dir', (Join-Path $folder 'assets')
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

    $native = Find-InBundle 'client' 'photon-ui.exe'
    $page = Find-InBundle 'client' 'ui\photon.html'
    if ($Ui -eq 'desktop') {
        if (-not $native -and -not $DryRun) {
            Write-Output 'out\dist\client has no photon-ui.exe. Run: photon build client --ui desktop'
            exit 1
        }
        $page = $null
    } elseif ($Ui -eq 'web') {
        if (-not $page -and -not $DryRun) {
            Write-Output 'out\dist\client has no page. Run: photon build client --ui web'
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
            Show-Command 'ui' (Join-Path $folder 'photon-ui.exe') $null @()
        } elseif ($page -or $Ui -eq 'web') {
            Show-Command 'page' 'python -m http.server 8080 --bind 127.0.0.1 --directory' $null @((Join-Path $folder 'ui'))
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
