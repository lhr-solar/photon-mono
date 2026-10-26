$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$Emsdk = Join-Path $Root ".external\emsdk"
# The web-release preset's binaryDir (CMakePresets.json).
$Build = Join-Path $Root "..\out\build\web-release"

if (-not (Test-Path (Join-Path $Emsdk "emsdk_env.ps1"))) {
    & (Join-Path $PSScriptRoot "setup-emsdk.ps1")
}

. (Join-Path $Emsdk "emsdk_env.ps1")

if (-not $env:EMSDK) {
    throw "EMSDK is not set after emsdk_env.ps1"
}

# The preset finds the Emscripten toolchain through EMSDK, which emsdk_env.ps1 just set.
Push-Location $Root
try {
    cmake --preset web-release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    cmake --build --preset web-release
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}
$Build = (Resolve-Path $Build).Path

$html = Join-Path $Build "photon.html"
$js = Join-Path $Build "photon.js"
$wasm = Join-Path $Build "photon.wasm"
# One cache token for html+js+wasm+data so a partial deploy cannot mix glue
# (LinkError: import "a" "a" is not a callable).
$ver = "20260906a"
if (Test-Path $wasm) {
    # .NET rather than Get-FileHash: Windows PowerShell started from pwsh 7 cannot load that cmdlet.
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = $sha.ComputeHash([IO.File]::ReadAllBytes($wasm))
    } finally {
        $sha.Dispose()
    }
    $ver = "20260905ab-" + ([BitConverter]::ToString($digest) -replace '-', '').Substring(0, 10).ToLower()
}
if (Test-Path $html) {
    $text = [IO.File]::ReadAllText($html)
    $text = $text.Replace("__PHOTON_ASSET_V__", $ver)
    $text = $text.Replace("src=photon.js", "src=photon.js?v=$ver")
    $text = $text.Replace('src="photon.js"', "src=`"photon.js?v=$ver`"")
    foreach ($old in @("20260906a", "20260905ab", "20260905aa", "20260905z", "20260905y", "20260905x", "20260905w", "20260905v", "20260905u", "20260905t", "20260905s", "20260905r", "20260905q", "20260905p", "20260905n", "20260905m", "20260905l", "20260905k", "20260905j", "20260905i", "20260905h", "20260905g", "20260905f", "20260905e", "20260905d", "20260905c", "20260905a", "20260904c")) {
        # Skip prefixes of the current token (`20260905a` inside `20260905ab-...`).
        if ($ver -eq $old -or $ver.StartsWith("$old-") -or $ver.StartsWith($old)) {
            continue
        }
        $text = $text.Replace("?v=$old", "?v=$ver")
    }
    [IO.File]::WriteAllText($html, $text)
}
if (Test-Path $js) {
    $text = [IO.File]::ReadAllText($js)
    $old = "var args=readEmAsmArgs(sigPtr,argbuf);return ASM_CONSTS[emAsmAddr](...args)}"
    $new = "var args=readEmAsmArgs(sigPtr,argbuf);var fn=ASM_CONSTS[emAsmAddr];if(typeof fn!==""function""){return 0}return fn(...args)}"
    if ($text.Contains($old)) {
        [IO.File]::WriteAllText($js, $text.Replace($old, $new))
    }
}

$py = @'
import pathlib, sys
def leb(b,i):
    n=0;s=0
    while True:
        x=b[i]; i+=1; n |= (x&0x7f)<<s
        if x<0x80: return n,i
        s+=7
def mods(data):
    if data[:4]!=b"\x00asm": return ["NOT_WASM"]
    i=8
    while i<len(data):
        sid=data[i]; i+=1
        slen,i=leb(data,i); end=i+slen
        if sid==2:
            cnt,i=leb(data,i); out=[]
            for _ in range(cnt):
                ln,i=leb(data,i); m=data[i:i+ln].decode(); i+=ln
                ln,i=leb(data,i); i+=ln
                kind=data[i]; i+=1
                if kind==0:
                    _,i=leb(data,i)
                elif kind in (1,2):
                    i+=1; lim=data[i]; i+=1; _,i=leb(data,i)
                    if lim==1: _,i=leb(data,i)
                elif kind==3: i+=2
                out.append(m)
            return sorted(set(out))
        i=end
    return []
js=pathlib.Path(sys.argv[1]).read_text(encoding="utf-8",errors="replace")
wasm=pathlib.Path(sys.argv[2]).read_bytes()
m=mods(wasm)
a="imports={a:wasmImports}" in js
env="env" in m
print("wasm mods", m, "js a_wrap", a)
if a and env:
    raise SystemExit("glue/wasm mismatch: JS minified imports as a, WASM expects env")
if (not a) and m==["a"]:
    raise SystemExit("glue/wasm mismatch: JS expects env, WASM minified as a")
'@
$pyFile = Join-Path $Build "_check_glue.py"
[IO.File]::WriteAllText($pyFile, $py)
python $pyFile $js $wasm
if ($LASTEXITCODE -ne 0) { throw "photon.js / photon.wasm import modules do not match" }
# The repo's photon build installs this page and models\ (the web component) into its bundles' ui\.

Write-Host "Built $Build\photon.html ?v=$ver"
