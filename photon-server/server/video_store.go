package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"sync"
	"time"
)

const (
	videoIndexMagic      = uint32(0x56494458) // "VIDX"
	videoIndexVersion    = uint16(2)
	videoIndexHeaderSize = 2048
	// v1 records held only the receive-stamped wall clock. v2 carries the RTP
	// timestamp and the raw receive time alongside it, plus how the wall clock
	// was derived, so a recording's A/V skew is knowable after the fact
	// (tsdb-architecture.md §8.5). v1 files stay readable.
	videoNALRecordSizeV1 = 24
	videoNALRecordSizeV2 = 40
	videoNALRecordSize   = videoNALRecordSizeV2
	videoSPSCap          = 1024
	videoPPSCap          = 256

	videoSegmentMaxBytes    = 64 * 1024 * 1024
	videoSegmentMaxDurMS    = 60_000
	videoSegmentForceBytes  = 128 * 1024 * 1024
	videoSegmentForceDurMS  = 120_000
	videoHotTailBytes       = 8 * 1024 * 1024
	videoRecoverFrameStepMS = 33

	// One RTP ingress means one stream. streamId stays in the pointer table
	// because multi-camera is a live open decision (§14.5) and the column is
	// free, but nothing writes anything but 0 today.
	videoDefaultStreamID = uint8(0)
)

type videoNALRecord struct {
	TimestampMs uint64
	Offset      uint64
	Size        uint32
	Flags       uint8
	NALType     uint8
	// v2 fields.
	ClockProv    uint8
	RTPTimestamp uint32
	ReceiveMs    uint64
}

type videoIndexEntry struct {
	seq          uint64
	segmentID    uint64
	timestampMs  uint64
	offset       int64
	size         int32
	flags        uint8
	nalType      uint8
	clockProv    uint8
	rtpTimestamp uint32
	receiveMs    uint64
}

type videoSegment struct {
	id       uint64
	dataPath string
	idxPath  string
	startMs  uint64
	endMs    uint64
	size     int64
	active   bool
	sps      []byte
	pps      []byte
	dataFile *os.File
	idxFile  *os.File
	readFile *os.File
}

type VideoStoreStats struct {
	BytesStored  uint64
	SegmentCount int
	LastNALMS    uint64
	LatestSeq    uint64
}

type VideoStore struct {
	mu   sync.Mutex
	cond *sync.Cond

	dir    string
	closed bool

	nextSeq   uint64
	nextSegID uint64
	active    *videoSegment
	segments  map[uint64]*videoSegment
	entries   []videoIndexEntry
	keyframes []int
	hot       map[uint64][]byte
	hotOrder  []uint64
	hotBytes  int64
	diskBytes int64
	sps       []byte
	pps       []byte
	lastNALMS uint64

	vpt        vptBuilder
	clock      *RTPClock
	backfilled bool
}

func OpenVideoStore(dataDir string) (*VideoStore, error) {
	dir := filepath.Join(dataDir, "video", "segments")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	store := &VideoStore{
		dir:       dir,
		nextSeq:   1,
		nextSegID: 1,
		segments:  make(map[uint64]*videoSegment),
		hot:       make(map[uint64][]byte),
	}
	store.cond = sync.NewCond(&store.mu)
	store.vpt.reset(filepath.Join(dataDir, "video", "video.vpt"))
	if err := store.recover(); err != nil {
		return nil, err
	}
	if err := store.openActive(); err != nil {
		return nil, err
	}
	// The table is derived from the .idx records, so rebuilding it on open
	// keeps it consistent with whatever recovery decided and makes deleting
	// video.vpt harmless.
	store.rebuildVptLocked()
	if err := store.vpt.flush(); err != nil {
		return nil, err
	}
	return store, nil
}

// SetClock attaches the RTP/RTCP clock. Without one the store falls back to
// receive-time stamping, which is exactly what §8.5 says must stay available
// but labelled.
func (s *VideoStore) SetClock(clock *RTPClock) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.clock = clock
	if clock != nil {
		s.vpt.clockOffset = clock.ManualOffset()
		s.vpt.clockProv = clock.Provenance()
	}
}

// SetManualOffset persists the UI nudge and publishes it on the pointer table
// so the next video_locate carries the new offset (§8.5 item 3).
func (s *VideoStore) SetManualOffset(offsetMs int64) error {
	s.mu.Lock()
	clock := s.clock
	s.mu.Unlock()
	if clock == nil {
		return errors.New("video clock is not attached")
	}
	if err := clock.SetManualOffset(offsetMs); err != nil {
		return err
	}
	s.mu.Lock()
	s.vpt.clockOffset = offsetMs
	s.vpt.clockProv = clock.Provenance()
	s.mu.Unlock()
	return nil
}

func (s *VideoStore) ManualOffset() int64 {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.clock == nil {
		return s.vpt.clockOffset
	}
	return s.clock.ManualOffset()
}

func (s *VideoStore) ClockProvenance() uint8 {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.clock == nil {
		return s.vpt.clockProv
	}
	return s.clock.Provenance()
}

// Append stamps a NAL with receive time only. Kept for callers that have no
// RTP timestamp to offer.
func (s *VideoStore) Append(payload []byte, timestampMs uint64) (uint64, error) {
	return s.AppendStamped(payload, timestampMs, 0, timestampMs, clockProvReceive)
}

// AppendStamped records a NAL with the wall clock the caller derived, the RTP
// timestamp it came from, the raw receive time, and how the two were related.
func (s *VideoStore) AppendStamped(payload []byte, timestampMs uint64, rtpTs uint32, receiveMs uint64,
	prov uint8) (uint64, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed {
		return 0, errors.New("video store is closed")
	}
	if len(payload) < annexBStartCodeBytes+1 {
		return 0, errors.New("video NAL is too short")
	}

	nalType := nalTypeOfAnnexB(payload)
	if discardableNALType(nalType) {
		return 0, nil
	}
	flags := flagsForAnnexBNAL(payload)
	if nalType == nalTypeSPS {
		s.sps = cloneBytes(payload)
	}
	if nalType == nalTypePPS {
		s.pps = cloneBytes(payload)
	}

	if s.shouldRotateLocked(timestampMs, flags) {
		if err := s.rotateLocked(timestampMs); err != nil {
			return 0, err
		}
	}

	seq, err := s.writeNALStampedLocked(payload, timestampMs, flags, nalType, rtpTs, receiveMs, prov)
	if err != nil {
		return 0, err
	}
	// The mapping only becomes available once the first sender report lands,
	// so everything before it is receive-stamped. Fix those in place rather
	// than leaving the head of the recording with unknown skew.
	if prov == clockProvRTCP && !s.backfilled {
		if err := s.backfillClockLocked(); err != nil {
			return 0, err
		}
	}
	if err := s.vpt.flush(); err != nil {
		return 0, err
	}
	s.cond.Broadcast()
	return seq, nil
}

func (s *VideoStore) shouldRotateLocked(timestampMs uint64, flags uint8) bool {
	if s.active == nil || s.active.size == 0 {
		return false
	}
	dur := uint64(0)
	if timestampMs >= s.active.startMs {
		dur = timestampMs - s.active.startMs
	}
	overSize := s.active.size >= videoSegmentMaxBytes
	overDur := s.active.startMs != 0 && dur >= videoSegmentMaxDurMS
	force := s.active.size >= videoSegmentForceBytes || (s.active.startMs != 0 && dur >= videoSegmentForceDurMS)
	if force {
		return true
	}
	if (overSize || overDur) && flags&VideoFlagKeyframe != 0 {
		return true
	}
	return false
}

func (s *VideoStore) writeNALLocked(payload []byte, timestampMs uint64, flags uint8, nalType uint8) (uint64, error) {
	return s.writeNALStampedLocked(payload, timestampMs, flags, nalType, 0, timestampMs, clockProvReceive)
}

func (s *VideoStore) writeNALStampedLocked(payload []byte, timestampMs uint64, flags uint8, nalType uint8,
	rtpTs uint32, receiveMs uint64, prov uint8) (uint64, error) {
	if s.active == nil {
		return 0, errors.New("video store has no active segment")
	}
	offset := s.active.size
	n, err := s.active.dataFile.Write(payload)
	if err != nil {
		return 0, err
	}
	if n != len(payload) {
		return 0, io.ErrShortWrite
	}

	rec := videoNALRecord{
		TimestampMs:  timestampMs,
		Offset:       uint64(offset),
		Size:         uint32(len(payload)),
		Flags:        flags,
		NALType:      nalType,
		ClockProv:    prov,
		RTPTimestamp: rtpTs,
		ReceiveMs:    receiveMs,
	}
	if err := writeNALRecord(s.active.idxFile, rec); err != nil {
		return 0, err
	}

	if s.active.startMs == 0 || timestampMs < s.active.startMs {
		s.active.startMs = timestampMs
	}
	if timestampMs > s.active.endMs {
		s.active.endMs = timestampMs
	}
	if flags&VideoFlagConfig != 0 {
		if nalType == nalTypeSPS {
			s.active.sps = cloneBytes(payload)
		}
		if nalType == nalTypePPS {
			s.active.pps = cloneBytes(payload)
		}
	}
	s.active.size += int64(len(payload))
	s.diskBytes += int64(len(payload))
	s.lastNALMS = timestampMs

	seq := s.nextSeq
	s.nextSeq++
	entry := videoIndexEntry{
		seq:          seq,
		segmentID:    s.active.id,
		timestampMs:  timestampMs,
		offset:       offset,
		size:         int32(len(payload)),
		flags:        flags,
		nalType:      nalType,
		clockProv:    prov,
		rtpTimestamp: rtpTs,
		receiveMs:    receiveMs,
	}
	s.entries = append(s.entries, entry)
	if flags&VideoFlagKeyframe != 0 {
		s.keyframes = append(s.keyframes, len(s.entries)-1)
	}
	// One row per GOP, built here rather than by a later pass (§8.1).
	s.vpt.add(videoDefaultStreamID, uint32(s.active.id), timestampMs, uint64(offset), uint32(len(payload)),
		flags, nalType)
	if s.clock != nil {
		s.vpt.clockOffset = s.clock.ManualOffset()
		s.vpt.clockProv = s.clock.Provenance()
	}
	s.addHotLocked(seq, payload)
	return seq, nil
}

func (s *VideoStore) rotateLocked(timestampMs uint64) error {
	if err := s.flushActiveLocked(); err != nil {
		return err
	}
	// A rotation ends the byte run, so no row may straddle the boundary.
	s.vpt.closeOpen()
	s.vpt.clearConfigRun()
	if err := s.vpt.flush(); err != nil {
		return err
	}
	if s.active.dataFile != nil {
		_ = s.active.dataFile.Close()
		s.active.dataFile = nil
	}
	if s.active.idxFile != nil {
		_ = s.active.idxFile.Close()
		s.active.idxFile = nil
	}
	s.active.active = false

	nextID := s.active.id + 1
	if nextID < s.nextSegID {
		nextID = s.nextSegID
	}
	seg, err := createVideoSegment(s.dir, nextID)
	if err != nil {
		return err
	}
	s.segments[nextID] = seg
	s.active = seg
	s.nextSegID = nextID + 1

	// Prefix the new segment with the current SPS/PPS so it is independently decodable.
	if len(s.sps) > 0 {
		if _, err := s.writeNALLocked(s.sps, timestampMs, VideoFlagConfig, nalTypeSPS); err != nil {
			return err
		}
	}
	if len(s.pps) > 0 {
		if _, err := s.writeNALLocked(s.pps, timestampMs, VideoFlagConfig, nalTypePPS); err != nil {
			return err
		}
	}
	return nil
}

func (s *VideoStore) LatestKeyframe() (VideoFrame, uint64, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.latestKeyframeLocked()
}

func (s *VideoStore) latestKeyframeLocked() (VideoFrame, uint64, bool) {
	if len(s.keyframes) == 0 {
		return VideoFrame{}, 0, false
	}
	entry := s.entries[s.keyframes[len(s.keyframes)-1]]
	payload, err := s.readPayloadLocked(entry)
	if err != nil {
		return VideoFrame{}, 0, false
	}
	return VideoFrame{
		TimestampMs: entry.timestampMs,
		Flags:       entry.flags,
		Payload:     payload,
	}, entry.seq, true
}

func (s *VideoStore) ConfigNALs() (sps, pps []byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return cloneBytes(s.sps), cloneBytes(s.pps)
}

func (s *VideoStore) keyframeIndexAtOrBeforeLocked(t uint64) (int, bool) {
	if len(s.keyframes) == 0 {
		return 0, false
	}
	i := sort.Search(len(s.keyframes), func(i int) bool {
		return s.entries[s.keyframes[i]].timestampMs > t
	})
	if i == 0 {
		return 0, false
	}
	return i - 1, true
}

func (s *VideoStore) ReplayStart(t uint64) (seq uint64, sps, pps []byte, ok bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	start := 0
	found := false
	if idx, has := s.keyframeIndexAtOrBeforeLocked(t); has {
		start = s.keyframes[idx]
		found = true
	} else if len(s.keyframes) > 0 {
		start = s.keyframes[0]
		found = true
	}
	if !found {
		return 0, nil, nil, false
	}
	entry := s.entries[start]
	return entry.seq, s.configForEntryLocked(start, nalTypeSPS), s.configForEntryLocked(start, nalTypePPS), true
}

func (s *VideoStore) ReadSeq(seq uint64) (VideoFrame, bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	entry, ok := s.entryBySeqLocked(seq)
	if !ok {
		return VideoFrame{}, false, nil
	}
	payload, err := s.readPayloadLocked(entry)
	if err != nil {
		return VideoFrame{}, false, err
	}
	return VideoFrame{
		TimestampMs: entry.timestampMs,
		Flags:       entry.flags,
		Payload:     payload,
	}, true, nil
}

func (s *VideoStore) configForEntryLocked(entryIdx int, want uint8) []byte {
	for i := entryIdx; i >= 0; i-- {
		if s.entries[i].nalType == want {
			payload, err := s.readPayloadLocked(s.entries[i])
			if err == nil {
				return payload
			}
		}
	}
	if want == nalTypeSPS {
		return cloneBytes(s.sps)
	}
	if want == nalTypePPS {
		return cloneBytes(s.pps)
	}
	return nil
}

func (s *VideoStore) ReadOrWait(seq uint64) (VideoFrame, uint64, error) {
	for {
		s.mu.Lock()
		if s.closed {
			s.mu.Unlock()
			return VideoFrame{}, 0, errors.New("video store is closed")
		}
		if seq >= s.nextSeq {
			for seq >= s.nextSeq && !s.closed {
				s.cond.Wait()
			}
			if s.closed {
				s.mu.Unlock()
				return VideoFrame{}, 0, errors.New("video store is closed")
			}
		}

		if seq+1 < s.nextSeq && s.nextSeq-seq > videoLiveMaxLag+1 {
			frame, kfSeq, ok := s.latestKeyframeLocked()
			if ok && kfSeq > seq {
				s.mu.Unlock()
				return frame, kfSeq, nil
			}
			// Already past the latest IDR (long GOP). Wait at the live edge
			// instead of rewinding to the same keyframe forever.
			seq = s.nextSeq
			s.mu.Unlock()
			continue
		}

		entry, ok := s.entryBySeqLocked(seq)
		if !ok {
			frame, kfSeq, found := s.latestKeyframeLocked()
			if found && kfSeq >= seq {
				s.mu.Unlock()
				return frame, kfSeq, nil
			}
			if s.nextSeq > seq {
				seq = s.nextSeq
			}
			s.mu.Unlock()
			continue
		}
		payload, err := s.readPayloadLocked(entry)
		s.mu.Unlock()
		if err != nil {
			return VideoFrame{}, 0, err
		}
		return VideoFrame{
			TimestampMs: entry.timestampMs,
			Flags:       entry.flags,
			Payload:     payload,
		}, entry.seq, nil
	}
}

func (s *VideoStore) entryBySeqLocked(seq uint64) (videoIndexEntry, bool) {
	if seq == 0 || len(s.entries) == 0 {
		return videoIndexEntry{}, false
	}
	first := s.entries[0].seq
	if seq < first || seq >= s.nextSeq {
		return videoIndexEntry{}, false
	}
	idx := int(seq - first)
	if idx >= 0 && idx < len(s.entries) && s.entries[idx].seq == seq {
		return s.entries[idx], true
	}
	for i := len(s.entries) - 1; i >= 0; i-- {
		if s.entries[i].seq == seq {
			return s.entries[i], true
		}
	}
	return videoIndexEntry{}, false
}

func (s *VideoStore) NextSeq() uint64 {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.nextSeq
}

func (s *VideoStore) LatestSeq() uint64 {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.nextSeq == 1 {
		return 0
	}
	return s.nextSeq - 1
}

func (s *VideoStore) Stats() VideoStoreStats {
	s.mu.Lock()
	defer s.mu.Unlock()
	latest := uint64(0)
	if s.nextSeq > 1 {
		latest = s.nextSeq - 1
	}
	return VideoStoreStats{
		BytesStored:  uint64(s.diskBytes),
		SegmentCount: len(s.segments),
		LastNALMS:    s.lastNALMS,
		LatestSeq:    latest,
	}
}

func (s *VideoStore) Close() error {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return nil
	}
	s.closed = true
	s.cond.Broadcast()
	err := s.flushActiveLocked()
	// The last GOP has no following keyframe to close it, so persist it here
	// or the tail of every recording is unreachable by pointer.
	if e := s.vpt.flushAll(); e != nil && err == nil {
		err = e
	}
	if s.active != nil {
		if s.active.dataFile != nil {
			if e := s.active.dataFile.Close(); e != nil && err == nil {
				err = e
			}
			s.active.dataFile = nil
		}
		if s.active.idxFile != nil {
			if e := s.active.idxFile.Close(); e != nil && err == nil {
				err = e
			}
			s.active.idxFile = nil
		}
	}
	for _, seg := range s.segments {
		if seg.readFile != nil {
			_ = seg.readFile.Close()
			seg.readFile = nil
		}
	}
	s.mu.Unlock()
	return err
}

func (s *VideoStore) recover() error {
	entries, err := os.ReadDir(s.dir)
	if err != nil {
		return err
	}
	var ids []uint64
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		var id uint64
		if _, err := fmt.Sscanf(entry.Name(), "%012d.h264", &id); err == nil {
			ids = append(ids, id)
		}
	}
	sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
	for _, id := range ids {
		if err := s.recoverSegment(id); err != nil {
			return err
		}
		if id >= s.nextSegID {
			s.nextSegID = id + 1
		}
	}
	return nil
}

func (s *VideoStore) recoverSegment(id uint64) error {
	seg := &videoSegment{
		id:       id,
		dataPath: videoDataPath(s.dir, id),
		idxPath:  videoIndexPath(s.dir, id),
	}
	data, err := os.ReadFile(seg.dataPath)
	if err != nil {
		return err
	}
	records, header, idxOK := readIndexFile(seg.idxPath)
	if idxOK {
		seg.startMs = header.startMs
		seg.endMs = header.endMs
		seg.sps = header.sps
		seg.pps = header.pps
		if len(seg.sps) > 0 {
			s.sps = cloneBytes(seg.sps)
		}
		if len(seg.pps) > 0 {
			s.pps = cloneBytes(seg.pps)
		}
	}

	covered := int64(0)
	if len(records) > 0 {
		last := records[len(records)-1]
		covered = int64(last.Offset) + int64(last.Size)
	}

	if int64(len(data)) > covered {
		tail := splitAnnexB(data[covered:])
		ts := uint64(0)
		if len(records) > 0 {
			ts = records[len(records)-1].TimestampMs
		} else if info, err := os.Stat(seg.dataPath); err == nil {
			ts = uint64(info.ModTime().UnixMilli())
		}
		for _, nal := range tail {
			ts += videoRecoverFrameStepMS
			nalType := nalTypeOfAnnexB(nal)
			rec := videoNALRecord{
				TimestampMs: ts,
				Offset:      uint64(covered),
				Size:        uint32(len(nal)),
				Flags:       flagsForAnnexBNAL(nal),
				NALType:     nalType,
			}
			records = append(records, rec)
			covered += int64(len(nal))
			if nalType == nalTypeSPS {
				seg.sps = cloneBytes(nal)
				s.sps = cloneBytes(nal)
			}
			if nalType == nalTypePPS {
				seg.pps = cloneBytes(nal)
				s.pps = cloneBytes(nal)
			}
		}
		if err := rewriteIndex(seg.idxPath, indexHeader{
			startMs: firstRecordTS(records),
			endMs:   lastRecordTS(records),
			sps:     seg.sps,
			pps:     seg.pps,
		}, records); err != nil {
			return err
		}
		if len(records) > 0 {
			seg.startMs = firstRecordTS(records)
			seg.endMs = lastRecordTS(records)
		}
	} else if !idxOK && len(data) > 0 {
		nals := splitAnnexB(data)
		ts := uint64(0)
		if info, err := os.Stat(seg.dataPath); err == nil {
			ts = uint64(info.ModTime().UnixMilli())
		}
		offset := int64(0)
		for _, nal := range nals {
			nalType := nalTypeOfAnnexB(nal)
			records = append(records, videoNALRecord{
				TimestampMs: ts,
				Offset:      uint64(offset),
				Size:        uint32(len(nal)),
				Flags:       flagsForAnnexBNAL(nal),
				NALType:     nalType,
			})
			if nalType == nalTypeSPS {
				seg.sps = cloneBytes(nal)
				s.sps = cloneBytes(nal)
			}
			if nalType == nalTypePPS {
				seg.pps = cloneBytes(nal)
				s.pps = cloneBytes(nal)
			}
			offset += int64(len(nal))
			ts += videoRecoverFrameStepMS
		}
		if err := rewriteIndex(seg.idxPath, indexHeader{
			startMs: firstRecordTS(records),
			endMs:   lastRecordTS(records),
			sps:     seg.sps,
			pps:     seg.pps,
		}, records); err != nil {
			return err
		}
		if len(records) > 0 {
			seg.startMs = firstRecordTS(records)
			seg.endMs = lastRecordTS(records)
		}
	}

	seg.size = int64(len(data))
	s.diskBytes += seg.size
	if len(seg.sps) > 0 {
		s.sps = cloneBytes(seg.sps)
	}
	if len(seg.pps) > 0 {
		s.pps = cloneBytes(seg.pps)
	}
	s.segments[id] = seg

	for _, rec := range records {
		// Old indexes marked every IDR slice as a keyframe. Normalize in
		// memory from authoritative payload bytes before rebuilding the VPT.
		if rec.NALType == nalTypeIDR && rec.Size > 0 && rec.Offset+uint64(rec.Size) <= uint64(len(data)) {
			rec.Flags = (rec.Flags &^ VideoFlagKeyframe) |
				(flagsForAnnexBNAL(data[rec.Offset:rec.Offset+uint64(rec.Size)]) & VideoFlagKeyframe)
		}
		entry := videoIndexEntry{
			seq:          s.nextSeq,
			segmentID:    id,
			timestampMs:  rec.TimestampMs,
			offset:       int64(rec.Offset),
			size:         int32(rec.Size),
			flags:        rec.Flags,
			nalType:      rec.NALType,
			clockProv:    rec.ClockProv,
			rtpTimestamp: rec.RTPTimestamp,
			receiveMs:    rec.ReceiveMs,
		}
		s.nextSeq++
		s.entries = append(s.entries, entry)
		if rec.Flags&VideoFlagKeyframe != 0 {
			s.keyframes = append(s.keyframes, len(s.entries)-1)
		}
		if rec.TimestampMs > s.lastNALMS {
			s.lastNALMS = rec.TimestampMs
		}
		if rec.NALType == nalTypeSPS && rec.Size > 0 && int(rec.Offset)+int(rec.Size) <= len(data) {
			s.sps = cloneBytes(data[rec.Offset : int(rec.Offset)+int(rec.Size)])
		}
		if rec.NALType == nalTypePPS && rec.Size > 0 && int(rec.Offset)+int(rec.Size) <= len(data) {
			s.pps = cloneBytes(data[rec.Offset : int(rec.Offset)+int(rec.Size)])
		}
	}
	return nil
}

func (s *VideoStore) openActive() error {
	var id uint64 = 1
	if s.nextSegID > 1 {
		id = s.nextSegID - 1
	}
	existing := s.segments[id]
	if existing != nil && (existing.size >= videoSegmentMaxBytes ||
		(existing.startMs != 0 && existing.endMs >= existing.startMs && existing.endMs-existing.startMs >= videoSegmentMaxDurMS)) {
		id++
		existing = nil
	}
	if existing == nil {
		seg, err := createVideoSegment(s.dir, id)
		if err != nil {
			return err
		}
		s.segments[id] = seg
		existing = seg
	} else {
		dataFile, err := os.OpenFile(existing.dataPath, os.O_CREATE|os.O_RDWR|os.O_APPEND, 0o644)
		if err != nil {
			return err
		}
		idxFile, err := os.OpenFile(existing.idxPath, os.O_CREATE|os.O_RDWR, 0o644)
		if err != nil {
			_ = dataFile.Close()
			return err
		}
		if _, err := idxFile.Seek(0, io.SeekEnd); err != nil {
			_ = dataFile.Close()
			_ = idxFile.Close()
			return err
		}
		existing.dataFile = dataFile
		existing.idxFile = idxFile
		existing.active = true
	}
	if existing.idxFile == nil || existing.dataFile == nil {
		// createVideoSegment already opened files
	}
	s.active = existing
	if id+1 > s.nextSegID {
		s.nextSegID = id + 1
	}
	return nil
}

func (s *VideoStore) flushActiveLocked() error {
	if s.active == nil {
		return nil
	}
	if s.active.dataFile != nil {
		if err := s.active.dataFile.Sync(); err != nil {
			return err
		}
	}
	if s.active.idxFile == nil {
		return nil
	}
	header := indexHeader{
		startMs: s.active.startMs,
		endMs:   s.active.endMs,
		sps:     s.active.sps,
		pps:     s.active.pps,
	}
	if len(header.sps) == 0 {
		header.sps = s.sps
	}
	if len(header.pps) == 0 {
		header.pps = s.pps
	}
	if err := writeIndexHeader(s.active.idxFile, header, s.segmentRecordCountLocked(s.active.id)); err != nil {
		return err
	}
	return s.active.idxFile.Sync()
}

func (s *VideoStore) segmentRecordCountLocked(id uint64) uint32 {
	var n uint32
	for i := range s.entries {
		if s.entries[i].segmentID == id {
			n++
		}
	}
	return n
}

func (s *VideoStore) readPayloadLocked(entry videoIndexEntry) ([]byte, error) {
	if payload, ok := s.hot[entry.seq]; ok {
		return cloneBytes(payload), nil
	}
	seg, ok := s.segments[entry.segmentID]
	if !ok {
		return nil, fmt.Errorf("video segment %d is missing", entry.segmentID)
	}
	file, err := s.segmentReadFileLocked(seg)
	if err != nil {
		return nil, err
	}
	buf := make([]byte, entry.size)
	if _, err := file.ReadAt(buf, entry.offset); err != nil {
		return nil, err
	}
	return buf, nil
}

func (s *VideoStore) segmentReadFileLocked(seg *videoSegment) (*os.File, error) {
	if seg.active && seg.dataFile != nil {
		return seg.dataFile, nil
	}
	if seg.readFile != nil {
		return seg.readFile, nil
	}
	file, err := os.Open(seg.dataPath)
	if err != nil {
		return nil, err
	}
	seg.readFile = file
	return file, nil
}

func (s *VideoStore) addHotLocked(seq uint64, payload []byte) {
	cp := cloneBytes(payload)
	s.hot[seq] = cp
	s.hotOrder = append(s.hotOrder, seq)
	s.hotBytes += int64(len(cp))
	for s.hotBytes > videoHotTailBytes && len(s.hotOrder) > 0 {
		oldest := s.hotOrder[0]
		s.hotOrder = s.hotOrder[1:]
		if p, ok := s.hot[oldest]; ok {
			s.hotBytes -= int64(len(p))
			delete(s.hot, oldest)
		}
	}
}

type indexHeader struct {
	startMs uint64
	endMs   uint64
	sps     []byte
	pps     []byte
}

func createVideoSegment(dir string, id uint64) (*videoSegment, error) {
	seg := &videoSegment{
		id:       id,
		dataPath: videoDataPath(dir, id),
		idxPath:  videoIndexPath(dir, id),
		active:   true,
	}
	dataFile, err := os.OpenFile(seg.dataPath, os.O_CREATE|os.O_RDWR|os.O_APPEND, 0o644)
	if err != nil {
		return nil, err
	}
	idxFile, err := os.OpenFile(seg.idxPath, os.O_CREATE|os.O_RDWR, 0o644)
	if err != nil {
		_ = dataFile.Close()
		return nil, err
	}
	info, err := idxFile.Stat()
	if err != nil {
		_ = dataFile.Close()
		_ = idxFile.Close()
		return nil, err
	}
	if info.Size() == 0 {
		if err := writeIndexHeader(idxFile, indexHeader{}, 0); err != nil {
			_ = dataFile.Close()
			_ = idxFile.Close()
			return nil, err
		}
	} else if _, err := idxFile.Seek(0, io.SeekEnd); err != nil {
		_ = dataFile.Close()
		_ = idxFile.Close()
		return nil, err
	}
	seg.dataFile = dataFile
	seg.idxFile = idxFile
	return seg, nil
}

func writeIndexHeader(file *os.File, header indexHeader, nalCount uint32) error {
	buf := make([]byte, videoIndexHeaderSize)
	binary.BigEndian.PutUint32(buf[0:4], videoIndexMagic)
	binary.BigEndian.PutUint16(buf[4:6], videoIndexVersion)
	binary.BigEndian.PutUint16(buf[6:8], uint16(videoIndexHeaderSize))
	// Record width is explicit from v2 on, so a reader does not have to infer
	// it from the version number.
	binary.BigEndian.PutUint16(buf[28:30], uint16(videoNALRecordSizeV2))
	binary.BigEndian.PutUint64(buf[8:16], header.startMs)
	binary.BigEndian.PutUint64(buf[16:24], header.endMs)
	binary.BigEndian.PutUint32(buf[24:28], nalCount)
	sps := header.sps
	pps := header.pps
	if len(sps) > videoSPSCap {
		sps = sps[:videoSPSCap]
	}
	if len(pps) > videoPPSCap {
		pps = pps[:videoPPSCap]
	}
	binary.BigEndian.PutUint16(buf[32:34], uint16(len(sps)))
	binary.BigEndian.PutUint16(buf[34:36], uint16(len(pps)))
	copy(buf[48:48+videoSPSCap], sps)
	copy(buf[48+videoSPSCap:48+videoSPSCap+videoPPSCap], pps)
	if _, err := file.WriteAt(buf, 0); err != nil {
		return err
	}
	if _, err := file.Seek(0, io.SeekEnd); err != nil {
		return err
	}
	return nil
}

func writeNALRecord(w io.Writer, rec videoNALRecord) error {
	var buf [videoNALRecordSizeV2]byte
	encodeNALRecord(buf[:], rec)
	_, err := w.Write(buf[:])
	return err
}

func encodeNALRecord(dst []byte, rec videoNALRecord) {
	binary.BigEndian.PutUint64(dst[0:8], rec.TimestampMs)
	binary.BigEndian.PutUint64(dst[8:16], rec.Offset)
	binary.BigEndian.PutUint32(dst[16:20], rec.Size)
	dst[20] = rec.Flags
	dst[21] = rec.NALType
	dst[22] = rec.ClockProv
	dst[23] = 0
	binary.BigEndian.PutUint32(dst[24:28], rec.RTPTimestamp)
	binary.BigEndian.PutUint32(dst[28:32], 0)
	binary.BigEndian.PutUint64(dst[32:40], rec.ReceiveMs)
}

func decodeNALRecord(src []byte, recordBytes int) videoNALRecord {
	rec := videoNALRecord{
		TimestampMs: binary.BigEndian.Uint64(src[0:8]),
		Offset:      binary.BigEndian.Uint64(src[8:16]),
		Size:        binary.BigEndian.Uint32(src[16:20]),
		Flags:       src[20],
		NALType:     src[21],
	}
	if recordBytes >= videoNALRecordSizeV2 {
		rec.ClockProv = src[22]
		rec.RTPTimestamp = binary.BigEndian.Uint32(src[24:28])
		rec.ReceiveMs = binary.BigEndian.Uint64(src[32:40])
	} else {
		// A v1 record only ever held receive time.
		rec.ClockProv = clockProvReceive
		rec.ReceiveMs = rec.TimestampMs
	}
	return rec
}

func readIndexFile(path string) ([]videoNALRecord, indexHeader, bool) {
	data, err := os.ReadFile(path)
	if err != nil || len(data) < videoIndexHeaderSize {
		return nil, indexHeader{}, false
	}
	if binary.BigEndian.Uint32(data[0:4]) != videoIndexMagic {
		return nil, indexHeader{}, false
	}
	version := binary.BigEndian.Uint16(data[4:6])
	if version == 0 || version > videoIndexVersion {
		return nil, indexHeader{}, false
	}
	headerBytes := int(binary.BigEndian.Uint16(data[6:8]))
	if headerBytes < 48 || headerBytes > len(data) {
		headerBytes = videoIndexHeaderSize
	}
	recordBytes := videoNALRecordSizeV1
	if version >= 2 {
		recordBytes = int(binary.BigEndian.Uint16(data[28:30]))
		if recordBytes < videoNALRecordSizeV2 {
			recordBytes = videoNALRecordSizeV2
		}
	}
	header := indexHeader{
		startMs: binary.BigEndian.Uint64(data[8:16]),
		endMs:   binary.BigEndian.Uint64(data[16:24]),
	}
	spsLen := int(binary.BigEndian.Uint16(data[32:34]))
	ppsLen := int(binary.BigEndian.Uint16(data[34:36]))
	if spsLen > 0 && 48+spsLen <= len(data) && spsLen <= videoSPSCap {
		header.sps = cloneBytes(data[48 : 48+spsLen])
	}
	ppsOff := 48 + videoSPSCap
	if ppsLen > 0 && ppsOff+ppsLen <= len(data) && ppsLen <= videoPPSCap {
		header.pps = cloneBytes(data[ppsOff : ppsOff+ppsLen])
	}

	var records []videoNALRecord
	for off := headerBytes; off+recordBytes <= len(data); off += recordBytes {
		records = append(records, decodeNALRecord(data[off:off+recordBytes], recordBytes))
	}
	return records, header, true
}

func rewriteIndex(path string, header indexHeader, records []videoNALRecord) error {
	file, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR|os.O_TRUNC, 0o644)
	if err != nil {
		return err
	}
	defer file.Close()
	if err := writeIndexHeader(file, header, uint32(len(records))); err != nil {
		return err
	}
	for _, rec := range records {
		if err := writeNALRecord(file, rec); err != nil {
			return err
		}
	}
	return file.Sync()
}

func firstRecordTS(records []videoNALRecord) uint64 {
	if len(records) == 0 {
		return 0
	}
	return records[0].TimestampMs
}

func lastRecordTS(records []videoNALRecord) uint64 {
	if len(records) == 0 {
		return 0
	}
	return records[len(records)-1].TimestampMs
}

func videoDataPath(dir string, id uint64) string {
	return filepath.Join(dir, fmt.Sprintf("%012d.h264", id))
}

func videoIndexPath(dir string, id uint64) string {
	return filepath.Join(dir, fmt.Sprintf("%012d.idx", id))
}

func cloneBytes(in []byte) []byte {
	if len(in) == 0 {
		return nil
	}
	out := make([]byte, len(in))
	copy(out, in)
	return out
}

func videoNowMS() uint64 {
	return uint64(time.Now().UnixMilli())
}

// video_locate and byte-range reads (tsdb-architecture.md §8.2).
//
// Returning byte ranges instead of frames is what makes seeking seamless: the
// client gets the entry GOP for a timestamp plus the pointers that follow, and
// walks forward through the byte stream instead of tearing down the connection
// and re-subscribing.

// VideoLocateResult is one answer to "where does this timestamp live".
type VideoLocateResult struct {
	Found bool

	// Entry is the GOP containing AtMs: the last keyframe at or before it.
	// A decoder starts here and discards output before AtMs.
	Entry GopRow
	// Prefetch are the GOPs immediately after Entry, so the client can request
	// ahead instead of stalling at each GOP boundary.
	Prefetch []GopRow
	Coverage []SegmentCoverage

	SPS []byte
	PPS []byte

	// ClockOffsetMs is the manual nudge a client must add to stored video
	// timestamps before comparing them to telemetry timestamps.
	ClockOffsetMs int64
	// ClockProv is how the entry GOP's own timestamps were derived. Anything
	// below clockProvRTCP means A/V sync is unverified (§8.5).
	ClockProv uint8
	// ResidualMs is mapped wall clock minus raw receive time for the entry
	// keyframe: network delay plus sender/receiver clock disagreement. §8.5
	// item 4 asks for this to be recorded rather than assumed away.
	ResidualMs int64

	TotalRows uint32
	MinMs     uint64
	MaxMs     uint64
}

// Locate maps a timestamp to a byte offset and the containing GOP.
func (s *VideoStore) Locate(streamID uint8, atMs uint64, prefetch int) VideoLocateResult {
	s.mu.Lock()
	defer s.mu.Unlock()

	rows := s.vpt.snapshot()
	lo, hi := s.vpt.bounds()
	out := VideoLocateResult{
		ClockOffsetMs: s.vpt.clockOffset,
		TotalRows:     uint32(len(rows)),
		MinMs:         lo,
		MaxMs:         hi,
		ClockProv:     s.vpt.clockProv,
	}
	// Publish coverage even on a miss. Historical requests after EOF must
	// not masquerade as successful reads of the final (often partial) GOP.
	out.Coverage = coverageOf(rows, streamID, lo, hi)
	streamLo, streamHi := streamBounds(rows, streamID)
	if streamLo == streamHi {
		return out
	}
	if atMs != 0 && atMs > rows[streamHi-1].EndMs {
		return out
	}
	// atMs == 0 is the live probe ("newest GOP"), not "before the epoch".
	// locateEntry(0) used to return the earliest keyframe, so Live View
	// froze on the first still and every later t=0 locate was the same GOP.
	idx := -1
	if atMs == 0 {
		idx = lastKeyframe(rows, streamID)
	} else {
		idx = locateEntry(rows, streamID, atMs)
	}
	if idx < 0 {
		return out
	}
	entry := rows[idx]
	// locateEntry is "last keyframe at or before atMs". Across a recording
	// hole that is the final GOP of the previous clip, which is what made
	// Sep 7 11:16 catalog pins replay the Sep 6 22:50 0.45s tail after the
	// camera resumed. Consecutive GOPs are 1–2 s apart; anything farther
	// is a gap, not "hold the last picture".
	if atMs != 0 && atMs > entry.EndMs+videoLocateHoldMs {
		next := nextKeyframe(rows, idx, streamID)
		if next < 0 || rows[next].StartMs > atMs+videoLocateHoldMs {
			return out
		}
	}
	out.Found = true
	out.Entry = entry

	if prefetch > 0 {
		_, streamHi := streamBounds(rows, streamID)
		end := idx + 1 + prefetch
		if end > streamHi {
			end = streamHi
		}
		if end > idx+1 {
			out.Prefetch = append(out.Prefetch, rows[idx+1:end]...)
		}
	}

	// Coverage spans the whole table rather than the requested window: §8.2
	// says return the coverage map for the range plus the single entry
	// pointer, and at segment granularity the whole table is ~240 rows for 4 h.
	out.Coverage = coverageOf(rows, streamID, lo, hi)

	// SPS/PPS as of the entry keyframe, so a decoder configured from this
	// reply matches the bytes it is about to be fed.
	if e := s.entryIndexInRowLocked(out.Entry); e >= 0 {
		out.SPS = s.configForEntryLocked(e, nalTypeSPS)
		out.PPS = s.configForEntryLocked(e, nalTypePPS)
		out.ClockProv = s.entries[e].clockProv
		out.ResidualMs = int64(s.entries[e].timestampMs) - int64(s.entries[e].receiveMs)
	} else {
		out.SPS = cloneBytes(s.sps)
		out.PPS = cloneBytes(s.pps)
	}
	return out
}

// entryIndexInRowLocked finds the keyframe NAL inside a GOP row.
//
// Entries are appended in (segment, offset) order, so this is a binary search
// rather than a scan.
func (s *VideoStore) entryIndexInRowLocked(row GopRow) int {
	seg := uint64(row.SegmentID)
	start := int64(row.ByteOffset)
	end := start + int64(row.ByteLength)
	i := sort.Search(len(s.entries), func(k int) bool {
		e := s.entries[k]
		if e.segmentID != seg {
			return e.segmentID > seg
		}
		return e.offset >= start
	})
	for ; i < len(s.entries); i++ {
		e := s.entries[i]
		if e.segmentID != seg || e.offset >= end {
			break
		}
		if e.flags&VideoFlagKeyframe != 0 {
			return i
		}
	}
	return -1
}

// ReadRange returns a byte span from a segment so a pointer can be fetched
// directly, without a keyframe search (§8.2 transport change 2).
func (s *VideoStore) ReadRange(segmentID uint32, offset uint64, length uint32) ([]byte, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if length == 0 {
		return nil, nil
	}
	if length > videoRangeMaxBytes {
		return nil, fmt.Errorf("range of %d bytes exceeds the %d byte limit", length, videoRangeMaxBytes)
	}
	seg, ok := s.segments[uint64(segmentID)]
	if !ok {
		return nil, fmt.Errorf("video segment %d is missing", segmentID)
	}
	if int64(offset) >= seg.size {
		return nil, fmt.Errorf("offset %d is past the end of segment %d (%d bytes)", offset, segmentID, seg.size)
	}
	// Clamp rather than fail: the tail GOP of an active segment grows, so a
	// pointer fetched a moment ago can name more bytes than are on disk.
	avail := seg.size - int64(offset)
	if int64(length) > avail {
		length = uint32(avail)
	}
	file, err := s.segmentReadFileLocked(seg)
	if err != nil {
		return nil, err
	}
	buf := make([]byte, length)
	if _, err := file.ReadAt(buf, int64(offset)); err != nil && !errors.Is(err, io.EOF) {
		return nil, err
	}
	return buf, nil
}

// PointerRows exposes the table for tests and diagnostics.
func (s *VideoStore) PointerRows() []GopRow {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.vpt.snapshot()
}

// rebuildVptLocked regenerates every row from the .idx-derived entries.
//
// The table is derived state, so this is both the recovery path and the
// self-heal for a truncated or deleted video.vpt.
func (s *VideoStore) rebuildVptLocked() {
	rows := s.vpt.rows
	_ = rows
	path := s.vpt.path
	offset := s.vpt.clockOffset
	prov := s.vpt.clockProv
	s.vpt.reset(path)
	s.vpt.clockOffset = offset
	s.vpt.clockProv = prov
	s.vpt.persisted = 0
	for _, e := range s.entries {
		s.vpt.add(videoDefaultStreamID, uint32(e.segmentID), e.timestampMs, uint64(e.offset), uint32(e.size),
			e.flags, e.nalType)
	}
	// Truncate the file so stale rows behind the rebuilt set cannot survive.
	if path != "" {
		if err := os.Truncate(path, 0); err != nil && !os.IsNotExist(err) {
			// Non-fatal: flush() rewrites the header and truncates to the new
			// length anyway. Reported so a broken table is not silent.
			fmt.Printf("[photon-server] video.vpt truncate failed: %v\n", err)
		}
	}
}

// backfillClockLocked re-derives wall clock for NALs already stored with
// receive time, now that an RTCP mapping exists.
//
// Bounded to the active segment: an RTCP sender report arrives within seconds
// of the stream starting, so the receive-stamped head of a recording is short.
// Without this, the first segment of every recording carries unknown skew even
// though the information needed to fix it was recorded alongside it.
func (s *VideoStore) backfillClockLocked() error {
	s.backfilled = true
	if s.clock == nil || s.active == nil {
		return nil
	}
	activeID := s.active.id
	changed := 0
	for i := range s.entries {
		e := &s.entries[i]
		if e.segmentID != activeID || e.clockProv != clockProvReceive || e.rtpTimestamp == 0 {
			continue
		}
		mapped, prov := s.clock.WallMs(e.rtpTimestamp, e.receiveMs)
		if prov != clockProvRTCP {
			continue
		}
		e.timestampMs = mapped
		e.clockProv = prov
		changed++
	}
	if changed == 0 {
		return nil
	}
	// Rewrite the segment's records so the correction survives a restart.
	if err := s.rewriteActiveIndexLocked(); err != nil {
		return err
	}
	s.rebuildVptLocked()
	fmt.Printf("[photon-server] video clock backfill: %d NALs in segment %d re-stamped from RTP\n",
		changed, activeID)
	return nil
}

func (s *VideoStore) rewriteActiveIndexLocked() error {
	if s.active == nil || s.active.idxFile == nil {
		return nil
	}
	activeID := s.active.id
	var records []videoNALRecord
	for _, e := range s.entries {
		if e.segmentID != activeID {
			continue
		}
		records = append(records, videoNALRecord{
			TimestampMs:  e.timestampMs,
			Offset:       uint64(e.offset),
			Size:         uint32(e.size),
			Flags:        e.flags,
			NALType:      e.nalType,
			ClockProv:    e.clockProv,
			RTPTimestamp: e.rtpTimestamp,
			ReceiveMs:    e.receiveMs,
		})
	}
	buf := make([]byte, len(records)*videoNALRecordSizeV2)
	for i, rec := range records {
		encodeNALRecord(buf[i*videoNALRecordSizeV2:(i+1)*videoNALRecordSizeV2], rec)
	}
	if _, err := s.active.idxFile.WriteAt(buf, int64(videoIndexHeaderSize)); err != nil {
		return err
	}
	if _, err := s.active.idxFile.Seek(0, io.SeekEnd); err != nil {
		return err
	}
	// Segment bounds move with the timestamps.
	if len(records) > 0 {
		s.active.startMs = records[0].TimestampMs
		s.active.endMs = records[len(records)-1].TimestampMs
		for _, rec := range records {
			if rec.TimestampMs < s.active.startMs {
				s.active.startMs = rec.TimestampMs
			}
			if rec.TimestampMs > s.active.endMs {
				s.active.endMs = rec.TimestampMs
			}
		}
		s.lastNALMS = s.active.endMs
	}
	return nil
}

// video.vpt — the video pointer table (tsdb-architecture.md §8.1).
//
// One row per GOP, built live as NALs arrive rather than by a post-processing
// pass. A row is the byte span a decoder needs to start at a keyframe and play
// forward, so a timestamp can be turned into a seek without scanning the
// stream.
//
// The table is derived state: it is rebuilt from the authoritative per-segment
// .idx records on recovery, so deleting it loses nothing.
const (
	vptMagic      = uint32(0x56505431) // "VPT1"
	vptVersion    = uint16(1)
	vptHeaderSize = 64
	vptRowSize    = 40

	// Row flags.
	//
	// vptFlagConfigInline says the byte span begins with SPS/PPS, so a client
	// that fetches only this range can configure a decoder from it.
	vptFlagConfigInline = uint8(1 << 0)
	// vptFlagContinuation says the row does not begin at a keyframe. A GOP
	// split across a segment boundary (which the 128 MiB / 120 s hard rotation
	// cap can force) yields a keyframe row plus one continuation row. A
	// continuation row is a legal byte range to read but not a legal place to
	// start decoding, so Locate never returns one as an entry.
	vptFlagContinuation = uint8(1 << 1)
)

// Clock provenance for video timestamps (§8.5). Ordered worst to best so a
// caller can compare: anything below clockProvRTCP means A/V sync is unverified.
const (
	clockProvReceive = uint8(0)
	clockProvManual  = uint8(1)
	clockProvNTP     = uint8(2)
	clockProvRTCP    = uint8(3)
)

func clockProvenanceName(p uint8) string {
	switch p {
	case clockProvRTCP:
		return "rtcp"
	case clockProvNTP:
		return "ntp"
	case clockProvManual:
		return "manual"
	default:
		return "receive"
	}
}

// GopRow is the 40-byte on-disk pointer row. Field order and offsets are fixed
// by §8.1; do not reorder.
type GopRow struct {
	StartMs    uint64 // keyframe timestamp
	EndMs      uint64 // last NAL before the next keyframe
	ByteOffset uint64 // into the segment's .h264
	ByteLength uint32
	SegmentID  uint32
	NalCount   uint32
	StreamID   uint8
	Flags      uint8
}

func (r GopRow) IsContinuation() bool { return r.Flags&vptFlagContinuation != 0 }

func encodeGopRow(dst []byte, r GopRow) {
	binary.BigEndian.PutUint64(dst[0:8], r.StartMs)
	binary.BigEndian.PutUint64(dst[8:16], r.EndMs)
	binary.BigEndian.PutUint64(dst[16:24], r.ByteOffset)
	binary.BigEndian.PutUint32(dst[24:28], r.ByteLength)
	binary.BigEndian.PutUint32(dst[28:32], r.SegmentID)
	binary.BigEndian.PutUint32(dst[32:36], r.NalCount)
	dst[36] = r.StreamID
	dst[37] = r.Flags
	binary.BigEndian.PutUint16(dst[38:40], 0)
}

// vptHeader is the 64-byte file header. It carries the clock offset and its
// provenance so a reader of the table alone knows whether video timestamps are
// comparable to telemetry timestamps (§8.5).
type vptHeader struct {
	rowCount      uint32
	minMs         uint64
	maxMs         uint64
	clockOffsetMs int64
	clockProv     uint8
}

func encodeVptHeader(h vptHeader) []byte {
	buf := make([]byte, vptHeaderSize)
	binary.BigEndian.PutUint32(buf[0:4], vptMagic)
	binary.BigEndian.PutUint16(buf[4:6], vptVersion)
	binary.BigEndian.PutUint16(buf[6:8], uint16(vptHeaderSize))
	binary.BigEndian.PutUint16(buf[8:10], uint16(vptRowSize))
	binary.BigEndian.PutUint32(buf[12:16], h.rowCount)
	binary.BigEndian.PutUint64(buf[16:24], h.minMs)
	binary.BigEndian.PutUint64(buf[24:32], h.maxMs)
	binary.BigEndian.PutUint64(buf[32:40], uint64(h.clockOffsetMs))
	buf[40] = h.clockProv
	return buf
}

// vptBuilder turns the NAL stream into GOP rows as it arrives.
//
// It is fed in append order and holds exactly one open row: the GOP that the
// next keyframe will close. The open row is visible to queries immediately but
// is only appended to disk once closed, because its length and end timestamp
// are still growing.
type vptBuilder struct {
	rows []GopRow

	open      bool
	cur       GopRow
	persisted int // rows already appended to the file

	// Trailing run of config NALs (SPS/PPS) seen since the last keyframe.
	// A keyframe pulls them into its byte span so the GOP is independently
	// decodable, which is what vptFlagConfigInline advertises.
	cfgRun      bool
	cfgSegment  uint32
	cfgOffset   uint64
	cfgLength   uint32
	cfgCount    uint32
	cfgStartMs  uint64
	path        string
	clockOffset int64
	clockProv   uint8
	dirty       bool
}

func (b *vptBuilder) reset(path string) {
	b.rows = nil
	b.open = false
	b.cur = GopRow{}
	b.persisted = 0
	b.clearConfigRun()
	b.path = path
	b.dirty = true
}

func (b *vptBuilder) clearConfigRun() {
	b.cfgRun = false
	b.cfgSegment = 0
	b.cfgOffset = 0
	b.cfgLength = 0
	b.cfgCount = 0
	b.cfgStartMs = 0
}

func (b *vptBuilder) closeOpen() {
	if !b.open {
		return
	}
	b.rows = append(b.rows, b.cur)
	b.open = false
	b.cur = GopRow{}
	b.dirty = true
}

// add folds one appended NAL into the table.
func (b *vptBuilder) add(streamID uint8, segmentID uint32, timestampMs uint64, offset uint64, size uint32,
	flags uint8, nalType uint8) {

	if flags&VideoFlagConfig != 0 {
		// Extend or start the trailing config run, but only while it stays
		// contiguous in one segment. A gap means the bytes cannot be handed
		// out as a single range.
		if b.cfgRun && b.cfgSegment == segmentID && b.cfgOffset+uint64(b.cfgLength) == offset {
			b.cfgLength += size
			b.cfgCount++
		} else {
			b.cfgRun = true
			b.cfgSegment = segmentID
			b.cfgOffset = offset
			b.cfgLength = size
			b.cfgCount = 1
			b.cfgStartMs = timestampMs
		}
		// Config NALs also belong to whatever GOP is open, so its byte span
		// stays contiguous with the stream. EndMs deliberately does not move:
		// a config NAL that turns out to precede the next keyframe gets pulled
		// into that keyframe's row, and if EndMs had followed it the two rows
		// would overlap and a timestamp on the boundary would be in both.
		if b.open && b.cur.SegmentID == segmentID &&
			b.cur.ByteOffset+uint64(b.cur.ByteLength) == offset {
			b.cur.ByteLength += size
			b.cur.NalCount++
		}
		_ = nalType
		return
	}

	if flags&VideoFlagKeyframe != 0 {
		start := offset
		length := size
		nals := uint32(1)
		rowFlags := uint8(0)
		startMs := timestampMs
		// Pull an immediately preceding, contiguous config run into this GOP.
		if b.cfgRun && b.cfgSegment == segmentID && b.cfgOffset+uint64(b.cfgLength) == offset {
			// Those bytes may already be counted in the GOP we are about to
			// close; trim them off it so no byte is claimed twice.
			if b.open && b.cur.SegmentID == segmentID &&
				b.cur.ByteLength >= b.cfgLength &&
				b.cur.ByteOffset+uint64(b.cur.ByteLength) == offset {
				b.cur.ByteLength -= b.cfgLength
				if b.cur.NalCount >= b.cfgCount {
					b.cur.NalCount -= b.cfgCount
				}
			}
			start = b.cfgOffset
			length += b.cfgLength
			nals += b.cfgCount
			rowFlags |= vptFlagConfigInline
			if b.cfgStartMs < startMs {
				startMs = b.cfgStartMs
			}
		}
		b.closeOpen()
		b.clearConfigRun()
		b.open = true
		b.cur = GopRow{
			StartMs:    startMs,
			EndMs:      timestampMs,
			ByteOffset: start,
			ByteLength: length,
			SegmentID:  segmentID,
			NalCount:   nals,
			StreamID:   streamID,
			Flags:      rowFlags,
		}
		return
	}

	b.clearConfigRun()

	// Non-keyframe. Extend the open GOP when it stays contiguous, otherwise
	// start a continuation row: a forced rotation can move the rest of a GOP
	// into the next segment, and those bytes still have to be addressable.
	if b.open && b.cur.SegmentID == segmentID &&
		b.cur.ByteOffset+uint64(b.cur.ByteLength) == offset {
		b.cur.ByteLength += size
		b.cur.NalCount++
		if timestampMs > b.cur.EndMs {
			b.cur.EndMs = timestampMs
		}
		return
	}
	prevStream := streamID
	if b.open {
		prevStream = b.cur.StreamID
	}
	b.closeOpen()
	b.open = true
	b.cur = GopRow{
		StartMs:    timestampMs,
		EndMs:      timestampMs,
		ByteOffset: offset,
		ByteLength: size,
		SegmentID:  segmentID,
		NalCount:   1,
		StreamID:   prevStream,
		Flags:      vptFlagContinuation,
	}
}

// snapshot returns every row including the still-growing open one, sorted by
// (streamId, startMs) as §8.1 requires. Rows arrive in time order for a single
// stream, so the sort only does work once multi-camera is real.
func (b *vptBuilder) snapshot() []GopRow {
	out := make([]GopRow, 0, len(b.rows)+1)
	out = append(out, b.rows...)
	if b.open {
		out = append(out, b.cur)
	}
	if !sort.SliceIsSorted(out, func(i, j int) bool {
		if out[i].StreamID != out[j].StreamID {
			return out[i].StreamID < out[j].StreamID
		}
		return out[i].StartMs < out[j].StartMs
	}) {
		sort.SliceStable(out, func(i, j int) bool {
			if out[i].StreamID != out[j].StreamID {
				return out[i].StreamID < out[j].StreamID
			}
			return out[i].StartMs < out[j].StartMs
		})
	}
	return out
}

func (b *vptBuilder) bounds() (uint64, uint64) {
	var lo, hi uint64
	first := true
	for _, r := range b.snapshot() {
		if first || r.StartMs < lo {
			lo = r.StartMs
		}
		if first || r.EndMs > hi {
			hi = r.EndMs
		}
		first = false
	}
	return lo, hi
}

// flush appends newly closed rows to video.vpt and refreshes the header.
//
// Only closed rows are persisted, which keeps the file append-only in the
// common case: one 40-byte write per GOP, ~1800 rows/hour.
func (b *vptBuilder) flush() error {
	if b.path == "" {
		return nil
	}
	pending := len(b.rows) - b.persisted
	if pending <= 0 && !b.dirty {
		return nil
	}
	file, err := os.OpenFile(b.path, os.O_CREATE|os.O_RDWR, 0o644)
	if err != nil {
		return err
	}
	defer file.Close()

	if pending > 0 {
		buf := make([]byte, pending*vptRowSize)
		for i := 0; i < pending; i++ {
			encodeGopRow(buf[i*vptRowSize:(i+1)*vptRowSize], b.rows[b.persisted+i])
		}
		off := int64(vptHeaderSize) + int64(b.persisted)*vptRowSize
		if _, err := file.WriteAt(buf, off); err != nil {
			return err
		}
		b.persisted = len(b.rows)
	}
	lo, hi := b.bounds()
	hdr := encodeVptHeader(vptHeader{
		rowCount:      uint32(b.persisted),
		minMs:         lo,
		maxMs:         hi,
		clockOffsetMs: b.clockOffset,
		clockProv:     b.clockProv,
	})
	if _, err := file.WriteAt(hdr, 0); err != nil {
		return err
	}
	// A partial rewrite could leave a stale tail behind a shorter table.
	if err := file.Truncate(int64(vptHeaderSize) + int64(b.persisted)*vptRowSize); err != nil {
		return err
	}
	b.dirty = false
	return file.Sync()
}

// flushAll persists the open row too. Used at shutdown, where there is no
// following keyframe to close it.
func (b *vptBuilder) flushAll() error {
	b.closeOpen()
	return b.flush()
}

func nextKeyframe(rows []GopRow, idx int, streamID uint8) int {
	_, hi := streamBounds(rows, streamID)
	for j := idx + 1; j < hi; j++ {
		if !rows[j].IsContinuation() {
			return j
		}
	}
	return -1
}

func lastKeyframe(rows []GopRow, streamID uint8) int {
	lo, hi := streamBounds(rows, streamID)
	now := videoNowMS()
	// Skip only absurdly future rows (broken RTCP, hours ahead). A 2.5 s cap
	// treated RTP/NTP skew of ~1 min as "future" and walked back to an old
	// still — Live View froze on that shop GOP at 0 fps while new segments
	// were still being written.
	const futureSkewMs = 10 * 60 * 1000
	for j := hi - 1; j >= lo; j-- {
		if rows[j].IsContinuation() {
			continue
		}
		if rows[j].StartMs > now+futureSkewMs {
			continue
		}
		return j
	}
	return -1
}

// locateEntry finds the GOP a timestamp lands in: the last keyframe row at or
// before atMs. Returns the index into rows, or -1.
//
// Rows are sorted, so this is the O(log K) search §8 says already exists for
// keyframes, lifted to GOP granularity.
func locateEntry(rows []GopRow, streamID uint8, atMs uint64) int {
	lo, hi := streamBounds(rows, streamID)
	if lo == hi {
		return -1
	}
	// First row of this stream that starts strictly after atMs.
	i := lo + sort.Search(hi-lo, func(k int) bool { return rows[lo+k].StartMs > atMs })
	for j := i - 1; j >= lo; j-- {
		if !rows[j].IsContinuation() {
			return j
		}
	}
	// atMs is before every keyframe: the earliest one is the only place a
	// decoder can start, so hand that back rather than nothing.
	for j := lo; j < hi; j++ {
		if !rows[j].IsContinuation() {
			return j
		}
	}
	return -1
}

func streamBounds(rows []GopRow, streamID uint8) (int, int) {
	lo := sort.Search(len(rows), func(k int) bool { return rows[k].StreamID >= streamID })
	hi := sort.Search(len(rows), func(k int) bool { return rows[k].StreamID > streamID })
	return lo, hi
}

// SegmentCoverage is one row of the coarse coverage map (§8.2): segment
// granularity, so a 4 h range is ~240 rows and the scrubber can shade where
// video exists instead of the user finding gaps by landing in one.
type SegmentCoverage struct {
	SegmentID uint32
	StartMs   uint64
	EndMs     uint64
}

func coverageOf(rows []GopRow, streamID uint8, fromMs, toMs uint64) []SegmentCoverage {
	lo, hi := streamBounds(rows, streamID)
	var out []SegmentCoverage
	for i := lo; i < hi; i++ {
		r := rows[i]
		if r.EndMs < fromMs || r.StartMs > toMs {
			continue
		}
		if n := len(out); n > 0 && out[n-1].SegmentID == r.SegmentID {
			if r.EndMs > out[n-1].EndMs {
				out[n-1].EndMs = r.EndMs
			}
			continue
		}
		out = append(out, SegmentCoverage{SegmentID: r.SegmentID, StartMs: r.StartMs, EndMs: r.EndMs})
	}
	return out
}
