$ErrorActionPreference = "Continue"
Write-Output "=== newest crash ==="
$p = "F:\lhr\photon-server\photon-ui-crash.log"
$lines = Get-Content $p
Write-Output ("lines=" + $lines.Count)
$lines | Select-Object -Last 5 | ForEach-Object {
  if ($_ -match '"t":"([^"]+)".*"kind":"([^"]+)".*"message":"([^"]{0,180})') {
    Write-Output ("t=$($Matches[1]) kind=$($Matches[2]) msg=$($Matches[3])")
  }
}
Write-Output "=== live glue ==="
$py = "F:\lhr\photon-next\_check_live_glue.py"
@'
import pathlib
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
js=pathlib.Path(r"F:\lhr\photon-server\ui\photon.js").read_text(encoding="utf-8",errors="replace")
wasm=pathlib.Path(r"F:\lhr\photon-server\ui\photon.wasm").read_bytes()
m=mods(wasm)
a="imports={a:wasmImports}" in js
print("wasm mods", m, "js a_wrap", a, "js", len(js), "wasm", len(wasm))
'@ | Set-Content $py -Encoding ascii
& "C:\Users\alper\AppData\Local\Programs\Python\Python314\python.exe" $py
Write-Output "=== client.hpp ==="
Select-String -Path "F:\lhr\photon-next\frontend\include\photon_ui\client.hpp" -Pattern "beginInterest|commitInterest|class Client" | ForEach-Object { $_.Line.Trim() }
Write-Output "=== wasm build leftover ==="
if (Test-Path "F:\lhr\photon-next\.artifacts-web") {
  Get-ChildItem "F:\lhr\photon-next\.artifacts-web\photon.*" -ErrorAction SilentlyContinue | Format-Table Name,Length,LastWriteTime -AutoSize | Out-String
}
