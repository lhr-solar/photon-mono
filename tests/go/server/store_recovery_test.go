package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A chunk file missing from disk, and a manifest that does not parse, are logged
// and skipped at open. The missing chunk's IDs still count, so no ID is reused, and
// its messages say they are not on disk.
func TestStoreSkipsMissingChunkAndBadManifest(t *testing.T) {
	setStoreSizes(t, 32<<20, 1<<20, 8<<20)
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	if err := os.MkdirAll(segDir, 0o755); err != nil {
		t.Fatal(err)
	}
	writeRun(t, segDir, "S1",
		segmentRecord{id: 1, writeUnix: 1_700_000_000, payload: []byte("one")},
		segmentRecord{id: 2, writeUnix: 1_700_000_001, payload: []byte("two")},
		segmentRecord{id: 3, writeUnix: 1_700_000_002, payload: []byte("three")})
	missing := writeRunState(t, segDir, "S2", chunkStateSealed,
		segmentRecord{id: 4, writeUnix: 1_700_000_010, payload: []byte("four")},
		segmentRecord{id: 5, writeUnix: 1_700_000_011, payload: []byte("five")})
	if err := os.Remove(missing); err != nil {
		t.Fatal(err)
	}
	writeRun(t, segDir, "S3", segmentRecord{id: 6, writeUnix: 1_700_000_020, payload: []byte("six")})
	if err := os.MkdirAll(filepath.Join(segDir, "S4"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(segDir, "S4", manifestName), []byte("{broken"), 0o644); err != nil {
		t.Fatal(err)
	}

	store, err := OpenStreamStore(dataDir, "S5")
	if err != nil {
		t.Fatalf("open with a missing chunk and a bad manifest: %v", err)
	}
	defer store.Close()
	if got := store.LatestID(); got != 6 {
		t.Fatalf("latest ID %d, want 6 (the missing chunk's IDs count)", got)
	}
	if _, _, err := store.Read(4); err == nil || !strings.Contains(err.Error(), "not on disk") {
		t.Fatalf("read 4 from a missing chunk: err %v, want a not-on-disk error", err)
	}
	if msg, ok, err := store.Read(6); err != nil || !ok || string(msg.Payload) != "six" {
		t.Fatalf("read 6: %q ok=%v err=%v", msg.Payload, ok, err)
	}
	if id, err := store.Append([]byte("seven")); err != nil || id != 7 {
		t.Fatalf("append: id %d err %v, want 7", id, err)
	}
}

// Storage pressure counts every byte under segments/, the manifests included, and
// the running total matches a walk of the folder after manifests are rewritten.
func TestStorePressureCountsEveryFileUnderSegments(t *testing.T) {
	setStoreSizes(t, 4096, 2048, 8<<20)
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	store, err := OpenStreamStore(dataDir, "S1")
	if err != nil {
		t.Fatal(err)
	}
	store.SetMaxBytes(2000)
	if store.StoragePressureHigh() {
		t.Fatal("pressure high on an empty store")
	}
	// Enough records for several chunks, so the manifest is rewritten several times.
	for i := 0; i < 10; i++ {
		if _, err := store.Append(make([]byte, 1000)); err != nil {
			t.Fatal(err)
		}
	}
	if !store.StoragePressureHigh() {
		t.Fatal("pressure not high with 10 KB under a 2000-byte limit")
	}
	walked, err := dirBytes(segDir)
	if err != nil {
		t.Fatal(err)
	}
	if got := store.Stats().DiskBytes; got != walked {
		t.Fatalf("running disk bytes %d, walk says %d", got, walked)
	}
	store.SetMaxBytes(0)
	if store.StoragePressureHigh() {
		t.Fatal("pressure high with no limit")
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	reopened, err := OpenStreamStore(dataDir, "S2")
	if err != nil {
		t.Fatal(err)
	}
	defer reopened.Close()
	walked, err = dirBytes(segDir)
	if err != nil {
		t.Fatal(err)
	}
	if got := reopened.Stats().DiskBytes; got != walked {
		t.Fatalf("disk bytes after reopen %d, walk says %d", got, walked)
	}
}
