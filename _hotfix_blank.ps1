$ErrorActionPreference = "Stop"
$path = "F:\lhr\photon-server\ui\photon.html"
$html = [IO.File]::ReadAllText($path)

$oldLost = 't.addEventListener("webglcontextlost",(function(t){t.preventDefault(),photonReportCrash("webgl","WebGL context lost")}),!1)'
$newLost = 't.addEventListener("webglcontextlost",(function(t){t.preventDefault();try{if(sessionStorage.getItem("photon-gl-lost")!=="1"){sessionStorage.setItem("photon-gl-lost","1");location.reload();return}}catch(e){}photonReportCrash("webgl","WebGL context lost")}),!1)'
if ($html.Contains($oldLost)) {
  $html = $html.Replace($oldLost, $newLost)
  Write-Output "patched contextlost"
} else {
  Write-Output "contextlost snippet missing"
}

$oldWarm = 'window.photonMaps3d&&window.setTimeout((function(){window.photonMaps3d.warm()}),2500)'
$newWarm = 'window.photonMaps3d&&(function(){try{return sessionStorage.getItem("photon-gl-lost")!=="1"}catch(t){return!0}}())&&window.setTimeout((function(){window.photonMaps3d.warm()}),2500)'
if ($html.Contains($oldWarm)) {
  $html = $html.Replace($oldWarm, $newWarm)
  Write-Output "patched warm"
} else {
  Write-Output "warm snippet missing"
}

$oldStart = 'show:M,start:G,pose:'
$newStart = 'show:M,start:function(t,e,n,o){try{if(sessionStorage.getItem("photon-gl-lost")==="1")return}catch(r){}return G(t,e,n,o)},pose:'
if ($html.Contains($oldStart)) {
  $html = $html.Replace($oldStart, $newStart)
  Write-Output "patched start"
} else {
  Write-Output "start snippet missing"
}

if (-not $html.Contains("glfix-20260912")) {
  $html = $html.Replace("</title>", "</title><!-- glfix-20260912 -->")
  Write-Output "tagged html"
}

[IO.File]::WriteAllText($path, $html)
Write-Output ("html bytes=" + $html.Length)
