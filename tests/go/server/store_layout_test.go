package main

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

// setStoreSizes sets the chunk and checkpoint sizes, and the hot tail, for one
// test, and puts the defaults back afterwards.
func setStoreSizes(t *testing.T, seg, every, hot int64) {
	t.Helper()
	oldSeg, oldEvery, oldHot := segmentSize, checkpointEvery, hotTailBytes
	segmentSize, checkpointEvery, hotTailBytes = seg, every, hot
	t.Cleanup(func() {
		segmentSize, checkpointEvery, hotTailBytes = oldSeg, oldEvery, oldHot
	})
}

// writeRunState lays down one run folder with its records in one chunk,
// 000001.seg, and a manifest that lists it in the given state. A sealed chunk
// gets its final counts, as the store writes it; an active one gets the counts
// the store leaves in its manifest, which are none. The records keep the write
// times the test gives them. It returns the chunk's path.
func writeRunState(t *testing.T, segDir, run, state string, recs ...segmentRecord) string {
	t.Helper()
	if len(recs) == 0 {
		t.Fatal("writeRunState needs records")
	}
	dir := filepath.Join(segDir, run)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	var file []byte
	for _, r := range recs {
		file = append(file, encodeFrameAt(r.id, r.writeUnix, r.payload)...)
	}
	path := filepath.Join(dir, chunkFileName(1))
	if err := os.WriteFile(path, file, 0o644); err != nil {
		t.Fatal(err)
	}
	c := &chunk{number: 1, file: chunkFileName(1), path: path, state: state}
	if state == chunkStateSealed {
		c.firstID, c.lastID = recs[0].id, recs[len(recs)-1].id
		c.firstWrite, c.lastWrite = int64(recs[0].writeUnix), int64(recs[len(recs)-1].writeUnix)
		c.records, c.bytes = len(recs), int64(len(file))
	}
	w := &runWriter{id: run, dir: dir, chunks: []*chunk{c}}
	if _, err := w.writeManifest(); err != nil {
		t.Fatal(err)
	}
	return path
}

// writeRun lays down one sealed run folder. See writeRunState.
func writeRun(t *testing.T, segDir, run string, recs ...segmentRecord) {
	t.Helper()
	writeRunState(t, segDir, run, chunkStateSealed, recs...)
}

// runRecord is one record as scanRuns delivered it, with its run.
type runRecord struct {
	run string
	scanned
}

// collectRuns runs scanRuns over segDir and returns what it delivered.
func collectRuns(t *testing.T, segDir string) []runRecord {
	t.Helper()
	var got []runRecord
	err := scanRuns(segDir, func(run string, id MessageID, writeUnix uint32, payload []byte) error {
		got = append(got, runRecord{run, scanned{id, writeUnix, string(payload)}})
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return got
}

// chunkBytes returns the bytes of a chunk file, for comparing it with what a test
// expects.
func chunkBytes(t *testing.T, path string) []byte {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

// Records come back in order across chunks, with the chunk sizes the store writes.
// A record bigger than segmentSize gets a chunk of its own.
func TestStoreSealsChunksAtSegmentSize(t *testing.T) {
	setStoreSizes(t, 4096, 2048, 8<<20)
	dataDir := t.TempDir()
	store, err := OpenStreamStore(dataDir, "S1")
	if err != nil {
		t.Fatal(err)
	}
	payload := func(n int) []byte { return make([]byte, n) }
	// 1000-byte payloads make 1024-byte records: four to a 4096-byte chunk.
	for i := 0; i < 10; i++ {
		if _, err := store.Append(payload(1000)); err != nil {
			t.Fatal(err)
		}
	}
	// Too big to share a chunk with anything, so it gets one by itself.
	if _, err := store.Append(payload(6000)); err != nil {
		t.Fatal(err)
	}
	if _, err := store.Append(payload(1000)); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	w := readRunManifest(filepath.Join(dataDir, "segments"), "S1")
	if w == nil {
		t.Fatal("no manifest for S1")
	}
	type want struct {
		first, last MessageID
		records     int
		bytes       int64
	}
	wants := []want{
		{1, 4, 4, 4096},
		{5, 8, 4, 4096},
		{9, 10, 2, 2048},
		{11, 11, 1, 6024},
		{12, 12, 1, 1024},
	}
	if len(w.chunks) != len(wants) {
		t.Fatalf("manifest lists %d chunks, want %d", len(w.chunks), len(wants))
	}
	for i, wnt := range wants {
		c := w.chunks[i]
		if c.state != chunkStateSealed || c.firstID != wnt.first || c.lastID != wnt.last || c.records != wnt.records || c.bytes != wnt.bytes {
			t.Fatalf("chunk %d = state %s ids %d-%d records %d bytes %d, want sealed ids %d-%d records %d bytes %d",
				i+1, c.state, c.firstID, c.lastID, c.records, c.bytes, wnt.first, wnt.last, wnt.records, wnt.bytes)
		}
	}

	reopened, err := OpenStreamStore(dataDir, "S2")
	if err != nil {
		t.Fatal(err)
	}
	defer reopened.Close()
	for id := MessageID(1); id <= 12; id++ {
		msg, ok, err := reopened.Read(id)
		if err != nil || !ok || msg.ID != id {
			t.Fatalf("read %d: ok=%v err=%v id=%d", id, ok, err, msg.ID)
		}
	}
}

// The chunks are sealed at Close and the manifest says so; nothing is read back from
// a sealed chunk at open.
func TestStoreReopenSealedCorruptDoesNotStopOpen(t *testing.T) {
	setStoreSizes(t, 4096, 2048, 0)
	dataDir := t.TempDir()
	store, err := OpenStreamStore(dataDir, "S1")
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 10; i++ {
		if _, err := store.Append(make([]byte, 1000)); err != nil {
			t.Fatal(err)
		}
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	// Damage the body of the second chunk, after it was sealed.
	path := filepath.Join(dataDir, "segments", "S1", chunkFileName(2))
	data := chunkBytes(t, path)
	data[frameHeaderBytes+10] ^= 0xFF
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}

	reopened, err := OpenStreamStore(dataDir, "S2")
	if err != nil {
		t.Fatalf("open with a damaged sealed chunk: %v", err)
	}
	defer reopened.Close()
	if got := reopened.LatestID(); got != 10 {
		t.Fatalf("latest ID %d, want 10", got)
	}
	if _, ok, err := reopened.Read(1); err != nil || !ok {
		t.Fatalf("read from the undamaged first chunk: ok=%v err=%v", ok, err)
	}
	if _, _, err := reopened.Read(5); err == nil {
		t.Fatal("read from the damaged chunk succeeded")
	}

	id, err := reopened.Append([]byte("next"))
	if err != nil {
		t.Fatal(err)
	}
	if id != 11 {
		t.Fatalf("next ID %d, want 11", id)
	}
	if _, err := os.Stat(filepath.Join(dataDir, "segments", "S2", chunkFileName(1))); err != nil {
		t.Fatalf("new run has no chunk: %v", err)
	}
}

// A chunk left active by a crash, with a torn record at its end, is cut back to its
// last whole record and sealed when the store opens. The next ID follows.
func TestStoreRecoversActiveChunkWithTornTail(t *testing.T) {
	setStoreSizes(t, 32<<20, 1<<20, 8<<20)
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	if err := os.MkdirAll(segDir, 0o755); err != nil {
		t.Fatal(err)
	}
	path := writeRunState(t, segDir, "S1", chunkStateActive,
		segmentRecord{id: 1, writeUnix: 1_700_000_000, payload: []byte("one")},
		segmentRecord{id: 2, writeUnix: 1_700_000_001, payload: []byte("two")},
		segmentRecord{id: 3, writeUnix: 1_700_000_002, payload: []byte("three")})
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Truncate(path, info.Size()-5); err != nil {
		t.Fatal(err)
	}
	wantSize := int64(2*frameHeaderBytes + len("one") + len("two"))

	store, err := OpenStreamStore(dataDir, "S2")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	if got := store.LatestID(); got != 2 {
		t.Fatalf("latest ID %d, want 2", got)
	}
	if info, err := os.Stat(path); err != nil || info.Size() != wantSize {
		t.Fatalf("chunk size %v (err %v), want %d", info, err, wantSize)
	}
	w := readRunManifest(segDir, "S1")
	if w == nil || len(w.chunks) != 1 || w.chunks[0].state != chunkStateSealed || w.chunks[0].lastID != 2 || w.chunks[0].records != 2 {
		t.Fatalf("manifest after recovery: %+v", w)
	}
	msg, ok, err := store.Read(2)
	if err != nil || !ok || string(msg.Payload) != "two" {
		t.Fatalf("read 2 after recovery: %q ok=%v err=%v", msg.Payload, ok, err)
	}
	if _, ok, err := store.Read(3); ok || err != nil {
		t.Fatalf("read 3 after recovery: ok=%v err=%v, want not stored", ok, err)
	}
	if id, err := store.Append([]byte("after")); err != nil || id != 3 {
		t.Fatalf("append after recovery: id %d err %v, want 3", id, err)
	}
}

// Every run gets its own folder, named by its ID, and IDs carry on across runs.
func TestStoreNewRunIDGetsNewFolder(t *testing.T) {
	setStoreSizes(t, 32<<20, 1<<20, 8<<20)
	dataDir := t.TempDir()
	first, err := OpenStreamStore(dataDir, "S1")
	if err != nil {
		t.Fatal(err)
	}
	for _, p := range []string{"a", "b"} {
		if _, err := first.Append([]byte(p)); err != nil {
			t.Fatal(err)
		}
	}
	if err := first.Close(); err != nil {
		t.Fatal(err)
	}

	second, err := OpenStreamStore(dataDir, "S2")
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()
	id, err := second.Append([]byte("c"))
	if err != nil {
		t.Fatal(err)
	}
	if id != 3 {
		t.Fatalf("first ID of the new run is %d, want 3", id)
	}
	got := collectRuns(t, filepath.Join(dataDir, "segments"))
	if len(got) != 3 || got[0].run != "S1" || got[1].run != "S1" || got[2].run != "S2" || got[2].payload != "c" {
		t.Fatalf("runs scanned %+v, want S1 S1 S2", got)
	}
	if _, err := OpenStreamStore(dataDir, "bad/name"); err == nil {
		t.Fatal("a run ID with a slash was accepted")
	}
}

// Messages are read back by ID with the hot tail off, so every read goes to disk
// through a checkpoint: in order, in reverse, and in a stride that never runs in
// order. Each chunk's first, middle and last message are checked too.
func TestStoreReadsTenThousandRecordsWithoutHotTail(t *testing.T) {
	setStoreSizes(t, 8<<10, 2<<10, 0)
	dataDir := t.TempDir()
	store, err := OpenStreamStore(dataDir, "S1")
	if err != nil {
		t.Fatal(err)
	}
	const n = 10000
	payloadOf := func(id MessageID) []byte {
		b := make([]byte, 20+int(id%50))
		for i := range b {
			b[i] = byte(int(id) + i)
		}
		return b
	}
	for i := MessageID(1); i <= n; i++ {
		if _, err := store.Append(payloadOf(i)); err != nil {
			t.Fatal(err)
		}
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	check := func(s *StreamStore, id MessageID) {
		t.Helper()
		msg, ok, err := s.Read(id)
		if err != nil || !ok || msg.ID != id || string(msg.Payload) != string(payloadOf(id)) {
			t.Fatalf("read %d: ok=%v err=%v id=%d", id, ok, err, msg.ID)
		}
	}
	reopened, err := OpenStreamStore(dataDir, "S2")
	if err != nil {
		t.Fatal(err)
	}
	defer reopened.Close()
	// Reverse order: every read starts from a checkpoint.
	for id := MessageID(n); id >= 1; id-- {
		check(reopened, id)
	}
	// A stride of 7 from each of 1 to 7 reads every message once, in an order
	// that jumps about.
	for start := MessageID(1); start <= 7; start++ {
		for id := start; id <= n; id += 7 {
			check(reopened, id)
		}
	}
	// In order: most reads continue from the last one, without a checkpoint.
	for id := MessageID(1); id <= n; id++ {
		check(reopened, id)
	}
	reopened.mu.Lock()
	chunks := append([]*chunk(nil), reopened.chunks...)
	reopened.mu.Unlock()
	if len(chunks) < 10 {
		t.Fatalf("only %d chunks; the test wants many", len(chunks))
	}
	for _, c := range chunks {
		for _, id := range []MessageID{c.firstID, c.firstID + MessageID(c.records/2), c.lastID} {
			check(reopened, id)
		}
	}
}

// A reader waiting for the next message is woken by the append that stores it.
func TestStoreReadOrWaitWakesOnAppend(t *testing.T) {
	setStoreSizes(t, 32<<20, 1<<20, 8<<20)
	store, err := OpenStreamStore(t.TempDir(), "S1")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()

	type result struct {
		msg Message
		err error
	}
	done := make(chan result, 1)
	next := store.StartIDForNewClient()
	go func() {
		msg, err := store.ReadOrWait(next)
		done <- result{msg, err}
	}()
	if _, err := store.Append([]byte("woken")); err != nil {
		t.Fatal(err)
	}
	select {
	case r := <-done:
		if r.err != nil || string(r.msg.Payload) != "woken" || r.msg.ID != next {
			t.Fatalf("ReadOrWait = %q id %d err %v", r.msg.Payload, r.msg.ID, r.err)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("ReadOrWait did not wake on append")
	}
}

// scanRuns reads the active chunk of a store that is still open, up to the bytes it
// has when the scan opens it, and a torn record at its end ends the scan.
func TestScanRunsReadsActiveChunkAndStopsAtTornTail(t *testing.T) {
	setStoreSizes(t, 32<<20, 1<<20, 8<<20)
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	store, err := OpenStreamStore(dataDir, "S1")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	for _, p := range []string{"a", "b", "c"} {
		if _, err := store.Append([]byte(p)); err != nil {
			t.Fatal(err)
		}
	}
	path := filepath.Join(segDir, "S1", chunkFileName(1))
	if got := collectRuns(t, segDir); len(got) != 3 || got[2].payload != "c" {
		t.Fatalf("scan while open saw %+v, want 3 records ending in c", got)
	}

	// Cut the last record short, as a crash would leave it.
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.Truncate(path, info.Size()-2); err != nil {
		t.Fatal(err)
	}
	before := snapshotTree(t, dataDir)
	got := collectRuns(t, segDir)
	if len(got) != 2 || got[0].payload != "a" || got[1].payload != "b" {
		t.Fatalf("scan with a torn tail saw %+v, want records a and b", got)
	}
	assertSameTree(t, before, snapshotTree(t, dataDir))
}

// A run folder whose manifest is missing or does not parse, a legacy folder, and
// stray files are all left out of a scan, and nothing in them changes.
func TestScanRunsSkipsOddFoldersAndLeavesThemAlone(t *testing.T) {
	setStoreSizes(t, 32<<20, 1<<20, 8<<20)
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	if err := os.MkdirAll(segDir, 0o755); err != nil {
		t.Fatal(err)
	}
	// Run S1: record 2 is damaged, so the scan stops before it.
	writeRun(t, segDir, "S1",
		segmentRecord{id: 1, writeUnix: 1_700_000_000, payload: []byte("one")},
		segmentRecord{id: 2, writeUnix: 1_700_000_001, payload: []byte("two")},
		segmentRecord{id: 3, writeUnix: 1_700_000_002, payload: []byte("three")})
	data := chunkBytes(t, filepath.Join(segDir, "S1", chunkFileName(1)))
	data[frameHeaderBytes+len("one")+frameHeaderBytes] ^= 0xFF
	if err := os.WriteFile(filepath.Join(segDir, "S1", chunkFileName(1)), data, 0o644); err != nil {
		t.Fatal(err)
	}
	// Run S2 has a manifest that does not parse: skipped.
	if err := os.MkdirAll(filepath.Join(segDir, "S2"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(segDir, "S2", manifestName), []byte("{not json"), 0o644); err != nil {
		t.Fatal(err)
	}
	// Run S3 is fine, and its record is delivered.
	writeRun(t, segDir, "S3", segmentRecord{id: 4, writeUnix: 1_700_000_100, payload: []byte("four")})
	// Legacy holds old files; a stray file sits in S3.
	if err := os.MkdirAll(filepath.Join(segDir, legacyFolder), 0o755); err != nil {
		t.Fatal(err)
	}
	writeSegment(t, filepath.Join(segDir, legacyFolder), 1, segmentRecord{id: 9, payload: []byte("old")})
	if err := os.WriteFile(filepath.Join(segDir, "S3", "notes.txt"), []byte("junk"), 0o644); err != nil {
		t.Fatal(err)
	}
	before := snapshotTree(t, dataDir)

	var ids []MessageID
	for _, r := range collectRuns(t, segDir) {
		ids = append(ids, r.id)
	}
	if want := []MessageID{1, 4}; len(ids) != len(want) || ids[0] != want[0] || ids[1] != want[1] {
		t.Fatalf("scanned IDs %v, want %v", ids, want)
	}
	assertSameTree(t, before, snapshotTree(t, dataDir))
}
