package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

type MessageID uint64

const (
	frameHeaderBytes  = 24
	frameMagic        = uint32(0x56534431)
	storeInspectBytes = 20 + 64*29
	// forwardReadBytes is the read buffer for reading forward from a checkpoint.
	forwardReadBytes = 32 << 10
)

// These sizes are variables so that tests can use small ones.
var (
	// segmentSize is the most a chunk holds before it is sealed. A record that
	// is bigger still gets a chunk of its own.
	segmentSize int64 = 32 << 20
	// checkpointEvery is the distance between the checkpoints of a chunk.
	checkpointEvery int64 = 1 << 20
	// hotTailBytes is how much of the newest payloads stays in memory.
	hotTailBytes int64 = 8 << 20
)

type Message struct {
	ID      MessageID
	Payload []byte
}

type StoreStats struct {
	LatestID          MessageID
	OldestID          MessageID
	RetainedMessages  uint64
	Segments          int
	ActiveSegmentID   MessageID
	ActiveSegmentSize int64
	DiskBytes         int64
	HotBytes          int64
	PressureHigh      bool
}

type StoreRecord struct {
	ID       MessageID
	Bytes    int
	Captured int
	Data     [storeInspectBytes]byte
}

type StoreSnapshot struct {
	Stats  StoreStats
	Recent []StoreRecord
}

// StreamStore keeps every message the server receives, in run folders under
// segments/ (see store_run.go). Message IDs run on across runs. The store keeps
// no per-message index: a read finds its chunk by ID range, then a checkpoint
// in the chunk, then reads forward.
type StreamStore struct {
	mu   sync.Mutex
	cond *sync.Cond

	segmentsDir string
	runID       string // the run this server writes
	closed      bool

	nextID   MessageID
	oldestID MessageID

	runs   map[string]*runWriter // every run folder, by run ID
	cur    *runWriter            // the run this server writes; nil until its first record
	chunks []*chunk              // chunks that hold records, by first ID

	// The newest payloads, for readers that are caught up. Their IDs run from
	// hotFirst on without gaps.
	hot      [][]byte
	hotFirst MessageID
	hotBytes int64

	// The chunk, next ID and offset after the last record read from disk. A
	// reader going forward starts there instead of at a checkpoint.
	readChunk *chunk
	readID    MessageID
	readOff   int64

	maxBytes  int64 // storage pressure from this many bytes on disk; 0 means never
	diskBytes int64 // bytes of every file under segments/
}

// OpenStreamStore opens the store in dataDir for the run runID, which names the
// run's folder under segments/. It reads each run's manifest, but not the
// records of sealed chunks. An active chunk left by a crash is checked, cut
// back to its last whole record, and sealed.
func OpenStreamStore(dataDir, runID string) (*StreamStore, error) {
	if !validRunID(runID) {
		return nil, fmt.Errorf("invalid run ID %q", runID)
	}
	segmentsDir := filepath.Join(dataDir, "segments")
	if err := os.MkdirAll(segmentsDir, 0o755); err != nil {
		return nil, err
	}
	s := &StreamStore{
		segmentsDir: segmentsDir,
		runID:       runID,
		nextID:      1,
		runs:        make(map[string]*runWriter),
		maxBytes:    DefaultStoreMaxGB << 30,
	}
	s.cond = sync.NewCond(&s.mu)

	runs, err := loadRuns(segmentsDir)
	if err != nil {
		return nil, err
	}
	for _, w := range runs {
		s.runs[w.id] = w
		for _, c := range w.chunks {
			if c.records == 0 {
				continue
			}
			c.listed = true
			s.chunks = append(s.chunks, c)
			if c.lastID >= s.nextID {
				s.nextID = c.lastID + 1
			}
		}
	}
	sort.Slice(s.chunks, func(i, j int) bool { return s.chunks[i].firstID < s.chunks[j].firstID })
	if len(s.chunks) > 0 {
		s.oldestID = s.chunks[0].firstID
	}
	s.diskBytes, err = dirBytes(segmentsDir)
	if err != nil {
		return nil, err
	}
	return s, nil
}

// SetMaxBytes sets the storage pressure limit: appends wait while the bytes
// under segments/ are at or above it. Zero means no limit.
func (s *StreamStore) SetMaxBytes(n int64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.maxBytes = n
}

// raiseNextID makes the next message ID at least next. The server uses it to
// number new messages above old files it could not migrate.
func (s *StreamStore) raiseNextID(next MessageID) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if next > s.nextID {
		s.nextID = next
	}
}

// Append stores payload as the next message and returns its ID. The record goes
// to the active chunk of this run, and a new chunk is started when the record
// would pass segmentSize.
func (s *StreamStore) Append(payload []byte) (MessageID, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	if s.closed {
		return 0, errors.New("stream store is closed")
	}
	if s.cur == nil {
		s.cur = s.runs[s.runID]
		if s.cur == nil {
			s.cur = &runWriter{id: s.runID, dir: filepath.Join(s.segmentsDir, s.runID)}
			s.runs[s.runID] = s.cur
		}
	}

	id := s.nextID
	c, written, delta, err := s.cur.appendFrame(encodeFrame(id, payload), id)
	s.diskBytes += written + delta
	if err != nil {
		return 0, err
	}
	if !c.listed {
		c.listed = true
		s.chunks = append(s.chunks, c)
	}
	if s.oldestID == 0 {
		s.oldestID = id
	}
	s.nextID++
	s.addHotLocked(id, payload)
	s.cond.Broadcast()
	return id, nil
}

func (s *StreamStore) ReadOrWait(id MessageID) (Message, error) {
	for {
		s.mu.Lock()
		if s.closed {
			s.mu.Unlock()
			return Message{}, errors.New("stream store is closed")
		}
		s.mu.Unlock()

		msg, ok, err := s.Read(id)
		if err != nil || ok {
			return msg, err
		}

		s.mu.Lock()
		for id >= s.nextID && !s.closed {
			s.cond.Wait()
		}
		if s.closed {
			s.mu.Unlock()
			return Message{}, errors.New("stream store is closed")
		}
		s.mu.Unlock()
	}
}

// Read returns message id. The bool is false, with no error, when the message
// is not stored yet.
func (s *StreamStore) Read(id MessageID) (Message, bool, error) {
	for {
		s.mu.Lock()
		if payload, ok := s.hotLocked(id); ok {
			s.mu.Unlock()
			return Message{ID: id, Payload: payload}, true, nil
		}
		if id == 0 || id >= s.nextID {
			s.mu.Unlock()
			return Message{}, false, nil
		}
		c := s.chunkForLocked(id)
		if c == nil {
			s.mu.Unlock()
			return Message{}, false, fmt.Errorf("message %d is no longer available", id)
		}
		if c.missing {
			s.mu.Unlock()
			return Message{}, false, fmt.Errorf("message %d is in %s, which is not on disk", id, c.file)
		}
		if !c.plain() {
			s.mu.Unlock()
			return Message{}, false, fmt.Errorf("message %d is archived", id)
		}
		fromCursor := s.readChunk == c && s.readID == id
		if !fromCursor && !c.cpReady {
			// A sealed chunk from an earlier start: its checkpoints are read once,
			// outside the lock, then the read starts again.
			s.mu.Unlock()
			cps, err := buildCheckpoints(c)
			if err != nil {
				return Message{}, false, err
			}
			s.mu.Lock()
			if !c.cpReady {
				c.checkpoints = cps
				c.cpReady = true
			}
			s.mu.Unlock()
			continue
		}
		from, off := id, s.readOff
		if !fromCursor {
			cp := c.checkpointFor(id)
			from, off = cp.id, cp.offset
		}
		limit := c.bytes
		s.mu.Unlock()

		msg, next, err := readChunkRecord(c.path, from, off, id, limit)
		if err != nil {
			return Message{}, false, err
		}
		s.mu.Lock()
		s.readChunk, s.readID, s.readOff = c, id+1, next
		s.mu.Unlock()
		return msg, true, nil
	}
}

func (s *StreamStore) StartIDForNewClient() MessageID {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.nextID
}

func (s *StreamStore) LatestID() MessageID {
	s.mu.Lock()
	defer s.mu.Unlock()

	if s.nextID == 1 {
		return 0
	}
	return s.nextID - 1
}

func (s *StreamStore) StoragePressureHigh() bool {
	s.mu.Lock()
	defer s.mu.Unlock()

	return s.pressureLocked()
}

func (s *StreamStore) pressureLocked() bool {
	return s.maxBytes > 0 && s.diskBytes >= s.maxBytes
}

func (s *StreamStore) Stats() StoreStats {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.statsLocked()
}

func (s *StreamStore) Snapshot(recentLimit int) StoreSnapshot {
	s.mu.Lock()
	defer s.mu.Unlock()

	snapshot := StoreSnapshot{Stats: s.statsLocked()}
	if recentLimit <= 0 {
		return snapshot
	}
	snapshot.Recent = make([]StoreRecord, 0, min(recentLimit, len(s.hot)))
	for i := len(s.hot) - 1; i >= 0 && len(snapshot.Recent) < recentLimit; i-- {
		payload := s.hot[i]
		record := StoreRecord{ID: s.hotFirst + MessageID(i), Bytes: len(payload)}
		record.Captured = copy(record.Data[:], payload)
		snapshot.Recent = append(snapshot.Recent, record)
	}
	return snapshot
}

func (s *StreamStore) statsLocked() StoreStats {
	latest := MessageID(0)
	if s.nextID > 1 {
		latest = s.nextID - 1
	}
	retained := uint64(0)
	if s.oldestID != 0 && latest >= s.oldestID {
		retained = uint64(latest-s.oldestID) + 1
	}
	stats := StoreStats{
		LatestID:         latest,
		OldestID:         s.oldestID,
		RetainedMessages: retained,
		Segments:         len(s.chunks),
		DiskBytes:        s.diskBytes,
		HotBytes:         s.hotBytes,
		PressureHigh:     s.pressureLocked(),
	}
	if s.cur != nil && s.cur.active != nil {
		stats.ActiveSegmentID = s.cur.active.firstID
		stats.ActiveSegmentSize = s.cur.active.bytes
	}
	return stats
}

// Close seals the active chunk, so its manifest entry is final, and refuses
// further appends. Waiting readers are woken and get an error.
func (s *StreamStore) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return nil
	}
	s.closed = true
	s.cond.Broadcast()
	if s.cur == nil {
		return nil
	}
	_, err := s.cur.sealActive()
	return err
}

// chunkForLocked returns the chunk that holds message id, or nil.
func (s *StreamStore) chunkForLocked(id MessageID) *chunk {
	i := sort.Search(len(s.chunks), func(i int) bool { return s.chunks[i].firstID > id }) - 1
	if i < 0 || id > s.chunks[i].lastID {
		return nil
	}
	return s.chunks[i]
}

// hotLocked returns message id from the hot tail, if it is there.
func (s *StreamStore) hotLocked(id MessageID) ([]byte, bool) {
	if id < s.hotFirst || id-s.hotFirst >= MessageID(len(s.hot)) {
		return nil, false
	}
	return s.hot[id-s.hotFirst], true
}

// addHotLocked adds a copy of payload to the hot tail and drops the oldest
// payloads until the tail is within hotTailBytes.
func (s *StreamStore) addHotLocked(id MessageID, payload []byte) {
	copyPayload := make([]byte, len(payload))
	copy(copyPayload, payload)

	if len(s.hot) == 0 {
		s.hotFirst = id
	}
	s.hot = append(s.hot, copyPayload)
	s.hotBytes += int64(len(copyPayload))

	for s.hotBytes > hotTailBytes && len(s.hot) > 0 {
		s.hotBytes -= int64(len(s.hot[0]))
		s.hot[0] = nil
		s.hot = s.hot[1:]
		s.hotFirst++
	}
}

// validRunID reports whether id can name a run folder.
func validRunID(id string) bool {
	return id != "" && id != legacyFolder && !strings.HasPrefix(id, ".") && !strings.ContainsAny(id, `/\:`)
}

func encodeFrame(id MessageID, payload []byte) []byte {
	return encodeFrameAt(id, uint32(time.Now().Unix()), payload)
}

// encodeFrameAt makes one record with the given write time, in unix seconds.
func encodeFrameAt(id MessageID, writeUnix uint32, payload []byte) []byte {
	frame := make([]byte, frameHeaderBytes+len(payload))
	binary.BigEndian.PutUint32(frame[0:4], frameMagic)
	binary.BigEndian.PutUint32(frame[4:8], uint32(len(payload)))
	binary.BigEndian.PutUint64(frame[8:16], uint64(id))
	binary.BigEndian.PutUint32(frame[16:20], writeUnix)
	binary.BigEndian.PutUint32(frame[20:24], crc32.ChecksumIEEE(payload))
	copy(frame[frameHeaderBytes:], payload)
	return frame
}

func decodeFrame(frame []byte) (Message, error) {
	if len(frame) < frameHeaderBytes {
		return Message{}, errors.New("unexpected end of frame")
	}
	if binary.BigEndian.Uint32(frame[0:4]) != frameMagic {
		return Message{}, errors.New("invalid frame magic")
	}

	payloadLen := int(binary.BigEndian.Uint32(frame[4:8]))
	if len(frame) != frameHeaderBytes+payloadLen {
		return Message{}, errors.New("unexpected end of frame")
	}

	payload := frame[frameHeaderBytes:]
	expectedCRC := binary.BigEndian.Uint32(frame[20:24])
	if crc32.ChecksumIEEE(payload) != expectedCRC {
		return Message{}, errors.New("invalid frame crc")
	}

	return Message{
		ID:      MessageID(binary.BigEndian.Uint64(frame[8:16])),
		Payload: payload,
	}, nil
}

// dirBytes is the size of every file under dir. Each size comes from os.Stat:
// on Windows, the size a directory listing gives can lag for a file that is
// open for writing.
func dirBytes(dir string) (int64, error) {
	var total int64
	err := filepath.WalkDir(dir, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			// The old files a migration moved aside are a second copy of data that is
			// already in run folders, waiting to be checked and removed by hand; they
			// must not make ingest wait.
			if path != dir && d.Name() == legacyFolder {
				return filepath.SkipDir
			}
			return nil
		}
		info, err := os.Stat(path)
		if err != nil {
			return err
		}
		total += info.Size()
		return nil
	})
	return total, err
}
