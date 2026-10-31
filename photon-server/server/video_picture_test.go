package main

import (
	"testing"
	"time"
)

func TestClientClockOffsetAlreadyBaked(t *testing.T) {
	if got := clientClockOffset(-5000, -5000); got != 0 {
		t.Fatalf("baked offset reported %d", got)
	}
	if got := clientClockOffset(-5000, -4800); got != 0 {
		t.Fatalf("near residual reported %d", got)
	}
	if got := clientClockOffset(-5000, 0); got != -5000 {
		t.Fatalf("unbaked residual reported %d", got)
	}
	if got := clientClockOffset(0, -5000); got != 0 {
		t.Fatalf("no manual offset reported %d", got)
	}
	if got := clientClockOffset(-5000, -20000); got != -5000 {
		t.Fatalf("unrelated residual reported %d", got)
	}
}

func TestVideoSendLimitFollowsPinnedReplay(t *testing.T) {
	const runStart = uint64(1_789_250_725_144)
	const lastPic = uint64(1_789_250_860_315)
	if got := videoSendLimit(1, runStart, 0, 0); got != runStart {
		t.Fatalf("pause without a locate sent up to %d", got)
	}
	if got := videoSendLimit(1, runStart, lastPic, 0); got != lastPic+1500 {
		t.Fatalf("fresh locate limit %d", got)
	}
	if got := videoSendLimit(1, runStart, lastPic, 4*time.Second); got != lastPic+1500+4000 {
		t.Fatalf("paced locate limit %d", got)
	}
	if got := videoSendLimit(2, lastPic, lastPic, 0); got != lastPic+1500 {
		t.Fatalf("play limit %d", got)
	}
}

func TestFirstOverlappingGopSkipsLeadIn(t *testing.T) {
	rows := []GopRow{
		{StreamID: 0, StartMs: 1_000, EndMs: 1_400},
		{StreamID: 0, StartMs: 5_000, EndMs: 5_800, Flags: vptFlagContinuation},
		{StreamID: 0, StartMs: 10_000, EndMs: 11_000},
		{StreamID: 1, StartMs: 9_000, EndMs: 9_500},
	}
	if got := firstOverlappingGop(rows, 2_000, 12_000); got != 9_000 {
		t.Fatalf("lead-in GOP %d", got)
	}
	if got := firstOverlappingGop(rows, 10_500, 12_000); got != 10_000 {
		t.Fatalf("overlapping GOP %d", got)
	}
	if got := firstOverlappingGop(rows, 12_000, 13_000); got != 0 {
		t.Fatalf("empty window returned %d", got)
	}
}

func TestAdvancePastStaleGopUsesTheNearbyKeyframe(t *testing.T) {
	rows := []GopRow{
		{StreamID: 0, StartMs: 1_000, EndMs: 1_400},
		{StreamID: 0, StartMs: 500_000, EndMs: 500_070, Flags: vptFlagContinuation},
		{StreamID: 0, StartMs: 500_140, EndMs: 501_000},
	}
	// Continuation just before the IDR. The previous clip ended long ago.
	if got := advancePastStaleGop(rows, 0, 500_070); got != 500_140 {
		t.Fatalf("stale seek landed on %d", got)
	}
	if got := advancePastStaleGop(rows, 0, 1_200); got != 1_200 {
		t.Fatalf("in-GOP seek moved to %d", got)
	}
	// Next picture is minutes away. Leave the timestamp so Locate can miss.
	if got := advancePastStaleGop(rows, 0, 10_000); got != 10_000 {
		t.Fatalf("real hole jumped to %d", got)
	}
}

func TestPictureMsGroupsSlicesAndStepsFrames(t *testing.T) {
	c := &browserClient{}
	first := c.pictureMs(1_789_233_391_589, 24_711_000)
	same := c.pictureMs(1_789_233_391_605, 24_711_000)
	if same != first {
		t.Fatalf("same picture stamped %d and %d", first, same)
	}
	next := c.pictureMs(1_789_233_391_626, 24_714_000)
	if next != first+33 {
		t.Fatalf("next picture %d, want %d", next, first+33)
	}
	third := c.pictureMs(1_789_233_391_672, 24_717_000)
	if third != first+66 {
		t.Fatalf("third picture %d, want %d", third, first+66)
	}
}

func TestPictureMsRebasesAfterBackwardSeek(t *testing.T) {
	c := &browserClient{}
	start := c.pictureMs(5_000_000, 1_000_000)
	back := c.pictureMs(4_000_000, 10_000)
	if back != 4_000_000 {
		t.Fatalf("seek landed on %d", back)
	}
	if back == start {
		t.Fatal("seek kept the old picture clock")
	}
}
