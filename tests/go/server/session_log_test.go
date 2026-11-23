package main

import (
	"os"
	"path/filepath"
	"slices"
	"testing"
	"time"
)

// writeSessionLog replaces sessions.log in dir with text.
func writeSessionLog(t *testing.T, dir, text string) {
	t.Helper()
	if err := os.WriteFile(filepath.Join(dir, sessionLogName), []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
}

// The session ID is the server start time in UTC, to the second, and the live
// frame rows carry it.
func TestSessionIDIsUTCStartTime(t *testing.T) {
	startedAt := time.Date(2026, 11, 22, 11, 30, 0, 0, time.FixedZone("UTC+2", 2*3600))
	if got := sessionID(startedAt); got != testSID {
		t.Fatalf("sessionID = %q, want %q", got, testSID)
	}
	if got := newFrameSession(startedAt, &recordSink{}).sid; got != testSID {
		t.Fatalf("live session ID = %q, want %q", got, testSID)
	}
}

func TestSessionLogRoundTrip(t *testing.T) {
	dir := t.TempDir()
	if err := appendSessionStart(dir, "S2", 500); err != nil {
		t.Fatal(err)
	}
	if err := appendSessionStart(dir, "S1", 1); err != nil {
		t.Fatal(err)
	}
	got, err := readSessionStarts(dir)
	if err != nil {
		t.Fatal(err)
	}
	// Read back sorted by first ID, whatever order they were appended in.
	want := []sessionStart{{SID: "S1", FirstID: 1}, {SID: "S2", FirstID: 500}}
	if !slices.Equal(got, want) {
		t.Fatalf("read %+v, want %+v", got, want)
	}
}

func TestSessionLogMissingFileIsEmpty(t *testing.T) {
	got, err := readSessionStarts(t.TempDir())
	if err != nil {
		t.Fatalf("missing sessions.log: %v", err)
	}
	if len(got) != 0 {
		t.Fatalf("missing sessions.log read as %+v, want none", got)
	}
}

// Lines that are not "<sid> <first id>" are skipped, and so is a first ID of 0.
// A last line without its newline may be half written, so it is skipped too.
func TestSessionLogSkipsMalformedLines(t *testing.T) {
	dir := t.TempDir()
	writeSessionLog(t, dir, "garbage\n"+
		"S1 notanumber\n"+
		"S1 1 extra\n"+
		"\n"+
		"S0 0\n"+
		"S2 7\n"+
		"  S4 8  \n"+
		"S3 9") // no newline: the write may not have finished
	got, err := readSessionStarts(dir)
	if err != nil {
		t.Fatal(err)
	}
	want := []sessionStart{{SID: "S2", FirstID: 7}, {SID: "S4", FirstID: 8}}
	if !slices.Equal(got, want) {
		t.Fatalf("read %+v, want %+v", got, want)
	}
}

// Two runs that share a first ID: the later line wins, since the earlier run
// stopped without writing anything.
func TestSessionLogDuplicateFirstIDLaterWins(t *testing.T) {
	dir := t.TempDir()
	writeSessionLog(t, dir, "S1 10\nS2 20\nS3 10\n")
	got, err := readSessionStarts(dir)
	if err != nil {
		t.Fatal(err)
	}
	want := []sessionStart{{SID: "S3", FirstID: 10}, {SID: "S2", FirstID: 20}}
	if !slices.Equal(got, want) {
		t.Fatalf("read %+v, want %+v", got, want)
	}
}
