package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"log"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"testing"
	"time"
)

// fakeRemote is the rclone destination the archive tests use. It never reaches a
// real remote.
const fakeRemote = "sharepoint:photon/segments"

// fakeRclone replaces runRclone for one test. The remote is a folder on this disk:
// "sharepoint:photon/segments/<run>/<file>" is dir/<run>/<file>.
type fakeRclone struct {
	dir   string     // the fake remote folder
	calls [][]string // every call, in order
	fail  string     // when set, every call fails: this is its stderr, with a second line
}

func (f *fakeRclone) run(ctx context.Context, args ...string) ([]byte, error) {
	f.calls = append(f.calls, append([]string(nil), args...))
	if f.fail != "" {
		return []byte(f.fail + "\nsecond line of stderr"), errors.New("exit status 1")
	}
	switch args[0] {
	case "copyto":
		data, err := os.ReadFile(args[1])
		if err != nil {
			return []byte(err.Error()), err
		}
		dst := f.local(args[2])
		if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
			return nil, err
		}
		return nil, os.WriteFile(dst, data, 0o644)
	case "lsjson":
		p := f.local(args[1])
		st, err := os.Stat(p)
		if err != nil {
			return []byte("ERROR : not found"), err
		}
		listing := []map[string]any{{"Path": filepath.Base(p), "Name": filepath.Base(p), "Size": st.Size(), "IsDir": false}}
		out, err := json.Marshal(listing)
		return out, err
	}
	return []byte("unexpected rclone call"), errors.New("unexpected rclone call")
}

// local is the fake remote's file for a remote path.
func (f *fakeRclone) local(remote string) string {
	rel := strings.TrimPrefix(remote, fakeRemote+"/")
	return filepath.Join(f.dir, filepath.FromSlash(rel))
}

// useFakeRclone makes runRclone the fake for the rest of the test.
func useFakeRclone(t *testing.T) *fakeRclone {
	t.Helper()
	f := &fakeRclone{dir: t.TempDir()}
	old := runRclone
	runRclone = f.run
	t.Cleanup(func() { runRclone = old })
	return f
}

// captureLog sends the standard logger to a buffer for the rest of the test.
func captureLog(t *testing.T) *bytes.Buffer {
	t.Helper()
	var buf bytes.Buffer
	log.SetOutput(&buf)
	t.Cleanup(func() { log.SetOutput(os.Stderr) })
	return &buf
}

// writeTenRecords makes a store with ten 1000-byte records, which make three chunks
// of 4 KiB, 4 KiB and 2 KiB, and closes it, so all three are sealed.
func writeTenRecords(t *testing.T, dataDir string) {
	t.Helper()
	store, err := OpenStreamStore(dataDir, "S1")
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 10; i++ {
		if _, err := store.Append(bytes.Repeat([]byte{byte(i + 1)}, 1000)); err != nil {
			t.Fatal(err)
		}
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
}

// runArchiver steps the archiver until it has nothing to do.
func runArchiver(t *testing.T, a *archiver) {
	t.Helper()
	ctx := context.Background()
	for i := 0; a.step(ctx); i++ {
		if i > 1000 {
			t.Fatal("archiver kept working")
		}
	}
}

// fileExists reports whether path is a file on disk.
func fileExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

// sha256Hex is the hex sha256 of data.
func sha256Hex(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

// checkDiskCount checks that the store's count of bytes on disk matches a walk of segments/.
func checkDiskCount(t *testing.T, store *StreamStore, segDir string) {
	t.Helper()
	walked, err := dirBytes(segDir)
	if err != nil {
		t.Fatal(err)
	}
	if got := store.Stats().DiskBytes; got != walked {
		t.Fatalf("store counts %d bytes on disk, a walk of segments/ finds %d", got, walked)
	}
}

// Sealed chunks compress, their .seg files go, and the store keeps reading them as
// archived; the active chunk is left alone.
func TestArchiveCompressesSealedChunksOnly(t *testing.T) {
	setStoreSizes(t, 4096, 2048, 1)
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	store, err := OpenStreamStore(dataDir, "S1")
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 10; i++ {
		if _, err := store.Append(bytes.Repeat([]byte{byte(i + 1)}, 1000)); err != nil {
			t.Fatal(err)
		}
	}
	arc, err := newArchiver(store, "")
	if err != nil {
		t.Fatal(err)
	}
	defer arc.close()
	arc.startup()
	runArchiver(t, arc)

	w := readRunManifest(segDir, "S1")
	if w == nil || len(w.chunks) != 3 {
		t.Fatalf("manifest %+v, want three chunks", w)
	}
	for i, want := range []string{chunkStateCompressed, chunkStateCompressed, chunkStateActive} {
		if got := w.chunks[i].state; got != want {
			t.Fatalf("chunk %d is %q, want %q", i+1, got, want)
		}
	}
	for _, c := range w.chunks[:2] {
		if fileExists(c.path) {
			t.Fatalf("%s is still there after its copy was recorded", c.path)
		}
		if !fileExists(filepath.Join(segDir, "S1", c.archive.fileZ)) || c.archive.bytesZ == 0 {
			t.Fatalf("chunk %d: no .segz file recorded (%q, %d bytes)", c.number, c.archive.fileZ, c.archive.bytesZ)
		}
		if c.archive.sha256 == "" || c.archive.compressedAt == "" || c.archive.uploadedAt != "" {
			t.Fatalf("chunk %d manifest fields: sha256 %q compressed_at %q uploaded_at %q", c.number, c.archive.sha256, c.archive.compressedAt, c.archive.uploadedAt)
		}
	}
	if !fileExists(w.chunks[2].path) {
		t.Fatal("the active chunk's .seg file is gone")
	}

	// A compressed message is archived; the active chunk still reads.
	if _, _, err := store.Read(1); err == nil || !strings.Contains(err.Error(), "archived") {
		t.Fatalf("read 1 after compression: %v, want an archived error", err)
	}
	msg, ok, err := store.Read(9)
	if err != nil || !ok || !bytes.Equal(msg.Payload, bytes.Repeat([]byte{9}, 1000)) {
		t.Fatalf("read 9 from the active chunk: ok=%v err=%v", ok, err)
	}
	checkDiskCount(t, store, segDir)

	m := archiveMetadata(arc)
	if !m.Enabled || m.Uploading || m.ChunksCompressed != 2 || m.ChunksSealed != 0 || m.ChunksUploaded != 0 ||
		m.BytesRaw != 8192 || m.BytesCompressed != w.chunks[0].archive.bytesZ+w.chunks[1].archive.bytesZ || m.PendingUploads != 0 {
		t.Fatalf("metadata %+v", m)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}

	// A restart reads the manifest and still reads the archived chunks.
	reopened, err := OpenStreamStore(dataDir, "S2")
	if err != nil {
		t.Fatal(err)
	}
	defer reopened.Close()
	if _, _, err := reopened.Read(1); err == nil || !strings.Contains(err.Error(), "archived") {
		t.Fatalf("read 1 after restart: %v, want an archived error", err)
	}
	checkDiskCount(t, reopened, segDir)
}

// A compressed copy that fails its check keeps the .seg file, leaves the manifest
// as it was, is counted, and is not tried again until the next start.
func TestArchiveVerifyFailureKeepsSource(t *testing.T) {
	setStoreSizes(t, 4096, 2048, 1)
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	store, err := OpenStreamStore(dataDir, "S1")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	for i := 0; i < 5; i++ {
		if _, err := store.Append(bytes.Repeat([]byte{byte(i)}, 1000)); err != nil {
			t.Fatal(err)
		}
	}
	old := chunkzAfterWrite
	chunkzAfterWrite = func(tmp string) {
		b, err := os.ReadFile(tmp)
		if err != nil {
			t.Fatal(err)
		}
		b[100] ^= 0xFF
		if err := os.WriteFile(tmp, b, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	t.Cleanup(func() { chunkzAfterWrite = old })

	arc, err := newArchiver(store, "")
	if err != nil {
		t.Fatal(err)
	}
	defer arc.close()
	arc.startup()
	runArchiver(t, arc)

	w := readRunManifest(segDir, "S1")
	if got := w.chunks[0].state; got != chunkStateSealed {
		t.Fatalf("chunk 1 is %q after a failed check, want sealed", got)
	}
	zpath := filepath.Join(segDir, "S1", chunkzFileName(1))
	if fileExists(zpath) || fileExists(zpath+".tmp") {
		t.Fatalf("%s or its temporary file is on disk after a failed check", zpath)
	}
	if !fileExists(w.chunks[0].path) {
		t.Fatalf("%s was removed after a failed check", w.chunks[0].path)
	}
	if got := archiveMetadata(arc).VerifyFailures; got != 1 {
		t.Fatalf("verify failures %d, want 1", got)
	}
	if arc.step(context.Background()) {
		t.Fatal("the archiver tried the failed chunk again in the same run")
	}
	checkDiskCount(t, store, segDir)
}

// A start after a crash finishes what the crash left: a .segz file that was renamed
// but not recorded is checked and recorded, and a leftover temporary file goes.
func TestArchiveStartupFinishesInterruptedCompression(t *testing.T) {
	setStoreSizes(t, 4096, 2048, 1)
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	writeTenRecords(t, dataDir)
	run := filepath.Join(segDir, "S1")

	raw1, err := os.ReadFile(filepath.Join(run, chunkFileName(1)))
	if err != nil {
		t.Fatal(err)
	}
	if _, err := compressChunk(filepath.Join(run, chunkFileName(1)), filepath.Join(run, chunkzFileName(1))); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(run, chunkzFileName(2)+".tmp"), []byte("half written"), 0o644); err != nil {
		t.Fatal(err)
	}

	store, err := OpenStreamStore(dataDir, "S2")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	arc, err := newArchiver(store, "")
	if err != nil {
		t.Fatal(err)
	}
	defer arc.close()
	arc.startup()
	runArchiver(t, arc)

	if fileExists(filepath.Join(run, chunkzFileName(2)+".tmp")) {
		t.Fatal("the leftover temporary file is still there")
	}
	w := readRunManifest(segDir, "S1")
	for _, c := range w.chunks[:2] {
		if c.state != chunkStateCompressed {
			t.Fatalf("chunk %d is %q, want compressed", c.number, c.state)
		}
		if fileExists(c.path) {
			t.Fatalf("%s is still there", c.path)
		}
	}
	if w.chunks[0].archive.sha256 != sha256Hex(raw1) {
		t.Fatalf("chunk 1 sha256 %s, want %s", w.chunks[0].archive.sha256, sha256Hex(raw1))
	}
	if got := w.chunks[0].archive.bytesZ; got != fileLen(t, filepath.Join(run, chunkzFileName(1))) {
		t.Fatalf("chunk 1 bytes_z %d, want the file's size", got)
	}
	checkDiskCount(t, store, segDir)
}

// A compressed chunk uploads to the remote with rclone, its manifest goes with it,
// and the manifest records the upload.
func TestArchiveUploadsWithFakeRclone(t *testing.T) {
	setStoreSizes(t, 4096, 2048, 1)
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	writeTenRecords(t, dataDir)
	store, err := OpenStreamStore(dataDir, "S2")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	f := useFakeRclone(t)
	arc, err := newArchiver(store, fakeRemote)
	if err != nil {
		t.Fatal(err)
	}
	defer arc.close()
	arc.startup()
	runArchiver(t, arc)

	w := readRunManifest(segDir, "S1")
	for _, c := range w.chunks {
		if c.state != chunkStateUploaded || c.archive.uploadedAt == "" {
			t.Fatalf("chunk %d is %q, uploaded_at %q", c.number, c.state, c.archive.uploadedAt)
		}
		local, err := os.ReadFile(filepath.Join(segDir, "S1", c.archive.fileZ))
		if err != nil {
			t.Fatal(err)
		}
		remote, err := os.ReadFile(filepath.Join(f.dir, "S1", c.archive.fileZ))
		if err != nil || !bytes.Equal(local, remote) {
			t.Fatalf("remote copy of %s differs from the local one: %v", c.archive.fileZ, err)
		}
	}
	localManifest, err := os.ReadFile(filepath.Join(segDir, "S1", manifestName))
	if err != nil {
		t.Fatal(err)
	}
	remoteManifest, err := os.ReadFile(filepath.Join(f.dir, "S1", manifestName))
	if err != nil || !bytes.Equal(localManifest, remoteManifest) {
		t.Fatalf("remote manifest differs from the local one: %v", err)
	}
	for _, call := range f.calls {
		if call[0] == "copyto" && !slices.Equal(call[len(call)-4:], []string{"--retries", "1", "--low-level-retries", "3"}) {
			t.Fatalf("copyto without the retry flags: %v", call)
		}
	}
	if got := archiveMetadata(arc); got.ChunksUploaded != 3 || got.PendingUploads != 0 || !got.Uploading {
		t.Fatalf("metadata %+v, want three uploaded and nothing pending", got)
	}
	checkDiskCount(t, store, segDir)
}

// A failing upload keeps its file, waits before trying again, doubles the wait up to
// 30 minutes, and logs its first failure only. A success after that clears the wait.
func TestArchiveUploadBacksOffAndLogsOnce(t *testing.T) {
	setStoreSizes(t, 4096, 2048, 1)
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	writeTenRecords(t, dataDir)
	store, err := OpenStreamStore(dataDir, "S2")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	f := useFakeRclone(t)
	f.fail = "ERROR : Failed to copy: quota exceeded"
	logs := captureLog(t)
	arc, err := newArchiver(store, fakeRemote)
	if err != nil {
		t.Fatal(err)
	}
	defer arc.close()
	now := time.Date(2026, 11, 23, 12, 0, 0, 0, time.UTC)
	arc.now = func() time.Time { return now }
	arc.startup()
	runArchiver(t, arc)

	if len(f.calls) != 1 {
		t.Fatalf("%d rclone calls before the first wait, want 1", len(f.calls))
	}
	w := readRunManifest(segDir, "S1")
	if got := w.chunks[0].state; got != chunkStateCompressed {
		t.Fatalf("chunk 1 is %q after a failed upload, want compressed", got)
	}
	if !fileExists(w.chunks[0].archive.zpath) {
		t.Fatal("the .segz file went away after a failed upload")
	}
	if got := strings.Count(logs.String(), "upload failed"); got != 1 {
		t.Fatalf("%d upload failure lines, want 1:\n%s", got, logs.String())
	}
	if !strings.Contains(logs.String(), "rclone: ERROR : Failed to copy: quota exceeded") || strings.Contains(logs.String(), "second line") {
		t.Fatalf("the log line is not the first line of rclone's stderr:\n%s", logs.String())
	}

	now = now.Add(59 * time.Second)
	runArchiver(t, arc)
	if len(f.calls) != 1 {
		t.Fatalf("rclone was called during the wait: %d calls", len(f.calls))
	}

	for _, want := range []time.Duration{2 * time.Minute, 4 * time.Minute, 8 * time.Minute, 16 * time.Minute, 30 * time.Minute, 30 * time.Minute} {
		before := len(f.calls)
		now = arc.retryAt
		arc.step(context.Background())
		if len(f.calls) != before+1 {
			t.Fatalf("no upload attempt when the wait ended (%d calls)", len(f.calls))
		}
		if arc.backoff != want {
			t.Fatalf("wait after a failure is %s, want %s", arc.backoff, want)
		}
	}
	if got := strings.Count(logs.String(), "upload failed"); got != 1 {
		t.Fatalf("%d upload failure lines after %d failures, want 1", got, len(f.calls))
	}
	m := archiveMetadata(arc)
	if !strings.Contains(m.LastUploadError, "quota exceeded") || m.LastUploadErrorMS != now.UnixMilli() {
		t.Fatalf("metadata error %q at %d, want the quota error at %d", m.LastUploadError, m.LastUploadErrorMS, now.UnixMilli())
	}

	f.fail = ""
	now = arc.retryAt
	runArchiver(t, arc)
	if got := archiveMetadata(arc).ChunksUploaded; got != 3 {
		t.Fatalf("%d chunks uploaded after the remote recovered, want 3", got)
	}
	if !strings.Contains(logs.String(), "uploads work again") {
		t.Fatalf("no recovery line:\n%s", logs.String())
	}
}

// Compressed chunks from an earlier start upload when the next start has a remote.
func TestArchiveRestartResumesUploads(t *testing.T) {
	setStoreSizes(t, 4096, 2048, 1)
	dataDir := t.TempDir()
	segDir := filepath.Join(dataDir, "segments")
	writeTenRecords(t, dataDir)

	first, err := OpenStreamStore(dataDir, "S2")
	if err != nil {
		t.Fatal(err)
	}
	f := useFakeRclone(t)
	compressOnly, err := newArchiver(first, "")
	if err != nil {
		t.Fatal(err)
	}
	compressOnly.startup()
	runArchiver(t, compressOnly)
	compressOnly.close()
	if len(f.calls) != 0 {
		t.Fatalf("compress-only archiver called rclone %d times", len(f.calls))
	}
	if err := first.Close(); err != nil {
		t.Fatal(err)
	}

	second, err := OpenStreamStore(dataDir, "S3")
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()
	uploading, err := newArchiver(second, fakeRemote)
	if err != nil {
		t.Fatal(err)
	}
	defer uploading.close()
	uploading.startup()
	runArchiver(t, uploading)
	for _, name := range []string{"000001.segz", "000002.segz", "000003.segz", manifestName} {
		if !fileExists(filepath.Join(f.dir, "S1", name)) {
			t.Fatalf("%s did not reach the remote after the restart", name)
		}
	}
	if w := readRunManifest(segDir, "S1"); w.chunks[2].state != chunkStateUploaded {
		t.Fatalf("chunk 3 is %q after the restart, want uploaded", w.chunks[2].state)
	}
}

// With no remote set, the archiver compresses and never calls rclone.
func TestArchiveWithoutRemoteMakesNoRcloneCalls(t *testing.T) {
	setStoreSizes(t, 4096, 2048, 1)
	dataDir := t.TempDir()
	writeTenRecords(t, dataDir)
	store, err := OpenStreamStore(dataDir, "S2")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	f := useFakeRclone(t)
	arc, err := newArchiver(store, "")
	if err != nil {
		t.Fatal(err)
	}
	defer arc.close()
	arc.startup()
	runArchiver(t, arc)
	if len(f.calls) != 0 {
		t.Fatalf("rclone was called %d times with no remote set", len(f.calls))
	}
	if m := archiveMetadata(arc); m.ChunksCompressed != 3 || m.Uploading || m.PendingUploads != 0 {
		t.Fatalf("metadata %+v", m)
	}
}

// The archiver started by the server compresses in the background and stops when
// asked, leaving the chunks it finished in place.
func TestArchiveStartAndStop(t *testing.T) {
	setStoreSizes(t, 4096, 2048, 1)
	dataDir := t.TempDir()
	writeTenRecords(t, dataDir)
	store, err := OpenStreamStore(dataDir, "S2")
	if err != nil {
		t.Fatal(err)
	}
	defer store.Close()
	cfg, err := ParseConfigWithEnv(noEnvironment, []string{"-data-dir", dataDir}, &bytes.Buffer{})
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	arc, err := startArchiver(ctx, cfg, store)
	if err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(10 * time.Second)
	for store.archiveCounts().compressed < 2 {
		if time.Now().After(deadline) {
			t.Fatal("the background archiver did not compress the sealed chunks")
		}
		time.Sleep(10 * time.Millisecond)
	}
	stopped := time.Now()
	arc.Stop()
	if d := time.Since(stopped); d > 2*time.Second {
		t.Fatalf("Stop took %s with nothing to finish", d)
	}
	var nilArchiver *archiver
	nilArchiver.Stop()
}

// Replaying a run with compressed chunks writes the same rows as the raw chunks.
func TestArchiveBackfillRowsSameAfterCompression(t *testing.T) {
	dbc := writeFrameTestDBC(t)
	setStoreSizes(t, 300, 100, 1<<20)
	dataDir, segDir := backfillDirs(t)
	var bufs [][]byte
	for i := 0; i < 8; i++ {
		bufs = append(bufs, frameBatch(testTSMS+uint64(i)*1000,
			testFrame{id: 100, data: plainData},
			testFrame{id: 101, data: muxData(byte(i%3 + 1))},
			testFrame{id: 100, data: plainData}))
	}
	storeBuffers(t, dataDir, "S1", bufs...)
	before, beforeResults := backfillRows(t, segDir, dbc)

	store, err := OpenStreamStore(dataDir, "S2")
	if err != nil {
		t.Fatal(err)
	}
	arc, err := newArchiver(store, "")
	if err != nil {
		t.Fatal(err)
	}
	arc.startup()
	runArchiver(t, arc)
	arc.close()
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	if w := readRunManifest(segDir, "S1"); w.chunks[0].state != chunkStateCompressed {
		t.Fatalf("chunk 1 is %q, want compressed", w.chunks[0].state)
	}

	after, afterResults := backfillRows(t, segDir, dbc)
	if beforeResults != afterResults || beforeResults != 8 {
		t.Fatalf("records replayed before %d, after %d, want 8 both times", beforeResults, afterResults)
	}
	for _, prefix := range []string{"frame,", "signal_def,"} {
		want := rowsWithPrefix(before, prefix)
		got := rowsWithPrefix(after, prefix)
		slices.Sort(want)
		slices.Sort(got)
		if len(want) == 0 || !slices.Equal(got, want) {
			t.Fatalf("%s rows differ after compression\nraw:        %q\ncompressed: %q", prefix, want, got)
		}
	}
	last := func(lines []string) string {
		rows := rowsWithPrefix(lines, "session,")
		return revField.ReplaceAllString(rows[len(rows)-1], "")
	}
	if last(after) != last(before) {
		t.Fatalf("final session row %q, want %q", last(after), last(before))
	}
}

// backfillRows replays every run in segDir into a recorder and returns its lines and
// the number of records replayed.
func backfillRows(t *testing.T, segDir, dbcDir string) ([]string, int64) {
	t.Helper()
	sink := &recordSink{}
	results, err := runBackfill(backfillOptions{SegmentsDir: segDir, CAN: CANConfig{DBCDir: dbcDir}, All: true}, sink)
	if err != nil {
		t.Fatal(err)
	}
	var records int64
	for _, r := range results {
		records += r.Records
	}
	return sink.lines(t), records
}

// The archive settings parse from flags and the environment, with the defaults.
func TestArchiveSettingsParse(t *testing.T) {
	noEnv := func(string) string { return "" }
	cfg, err := ParseConfigWithEnv(noEnv, []string{}, &bytes.Buffer{})
	if err != nil {
		t.Fatal(err)
	}
	if !cfg.Archive || cfg.ArchiveRemote != "" || cfg.Rclone != "rclone" {
		t.Fatalf("defaults: archive %v remote %q rclone %q", cfg.Archive, cfg.ArchiveRemote, cfg.Rclone)
	}

	cfg, err = ParseConfigWithEnv(noEnv, []string{"-archive=false", "-archive-remote", "sharepoint:photon/segments", "-rclone", "/opt/rclone"}, &bytes.Buffer{})
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Archive || cfg.ArchiveRemote != "sharepoint:photon/segments" || cfg.Rclone != "/opt/rclone" {
		t.Fatalf("flags: archive %v remote %q rclone %q", cfg.Archive, cfg.ArchiveRemote, cfg.Rclone)
	}

	env := map[string]string{"PHOTON_ARCHIVE": "0", "PHOTON_ARCHIVE_REMOTE": "sharepoint:photon/from-env", "PHOTON_RCLONE": "rclone.exe"}
	fromEnv := func(k string) string { return env[k] }
	cfg, err = ParseConfigWithEnv(fromEnv, []string{}, &bytes.Buffer{})
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Archive || cfg.ArchiveRemote != "sharepoint:photon/from-env" || cfg.Rclone != "rclone.exe" {
		t.Fatalf("env: archive %v remote %q rclone %q", cfg.Archive, cfg.ArchiveRemote, cfg.Rclone)
	}
	cfg, err = ParseConfigWithEnv(fromEnv, []string{"-archive=true"}, &bytes.Buffer{})
	if err != nil || !cfg.Archive {
		t.Fatalf("flag over env: archive %v, err %v", cfg.Archive, err)
	}

	bad := map[string]string{"PHOTON_ARCHIVE": "maybe"}
	if _, err := ParseConfigWithEnv(func(k string) string { return bad[k] }, []string{}, &bytes.Buffer{}); err == nil {
		t.Fatal("PHOTON_ARCHIVE=maybe was accepted")
	}
}

// remoteChunkPath keeps the remote's own spelling and joins the run and file with slashes.
func TestArchiveRemotePathsAndErrors(t *testing.T) {
	if got := remoteChunkPath("sharepoint:photon/segments/", "20261123T000000Z", "000001.segz"); got != "sharepoint:photon/segments/20261123T000000Z/000001.segz" {
		t.Fatalf("remote path %q", got)
	}
	if got := firstLine([]byte("  first line  \nsecond")); got != "first line" {
		t.Fatalf("first line %q", got)
	}
	if got := rcloneError(nil, errors.New("exit status 3")).Error(); got != "rclone: exit status 3" {
		t.Fatalf("error without stderr %q", got)
	}
}
