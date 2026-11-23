package main

// The archiver runs in the background. It compresses each sealed chunk to a
// .segz file (chunkz.go), checks the copy, records it in the run's manifest and
// then removes the .seg file. With an archive remote set it also uploads the
// .segz files with rclone.
//
// Data is deleted only in two cases: a .seg file whose .segz copy passed the
// check, and a leftover .segz.tmp file.

import (
	"bytes"
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	// archivePoll is how long the archiver waits when it has nothing to do.
	archivePoll = time.Second
	// The wait after a failed upload starts at uploadBackoffMin and doubles up to
	// uploadBackoffMax.
	uploadBackoffMin = time.Minute
	uploadBackoffMax = 30 * time.Minute
	// archiveStopWait is how long shutdown waits for the archiver to finish its file.
	archiveStopWait = 5 * time.Second
)

// rcloneBinary is the rclone program. main sets it from -rclone before the
// archiver starts.
var rcloneBinary = "rclone"

// runRclone runs rclone with args and returns its stdout. On failure it returns
// stderr and the error. Tests replace it, so they never reach a real remote.
var runRclone = func(ctx context.Context, args ...string) ([]byte, error) {
	cmd := exec.CommandContext(ctx, rcloneBinary, args...)
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		return stderr.Bytes(), err
	}
	return stdout.Bytes(), nil
}

// archiveCurrent is the running archiver, or nil when archiving is off. The
// observer reads it.
var archiveCurrent atomic.Pointer[archiver]

// ArchiveMetadata is what the archiver reports to the observer.
type ArchiveMetadata struct {
	Enabled           bool   `json:"enabled"`
	Uploading         bool   `json:"uploading"`         // an archive remote is set
	ChunksSealed      int    `json:"chunks_sealed"`     // sealed, waiting to be compressed
	ChunksCompressed  int    `json:"chunks_compressed"` // compressed, not uploaded
	ChunksUploaded    int    `json:"chunks_uploaded"`   // compressed and uploaded
	PendingUploads    int    `json:"pending_uploads"`   // compressed, waiting for the remote
	BytesRaw          int64  `json:"bytes_raw"`         // .seg bytes of the compressed and uploaded chunks
	BytesCompressed   int64  `json:"bytes_compressed"`  // .segz bytes of the same chunks
	VerifyFailures    uint64 `json:"verify_failures"`   // compressed copies that failed the check
	LastUploadError   string `json:"last_upload_error"` // the last failed upload, empty if none
	LastUploadErrorMS int64  `json:"last_upload_error_ms"`
}

// archiveMetadata reports the archiver's state. a is nil when archiving is off.
func archiveMetadata(a *archiver) ArchiveMetadata {
	if a == nil {
		return ArchiveMetadata{}
	}
	n := a.store.archiveCounts()
	m := ArchiveMetadata{
		Enabled:          true,
		Uploading:        a.remote != "",
		ChunksSealed:     n.sealed,
		ChunksCompressed: n.compressed,
		ChunksUploaded:   n.uploaded,
		BytesRaw:         n.bytesRaw,
		BytesCompressed:  n.bytesZ,
	}
	if a.remote != "" {
		m.PendingUploads = n.compressed
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	m.VerifyFailures = a.verifyFailures
	m.LastUploadError = a.lastUploadErr
	if !a.lastUploadAt.IsZero() {
		m.LastUploadErrorMS = a.lastUploadAt.UnixMilli()
	}
	return m
}

// archiver compresses sealed chunks and uploads compressed ones. Its goroutine
// alone changes the fields that are not under mu.
type archiver struct {
	store  *StreamStore
	remote string // rclone destination; empty means compress only
	now    func() time.Time

	worker  *chunkzWorker
	skipped map[string]bool  // .seg paths given up on until restart
	removal map[*chunk]int64 // .seg files still to remove, with their size
	stale   map[string]bool  // runs whose manifest did not reach the remote
	retryAt time.Time        // no upload before this time
	backoff time.Duration
	failing bool // the current upload failure streak has been logged

	mu             sync.Mutex // guards the fields below, read by the observer
	verifyFailures uint64
	lastUploadErr  string
	lastUploadAt   time.Time

	cancel context.CancelFunc
	done   chan struct{}
}

// newArchiver makes an archiver for the store. It does not start it.
func newArchiver(store *StreamStore, remote string) (*archiver, error) {
	w, err := newChunkzWorker()
	if err != nil {
		return nil, err
	}
	return &archiver{
		store:   store,
		remote:  remote,
		now:     time.Now,
		worker:  w,
		skipped: map[string]bool{},
		removal: map[*chunk]int64{},
		stale:   map[string]bool{},
	}, nil
}

// startArchiver starts the archiver for the store, until ctx is done. Stop waits
// for it.
func startArchiver(ctx context.Context, cfg *Config, store *StreamStore) (*archiver, error) {
	a, err := newArchiver(store, cfg.ArchiveRemote)
	if err != nil {
		return nil, err
	}
	rcloneBinary = cfg.Rclone
	ctx, a.cancel = context.WithCancel(ctx)
	a.done = make(chan struct{})
	go a.run(ctx)
	return a, nil
}

// Stop asks the archiver to stop and waits for it to finish the file it is on. A
// compression that is cut short removes its temporary file; the next start
// compresses that chunk again. Stop does nothing on a nil archiver.
func (a *archiver) Stop() {
	if a == nil {
		return
	}
	a.cancel()
	t := time.NewTimer(archiveStopWait)
	defer t.Stop()
	select {
	case <-a.done:
	case <-t.C:
		log.Printf("[photon-server] archive: still busy after %s; exiting without waiting for it", archiveStopWait)
	}
}

func (a *archiver) run(ctx context.Context) {
	defer close(a.done)
	defer a.close()
	a.startup()
	for ctx.Err() == nil {
		if a.step(ctx) {
			continue
		}
		t := time.NewTimer(archivePoll)
		select {
		case <-ctx.Done():
			t.Stop()
		case <-t.C:
		}
	}
}

// close frees the encoder and decoder. The archiver calls it when it stops.
func (a *archiver) close() {
	a.worker.close()
}

// startup removes the .segz.tmp files that a crash left, and removes the .seg
// files of compressed chunks that still have one. The verification of a .segz
// file that is already there happens when its chunk comes up in step.
func (a *archiver) startup() {
	names, err := listRunFolders(a.store.segmentsDir)
	if err != nil {
		log.Printf("[photon-server] archive: cannot list %s: %v", a.store.segmentsDir, err)
		return
	}
	for _, name := range names {
		dir := filepath.Join(a.store.segmentsDir, name)
		entries, err := os.ReadDir(dir)
		if err != nil {
			continue
		}
		for _, e := range entries {
			if e.IsDir() || !strings.HasSuffix(e.Name(), chunkzTmpExt) {
				continue
			}
			p := filepath.Join(dir, e.Name())
			var size int64
			if st, err := os.Stat(p); err == nil {
				size = st.Size()
			}
			if err := os.Remove(p); err != nil {
				log.Printf("[photon-server] archive: could not remove %s: %v", p, err)
				continue
			}
			a.store.adjustDisk(-size)
			log.Printf("[photon-server] archive: removed %s, left by an earlier run", p)
		}
	}

	var compressed []*chunk
	a.store.forEachChunk(func(c *chunk) {
		if c.compressed() {
			compressed = append(compressed, c)
		}
	})
	for _, c := range compressed {
		if st, err := os.Stat(c.path); err == nil {
			a.removal[c] = st.Size()
		}
	}
	a.retryRemovals()
}

// step does one piece of work: it retries removals, compresses the oldest sealed
// chunk, or uploads the oldest compressed one. It reports whether it did any.
func (a *archiver) step(ctx context.Context) bool {
	if a.retryRemovals() {
		return true
	}
	if c := a.store.findChunk(func(c *chunk) bool {
		return c.state == chunkStateSealed && !c.missing && !a.skipped[c.path]
	}); c != nil {
		a.compressOne(ctx, c)
		return true
	}
	if a.remote != "" {
		return a.uploadOne(ctx)
	}
	return false
}

// compressOne compresses a sealed chunk and records the copy. A chunk that fails
// is kept as it is, with its .seg file, until the next start.
func (a *archiver) compressOne(ctx context.Context, c *chunk) {
	zfile := chunkzFileName(c.number)
	zpath := filepath.Join(c.owner.dir, zfile)
	var (
		info    chunkzInfo
		created bool // the .segz file is new on disk, so its size is not counted yet
		err     error
	)
	if _, serr := os.Stat(zpath); serr == nil {
		// A copy from an earlier attempt is on disk: check it again, do not write it again.
		info, err = a.worker.verifyExisting(c.path, zpath)
	} else {
		info, err = a.worker.compress(ctx, c.path, zpath)
		created = err == nil
	}
	if err != nil {
		if ctx.Err() != nil {
			return // stopping: the temporary file is gone; the next start tries again
		}
		a.skipped[c.path] = true
		if errors.Is(err, errChunkzVerify) {
			a.mu.Lock()
			a.verifyFailures++
			a.mu.Unlock()
		}
		log.Printf("[photon-server] archive: %s stays uncompressed until restart: %v", c.path, err)
		return
	}
	if err := a.store.commitCompressed(c, zfile, info, created, a.now()); err != nil {
		a.skipped[c.path] = true
		log.Printf("[photon-server] archive: could not record the copy of %s; it stays uncompressed until restart: %v", c.path, err)
		return
	}
	log.Printf("[photon-server] archive: %s compressed %d -> %d bytes (%.2fx)",
		c.file, info.Size, info.ZBytes, float64(info.Size)/float64(info.ZBytes))
	a.removeSource(c, info.Size)
}

// removeSource removes the .seg file of a chunk whose copy is recorded. A file a
// reader still has open cannot be removed yet, so it is retried later.
func (a *archiver) removeSource(c *chunk, size int64) {
	err := os.Remove(c.path)
	if err == nil || errors.Is(err, fs.ErrNotExist) {
		a.store.adjustDisk(-size)
		return
	}
	log.Printf("[photon-server] archive: could not remove %s, which has a compressed copy; trying again: %v", c.path, err)
	a.removal[c] = size
}

// retryRemovals removes the .seg files that could not be removed before. It
// reports whether it removed any.
func (a *archiver) retryRemovals() bool {
	removed := false
	for c, size := range a.removal {
		err := os.Remove(c.path)
		if err != nil && !errors.Is(err, fs.ErrNotExist) {
			continue
		}
		delete(a.removal, c)
		a.store.adjustDisk(-size)
		removed = true
	}
	return removed
}

// uploadOne uploads the oldest compressed chunk, or copies a run manifest that
// did not reach the remote before. It reports whether it tried anything.
func (a *archiver) uploadOne(ctx context.Context) bool {
	if ctx.Err() != nil || a.now().Before(a.retryAt) {
		return false
	}
	for run := range a.stale {
		if err := a.copyManifest(ctx, run); err != nil {
			a.uploadFailed(err)
			return true
		}
		delete(a.stale, run)
	}
	c := a.store.findChunk(func(c *chunk) bool {
		return c.state == chunkStateCompressed && !c.missing
	})
	if c == nil {
		return false
	}
	if err := a.uploadChunk(ctx, c); err != nil {
		a.uploadFailed(err)
		return true
	}
	if err := a.store.commitUploaded(c, a.now()); err != nil {
		a.uploadFailed(fmt.Errorf("could not record the upload of %s: %w", c.file, err))
		return true
	}
	if err := a.copyManifest(ctx, c.owner.id); err != nil {
		a.stale[c.owner.id] = true
		a.uploadFailed(err)
		return true
	}
	a.uploadOK()
	return true
}

// uploadChunk copies the compressed file of c to the remote, then checks that the
// remote file has the same size.
func (a *archiver) uploadChunk(ctx context.Context, c *chunk) error {
	dst := remoteChunkPath(a.remote, c.owner.id, c.archive.fileZ)
	if out, err := runRclone(ctx, "copyto", c.archive.zpath, dst, "--retries", "1", "--low-level-retries", "3"); err != nil {
		return rcloneError(out, err)
	}
	out, err := runRclone(ctx, "lsjson", dst)
	if err != nil {
		return rcloneError(out, err)
	}
	var listing []struct {
		Name  string
		Size  int64
		IsDir bool
	}
	if err := json.Unmarshal(out, &listing); err != nil {
		return fmt.Errorf("rclone lsjson %s: %w", dst, err)
	}
	for _, e := range listing {
		if e.Name == c.archive.fileZ && !e.IsDir && e.Size == c.archive.bytesZ {
			return nil
		}
	}
	return fmt.Errorf("%s on the remote does not have the %d bytes it should", dst, c.archive.bytesZ)
}

// copyManifest copies the manifest of a run to the remote.
func (a *archiver) copyManifest(ctx context.Context, run string) error {
	src := filepath.Join(a.store.segmentsDir, run, manifestName)
	dst := remoteChunkPath(a.remote, run, manifestName)
	if out, err := runRclone(ctx, "copyto", src, dst, "--retries", "1", "--low-level-retries", "3"); err != nil {
		return rcloneError(out, err)
	}
	return nil
}

// uploadFailed records a failed upload and sets the wait before the next try. A
// failure that starts a streak is logged once; the rest only update the observer.
func (a *archiver) uploadFailed(err error) {
	now := a.now()
	if a.backoff == 0 {
		a.backoff = uploadBackoffMin
	} else {
		a.backoff = min(2*a.backoff, uploadBackoffMax)
	}
	a.retryAt = now.Add(a.backoff)
	if !a.failing {
		a.failing = true
		log.Printf("[photon-server] archive: upload failed: %v; trying again in %s", err, a.backoff)
	}
	a.mu.Lock()
	a.lastUploadErr, a.lastUploadAt = err.Error(), now
	a.mu.Unlock()
}

// uploadOK clears the wait after a successful upload.
func (a *archiver) uploadOK() {
	if a.failing {
		a.failing = false
		log.Printf("[photon-server] archive: uploads work again")
	}
	a.backoff, a.retryAt = 0, time.Time{}
}

// rcloneError is the error for a failed rclone run: the first line of its stderr,
// or the error when there is none.
func rcloneError(out []byte, err error) error {
	if line := firstLine(out); line != "" {
		return fmt.Errorf("rclone: %s", line)
	}
	return fmt.Errorf("rclone: %w", err)
}

// firstLine returns the first line of b, without surrounding space.
func firstLine(b []byte) string {
	s := strings.TrimSpace(string(b))
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		s = strings.TrimSpace(s[:i])
	}
	return s
}

// remoteChunkPath is where a file of a run goes on the remote.
func remoteChunkPath(remote, run, file string) string {
	return strings.TrimRight(remote, "/") + "/" + run + "/" + file
}

// findChunk returns the oldest chunk for which match is true, or nil. match runs
// under the store's lock.
func (s *StreamStore) findChunk(match func(c *chunk) bool) *chunk {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, c := range s.chunks {
		if match(c) {
			return c
		}
	}
	return nil
}

// forEachChunk calls fn for every chunk, under the store's lock.
func (s *StreamStore) forEachChunk(fn func(c *chunk)) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, c := range s.chunks {
		fn(c)
	}
}

// adjustDisk changes the bytes counted under segments/ by delta.
func (s *StreamStore) adjustDisk(delta int64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.diskBytes += delta
}

// archivedSince reports whether chunk c has been compressed since a read of it
// began. Its .seg file may be gone, so the read is tried again and then reports
// the message as archived.
func (s *StreamStore) archivedSince(c *chunk) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return !c.plain()
}

// commitCompressed records the compressed copy of chunk c in its run's manifest.
// zAdded says the .segz file is new on disk, so its size is counted now. If the
// manifest cannot be written, the chunk is left as it was.
func (s *StreamStore) commitCompressed(c *chunk, zfile string, info chunkzInfo, zAdded bool, at time.Time) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if zAdded {
		s.diskBytes += info.ZBytes
	}
	prevState, prevArchive := c.state, c.archive
	c.state = chunkStateCompressed
	c.archive = chunkArchive{
		fileZ:        zfile,
		zpath:        filepath.Join(c.owner.dir, zfile),
		bytesZ:       info.ZBytes,
		sha256:       hex.EncodeToString(info.Sha256[:]),
		compressedAt: at.UTC().Format(time.RFC3339),
	}
	delta, err := c.owner.writeManifest()
	if err != nil {
		c.state, c.archive = prevState, prevArchive
		return err
	}
	s.diskBytes += delta
	return nil
}

// commitUploaded records in the manifest that the copy of chunk c is on the
// remote. If the manifest cannot be written, the chunk is left as it was.
func (s *StreamStore) commitUploaded(c *chunk, at time.Time) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	prevState, prevUploaded := c.state, c.archive.uploadedAt
	c.state = chunkStateUploaded
	c.archive.uploadedAt = at.UTC().Format(time.RFC3339)
	delta, err := c.owner.writeManifest()
	if err != nil {
		c.state, c.archive.uploadedAt = prevState, prevUploaded
		return err
	}
	s.diskBytes += delta
	return nil
}

// archiveCounts is how many chunks are in each archive state, and their sizes.
type archiveCounts struct {
	sealed, compressed, uploaded int
	bytesRaw, bytesZ             int64
}

// archiveCounts counts the chunks by archive state.
func (s *StreamStore) archiveCounts() archiveCounts {
	s.mu.Lock()
	defer s.mu.Unlock()
	var n archiveCounts
	for _, c := range s.chunks {
		switch c.state {
		case chunkStateSealed:
			n.sealed++
		case chunkStateCompressed:
			n.compressed++
			n.bytesRaw += c.bytes
			n.bytesZ += c.archive.bytesZ
		case chunkStateUploaded:
			n.uploaded++
			n.bytesRaw += c.bytes
			n.bytesZ += c.archive.bytesZ
		}
	}
	return n
}
