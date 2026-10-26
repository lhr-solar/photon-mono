# What a build or a setup is for. photon.ps1 and setup.ps1 both read these words.
#
#   server   photon-server, photon-sshd, photon-index, assets
#   client   photon-backend, assets, and the UI that --ui names (desktop when omitted)
#   station  server and client with the web page, which photon-server serves itself
#   tools    photon-export, photon-observer
#
#   --ui desktop|web|both     which client UI; naming a UI implies client
#   --config Debug|Release    native build type (Debug when omitted); picks the
#                             headless-<config> or desktop-<config> CMake preset
#
# No component words means server and the desktop client.

function Read-Selection {
    param([string[]]$Words)
    $sel = @{
        Server  = $false
        Client  = $false
        Desktop = $false
        Web     = $false
        Tools   = $false
        Config  = 'Debug'
        Rest    = @()
    }
    $ui = $null
    $i = 0
    while ($i -lt $Words.Count) {
        $word = $Words[$i]
        $key = $word.ToLowerInvariant()
        if ($key -in @('--ui', '-ui', '--config', '-config')) {
            if ($i + 1 -ge $Words.Count) {
                throw ($word + ' needs a value')
            }
            $value = $Words[$i + 1].ToLowerInvariant()
            if ($key.TrimStart('-') -eq 'ui') {
                if ($value -notin @('desktop', 'web', 'both')) {
                    throw ('--ui is desktop, web, or both, not ' + $Words[$i + 1])
                }
                $ui = $value
            } else {
                # The presets in photon-next\CMakePresets.json.
                $configs = @{ debug = 'Debug'; release = 'Release' }
                if (-not $configs.ContainsKey($value)) {
                    throw ('--config is Debug or Release, not ' + $Words[$i + 1])
                }
                $sel.Config = $configs[$value]
            }
            $i += 2
            continue
        }
        switch ($key) {
            'server' { $sel.Server = $true }
            'client' { $sel.Client = $true }
            'station' { $sel.Server = $true; $sel.Client = $true; $sel.Web = $true }
            'tools' { $sel.Tools = $true }
            default { $sel.Rest += $word }
        }
        $i += 1
    }
    if ($ui) {
        $sel.Client = $true
        $sel.Desktop = $sel.Desktop -or $ui -in @('desktop', 'both')
        $sel.Web = $sel.Web -or $ui -in @('web', 'both')
    }
    if (-not ($sel.Server -or $sel.Client -or $sel.Tools)) {
        $sel.Server = $true
        $sel.Client = $true
    }
    if ($sel.Client -and -not ($sel.Desktop -or $sel.Web)) {
        $sel.Desktop = $true
    }
    return $sel
}

function Format-Selection {
    param($Sel)
    $parts = @()
    if ($Sel.Server) { $parts += 'server' }
    if ($Sel.Client) {
        $uis = @()
        if ($Sel.Desktop) { $uis += 'desktop' }
        if ($Sel.Web) { $uis += 'web' }
        $parts += ('client (' + ($uis -join ' + ') + ')')
    }
    if ($Sel.Tools) { $parts += 'tools' }
    return ($parts -join ', ')
}
