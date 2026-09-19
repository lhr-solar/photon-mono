# Revert-check for tests/test_video_index.cpp.
#
# A test that cannot fail is worse than no test. Each case below breaks one
# specific behaviour in core/src/video_index.cpp and asserts that the test
# covering it actually fails. Any case reported as NOT LOAD-BEARING means the
# test is decorative and has to be rewritten.
#
# Usage: pwsh -File tests/revert_check_video_index.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$src = Join-Path $root 'core\src\video_index.cpp'
$exe = Join-Path $root '.artifacts\tests\photon-tests.exe'
$original = Get-Content -Raw -LiteralPath $src

# Each case: name, the exact text to replace, its replacement, and the
# substring that must appear in a FAIL line from test_video_index.cpp.
$cases = @(
    @{ name = 'gop row field order';
       find = '    pushU32(out, row.byteLength);
    pushU32(out, row.segmentId);';
       repl = '    pushU32(out, row.segmentId);
    pushU32(out, row.byteLength);';
       expect = 'reencoded == wire' }

    @{ name = 'locate request mode byte';
       find = '    out.push_back(kModeLocate);';
       repl = '    out.push_back(kModeReplay);';
       expect = 'locate[0] == photon::vidx::kModeLocate' }

    @{ name = 'range request leaves a stray startTimeMs';
       find = '    // startTimeMs is unused for a range read; the span is named explicitly.
    pushU64(out, 0);';
       repl = '    pushU64(out, 1);';
       expect = 'range[i] == 0' }

    @{ name = 'locate reply accepts trailing bytes';
       find = '    return off == payload.size();';
       repl = '    return off <= payload.size();';
       expect = 'decodeLocateReply(extra, drifted)' }

    @{ name = 'locate reply negative clock offset read as unsigned';
       find = '    out.clockOffsetMs = static_cast<std::int64_t>(clockOffset);';
       repl = '    out.clockOffsetMs = static_cast<std::int64_t>(static_cast<std::uint32_t>(clockOffset));';
       expect = 'reply.clockOffsetMs == -137' }

    @{ name = 'cursor-to-video conversion ignores the clock offset';
       find = '    if (offsetMs >= 0) {
        const auto off = static_cast<std::uint64_t>(offsetMs);
        return cursorMs > off ? cursorMs - off : 0;
    }
    return cursorMs + static_cast<std::uint64_t>(-offsetMs);';
       repl = '    (void)offsetMs;
    return cursorMs;';
       expect = 'videoMsForCursor' }

    @{ name = 'cursor-to-video conversion wraps instead of saturating';
       find = '        return cursorMs > off ? cursorMs - off : 0;';
       repl = '        return cursorMs - off;';
       expect = 'photon::vidx::videoMsForCursor(100, 5''000) == 0' }

    @{ name = 'entry row is the next keyframe, not the previous one';
       find = '        if (r.startMs > videoMs) {
            continue;
        }';
       repl = '        if (r.startMs < videoMs) {
            continue;
        }';
       expect = 'got == c.want' }

    @{ name = 'entry row accepts continuation rows';
       find = '        if (r.streamId != streamId || rowIsContinuation(r)) {
            continue;
        }';
       repl = '        if (r.streamId != streamId) {
            continue;
        }';
       expect = 'entryRowFor(split, 0, 1066) == 0' }

    @{ name = 'scrubbing issues locate requests';
       find = '        plan.keyframeOnly = true;
        plan.feedUpToMs = plan.haveTarget ? plan.target.startMs : 0;
        return plan;';
       repl = '        plan.keyframeOnly = true;
        plan.feedUpToMs = plan.haveTarget ? plan.target.startMs : 0;
        if (!plan.haveTarget) {
            plan.requestLocate = true;
            plan.locateAtMs = videoMs;
            ++locateRequests_;
        }
        return plan;';
       expect = 'plan.requestLocate' }

    @{ name = 'scrubbing decodes forward to the cursor';
       find = '        plan.feedUpToMs = plan.haveTarget ? plan.target.startMs : 0;';
       repl = '        plan.feedUpToMs = videoMs;';
       expect = 'plan.feedUpToMs == 102' }

    @{ name = 'commit only shows the keyframe';
       find = '    plan.feedUpToMs = videoMs;
    const bool inside';
       repl = '    plan.feedUpToMs = plan.haveTarget ? plan.target.startMs : videoMs;
    plan.keyframeOnly = true;
    const bool inside';
       expect = '!plan.keyframeOnly' }

    @{ name = 'entry change is not reported, so the decoder is never re-entered';
       find = '        plan.reenter = !haveEntry_ || plan.target.startMs != entryStartMs_;';
       repl = '        plan.reenter = false;';
       expect = 'plan.reenter' }

    @{ name = 'every cursor move re-enters the decoder';
       find = '        plan.reenter = !haveEntry_ || plan.target.startMs != entryStartMs_;';
       repl = '        plan.reenter = true;';
       expect = '!plan.reenter' }

    @{ name = 'a pending locate is not tracked, so requests repeat';
       find = '    if (!inside && !locatePending_) {';
       repl = '    if (!inside) {';
       expect = '!repeat.requestLocate' }

    @{ name = 'frame pacing ignores the cursor';
       find = '    return cursorMsForVideo(frameVideoMs, clockOffsetMs_) <= cursorMs;';
       repl = '    (void)frameVideoMs;
    (void)cursorMs;
    return true;';
       expect = 'pb.frameDue(101' }

    @{ name = 'live mode gates on the cursor';
       find = '        plan.feedUpToMs = std::numeric_limits<std::uint64_t>::max();';
       repl = '        plan.feedUpToMs = cursorMs;';
       expect = 'live.feedUpToMs == UINT64_MAX' }

    @{ name = 'coverage map is ignored, so gaps look covered';
       find = '        if (videoMs >= c.startMs && videoMs <= c.endMs) {
            return true;
        }
    }
    return false;
}

bool CursorPlayback::frameDue';
       repl = '        (void)c;
        return true;
    }
    return false;
}

bool CursorPlayback::frameDue';
       expect = '!pb.covered(170' }

    @{ name = 'grown rows are appended beside the stale copy';
       find = '            *it = row;
            continue;';
       repl = '            continue;';
       expect = 'pb.rows()[0].endMs == 101' }

    @{ name = 'gop ring is unbounded';
       find = '    while (bytes_ > budget_ && drop + 1 < nals_.size()) {';
       repl = '    while (false && drop + 1 < nals_.size()) {';
       expect = 'ring.bytes() <= ring.budget()' }

    @{ name = 'gop ring evicts its last entry, losing the keyframe';
       find = '    while (bytes_ > budget_ && drop + 1 < nals_.size()) {';
       repl = '    while (bytes_ > budget_ && drop < nals_.size()) {';
       expect = 'tiny.size() == 1' }

    @{ name = 'keyframe search returns a keyframe after the cursor';
       find = '        if (nals_[i].videoMs > videoMs) {
            break;
        }';
       repl = '        if (false) {
            break;
        }';
       expect = 'videoMs == 118' }

    @{ name = 'keyframe search ignores the keyframe flag';
       find = '        if ((nals_[i].flags & kNalFlagKeyframe) == 0) {
            continue;
        }';
       repl = '        if (false) {
            continue;
        }';
       expect = 'flags & 1' }
)

function Invoke-Suite {
    Push-Location $root
    # cmake and ninja write progress to stderr, which a 'Stop' preference turns
    # into a terminating error.
    $ErrorActionPreference = 'Continue'
    try {
        $build = & cmake --build .artifacts --target photon-tests 2>&1
        if ($LASTEXITCODE -ne 0) {
            return @{ built = $false; fails = @(); log = ($build -join "`n") }
        }
        $out = & $exe 2>&1
        $fails = @($out | Select-String -Pattern 'FAIL .*test_video_index\.cpp' | ForEach-Object { $_.Line })
        return @{ built = $true; fails = $fails; log = '' }
    } finally {
        Pop-Location
    }
}

Write-Host '--- baseline ---'
$baseline = Invoke-Suite
if (-not $baseline.built) {
    Write-Host 'BASELINE BUILD FAILED'
    Write-Host $baseline.log
    exit 1
}
if ($baseline.fails.Count -ne 0) {
    Write-Host "BASELINE ALREADY FAILING ($($baseline.fails.Count)):"
    $baseline.fails | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host 'baseline: 0 video-index failures'
Write-Host ''

$notLoadBearing = @()
$i = 0
foreach ($c in $cases) {
    $i++
    # A full pass rebuilds the suite once per case, which is slow. RCV_ONLY=3,7
    # re-runs just those cases.
    if ($env:RCV_ONLY -and ($env:RCV_ONLY -split ',') -notcontains [string]$i) { continue }
    if (-not $original.Contains($c.find)) {
        Write-Host ("[{0,2}] {1}: PATCH TARGET NOT FOUND" -f $i, $c.name)
        $notLoadBearing += $c.name
        continue
    }
    $broken = $original.Replace($c.find, $c.repl)
    Set-Content -LiteralPath $src -Value $broken -NoNewline
    $r = Invoke-Suite
    Set-Content -LiteralPath $src -Value $original -NoNewline

    if (-not $r.built) {
        Write-Host ("[{0,2}] {1}: BUILD FAILED (break was not compilable)" -f $i, $c.name)
        $notLoadBearing += $c.name
        continue
    }
    # -match, not -like: assertion text contains [0] and [i], which -like reads
    # as character classes and silently fails to match.
    $needle = [regex]::Escape($c.expect)
    $matched = @($r.fails | Where-Object { $_ -match $needle })
    if ($matched.Count -gt 0) {
        Write-Host ("[{0,2}] {1}: caught ({2} failing checks; e.g. {3})" -f `
            $i, $c.name, $r.fails.Count, $matched[0].Trim())
    } elseif ($r.fails.Count -gt 0) {
        Write-Host ("[{0,2}] {1}: caught by a DIFFERENT check ({2} failing): {3}" -f `
            $i, $c.name, $r.fails.Count, $r.fails[0].Trim())
    } else {
        Write-Host ("[{0,2}] {1}: NOT LOAD-BEARING - the suite still passes" -f $i, $c.name)
        $notLoadBearing += $c.name
    }
}

Write-Host ''
if ($notLoadBearing.Count -eq 0) {
    Write-Host "all $($cases.Count) revert-checks caught their break"
    exit 0
}
Write-Host "$($notLoadBearing.Count) of $($cases.Count) breaks went undetected:"
$notLoadBearing | ForEach-Object { Write-Host "  $_" }
exit 1
