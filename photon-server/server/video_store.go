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
	"strconv"
	"strings"
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

	videoSegmentMaxBytes   = 64 * 1024 * 1024
	videoSegmentMaxDurMS   = 60_000
	videoSegmentForceBytes = 128 * 1024 * 1024
	videoSegmentForceDurMS = 120_000
	// About one 2 s GOP at 720p24 is 0.5 MB. Live readers take the NALs they
	// need straight after they are written, so the hot tail is kept small.
	videoHotTailBytes       = 2 * 1024 * 1024
	videoRecoverFrameStepMS = 33
	// Replay reports a stretch this long with no frame as a gap.
	videoGapMs = uint64(2000)

	// A keyframe file (NNNNNNNNNNNN.kf) is written when a segment is sealed.
	videoKeyframeMagic      = uint32(0x564B4631) // "VKF1"
	videoKeyframeVersion    = uint16(1)
	videoKeyframeEntryBytes = 28

	// One RTP ingress means one stream. streamId stays in the pointer table
	// because multi-camera is a live open decision (§14.5) and the column is
	// free, but nothing writes anything but 0 today.
	videoDefaultStreamID = uint8(0)
)

// errVideoArchived is returned for a NAL whose segment's .h264 is not on disk:
// the segment has been archived elsewhere.
var errVideoArchived = errors.New("video segment is archived")

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

// videoKeyframe is the in-memory index entry for one keyframe: enough to read
// its bytes without the segment's .idx. No other NAL has an entry; a NAL's
// record is read from the .idx when it is needed.
type videoKeyframe struct {
	seq         uint64
	timestampMs uint64
	offset      uint64
	size        uint32
	segmentID   uint32
}

type videoSegment struct {
	id       uint64
	dataPath string
	idxPath  string
	startMs  uint64
	endMs    uint64
	size     int64
	active   bool
	// sealed: written to completion, so its keyframe file is authoritative.
	sealed bool
	// archived: the .h264 is not on disk.
	archived bool
	sps      []byte
	pps      []byte
	// firstSeq and count give the segment's NAL sequence numbers; record i of
	// the .idx is seq firstSeq+i.
	firstSeq uint64
	count    uint32
	recBytes int
	idxHead  int
	// kfFrom is the index in keyframes of the segment's first keyframe.
	kfFrom   int
	dataFile *os.File
	idxFile  *os.File
	readFile *os.File
	idxRead  *os.File
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
	// order holds every segment by id, which is also seq order.
	order     []*videoSegment
	keyframes []videoKeyframe
	hot       map[uint64][]byte
	hotOrder  []uint64
	hotBytes  int64
	diskBytes int64
	sps       []byte
	pps       []byte
	lastNALMS uint64
	// activeRows is the index in vpt.rows where the active segment's rows begin.
	activeRows int

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
	// The table is derived from the segments, so rebuilding it on open keeps it
	// consistent with whatever recovery decided and makes deleting video.vpt
	// harmless.
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
	flags := flagsForNALType(nalType)
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
	s.active.count++
	if flags&VideoFlagKeyframe != 0 {
		s.keyframes = append(s.keyframes, videoKeyframe{
			seq:         seq,
			timestampMs: timestampMs,
			offset:      uint64(offset),
			size:        uint32(len(payload)),
			segmentID:   uint32(s.active.id),
		})
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

	// The segment is complete: its keyframe file lets a restart load it
	// without reading its .idx.
	if err := s.writeKeyframeFileLocked(s.active, s.vpt.rows[s.activeRows:]); err != nil {
		return err
	}
	s.active.sealed = true

	nextID := s.active.id + 1
	if nextID < s.nextSegID {
		nextID = s.nextSegID
	}
	seg, err := createVideoSegment(s.dir, nextID)
	if err != nil {
		return err
	}
	seg.firstSeq = s.nextSeq
	seg.kfFrom = len(s.keyframes)
	s.register(seg)
	s.active = seg
	s.nextSegID = nextID + 1
	s.activeRows = len(s.vpt.rows)

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
	kf := s.keyframes[len(s.keyframes)-1]
	frame, err := s.readKeyframeLocked(kf)
	if err != nil {
		return VideoFrame{}, 0, false
	}
	return frame, kf.seq, true
}

func (s *VideoStore) ConfigNALs() (sps, pps []byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return cloneBytes(s.sps), cloneBytes(s.pps)
}

func (s *VideoStore) FindKeyframeAtOrBefore(t uint64) (VideoFrame, uint64, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	idx, ok := s.keyframeIndexAtOrBeforeLocked(t)
	if !ok {
		return VideoFrame{}, 0, false
	}
	kf := s.keyframes[idx]
	frame, err := s.readKeyframeLocked(kf)
	if err != nil {
		return VideoFrame{}, 0, false
	}
	return frame, kf.seq, true
}

func (s *VideoStore) keyframeIndexAtOrBeforeLocked(t uint64) (int, bool) {
	if len(s.keyframes) == 0 {
		return 0, false
	}
	i := sort.Search(len(s.keyframes), func(i int) bool {
		return s.keyframes[i].timestampMs > t
	})
	if i == 0 {
		return 0, false
	}
	return i - 1, true
}

// replayKeyframeLocked is the keyframe a replay from t starts at: the last one
// at or before t, or the first one when t comes before all of them.
func (s *VideoStore) replayKeyframeLocked(t uint64) (int, bool) {
	if idx, ok := s.keyframeIndexAtOrBeforeLocked(t); ok {
		return idx, true
	}
	if len(s.keyframes) > 0 {
		return 0, true
	}
	return 0, false
}

func (s *VideoStore) ReplayStart(t uint64) (seq uint64, sps, pps []byte, ok bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	idx, found := s.replayKeyframeLocked(t)
	if !found {
		return 0, nil, nil, false
	}
	kf := s.keyframes[idx]
	sps, pps = s.configBeforeLocked(kf.seq)
	return kf.seq, sps, pps, true
}

// ReplayHole reports whether a replay from t starts in a recording hole: the
// last stored frame before the keyframe after t is more than videoGapMs before
// that keyframe, and t comes after that frame. It returns the frame times
// either side of the hole and the sequence number of the keyframe after it.
func (s *VideoStore) ReplayHole(t uint64) (from, to, seq uint64, ok bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	idx, has := s.keyframeIndexAtOrBeforeLocked(t)
	if !has || idx+1 >= len(s.keyframes) {
		return 0, 0, 0, false
	}
	next := s.keyframes[idx+1]
	last, found := s.lastMediaBeforeLocked(next.seq)
	if !found || t <= last || next.timestampMs <= last+videoGapMs {
		return 0, 0, 0, false
	}
	return last, next.timestampMs, next.seq, true
}

// lastMediaBeforeLocked is the time of the last picture stored before seq. Config
// NALs are skipped, as a keyframe's SPS and PPS come before it.
func (s *VideoStore) lastMediaBeforeLocked(seq uint64) (uint64, bool) {
	for back := uint64(1); back <= 64 && back < seq; back++ {
		rec, _, ok, err := s.recordForSeqLocked(seq - back)
		if !ok || err != nil {
			return 0, false
		}
		if rec.Flags&VideoFlagConfig == 0 {
			return rec.TimestampMs, true
		}
	}
	return 0, false
}

// ArchivedSpan describes the segment seq is in, when its data is archived: its
// time range, and the sequence number of the first keyframe after it. more is
// false when no keyframe follows.
func (s *VideoStore) ArchivedSpan(seq uint64) (from, to, next uint64, more bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	seg, _, ok := s.segmentForSeqLocked(seq)
	if !ok {
		return 0, 0, 0, false
	}
	from, to = seg.startMs, seg.endMs
	i := sort.Search(len(s.keyframes), func(k int) bool {
		return uint64(s.keyframes[k].segmentID) > seg.id
	})
	if i < len(s.keyframes) {
		return from, to, s.keyframes[i].seq, true
	}
	return from, to, 0, false
}

// ConfigBefore returns the SPS and PPS a decoder needs before the NAL at seq.
func (s *VideoStore) ConfigBefore(seq uint64) (sps, pps []byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.configBeforeLocked(seq)
}

// NewestMs is the timestamp of the newest NAL stored.
func (s *VideoStore) NewestMs() uint64 {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.lastNALMS
}

// configBeforeLocked finds the last SPS and the last PPS stored before seq, on
// disk, falling back to the store's latest pair when there is none.
func (s *VideoStore) configBeforeLocked(seq uint64) (sps, pps []byte) {
	needSPS, needPPS := true, true
	for i := len(s.order) - 1; i >= 0 && (needSPS || needPPS); i-- {
		seg := s.order[i]
		if seg.archived || seg.count == 0 || seg.firstSeq >= seq {
			continue
		}
		n := seq - seg.firstSeq
		if n > uint64(seg.count) {
			n = uint64(seg.count)
		}
		for j := int(n) - 1; j >= 0 && (needSPS || needPPS); j-- {
			rec, err := s.readRecordLocked(seg, uint32(j))
			if err != nil {
				break
			}
			if rec.Size == 0 {
				continue
			}
			if rec.NALType == nalTypeSPS && needSPS {
				if payload, err := s.readPayloadAtLocked(seg.firstSeq+uint64(j), seg, rec.Offset, rec.Size); err == nil {
					sps, needSPS = payload, false
				}
			} else if rec.NALType == nalTypePPS && needPPS {
				if payload, err := s.readPayloadAtLocked(seg.firstSeq+uint64(j), seg, rec.Offset, rec.Size); err == nil {
					pps, needPPS = payload, false
				}
			}
		}
	}
	if needSPS {
		sps = cloneBytes(s.sps)
	}
	if needPPS {
		pps = cloneBytes(s.pps)
	}
	return sps, pps
}

func (s *VideoStore) ReadSeq(seq uint64) (VideoFrame, bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rec, seg, ok, err := s.recordForSeqLocked(seq)
	if !ok {
		return VideoFrame{}, false, nil
	}
	if err != nil {
		return VideoFrame{}, false, err
	}
	payload, err := s.readPayloadAtLocked(seq, seg, rec.Offset, rec.Size)
	if err != nil {
		return VideoFrame{}, false, err
	}
	return VideoFrame{
		TimestampMs: rec.TimestampMs,
		Flags:       rec.Flags,
		Payload:     payload,
	}, true, nil
}

func (s *VideoStore) ReplayFrom(t uint64) ([]VideoFrame, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.nextSeq == 1 {
		return nil, nil
	}
	start := uint64(1)
	if idx, ok := s.replayKeyframeLocked(t); ok {
		start = s.keyframes[idx].seq
	}

	var frames []VideoFrame
	first, _, _, err := s.recordForSeqLocked(start)
	if err != nil {
		return nil, err
	}
	sps, pps := s.configBeforeLocked(start)
	if len(sps) > 0 {
		frames = append(frames, VideoFrame{TimestampMs: first.TimestampMs, Flags: VideoFlagConfig, Payload: sps})
	}
	if len(pps) > 0 {
		frames = append(frames, VideoFrame{TimestampMs: first.TimestampMs, Flags: VideoFlagConfig, Payload: pps})
	}
	for seq := start; seq < s.nextSeq; seq++ {
		rec, seg, ok, err := s.recordForSeqLocked(seq)
		if err != nil {
			return nil, err
		}
		if !ok {
			break
		}
		payload, err := s.readPayloadAtLocked(seq, seg, rec.Offset, rec.Size)
		if err != nil {
			return nil, err
		}
		frames = append(frames, VideoFrame{
			TimestampMs: rec.TimestampMs,
			Flags:       rec.Flags,
			Payload:     payload,
		})
	}
	return frames, nil
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

		rec, seg, ok, err := s.recordForSeqLocked(seq)
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
		if err != nil {
			s.mu.Unlock()
			return VideoFrame{}, 0, err
		}
		payload, err := s.readPayloadAtLocked(seq, seg, rec.Offset, rec.Size)
		s.mu.Unlock()
		if err != nil {
			return VideoFrame{}, 0, err
		}
		return VideoFrame{
			TimestampMs: rec.TimestampMs,
			Flags:       rec.Flags,
			Payload:     payload,
		}, seq, nil
	}
}

func (s *VideoStore) LatestSeqUnlockedFallback() uint64 {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.nextSeq == 1 {
		return 0
	}
	return s.nextSeq - 1
}

// segmentForSeqLocked returns the segment that holds seq, and the seq's index
// in that segment's .idx.
func (s *VideoStore) segmentForSeqLocked(seq uint64) (*videoSegment, uint32, bool) {
	if seq == 0 || seq >= s.nextSeq || len(s.order) == 0 {
		return nil, 0, false
	}
	i := sort.Search(len(s.order), func(k int) bool { return s.order[k].firstSeq > seq }) - 1
	// Empty segments can share a first seq with the one that holds seq, or
	// sit between it and seq, so skip them rather than trust the last one.
	for ; i >= 0; i-- {
		seg := s.order[i]
		if seg.count == 0 {
			continue
		}
		if seq-seg.firstSeq < uint64(seg.count) {
			return seg, uint32(seq - seg.firstSeq), true
		}
		break
	}
	return nil, 0, false
}

// recordForSeqLocked reads the .idx record of seq. ok is false when seq has not
// been stored.
func (s *VideoStore) recordForSeqLocked(seq uint64) (videoNALRecord, *videoSegment, bool, error) {
	seg, index, ok := s.segmentForSeqLocked(seq)
	if !ok {
		return videoNALRecord{}, nil, false, nil
	}
	rec, err := s.readRecordLocked(seg, index)
	if err != nil {
		return videoNALRecord{}, seg, true, err
	}
	return rec, seg, true, nil
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
	for _, seg := range s.order {
		if seg.readFile != nil {
			_ = seg.readFile.Close()
			seg.readFile = nil
		}
		if seg.idxRead != nil {
			_ = seg.idxRead.Close()
			seg.idxRead = nil
		}
	}
	s.mu.Unlock()
	return err
}

// register adds a segment to the store's maps, in id order.
func (s *VideoStore) register(seg *videoSegment) {
	s.segments[seg.id] = seg
	s.order = append(s.order, seg)
}

// recover loads every segment on disk. A segment is either sealed, which is
// read from its keyframe file, or the newest one, which may still be written
// to and whose .idx is scanned.
func (s *VideoStore) recover() error {
	ids, err := s.segmentIDs()
	if err != nil {
		return err
	}
	for i, id := range ids {
		if err := s.recoverSegment(id, i == len(ids)-1); err != nil {
			return err
		}
		if id >= s.nextSegID {
			s.nextSegID = id + 1
		}
	}
	return nil
}

// segmentIDs lists the segments that have a .h264 or a keyframe file.
func (s *VideoStore) segmentIDs() ([]uint64, error) {
	entries, err := os.ReadDir(s.dir)
	if err != nil {
		return nil, err
	}
	seen := make(map[uint64]bool)
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		for _, ext := range []string{".h264", ".kf"} {
			if id, ok := parseSegmentName(entry.Name(), ext); ok {
				seen[id] = true
			}
		}
	}
	ids := make([]uint64, 0, len(seen))
	for id := range seen {
		ids = append(ids, id)
	}
	sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
	return ids, nil
}

func parseSegmentName(name, ext string) (uint64, bool) {
	if !strings.HasSuffix(name, ext) {
		return 0, false
	}
	digits := strings.TrimSuffix(name, ext)
	if len(digits) != 12 {
		return 0, false
	}
	id, err := strconv.ParseUint(digits, 10, 64)
	return id, err == nil
}

// recoverSegment loads one segment. last is true for the newest segment on disk,
// which is the one that may still be appended to.
func (s *VideoStore) recoverSegment(id uint64, last bool) error {
	seg := &videoSegment{
		id:       id,
		dataPath: videoDataPath(s.dir, id),
		idxPath:  videoIndexPath(s.dir, id),
		recBytes: videoNALRecordSizeV2,
		idxHead:  videoIndexHeaderSize,
	}
	info, err := os.Stat(seg.dataPath)
	switch {
	case err == nil:
		seg.size = info.Size()
	case os.IsNotExist(err):
		seg.archived = true
	default:
		return err
	}

	// A valid keyframe file means the segment was sealed. Its .idx is not read,
	// so a damaged index does not stop the store from starting.
	if kf, ok := readKeyframeFile(videoKeyframePath(s.dir, id)); ok && kf.id == id {
		s.loadSealedLocked(seg, kf)
		return nil
	}
	if seg.archived {
		// Neither the data nor a usable keyframe file: nothing to load.
		return nil
	}

	records, err := s.scanSegment(seg, seg.size)
	if err != nil {
		return err
	}
	seg.count = uint32(len(records))
	seg.firstSeq = s.nextSeq
	s.nextSeq += uint64(len(records))
	seg.kfFrom = len(s.keyframes)
	for i, rec := range records {
		if rec.Flags&VideoFlagKeyframe != 0 {
			s.keyframes = append(s.keyframes, videoKeyframe{
				seq:         seg.firstSeq + uint64(i),
				timestampMs: rec.TimestampMs,
				offset:      rec.Offset,
				size:        rec.Size,
				segmentID:   uint32(id),
			})
		}
		if rec.TimestampMs > s.lastNALMS {
			s.lastNALMS = rec.TimestampMs
		}
	}
	s.register(seg)
	s.diskBytes += seg.size
	s.adoptConfigLocked(seg)
	if !last {
		rows := videoRowsOf(records, id)
		s.vpt.rows = append(s.vpt.rows, rows...)
		seg.sealed = true
		if err := s.writeKeyframeFileLocked(seg, rows); err != nil {
			return err
		}
	}
	return nil
}

// loadSealedLocked adds a sealed segment from its keyframe file.
func (s *VideoStore) loadSealedLocked(seg *videoSegment, kf keyframeFile) {
	seg.sealed = true
	seg.firstSeq = s.nextSeq
	seg.count = kf.count
	seg.recBytes = kf.recBytes
	seg.idxHead = kf.idxHead
	seg.startMs = kf.startMs
	seg.endMs = kf.endMs
	seg.size = int64(kf.size)
	seg.sps = kf.sps
	seg.pps = kf.pps
	s.nextSeq += uint64(kf.count)
	seg.kfFrom = len(s.keyframes)
	s.keyframes = append(s.keyframes, kf.keyframes...)
	s.vpt.rows = append(s.vpt.rows, kf.rows...)
	s.register(seg)
	if !seg.archived {
		s.diskBytes += seg.size
	}
	s.adoptConfigLocked(seg)
	if seg.endMs > s.lastNALMS {
		s.lastNALMS = seg.endMs
	}
}

// adoptConfigLocked makes a segment's SPS and PPS the store's latest, when it has them.
func (s *VideoStore) adoptConfigLocked(seg *videoSegment) {
	if len(seg.sps) > 0 {
		s.sps = cloneBytes(seg.sps)
	}
	if len(seg.pps) > 0 {
		s.pps = cloneBytes(seg.pps)
	}
}

// scanSegment reads a segment's .idx and .h264 and returns its records. Where the
// data runs past the index, the index is repaired from the data. It sets the
// segment's time range and config.
func (s *VideoStore) scanSegment(seg *videoSegment, dataSize int64) ([]videoNALRecord, error) {
	contents, idxOK := readIndexFile(seg.idxPath)
	records := contents.records
	header := indexHeader{}
	if idxOK {
		header = contents.header
		seg.startMs, seg.endMs = header.startMs, header.endMs
		seg.recBytes, seg.idxHead = contents.recBytes, contents.headBytes
	}

	covered := int64(0)
	if len(records) > 0 {
		last := records[len(records)-1]
		covered = int64(last.Offset) + int64(last.Size)
	}
	repaired := false
	if dataSize > covered {
		tail, err := readFileRange(seg.dataPath, covered, dataSize-covered)
		if err != nil {
			return nil, err
		}
		ts := uint64(0)
		if len(records) > 0 {
			ts = records[len(records)-1].TimestampMs
		} else if info, err := os.Stat(seg.dataPath); err == nil {
			ts = uint64(info.ModTime().UnixMilli())
		}
		for _, nal := range splitAnnexB(tail) {
			ts += videoRecoverFrameStepMS
			nalType := nalTypeOfAnnexB(nal)
			records = append(records, videoNALRecord{
				TimestampMs: ts,
				Offset:      uint64(covered),
				Size:        uint32(len(nal)),
				Flags:       flagsForNALType(nalType),
				NALType:     nalType,
			})
			covered += int64(len(nal))
		}
		repaired = true
	}

	// The config is the last SPS and PPS in the segment, else the header's.
	seg.sps, seg.pps = header.sps, header.pps
	if rec, ok := lastRecordOfType(records, nalTypeSPS); ok {
		if payload, err := readFileRange(seg.dataPath, int64(rec.Offset), int64(rec.Size)); err == nil {
			seg.sps = payload
		}
	}
	if rec, ok := lastRecordOfType(records, nalTypePPS); ok {
		if payload, err := readFileRange(seg.dataPath, int64(rec.Offset), int64(rec.Size)); err == nil {
			seg.pps = payload
		}
	}

	if repaired {
		if err := rewriteIndex(seg.idxPath, indexHeader{
			startMs: firstRecordTS(records),
			endMs:   lastRecordTS(records),
			sps:     seg.sps,
			pps:     seg.pps,
		}, records); err != nil {
			return nil, err
		}
		seg.recBytes, seg.idxHead = videoNALRecordSizeV2, videoIndexHeaderSize
		if len(records) > 0 {
			seg.startMs = firstRecordTS(records)
			seg.endMs = lastRecordTS(records)
		}
	}
	return records, nil
}

// lastRecordOfType finds the last record of a NAL type that has bytes.
func lastRecordOfType(records []videoNALRecord, nalType uint8) (videoNALRecord, bool) {
	for i := len(records) - 1; i >= 0; i-- {
		if records[i].NALType == nalType && records[i].Size > 0 {
			return records[i], true
		}
	}
	return videoNALRecord{}, false
}

// readFileRange reads n bytes at offset from a file.
func readFileRange(path string, offset, n int64) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	buf := make([]byte, n)
	if _, err := file.ReadAt(buf, offset); err != nil && !errors.Is(err, io.EOF) {
		return nil, err
	}
	return buf, nil
}

// openActive makes the newest segment the one being written, or starts a new
// one when the newest is sealed, archived, full, or in an old record layout.
func (s *VideoStore) openActive() error {
	var existing *videoSegment
	if s.nextSegID > 1 {
		existing = s.segments[s.nextSegID-1]
	}
	if existing != nil && (existing.sealed || existing.archived || existing.recBytes != videoNALRecordSizeV2 ||
		segmentFull(existing)) {
		if !existing.sealed && !existing.archived {
			if err := s.sealScannedLocked(existing); err != nil {
				return err
			}
		}
		existing = nil
	}

	if existing == nil {
		seg, err := createVideoSegment(s.dir, s.nextSegID)
		if err != nil {
			return err
		}
		seg.firstSeq = s.nextSeq
		seg.kfFrom = len(s.keyframes)
		s.register(seg)
		s.nextSegID++
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
		info, err := idxFile.Stat()
		if err == nil && info.Size() == 0 {
			err = writeIndexHeader(idxFile, indexHeader{}, 0)
		} else if err == nil {
			_, err = idxFile.Seek(0, io.SeekEnd)
		}
		if err != nil {
			_ = dataFile.Close()
			_ = idxFile.Close()
			return err
		}
		existing.dataFile = dataFile
		existing.idxFile = idxFile
		existing.active = true
	}
	s.active = existing
	s.activeRows = len(s.vpt.rows)
	return nil
}

// segmentFull reports whether a segment has reached a size or duration limit.
func segmentFull(seg *videoSegment) bool {
	return seg.size >= videoSegmentMaxBytes ||
		(seg.startMs != 0 && seg.endMs >= seg.startMs && seg.endMs-seg.startMs >= videoSegmentMaxDurMS)
}

// sealScannedLocked seals a segment that was scanned on open rather than read
// from a keyframe file: its rows are added and its keyframe file is written.
func (s *VideoStore) sealScannedLocked(seg *videoSegment) error {
	records, err := s.readRecordsLocked(seg)
	if err != nil {
		return err
	}
	rows := videoRowsOf(records, seg.id)
	s.vpt.rows = append(s.vpt.rows, rows...)
	seg.sealed = true
	return s.writeKeyframeFileLocked(seg, rows)
}

// writeKeyframeFileLocked writes the keyframe file of a segment that is no longer
// written. rows are the GOP rows that belong to it.
func (s *VideoStore) writeKeyframeFileLocked(seg *videoSegment, rows []GopRow) error {
	sps, pps := seg.sps, seg.pps
	if len(sps) == 0 {
		sps = s.sps
	}
	if len(pps) == 0 {
		pps = s.pps
	}
	return writeKeyframeFile(videoKeyframePath(s.dir, seg.id), keyframeFile{
		id:        seg.id,
		firstSeq:  seg.firstSeq,
		count:     seg.count,
		recBytes:  seg.recBytes,
		idxHead:   seg.idxHead,
		startMs:   seg.startMs,
		endMs:     seg.endMs,
		size:      uint64(seg.size),
		sps:       sps,
		pps:       pps,
		keyframes: s.keyframes[seg.kfFrom:],
		rows:      rows,
	})
}

// videoRowsOf builds the GOP rows of a segment that is no longer written. A
// rotation closes every row, so these are the rows a vptBuilder fed the
// segment's records would hold.
func videoRowsOf(records []videoNALRecord, segmentID uint64) []GopRow {
	var b vptBuilder
	b.reset("")
	for _, rec := range records {
		b.add(videoDefaultStreamID, uint32(segmentID), rec.TimestampMs, rec.Offset, rec.Size, rec.Flags, rec.NALType)
	}
	b.closeOpen()
	return b.rows
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
	if err := writeIndexHeader(s.active.idxFile, header, s.active.count); err != nil {
		return err
	}
	return s.active.idxFile.Sync()
}

// readKeyframeLocked reads a keyframe's bytes. Its offset and size are in memory,
// so the segment's .idx is not needed.
func (s *VideoStore) readKeyframeLocked(kf videoKeyframe) (VideoFrame, error) {
	payload, err := s.readPayloadAtLocked(kf.seq, s.segments[uint64(kf.segmentID)], kf.offset, kf.size)
	if err != nil {
		return VideoFrame{}, err
	}
	return VideoFrame{TimestampMs: kf.timestampMs, Flags: VideoFlagKeyframe, Payload: payload}, nil
}

// readPayloadAtLocked returns a NAL's bytes from the hot tail, or else from the
// segment's .h264.
func (s *VideoStore) readPayloadAtLocked(seq uint64, seg *videoSegment, offset uint64, size uint32) ([]byte, error) {
	if payload, ok := s.hot[seq]; ok {
		return cloneBytes(payload), nil
	}
	if seg == nil {
		return nil, fmt.Errorf("video NAL %d has no segment", seq)
	}
	if seg.archived {
		return nil, errVideoArchived
	}
	file, err := s.segmentReadFileLocked(seg)
	if err != nil {
		if os.IsNotExist(err) {
			seg.archived = true
			return nil, errVideoArchived
		}
		return nil, err
	}
	buf := make([]byte, size)
	if _, err := file.ReadAt(buf, int64(offset)); err != nil {
		return nil, err
	}
	return buf, nil
}

// readRecordLocked reads one record of a segment's .idx.
func (s *VideoStore) readRecordLocked(seg *videoSegment, index uint32) (videoNALRecord, error) {
	if seg.archived {
		return videoNALRecord{}, errVideoArchived
	}
	if index >= seg.count {
		return videoNALRecord{}, fmt.Errorf("video segment %d has no NAL %d", seg.id, index)
	}
	file, err := s.segmentIdxFileLocked(seg)
	if err != nil {
		return videoNALRecord{}, err
	}
	var buf [videoNALRecordSizeV2]byte
	rec := buf[:seg.recBytes]
	if _, err := file.ReadAt(rec, int64(seg.idxHead)+int64(index)*int64(seg.recBytes)); err != nil {
		return videoNALRecord{}, err
	}
	return decodeNALRecord(rec, seg.recBytes), nil
}

// readRecordsLocked reads every record of a segment's .idx.
func (s *VideoStore) readRecordsLocked(seg *videoSegment) ([]videoNALRecord, error) {
	if seg.count == 0 {
		return nil, nil
	}
	if seg.archived {
		return nil, errVideoArchived
	}
	file, err := s.segmentIdxFileLocked(seg)
	if err != nil {
		return nil, err
	}
	buf := make([]byte, int(seg.count)*seg.recBytes)
	if _, err := file.ReadAt(buf, int64(seg.idxHead)); err != nil {
		return nil, err
	}
	records := make([]videoNALRecord, seg.count)
	for i := range records {
		records[i] = decodeNALRecord(buf[i*seg.recBytes:(i+1)*seg.recBytes], seg.recBytes)
	}
	return records, nil
}

func (s *VideoStore) segmentIdxFileLocked(seg *videoSegment) (*os.File, error) {
	if seg.idxRead != nil {
		return seg.idxRead, nil
	}
	file, err := os.Open(seg.idxPath)
	if err != nil {
		return nil, err
	}
	seg.idxRead = file
	return file, nil
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
		recBytes: videoNALRecordSizeV2,
		idxHead:  videoIndexHeaderSize,
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

// videoIndexFile is the decoded contents of a segment's .idx.
type videoIndexFile struct {
	header    indexHeader
	records   []videoNALRecord
	recBytes  int
	headBytes int
}

// readIndexFile reads a segment's .idx. ok is false when the file is missing or
// not an index.
func readIndexFile(path string) (videoIndexFile, bool) {
	data, err := os.ReadFile(path)
	if err != nil || len(data) < videoIndexHeaderSize {
		return videoIndexFile{}, false
	}
	if binary.BigEndian.Uint32(data[0:4]) != videoIndexMagic {
		return videoIndexFile{}, false
	}
	version := binary.BigEndian.Uint16(data[4:6])
	if version == 0 || version > videoIndexVersion {
		return videoIndexFile{}, false
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
	return videoIndexFile{header: header, records: records, recBytes: recordBytes, headBytes: headerBytes}, true
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

func videoKeyframePath(dir string, id uint64) string {
	return filepath.Join(dir, fmt.Sprintf("%012d.kf", id))
}

// keyframeFile is the sidecar a sealed segment keeps: its size and time range,
// its keyframes, and its GOP rows. A restart reads it instead of the .idx.
//
//	[4] magic  [2] version  [2] recBytes  [4] idxHead  [8] id  [8] firstSeq
//	[4] count  [8] startMs  [8] endMs  [8] size
//	[2] spsLen, sps  [2] ppsLen, pps
//	[4] keyframes, then per keyframe: [8] seq [8] timestampMs [8] offset [4] size
//	[4] rows, then 40 B per GOP row
//	[4] crc32 (IEEE) of everything before it
type keyframeFile struct {
	id        uint64
	firstSeq  uint64
	count     uint32
	recBytes  int
	idxHead   int
	startMs   uint64
	endMs     uint64
	size      uint64
	sps       []byte
	pps       []byte
	keyframes []videoKeyframe
	rows      []GopRow
}

func encodeKeyframeFile(f keyframeFile) []byte {
	buf := make([]byte, 0, 96+len(f.sps)+len(f.pps)+len(f.keyframes)*videoKeyframeEntryBytes+len(f.rows)*vptRowSize)
	buf = binary.BigEndian.AppendUint32(buf, videoKeyframeMagic)
	buf = binary.BigEndian.AppendUint16(buf, videoKeyframeVersion)
	buf = binary.BigEndian.AppendUint16(buf, uint16(f.recBytes))
	buf = binary.BigEndian.AppendUint32(buf, uint32(f.idxHead))
	buf = binary.BigEndian.AppendUint64(buf, f.id)
	buf = binary.BigEndian.AppendUint64(buf, f.firstSeq)
	buf = binary.BigEndian.AppendUint32(buf, f.count)
	buf = binary.BigEndian.AppendUint64(buf, f.startMs)
	buf = binary.BigEndian.AppendUint64(buf, f.endMs)
	buf = binary.BigEndian.AppendUint64(buf, f.size)
	buf = binary.BigEndian.AppendUint16(buf, uint16(len(f.sps)))
	buf = append(buf, f.sps...)
	buf = binary.BigEndian.AppendUint16(buf, uint16(len(f.pps)))
	buf = append(buf, f.pps...)
	buf = binary.BigEndian.AppendUint32(buf, uint32(len(f.keyframes)))
	for _, kf := range f.keyframes {
		buf = binary.BigEndian.AppendUint64(buf, kf.seq)
		buf = binary.BigEndian.AppendUint64(buf, kf.timestampMs)
		buf = binary.BigEndian.AppendUint64(buf, kf.offset)
		buf = binary.BigEndian.AppendUint32(buf, kf.size)
	}
	buf = binary.BigEndian.AppendUint32(buf, uint32(len(f.rows)))
	for _, row := range f.rows {
		var b [vptRowSize]byte
		encodeGopRow(b[:], row)
		buf = append(buf, b[:]...)
	}
	return binary.BigEndian.AppendUint32(buf, crc32.ChecksumIEEE(buf))
}

// kfReader reads the fields of a keyframe file in order. Once a read runs past
// the end, bad is set and every later read returns zero.
type kfReader struct {
	b   []byte
	bad bool
}

func (r *kfReader) take(n int) []byte {
	if r.bad || n < 0 || n > len(r.b) {
		r.bad = true
		return nil
	}
	out := r.b[:n:n]
	r.b = r.b[n:]
	return out
}

func (r *kfReader) u16() uint16 {
	b := r.take(2)
	if r.bad {
		return 0
	}
	return binary.BigEndian.Uint16(b)
}

func (r *kfReader) u32() uint32 {
	b := r.take(4)
	if r.bad {
		return 0
	}
	return binary.BigEndian.Uint32(b)
}

func (r *kfReader) u64() uint64 {
	b := r.take(8)
	if r.bad {
		return 0
	}
	return binary.BigEndian.Uint64(b)
}

// decodeKeyframeFile parses a keyframe file. ok is false when it is damaged, in
// which case the segment is scanned instead.
func decodeKeyframeFile(data []byte) (keyframeFile, bool) {
	if len(data) < 4 {
		return keyframeFile{}, false
	}
	body := data[:len(data)-4]
	if crc32.ChecksumIEEE(body) != binary.BigEndian.Uint32(data[len(data)-4:]) {
		return keyframeFile{}, false
	}
	r := kfReader{b: body}
	if r.u32() != videoKeyframeMagic || r.u16() != videoKeyframeVersion {
		return keyframeFile{}, false
	}
	var f keyframeFile
	f.recBytes = int(r.u16())
	f.idxHead = int(r.u32())
	f.id = r.u64()
	f.firstSeq = r.u64()
	f.count = r.u32()
	f.startMs = r.u64()
	f.endMs = r.u64()
	f.size = r.u64()
	n := int(r.u16())
	f.sps = cloneBytes(r.take(n))
	n = int(r.u16())
	f.pps = cloneBytes(r.take(n))
	if r.bad {
		return keyframeFile{}, false
	}
	if (f.recBytes != videoNALRecordSizeV1 && f.recBytes != videoNALRecordSizeV2) ||
		f.idxHead < 48 || f.idxHead > 1<<20 {
		return keyframeFile{}, false
	}

	kfCount := r.u32()
	if r.bad || uint64(kfCount) > uint64(f.count) {
		return keyframeFile{}, false
	}
	f.keyframes = make([]videoKeyframe, 0, kfCount)
	for i := uint32(0); i < kfCount; i++ {
		kf := videoKeyframe{seq: r.u64(), timestampMs: r.u64(), offset: r.u64(), size: r.u32(), segmentID: uint32(f.id)}
		if r.bad || kf.seq < f.firstSeq || kf.seq >= f.firstSeq+uint64(f.count) {
			return keyframeFile{}, false
		}
		f.keyframes = append(f.keyframes, kf)
	}

	rowCount := r.u32()
	if r.bad || uint64(rowCount)*vptRowSize != uint64(len(r.b)) {
		return keyframeFile{}, false
	}
	f.rows = make([]GopRow, 0, rowCount)
	for i := uint32(0); i < rowCount; i++ {
		f.rows = append(f.rows, decodeGopRow(r.take(vptRowSize)))
	}
	if r.bad {
		return keyframeFile{}, false
	}
	return f, true
}

func readKeyframeFile(path string) (keyframeFile, bool) {
	data, err := os.ReadFile(path)
	if err != nil {
		return keyframeFile{}, false
	}
	return decodeKeyframeFile(data)
}

// writeKeyframeFile writes a keyframe file through a temporary name, so a crash
// leaves either the old file or the new one.
func writeKeyframeFile(path string, f keyframeFile) error {
	tmp := path + ".tmp"
	file, err := os.OpenFile(tmp, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644)
	if err != nil {
		return err
	}
	if _, err := file.Write(encodeKeyframeFile(f)); err != nil {
		_ = file.Close()
		return err
	}
	if err := file.Sync(); err != nil {
		_ = file.Close()
		return err
	}
	if err := file.Close(); err != nil {
		return err
	}
	return os.Rename(tmp, path)
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
