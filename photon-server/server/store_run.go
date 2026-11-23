package main

import (
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"log"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

// On-disk layout. Each run is a folder under segments/, named by its session
// ID. A run's records are in chunk files (000001.seg, 000002.seg, ...), each
// at most segmentSize bytes, and manifest.json lists them:
//
//	segments/<run>/000001.seg
//	segments/<run>/manifest.json
//
// The manifest is rewritten when a chunk is created and when it is sealed,
// never per record. A record is a 24-byte header and its payload, as the
// store has always written them.

const (
	chunkStateActive = "active"
	chunkStateSealed = "sealed"
	manifestName     = "manifest.json"
	// legacyFolder holds the old single-file layout after it is migrated.
	legacyFolder = "legacy"
	// manifestTries is how often a manifest rename is tried. Windows will not
	// replace a file that another process has open, such as a backfill reading it.
	manifestTries = 20
)

// manifestFile is the top level of manifest.json. Fields this version does not
// know are kept when the file is rewritten.
type manifestFile struct {
	Run    string            `json:"run"`
	Chunks []json.RawMessage `json:"chunks"`
}

// manifestChunk is one entry of a manifest's chunk list.
type manifestChunk struct {
	File       string    `json:"file"`
	FirstID    MessageID `json:"first_id"`
	LastID     MessageID `json:"last_id"`
	FirstWrite int64     `json:"first_write"`
	LastWrite  int64     `json:"last_write"`
	Records    int       `json:"records"`
	Bytes      int64     `json:"bytes"`
	State      string    `json:"state"`
}

var (
	manifestFileKeys  = []string{"run", "chunks"}
	manifestChunkKeys = []string{"file", "first_id", "last_id", "first_write", "last_write", "records", "bytes", "state"}
)

// Sentinels that stop a scan early. errFound and errPassed are internal to a
// read; errStopScan ends a recovery scan at the first record out of order.
var (
	errStopScan = errors.New("stop scan")
	errFound    = errors.New("found")
	errPassed   = errors.New("passed the message")
)

// checkpoint says the record with this ID starts at offset in its chunk.
type checkpoint struct {
	id     MessageID
	offset int64
}

// chunk is one chunk file of a run and the records in it.
type chunk struct {
	number     int
	file       string // "000001.seg"
	path       string
	state      string                     // "active", "sealed", or a state from a later version
	extra      map[string]json.RawMessage // manifest fields this version does not know
	firstID    MessageID
	lastID     MessageID
	firstWrite int64 // unix seconds of the first record
	lastWrite  int64 // unix seconds of the last record
	records    int
	bytes      int64 // whole records only; anything after is not read
	missing    bool  // the file is not on disk
	listed     bool  // in the store's chunk list
	f          *os.File
	// checkpoints are sparse: about one every checkpointEvery bytes, the first at
	// offset 0. They are complete when cpReady is set. The active chunk keeps
	// them up to date as it grows. A sealed chunk from an earlier start gets them
	// on its first read.
	checkpoints []checkpoint
	cpReady     bool
}

// plain reports whether the chunk is an ordinary .seg file that can be read.
func (c *chunk) plain() bool {
	return c.state == chunkStateActive || c.state == chunkStateSealed
}

// checkpointFor returns the last checkpoint at or before message id.
func (c *chunk) checkpointFor(id MessageID) checkpoint {
	i := sort.Search(len(c.checkpoints), func(i int) bool { return c.checkpoints[i].id > id }) - 1
	if i < 0 {
		return checkpoint{id: c.firstID}
	}
	return c.checkpoints[i]
}

func (c *chunk) entry() manifestChunk {
	return manifestChunk{
		File:       c.file,
		FirstID:    c.firstID,
		LastID:     c.lastID,
		FirstWrite: c.firstWrite,
		LastWrite:  c.lastWrite,
		Records:    c.records,
		Bytes:      c.bytes,
		State:      c.state,
	}
}

// runWriter is one run folder: its chunks, and the manifest that lists them.
type runWriter struct {
	id           string
	dir          string
	chunks       []*chunk // by number
	active       *chunk   // the chunk open for appends, or nil
	extra        map[string]json.RawMessage
	manifestSize int64
}

// appendFrame writes one encoded record to the active chunk. It seals that chunk
// and starts another first when the record would pass segmentSize. It returns
// the chunk written to, and the bytes written to disk: the record, and any
// manifest rewrite (delta, which may be negative).
func (w *runWriter) appendFrame(frame []byte, id MessageID) (c *chunk, written, delta int64, err error) {
	size := int64(len(frame))
	if w.active != nil && w.active.bytes > 0 && w.active.bytes+size > segmentSize {
		d, err := w.sealActive()
		delta += d
		if err != nil {
			return nil, 0, delta, err
		}
	}
	if w.active == nil {
		d, err := w.newChunk(id)
		delta += d
		if err != nil {
			return nil, 0, delta, err
		}
	}

	c = w.active
	n, err := c.f.Write(frame)
	written = int64(n)
	if err == nil && n != len(frame) {
		err = io.ErrShortWrite
	}
	if err != nil {
		// Bytes after the last whole record are not part of any record, so the
		// chunk is sealed and later records go to a new one.
		d, serr := w.sealActive()
		delta += d
		if serr != nil {
			err = errors.Join(err, serr)
		}
		return nil, written, delta, err
	}

	writeUnix := int64(binary.BigEndian.Uint32(frame[16:20]))
	if checkpointDue(c.checkpoints, c.bytes) {
		c.checkpoints = append(c.checkpoints, checkpoint{id: id, offset: c.bytes})
	}
	if c.records == 0 {
		c.firstID, c.firstWrite = id, writeUnix
	}
	c.lastID, c.lastWrite = id, writeUnix
	c.records++
	c.bytes += size
	return c, written, delta, nil
}

// newChunk creates the next chunk of the run, whose first record is firstID,
// and writes the manifest that lists it.
func (w *runWriter) newChunk(firstID MessageID) (int64, error) {
	if err := os.MkdirAll(w.dir, 0o755); err != nil {
		return 0, err
	}
	number := 1
	for _, c := range w.chunks {
		if c.number >= number {
			number = c.number + 1
		}
	}
	// A file that exists but is not in the manifest is not ours to overwrite,
	// so the next number is tried.
	var f *os.File
	for {
		file, err := os.OpenFile(filepath.Join(w.dir, chunkFileName(number)), os.O_CREATE|os.O_EXCL|os.O_RDWR|os.O_APPEND, 0o644)
		if err == nil {
			f = file
			break
		}
		if !errors.Is(err, fs.ErrExist) {
			return 0, err
		}
		number++
	}
	c := &chunk{
		number:  number,
		file:    chunkFileName(number),
		path:    filepath.Join(w.dir, chunkFileName(number)),
		state:   chunkStateActive,
		firstID: firstID,
		f:       f,
		cpReady: true,
	}
	w.chunks = append(w.chunks, c)
	w.active = c
	delta, err := w.writeManifest()
	if err != nil {
		// The manifest does not list the chunk. The file is empty and this call
		// made it, so it goes.
		w.chunks = w.chunks[:len(w.chunks)-1]
		w.active = nil
		_ = f.Close()
		_ = os.Remove(c.path)
		return 0, err
	}
	return delta, nil
}

// sealActive syncs and closes the active chunk and marks it sealed in the
// manifest. It returns how much the manifest grew.
func (w *runWriter) sealActive() (int64, error) {
	c := w.active
	if c == nil {
		return 0, nil
	}
	w.active = nil
	c.state = chunkStateSealed
	var firstErr error
	if err := c.f.Sync(); err != nil {
		firstErr = err
	}
	if err := c.f.Close(); err != nil && firstErr == nil {
		firstErr = err
	}
	c.f = nil
	delta, err := w.writeManifest()
	if err != nil && firstErr == nil {
		firstErr = err
	}
	return delta, firstErr
}

// abandon closes the active chunk without sealing it. Migration uses it when it
// fails, since the run folder is removed then.
func (w *runWriter) abandon() {
	if w.active != nil && w.active.f != nil {
		_ = w.active.f.Close()
	}
	w.active = nil
}

// writeManifest rewrites the manifest through a temporary file, synced first,
// then renamed over it. It returns how much the manifest file grew.
func (w *runWriter) writeManifest() (int64, error) {
	entries := make([]json.RawMessage, 0, len(w.chunks))
	for _, c := range w.chunks {
		raw, err := joinExtra(c.entry(), c.extra)
		if err != nil {
			return 0, err
		}
		entries = append(entries, raw)
	}
	data, err := joinExtra(manifestFile{Run: w.id, Chunks: entries}, w.extra)
	if err != nil {
		return 0, err
	}
	data = append(data, '\n')
	path := filepath.Join(w.dir, manifestName)
	if err := writeFileSynced(path+".tmp", data); err != nil {
		return 0, err
	}
	if err := renameWithRetry(path+".tmp", path); err != nil {
		return 0, err
	}
	delta := int64(len(data)) - w.manifestSize
	w.manifestSize = int64(len(data))
	return delta, nil
}

// loadRuns reads the manifest of every run folder under segmentsDir. A folder
// whose manifest is missing or does not parse is logged and skipped, and never
// rewritten. A chunk that is not on disk is kept, so its IDs still count. Only
// an error reading a file this server must check is returned.
func loadRuns(segmentsDir string) ([]*runWriter, error) {
	names, err := listRunFolders(segmentsDir)
	if err != nil {
		return nil, err
	}
	var runs []*runWriter
	for _, name := range names {
		w := readRunManifest(segmentsDir, name)
		if w == nil {
			continue
		}
		dirty := false
		for _, c := range w.chunks {
			if _, err := os.Stat(c.path); err != nil {
				c.missing = true
				log.Printf("[photon-server] store: %s is listed in %s but is not on disk; its messages cannot be read", c.path, filepath.Join(w.dir, manifestName))
				continue
			}
			if c.state == chunkStateActive {
				if err := recoverChunk(c); err != nil {
					return nil, fmt.Errorf("recover %s: %w", c.path, err)
				}
				dirty = true
			}
		}
		if dirty {
			if _, err := w.writeManifest(); err != nil {
				log.Printf("[photon-server] store: could not rewrite %s after recovery: %v", filepath.Join(w.dir, manifestName), err)
			}
		}
		runs = append(runs, w)
	}
	return runs, nil
}

// readRunManifest reads and parses the manifest of one run folder. It returns nil,
// logging why, when the manifest is missing or does not parse.
func readRunManifest(segmentsDir, name string) *runWriter {
	dir := filepath.Join(segmentsDir, name)
	data, err := os.ReadFile(filepath.Join(dir, manifestName))
	if err != nil {
		log.Printf("[photon-server] store: skipping run %s: no readable manifest (%v)", name, err)
		return nil
	}
	w, err := parseManifest(dir, name, data)
	if err != nil {
		log.Printf("[photon-server] store: skipping run %s: manifest does not parse (%v)", name, err)
		return nil
	}
	return w
}

// parseManifest reads a manifest. Its chunks are in number order, and their
// states are as written; nothing is checked on disk.
func parseManifest(dir, id string, data []byte) (*runWriter, error) {
	var m manifestFile
	if err := json.Unmarshal(data, &m); err != nil {
		return nil, err
	}
	extra, err := splitExtra(data, manifestFileKeys)
	if err != nil {
		return nil, err
	}
	w := &runWriter{id: id, dir: dir, extra: extra, manifestSize: int64(len(data))}
	for _, raw := range m.Chunks {
		var e manifestChunk
		if err := json.Unmarshal(raw, &e); err != nil {
			return nil, fmt.Errorf("chunk entry: %w", err)
		}
		number, ok := chunkNumber(e.File)
		if !ok {
			return nil, fmt.Errorf("chunk file name %q", e.File)
		}
		cextra, err := splitExtra(raw, manifestChunkKeys)
		if err != nil {
			return nil, err
		}
		w.chunks = append(w.chunks, &chunk{
			number:     number,
			file:       e.File,
			path:       filepath.Join(dir, e.File),
			state:      e.State,
			extra:      cextra,
			firstID:    e.FirstID,
			lastID:     e.LastID,
			firstWrite: e.FirstWrite,
			lastWrite:  e.LastWrite,
			records:    e.Records,
			bytes:      e.Bytes,
		})
	}
	sort.Slice(w.chunks, func(i, j int) bool { return w.chunks[i].number < w.chunks[j].number })
	return w, nil
}

// recoverChunk checks an active chunk that a crash left open. It keeps the whole
// records, cuts a torn record off the end, and marks the chunk sealed. Bytes after
// a damaged record that is not at the end are kept, since they may be data.
func recoverChunk(c *chunk) error {
	info, err := os.Stat(c.path)
	if err != nil {
		return err
	}
	size := info.Size()

	var (
		good                  int64
		records               int
		first, last           MessageID
		firstWrite, lastWrite int64
		cps                   []checkpoint
	)
	_, err = scanRecordFile(c.path, 0, size, scanBufferBytes, func(off int64, id MessageID, writeUnix uint32, payload []byte) error {
		if records > 0 && id != last+1 {
			return errStopScan
		}
		if records == 0 {
			first, firstWrite = id, int64(writeUnix)
		}
		if checkpointDue(cps, off) {
			cps = append(cps, checkpoint{id: id, offset: off})
		}
		records++
		last, lastWrite = id, int64(writeUnix)
		good = off + frameHeaderBytes + int64(len(payload))
		return nil
	})
	if err != nil && !errors.Is(err, errStopScan) {
		return err
	}

	if good < size {
		torn, err := isTornTail(c.path, good, size)
		if err != nil {
			return err
		}
		if torn {
			if err := truncateSynced(c.path, good); err != nil {
				return err
			}
			log.Printf("[photon-server] store: cut %d torn bytes off the end of %s", size-good, c.path)
		} else {
			log.Printf("[photon-server] store: %s has %d bytes after a damaged record at byte %d; they are kept and not read", c.path, size-good, good)
		}
	}

	c.state = chunkStateSealed
	c.records = records
	if records > 0 {
		c.firstID, c.lastID = first, last
		c.firstWrite, c.lastWrite = firstWrite, lastWrite
	}
	c.bytes = good
	c.checkpoints = cps
	c.cpReady = true
	return nil
}

// isTornTail reports whether the bytes from good to size are one record that a
// crash cut short: less than a header is left, or the record's header says it
// runs to the end of the file. Anything else is damage, which is kept.
func isTornTail(path string, good, size int64) (bool, error) {
	if size-good < frameHeaderBytes {
		return true, nil
	}
	f, err := os.Open(path)
	if err != nil {
		return false, err
	}
	defer f.Close()
	header := make([]byte, frameHeaderBytes)
	if _, err := f.ReadAt(header, good); err != nil {
		return false, err
	}
	if binary.BigEndian.Uint32(header[0:4]) != frameMagic {
		return false, nil
	}
	n := int64(binary.BigEndian.Uint32(header[4:8]))
	return good+frameHeaderBytes+n >= size, nil
}

// buildCheckpoints reads a sealed chunk once to find its checkpoints. It checks
// the records against the manifest: the same count, IDs running on from firstID,
// and the last one lastID.
func buildCheckpoints(c *chunk) ([]checkpoint, error) {
	var cps []checkpoint
	records := 0
	var last MessageID
	_, err := scanRecordFile(c.path, 0, c.bytes, scanBufferBytes, func(off int64, id MessageID, _ uint32, _ []byte) error {
		want := c.firstID + MessageID(records)
		if id != want {
			return fmt.Errorf("chunk %s: message %d where %d was expected", c.file, id, want)
		}
		if checkpointDue(cps, off) {
			cps = append(cps, checkpoint{id: id, offset: off})
		}
		records++
		last = id
		return nil
	})
	if err != nil {
		return nil, err
	}
	if records != c.records || (records > 0 && last != c.lastID) {
		return nil, fmt.Errorf("chunk %s holds %d records ending at message %d; its manifest says %d ending at %d",
			c.file, records, last, c.records, c.lastID)
	}
	return cps, nil
}

// checkpointDue reports whether a record at off needs a checkpoint, given the
// checkpoints so far.
func checkpointDue(cps []checkpoint, off int64) bool {
	return len(cps) == 0 || off-cps[len(cps)-1].offset >= checkpointEvery
}

// readChunkRecord reads message want from the chunk file at path. Reading starts
// at off, which holds message from; limit is the end of the whole records. It
// returns the message and the offset after it.
func readChunkRecord(path string, from MessageID, off int64, want MessageID, limit int64) (Message, int64, error) {
	if from == want {
		return readRecordAt(path, off, want)
	}
	var (
		msg   Message
		next  int64
		found bool
	)
	_, err := scanRecordFile(path, off, limit, forwardReadBytes, func(o int64, id MessageID, _ uint32, payload []byte) error {
		if id < want {
			return nil
		}
		if id > want {
			return errPassed
		}
		msg = Message{ID: id, Payload: append([]byte(nil), payload...)}
		next = o + frameHeaderBytes + int64(len(payload))
		found = true
		return errFound
	})
	if err != nil && !errors.Is(err, errFound) && !errors.Is(err, errPassed) {
		return Message{}, 0, err
	}
	if !found {
		return Message{}, 0, fmt.Errorf("message %d is not in %s", want, path)
	}
	return msg, next, nil
}

// readRecordAt reads the record at off, which must hold message want.
func readRecordAt(path string, off int64, want MessageID) (Message, int64, error) {
	f, err := openChunkFile(path)
	if err != nil {
		return Message{}, 0, err
	}
	defer f.Close()
	header := make([]byte, frameHeaderBytes)
	if _, err := f.ReadAt(header, off); err != nil {
		return Message{}, 0, err
	}
	if binary.BigEndian.Uint32(header[0:4]) != frameMagic {
		return Message{}, 0, fmt.Errorf("message %d: invalid frame magic in %s", want, path)
	}
	n := int(binary.BigEndian.Uint32(header[4:8]))
	frame := make([]byte, frameHeaderBytes+n)
	copy(frame, header)
	if _, err := f.ReadAt(frame[frameHeaderBytes:], off+frameHeaderBytes); err != nil {
		return Message{}, 0, err
	}
	msg, err := decodeFrame(frame)
	if err != nil {
		return Message{}, 0, fmt.Errorf("message %d: %w", want, err)
	}
	if msg.ID != want {
		return Message{}, 0, fmt.Errorf("message %d: %s holds message %d at byte %d", want, path, msg.ID, off)
	}
	return msg, off + int64(len(frame)), nil
}

// listRunFolders returns the names of the run folders under segmentsDir, in name
// order. A folder whose name starts with a dot, and legacy, are not runs.
func listRunFolders(segmentsDir string) ([]string, error) {
	entries, err := os.ReadDir(segmentsDir)
	if err != nil {
		return nil, err
	}
	var names []string
	for _, e := range entries {
		name := e.Name()
		if !e.IsDir() || name == legacyFolder || strings.HasPrefix(name, ".") {
			continue
		}
		names = append(names, name)
	}
	return names, nil
}

// chunkFileName is the name of chunk number n.
func chunkFileName(n int) string {
	return fmt.Sprintf("%06d.seg", n)
}

// chunkNumber parses a chunk file name, reporting false for any other name.
func chunkNumber(file string) (int, bool) {
	var n int
	if _, err := fmt.Sscanf(file, "%06d.seg", &n); err != nil || file != chunkFileName(n) {
		return 0, false
	}
	return n, true
}

// splitExtra returns the fields of the JSON object raw that are not in known.
func splitExtra(raw []byte, known []string) (map[string]json.RawMessage, error) {
	var all map[string]json.RawMessage
	if err := json.Unmarshal(raw, &all); err != nil {
		return nil, err
	}
	for _, k := range known {
		delete(all, k)
	}
	if len(all) == 0 {
		return nil, nil
	}
	return all, nil
}

// joinExtra encodes known as a JSON object, with the fields in extra added.
func joinExtra(known any, extra map[string]json.RawMessage) ([]byte, error) {
	base, err := json.Marshal(known)
	if err != nil || len(extra) == 0 {
		return base, err
	}
	var m map[string]json.RawMessage
	if err := json.Unmarshal(base, &m); err != nil {
		return nil, err
	}
	for k, v := range extra {
		m[k] = v
	}
	return json.Marshal(m)
}

// writeFileSynced writes data to path and syncs it before returning.
func writeFileSynced(path string, data []byte) error {
	f, err := os.OpenFile(path, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	if _, err := f.Write(data); err != nil {
		_ = f.Close()
		return err
	}
	if err := f.Sync(); err != nil {
		_ = f.Close()
		return err
	}
	return f.Close()
}

// renameWithRetry renames from over to, retrying while another process holds to open.
func renameWithRetry(from, to string) error {
	var err error
	for i := 0; i < manifestTries; i++ {
		if err = os.Rename(from, to); err == nil {
			return nil
		}
		time.Sleep(25 * time.Millisecond)
	}
	return err
}

// truncateSynced cuts the file at path to size bytes and syncs it.
func truncateSynced(path string, size int64) error {
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return err
	}
	if err := f.Truncate(size); err != nil {
		_ = f.Close()
		return err
	}
	if err := f.Sync(); err != nil {
		_ = f.Close()
		return err
	}
	return f.Close()
}
