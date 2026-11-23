package main

import (
	"encoding/binary"
	"io/fs"
	"os"
	"path/filepath"
	"slices"
	"testing"
	"time"
)

// segmentRecord is one record to lay down by hand, with the write time it carries.
type segmentRecord struct {
	id        MessageID
	writeUnix uint32
	payload   []byte
}

// writeSegment writes records as the segment file that starts at first, the way
// the store lays them out, with the write times the test gives.
func writeSegment(t *testing.T, segDir string, first MessageID, recs ...segmentRecord) string {
	t.Helper()
	var file []byte
	for _, r := range recs {
		frame := encodeFrame(r.id, r.payload)
		binary.BigEndian.PutUint32(frame[16:20], r.writeUnix)
		file = append(file, frame...)
	}
	path := filepath.Join(segDir, segmentFileName(int(first)))
	if err := os.WriteFile(path, file, 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

// scanned is one record as scanSegments delivered it. The payload is copied.
type scanned struct {
	id        MessageID
	writeUnix uint32
	payload   string
}

// collectSegments runs scanSegments over dir and returns what it delivered.
func collectSegments(t *testing.T, dir string) []scanned {
	t.Helper()
	var got []scanned
	err := scanSegments(dir, func(id MessageID, writeUnix uint32, payload []byte) error {
		got = append(got, scanned{id, writeUnix, string(payload)})
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return got
}

// snapshotTree maps every file under dir, by relative path, to its contents.
func snapshotTree(t *testing.T, dir string) map[string]string {
	t.Helper()
	files := make(map[string]string)
	err := filepath.WalkDir(dir, func(path string, d fs.DirEntry, err error) error {
		if err != nil || d.IsDir() {
			return err
		}
		b, err := os.ReadFile(path)
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(dir, path)
		files[filepath.ToSlash(rel)] = string(b)
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return files
}

// Records the store wrote, across two segment files, come back in ID order with
// their payloads and write times.
func TestScanSegmentsReadsRecordsInOrder(t *testing.T) {
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	store, err := OpenStreamStore(dataDir)
	if err != nil {
		t.Fatal(err)
	}
	before := time.Now().Unix()
	for _, p := range []string{"one", "two", "three"} {
		if _, err := store.Append([]byte(p)); err != nil {
			t.Fatal(err)
		}
	}
	after := time.Now().Unix()
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	// The second file, as if the store had rotated: IDs 4 and 5 with fixed write
	// times. The second record has an empty payload.
	writeSegment(t, segDir, 4,
		segmentRecord{id: 4, writeUnix: 1_700_000_000, payload: []byte("four")},
		segmentRecord{id: 5, writeUnix: 1_700_000_100})

	got := collectSegments(t, segDir)
	if len(got) != 5 {
		t.Fatalf("got %d records, want 5: %+v", len(got), got)
	}
	for i, want := range []string{"one", "two", "three"} {
		r := got[i]
		if r.id != MessageID(i+1) || r.payload != want {
			t.Fatalf("record %d = %+v, want id %d payload %q", i, r, i+1, want)
		}
		if int64(r.writeUnix) < before || int64(r.writeUnix) > after {
			t.Fatalf("record %d write time %d outside [%d, %d]", i, r.writeUnix, before, after)
		}
	}
	if want := (scanned{4, 1_700_000_000, "four"}); got[3] != want {
		t.Fatalf("record 4 = %+v, want %+v", got[3], want)
	}
	if want := (scanned{5, 1_700_000_100, ""}); got[4] != want {
		t.Fatalf("record 5 = %+v, want %+v", got[4], want)
	}
}

// The server keeps the active segment open and writes to it. A scan sees the
// records on disk when it runs.
func TestScanSegmentsSeesActiveSegment(t *testing.T) {
	dataDir := t.TempDir()
	store, err := OpenStreamStore(dataDir)
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	for _, p := range []string{"a", "b"} {
		if _, err := store.Append([]byte(p)); err != nil {
			t.Fatal(err)
		}
	}
	if got := collectSegments(t, filepath.Join(dataDir, "segments")); len(got) != 2 {
		t.Fatalf("scan while open saw %d records, want 2", len(got))
	}
	if _, err := store.Append([]byte("c")); err != nil {
		t.Fatal(err)
	}
	if got := collectSegments(t, filepath.Join(dataDir, "segments")); len(got) != 3 || got[2].payload != "c" {
		t.Fatalf("scan after another write saw %+v, want 3 records ending in c", got)
	}
}

// A file ends at its first short record: a crash mid-write leaves one. The
// records before it are delivered, and the file is left as it was.
func TestScanSegmentsStopsAtShortTail(t *testing.T) {
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	if err := os.MkdirAll(segDir, 0o755); err != nil {
		t.Fatal(err)
	}
	path := writeSegment(t, segDir, 1,
		segmentRecord{id: 1, payload: []byte("one")},
		segmentRecord{id: 2, payload: []byte("two")},
		segmentRecord{id: 3, payload: []byte("three")})
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Truncate(path, info.Size()-5); err != nil {
		t.Fatal(err)
	}
	before := snapshotTree(t, dataDir)

	got := collectSegments(t, segDir)
	if len(got) != 2 || got[0].payload != "one" || got[1].payload != "two" {
		t.Fatalf("got %+v, want records 1 and 2 only", got)
	}
	assertSameTree(t, before, snapshotTree(t, dataDir))
}

// A bad checksum ends its file, and the scan goes on to the next file. Odd
// files in the folder are ignored, and nothing in the folder changes.
func TestScanSegmentsLeavesDataDirAlone(t *testing.T) {
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	if err := os.MkdirAll(segDir, 0o755); err != nil {
		t.Fatal(err)
	}
	// File 1: record 2's payload is corrupted, so its checksum fails.
	path := writeSegment(t, segDir, 1,
		segmentRecord{id: 1, payload: []byte("one")},
		segmentRecord{id: 2, payload: []byte("two")},
		segmentRecord{id: 3, payload: []byte("three")})
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	const record2Payload = (frameHeaderBytes + len("one")) + frameHeaderBytes // record 1, then record 2's header
	data[record2Payload] ^= 0xFF
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	writeSegment(t, segDir, 4, segmentRecord{id: 4, payload: []byte("four")})
	// Files the scan must skip: not a segment name, and a name with extra text.
	if err := os.WriteFile(filepath.Join(segDir, "000000000009.log.bak"), []byte("junk"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(segDir, "notes.txt"), []byte("junk"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := appendSessionStart(segDir, "S1", 1); err != nil {
		t.Fatal(err)
	}
	before := snapshotTree(t, dataDir)

	var ids []MessageID
	for _, r := range collectSegments(t, segDir) {
		ids = append(ids, r.id)
	}
	if want := []MessageID{1, 4}; !slices.Equal(ids, want) {
		t.Fatalf("scanned IDs %v, want %v", ids, want)
	}
	assertSameTree(t, before, snapshotTree(t, dataDir))
}

// assertSameTree fails the test if the files in after are not the files in
// before, with the same contents.
func assertSameTree(t *testing.T, before, after map[string]string) {
	t.Helper()
	if len(after) != len(before) {
		t.Fatalf("data directory has %d files, had %d", len(after), len(before))
	}
	for name, want := range before {
		got, ok := after[name]
		if !ok {
			t.Fatalf("%s is gone", name)
		}
		if got != want {
			t.Fatalf("%s changed", name)
		}
	}
}
