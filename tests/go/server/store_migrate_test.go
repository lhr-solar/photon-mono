package main

import (
	"encoding/binary"
	"errors"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"
)

// migrateFallbackRun is the run that names the records older than the first start
// in sessions.log. It is named from the write time of the first of them.
const migrateFallbackRun = "20240601T110000Z"

// oldRecords returns the records from..to, as the old layout wrote them: 1000-byte
// payloads, and write times that put records 1-2 before the first start, 3-22 in
// run S1 and 23-40 in run S2.
func oldRecords(from, to MessageID) []segmentRecord {
	var recs []segmentRecord
	for id := from; id <= to; id++ {
		var base time.Time
		switch {
		case id <= 2:
			base = time.Date(2024, 6, 1, 11, 0, 0, 0, time.UTC).Add(time.Duration(id-1) * time.Second)
		case id < 23:
			base = time.Date(2024, 6, 1, 12, 0, 0, 0, time.UTC).Add(time.Duration(id-3) * time.Second)
		default:
			base = time.Date(2024, 6, 1, 13, 0, 0, 0, time.UTC).Add(time.Duration(id-23) * time.Second)
		}
		payload := make([]byte, 1000)
		for i := range payload {
			payload[i] = byte(int(id) + i)
		}
		recs = append(recs, segmentRecord{id: id, writeUnix: uint32(base.Unix()), payload: payload})
	}
	return recs
}

// writeOldFiles lays down the old layout: two segment files, 000000000001.log with
// messages 1-25 and 000000000002.log with 26-40, and a sessions.log that starts run
// S1 at message 3 and run S2 at message 23.
func writeOldFiles(t *testing.T, segDir string) {
	t.Helper()
	writeSegment(t, segDir, 1, oldRecords(1, 25)...)
	writeSegment(t, segDir, 2, oldRecords(26, 40)...)
	writeSessionLog(t, segDir, "S1 3\nS2 23\n")
}

// frameHeader is one record as it is on disk: its header fields and payload.
type frameHeader struct {
	id        MessageID
	writeUnix uint32
	crc       uint32
	payload   string
}

// headersIn parses the whole records in data.
func headersIn(data []byte) []frameHeader {
	var out []frameHeader
	for len(data) >= frameHeaderBytes {
		n := int(binary.BigEndian.Uint32(data[4:8]))
		if len(data) < frameHeaderBytes+n {
			break
		}
		out = append(out, frameHeader{
			id:        MessageID(binary.BigEndian.Uint64(data[8:16])),
			writeUnix: binary.BigEndian.Uint32(data[16:20]),
			crc:       binary.BigEndian.Uint32(data[20:24]),
			payload:   string(data[frameHeaderBytes : frameHeaderBytes+n]),
		})
		data = data[frameHeaderBytes+n:]
	}
	return out
}

// runHeaders returns the records of every run folder, in run order and then
// chunk order, as their headers are on disk.
func runHeaders(t *testing.T, segDir string) []frameHeader {
	t.Helper()
	names, err := listRunFolders(segDir)
	if err != nil {
		t.Fatal(err)
	}
	var out []frameHeader
	for _, name := range names {
		w := readRunManifest(segDir, name)
		if w == nil {
			t.Fatalf("run %s has no readable manifest", name)
		}
		for _, c := range w.chunks {
			out = append(out, headersIn(chunkBytes(t, c.path)[:c.bytes])...)
		}
	}
	return out
}

// The old files become run folders with the same records: same IDs, payloads, write
// times and checksums, split into runs by sessions.log and the fallback rule. The
// old files move into legacy, and a second start does nothing.
func TestMigrateLegacySegmentsCopiesRecordsUnchanged(t *testing.T) {
	setStoreSizes(t, 4096, 2048, 8<<20)
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	if err := os.MkdirAll(segDir, 0o755); err != nil {
		t.Fatal(err)
	}
	writeOldFiles(t, segDir)
	oldFiles := []string{segmentFileName(1), segmentFileName(2), sessionLogName}
	oldBytes := snapshotTree(t, dataDir)

	var wantHeaders []frameHeader
	for _, name := range oldFiles[:2] {
		wantHeaders = append(wantHeaders, headersIn(chunkBytes(t, filepath.Join(segDir, name)))...)
	}
	wantRecords := collectSegments(t, segDir)
	if len(wantRecords) != 40 {
		t.Fatalf("old files hold %d records, want 40", len(wantRecords))
	}

	mig, err := migrateLegacySegments(segDir)
	if err != nil {
		t.Fatalf("migrate: %v", err)
	}
	if mig.Runs != 3 || mig.Records != 40 || mig.Moved != 3 {
		t.Fatalf("migration %+v, want 3 runs, 40 records, 3 files moved", mig)
	}

	// The old files are in legacy, unchanged, and not in segments.
	for _, name := range oldFiles {
		if _, err := os.Stat(filepath.Join(segDir, name)); !os.IsNotExist(err) {
			t.Fatalf("%s is still in segments (err %v)", name, err)
		}
	}
	legacyDir := filepath.Join(segDir, legacyFolder)
	for _, name := range oldFiles {
		want := oldBytes[filepath.ToSlash(filepath.Join("segments", name))]
		got, err := os.ReadFile(filepath.Join(legacyDir, name))
		if err != nil || string(got) != want {
			t.Fatalf("%s in legacy: err %v, differs from the old file", name, err)
		}
	}

	// Same records, headers and all, in the same order.
	gotHeaders := runHeaders(t, segDir)
	if !slices.Equal(gotHeaders, wantHeaders) {
		t.Fatalf("run folders hold %d records that differ from the %d old ones", len(gotHeaders), len(wantHeaders))
	}

	// Each record is in the run the old sessions.log and the fallback rule give it.
	runs := collectRuns(t, segDir)
	for _, r := range runs {
		want := "S2"
		switch {
		case r.id <= 2:
			want = migrateFallbackRun
		case r.id < 23:
			want = "S1"
		}
		if r.run != want {
			t.Fatalf("message %d is in run %s, want %s", r.id, r.run, want)
		}
	}
	if len(runs) != len(wantRecords) {
		t.Fatalf("runs scan delivered %d records, want %d", len(runs), len(wantRecords))
	}

	// A second start finds nothing to do and changes nothing.
	before := snapshotTree(t, dataDir)
	again, err := migrateLegacySegments(segDir)
	if err != nil {
		t.Fatalf("second migrate: %v", err)
	}
	if again != (legacyMigration{}) {
		t.Fatalf("second migrate did %+v, want nothing", again)
	}
	assertSameTree(t, before, snapshotTree(t, dataDir))

	// And the store opens on the migrated data, numbering on from the last record.
	store, err := OpenStreamStore(dataDir, "S9")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if got := store.LatestID(); got != 40 {
		t.Fatalf("latest ID %d, want 40", got)
	}
	msg, ok, err := store.Read(26)
	if err != nil || !ok || string(msg.Payload) != string(oldRecords(26, 26)[0].payload) {
		t.Fatalf("read 26 after migration: ok=%v err=%v", ok, err)
	}
}

// A migration that fails part way leaves the old files where they are and no run
// folders behind. The store then numbers new messages above the old ones.
func TestMigrateFailureLeavesOldFilesAndNoRunFolders(t *testing.T) {
	setStoreSizes(t, 4096, 2048, 8<<20)
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	if err := os.MkdirAll(segDir, 0o755); err != nil {
		t.Fatal(err)
	}
	writeOldFiles(t, segDir)
	before := snapshotTree(t, dataDir)

	// Reading the second old file fails, after the first has been copied.
	failing := true
	orig := openChunkFile
	openChunkFile = func(path string) (*os.File, error) {
		if failing && strings.HasSuffix(path, segmentFileName(2)) {
			return nil, errors.New("injected read failure")
		}
		return orig(path)
	}
	t.Cleanup(func() { openChunkFile = orig })

	if _, err := migrateLegacySegments(segDir); err == nil {
		t.Fatal("migrate succeeded with an unreadable old file")
	}
	assertSameTree(t, before, snapshotTree(t, dataDir))
	if names, err := listRunFolders(segDir); err != nil || len(names) != 0 {
		t.Fatalf("run folders after a failed migration: %v (err %v), want none", names, err)
	}
	if _, err := os.Stat(filepath.Join(segDir, legacyFolder)); !os.IsNotExist(err) {
		t.Fatalf("legacy folder after a failed migration (err %v), want none", err)
	}

	// The next ID is above the newest old record, so no ID is used twice.
	failing = false
	floor, err := legacyNextID(segDir)
	if err != nil {
		t.Fatal(err)
	}
	if floor != 41 {
		t.Fatalf("next ID above the old files is %d, want 41", floor)
	}
	store, err := OpenStreamStore(dataDir, "S9")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	store.raiseNextID(floor)
	if id, err := store.Append([]byte("new")); err != nil || id != 41 {
		t.Fatalf("append after a failed migration: id %d err %v, want 41", id, err)
	}

	// The next start tries again and now succeeds.
	openChunkFile = orig
	mig, err := migrateLegacySegments(segDir)
	if err != nil {
		t.Fatalf("retry: %v", err)
	}
	if mig.Records != 40 {
		t.Fatalf("retry migrated %d records, want 40", mig.Records)
	}
}

// Old files already in legacy are never overwritten: the migration refuses before it
// writes anything.
func TestMigrateRefusesToOverwriteLegacy(t *testing.T) {
	setStoreSizes(t, 4096, 2048, 8<<20)
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	if err := os.MkdirAll(filepath.Join(segDir, legacyFolder), 0o755); err != nil {
		t.Fatal(err)
	}
	writeOldFiles(t, segDir)
	writeSegment(t, filepath.Join(segDir, legacyFolder), 1, oldRecords(1, 1)...)
	before := snapshotTree(t, dataDir)

	if _, err := migrateLegacySegments(segDir); err == nil || !strings.Contains(err.Error(), segmentFileName(1)) {
		t.Fatalf("migrate err = %v, want a refusal naming %s", err, segmentFileName(1))
	}
	assertSameTree(t, before, snapshotTree(t, dataDir))
	if names, err := listRunFolders(segDir); err != nil || len(names) != 0 {
		t.Fatalf("run folders after a refusal: %v (err %v)", names, err)
	}
}

// With no old files there is nothing to migrate, and no legacy folder is made. A
// segments folder that does not exist yet, as in a new data directory, is the same.
func TestMigrateNothingToDo(t *testing.T) {
	segDir := t.TempDir()
	mig, err := migrateLegacySegments(segDir)
	if err != nil || mig != (legacyMigration{}) {
		t.Fatalf("migrate of an empty folder: %+v, err %v", mig, err)
	}
	missing := filepath.Join(t.TempDir(), "segments")
	if mig, err := migrateLegacySegments(missing); err != nil || mig != (legacyMigration{}) {
		t.Fatalf("migrate of a missing folder: %+v, err %v", mig, err)
	}
	if floor, err := legacyNextID(missing); err != nil || floor != 0 {
		t.Fatalf("next ID above a missing folder is %d (err %v), want 0", floor, err)
	}
	if _, err := os.Stat(filepath.Join(segDir, legacyFolder)); !os.IsNotExist(err) {
		t.Fatalf("legacy folder was made (err %v)", err)
	}
	if floor, err := legacyNextID(segDir); err != nil || floor != 0 {
		t.Fatalf("next ID above nothing is %d (err %v), want 0", floor, err)
	}
}
