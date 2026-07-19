package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"path/filepath"
	"sort"
	"sync"
	"time"
)

type MessageID uint64

const (
	frameHeaderBytes      = 24
	frameMagic            = uint32(0x56534431)
	storeInspectBytes     = 20 + 64*29
	streamDir             = "/home/ubuntu/logs/"
	segmentSize           = 256 * 1024 * 1024
	hotTailBytes          = 32 * 1024 * 1024
	diskHighWatermarkByte = 8 * 1024 * 1024 * 1024
)

type Message struct {
	ID      MessageID
	Payload []byte
}

type messageLocation struct {
	segmentID MessageID
	offset    int64
	size      int64
}

type segmentInfo struct {
	path     string
	minID    MessageID
	maxID    MessageID
	size     int64
	active   bool
	readFile *os.File
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

type StreamStore struct {
	mu   sync.Mutex
	cond *sync.Cond

	segmentsDir string

	nextID          MessageID
	oldestID        MessageID
	activeSegmentID MessageID
	activeFile      *os.File
	activeOffset    int64

	index    map[MessageID]messageLocation
	segments map[MessageID]*segmentInfo

	hotMessages map[MessageID][]byte
	hotOrder    []MessageID
	hotBytes    int64

	diskBytes int64
}

func OpenStreamStore() (*StreamStore, error) {
	segmentsDir := filepath.Join(streamDir, "segments")
	if err := os.MkdirAll(segmentsDir, 0o755); err != nil {
		return nil, err
	}
	store := &StreamStore{
		segmentsDir: segmentsDir,
		nextID:      1,
		index:       make(map[MessageID]messageLocation),
		segments:    make(map[MessageID]*segmentInfo),
		hotMessages: make(map[MessageID][]byte),
	}
	store.cond = sync.NewCond(&store.mu)

	if err := store.recoverSegments(); err != nil {
		return nil, err
	}
	if err := store.openActiveSegment(); err != nil {
		return nil, err
	}
	return store, nil
}

func (s *StreamStore) Append(payload []byte) (MessageID, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	if s.activeOffset+int64(frameHeaderBytes+len(payload)) > segmentSize && s.activeOffset > 0 {
		if err := s.rotateActiveSegmentLocked(); err != nil {
			return 0, err
		}
	}

	id := s.nextID
	frame := encodeFrame(id, payload)
	offset := s.activeOffset
	n, err := s.activeFile.Write(frame)
	if err != nil {
		return 0, err
	}
	if n != len(frame) {
		return 0, io.ErrShortWrite
	}

	size := int64(len(frame))
	s.index[id] = messageLocation{
		segmentID: s.activeSegmentID,
		offset:    offset,
		size:      size,
	}
	segment := s.segments[s.activeSegmentID]
	if segment.minID == 0 {
		segment.minID = id
	}
	segment.maxID = id
	segment.size += size
	s.activeOffset += size
	s.diskBytes += size
	s.nextID++
	if s.oldestID == 0 {
		s.oldestID = id
	}

	s.addHotLocked(id, payload)
	s.cond.Broadcast()
	return id, nil
}

func (s *StreamStore) ReadOrWait(id MessageID) (Message, error) {
	for {
		msg, ok, err := s.Read(id)
		if err != nil || ok {
			return msg, err
		}

		s.mu.Lock()
		for id >= s.nextID {
			s.cond.Wait()
		}
		s.mu.Unlock()
	}
}

func (s *StreamStore) Read(id MessageID) (Message, bool, error) {
	s.mu.Lock()
	if payload, ok := s.hotMessages[id]; ok {
		msg := Message{
			ID:      id,
			Payload: payload,
		}
		s.mu.Unlock()
		return msg, true, nil
	}

	location, ok := s.index[id]
	if !ok {
		nextID := s.nextID
		s.mu.Unlock()
		if id >= nextID {
			return Message{}, false, nil
		}
		return Message{}, false, fmt.Errorf("message %d is no longer available", id)
	}

	segment, ok := s.segments[location.segmentID]
	if !ok {
		s.mu.Unlock()
		return Message{}, false, fmt.Errorf("segment %d is missing", location.segmentID)
	}
	file, err := s.segmentReadFileLocked(segment)
	s.mu.Unlock()
	if err != nil {
		return Message{}, false, err
	}

	buf := make([]byte, location.size)
	if _, err := file.ReadAt(buf, location.offset); err != nil {
		return Message{}, false, err
	}
	msg, err := decodeFrame(buf)
	if err != nil {
		return Message{}, false, err
	}
	return msg, true, nil
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

	return diskHighWatermarkByte > 0 && s.diskBytes >= diskHighWatermarkByte
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
	snapshot.Recent = make([]StoreRecord, 0, min(recentLimit, len(s.hotOrder)))
	for i := len(s.hotOrder) - 1; i >= 0 && len(snapshot.Recent) < recentLimit; i-- {
		id := s.hotOrder[i]
		payload, ok := s.hotMessages[id]
		if !ok {
			continue
		}
		record := StoreRecord{ID: id, Bytes: len(payload)}
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
	return StoreStats{
		LatestID:          latest,
		OldestID:          s.oldestID,
		RetainedMessages:  retained,
		Segments:          len(s.segments),
		ActiveSegmentID:   s.activeSegmentID,
		ActiveSegmentSize: s.activeOffset,
		DiskBytes:         s.diskBytes,
		HotBytes:          s.hotBytes,
		PressureHigh:      diskHighWatermarkByte > 0 && s.diskBytes >= diskHighWatermarkByte,
	}
}

func (s *StreamStore) CompactBefore(id MessageID) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	for segmentID, segment := range s.segments {
		if segment.active || segment.maxID == 0 || segment.maxID >= id {
			continue
		}

		if segment.readFile != nil {
			_ = segment.readFile.Close()
		}
		if err := os.Remove(segment.path); err != nil && !errors.Is(err, os.ErrNotExist) {
			return err
		}

		for msgID, loc := range s.index {
			if loc.segmentID == segmentID {
				delete(s.index, msgID)
				if payload, ok := s.hotMessages[msgID]; ok {
					s.hotBytes -= int64(len(payload))
					delete(s.hotMessages, msgID)
				}
			}
		}
		s.diskBytes -= segment.size
		delete(s.segments, segmentID)
	}
	s.oldestID = 0
	for _, segment := range s.segments {
		if segment.minID != 0 && (s.oldestID == 0 || segment.minID < s.oldestID) {
			s.oldestID = segment.minID
		}
	}
	return nil
}

func (s *StreamStore) Close() error {
	s.mu.Lock()
	defer s.mu.Unlock()

	var firstErr error
	if s.activeFile != nil {
		if err := s.activeFile.Close(); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	for _, segment := range s.segments {
		if segment.readFile != nil {
			if err := segment.readFile.Close(); err != nil && firstErr == nil {
				firstErr = err
			}
		}
	}
	return firstErr
}

func (s *StreamStore) recoverSegments() error {
	entries, err := os.ReadDir(s.segmentsDir)
	if err != nil {
		return err
	}

	var ids []int
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		var id int
		if _, err := fmt.Sscanf(entry.Name(), "%012d.log", &id); err == nil {
			ids = append(ids, id)
		}
	}
	sort.Ints(ids)

	for _, rawID := range ids {
		segmentID := MessageID(rawID)
		path := s.segmentPath(segmentID)
		if err := s.recoverSegment(segmentID, path); err != nil {
			return err
		}
	}
	return nil
}

func (s *StreamStore) recoverSegment(segmentID MessageID, path string) error {
	file, err := os.OpenFile(path, os.O_RDWR, 0o644)
	if err != nil {
		return err
	}
	defer file.Close()

	info := &segmentInfo{path: path}
	var offset int64

	for {
		header := make([]byte, frameHeaderBytes)
		n, err := file.ReadAt(header, offset)
		if errors.Is(err, io.EOF) && n == 0 {
			break
		}
		if err != nil && !errors.Is(err, io.EOF) {
			return err
		}
		if n < frameHeaderBytes {
			break
		}

		payloadLen := int64(binary.BigEndian.Uint32(header[4:8]))
		frameSize := int64(frameHeaderBytes) + payloadLen
		frame := make([]byte, frameSize)
		if _, err := file.ReadAt(frame, offset); err != nil {
			break
		}
		msg, err := decodeFrame(frame)
		if err != nil {
			break
		}

		if info.minID == 0 {
			info.minID = msg.ID
		}
		info.maxID = msg.ID
		info.size += frameSize
		s.index[msg.ID] = messageLocation{
			segmentID: segmentID,
			offset:    offset,
			size:      frameSize,
		}
		s.addHotLocked(msg.ID, msg.Payload)
		if msg.ID >= s.nextID {
			s.nextID = msg.ID + 1
		}
		if s.oldestID == 0 || msg.ID < s.oldestID {
			s.oldestID = msg.ID
		}
		offset += frameSize
	}

	if err := file.Truncate(offset); err != nil {
		return err
	}
	s.diskBytes += offset
	s.segments[segmentID] = info
	return nil
}

func (s *StreamStore) openActiveSegment() error {
	activeID := MessageID(1)
	for id := range s.segments {
		if id >= activeID {
			activeID = id + 1
		}
	}

	if activeID > 1 {
		previous := s.segments[activeID-1]
		if previous != nil && previous.size < segmentSize {
			activeID = activeID - 1
		}
	}

	path := s.segmentPath(activeID)
	file, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR|os.O_APPEND, 0o644)
	if err != nil {
		return err
	}

	if existing := s.segments[activeID]; existing != nil {
		existing.active = true
		s.activeOffset = existing.size
	} else {
		s.segments[activeID] = &segmentInfo{path: path, active: true}
	}
	s.activeSegmentID = activeID
	s.activeFile = file
	return nil
}

func (s *StreamStore) rotateActiveSegmentLocked() error {
	if active := s.segments[s.activeSegmentID]; active != nil {
		active.active = false
	}
	if err := s.activeFile.Close(); err != nil {
		return err
	}

	s.activeSegmentID++
	s.activeOffset = 0
	path := s.segmentPath(s.activeSegmentID)
	file, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR|os.O_APPEND, 0o644)
	if err != nil {
		return err
	}

	s.activeFile = file
	s.segments[s.activeSegmentID] = &segmentInfo{
		path:   path,
		active: true,
	}
	return nil
}

func (s *StreamStore) segmentReadFileLocked(segment *segmentInfo) (*os.File, error) {
	if segment.active {
		return s.activeFile, nil
	}
	if segment.readFile != nil {
		return segment.readFile, nil
	}

	file, err := os.Open(segment.path)
	if err != nil {
		return nil, err
	}
	segment.readFile = file
	return file, nil
}

func (s *StreamStore) addHotLocked(id MessageID, payload []byte) {
	copyPayload := make([]byte, len(payload))
	copy(copyPayload, payload)

	s.hotMessages[id] = copyPayload
	s.hotOrder = append(s.hotOrder, id)
	s.hotBytes += int64(len(copyPayload))

	for s.hotBytes > hotTailBytes && len(s.hotOrder) > 0 {
		oldest := s.hotOrder[0]
		s.hotOrder = s.hotOrder[1:]
		if payload, ok := s.hotMessages[oldest]; ok {
			s.hotBytes -= int64(len(payload))
			delete(s.hotMessages, oldest)
		}
	}
}

func (s *StreamStore) segmentPath(id MessageID) string {
	return filepath.Join(s.segmentsDir, fmt.Sprintf("%012d.log", id))
}

func encodeFrame(id MessageID, payload []byte) []byte {
	frame := make([]byte, frameHeaderBytes+len(payload))
	binary.BigEndian.PutUint32(frame[0:4], frameMagic)
	binary.BigEndian.PutUint32(frame[4:8], uint32(len(payload)))
	binary.BigEndian.PutUint64(frame[8:16], uint64(id))
	binary.BigEndian.PutUint32(frame[16:20], uint32(time.Now().Unix()))
	binary.BigEndian.PutUint32(frame[20:24], crc32.ChecksumIEEE(payload))
	copy(frame[frameHeaderBytes:], payload)
	return frame
}

func decodeFrame(frame []byte) (Message, error) {
	if len(frame) < frameHeaderBytes {
		return Message{}, io.ErrUnexpectedEOF
	}
	if binary.BigEndian.Uint32(frame[0:4]) != frameMagic {
		return Message{}, errors.New("invalid frame magic")
	}

	payloadLen := int(binary.BigEndian.Uint32(frame[4:8]))
	if len(frame) != frameHeaderBytes+payloadLen {
		return Message{}, io.ErrUnexpectedEOF
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
