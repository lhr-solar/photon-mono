package main

import (
	"sync"
	"time"
	"unsafe"
)

// liveFrame is one CAN frame in the live ring. CANP v3 carries at most 8 data
// bytes, so the data is a fixed array.
type liveFrame struct {
	tNS  int64
	id   uint32 // wire ID as the CANP v3 sender sent it: bit 31 set for 29-bit IDs, bit 12 for can0
	n    uint8  // data bytes in use
	data [8]byte
}

// liveRingMinFrames is the smallest live ring, whatever the rate and window say.
const liveRingMinFrames = 1024

// liveRing keeps the most recent CAN frames for live clients. Each frame has a
// sequence number; frame s lives in slots[s % len(slots)] until a newer frame
// overwrites it. The writer is appendLoop, once per ingest buffer. Readers are
// the per-client pushers, which keep their own cursor and never block the writer
// for long.
type liveRing struct {
	mu     sync.RWMutex
	slots  []liveFrame
	head   uint64        // sequence number of the next frame to write
	notify chan struct{} // closed and replaced every time frames are added
}

func newLiveRing(capacity int) *liveRing {
	return &liveRing{slots: make([]liveFrame, max(capacity, 1)), notify: make(chan struct{})}
}

// liveRingFrames is how many frames the ring holds: the peak frame rate times
// the window, and at least liveRingMinFrames.
func liveRingFrames(cfg *Config) int {
	perSecond := float64(cfg.Ingest.Rate) / canpFrameSize
	return max(int(perSecond*cfg.LiveSeconds.Seconds()), liveRingMinFrames)
}

// appendBuffer adds the frames of one ingest buffer. Frames whose car time fails
// clockOK are left out, as the decoder leaves them out. Waiting readers are woken
// once, after the whole buffer is in.
func (r *liveRing) appendBuffer(buf []byte) {
	maxNS := time.Now().Add(frameMaxFuture).UnixNano()
	r.mu.Lock()
	defer r.mu.Unlock()
	added := false
	walkCANP3(buf, func(id uint32, bus string, data []byte, tNS int64) {
		if !clockOK(tNS, maxNS) {
			return
		}
		if bus == "can0" {
			id |= canpBus0Bit // keep the ID as sent, so can0 and can1 frames stay apart
		}
		s := &r.slots[r.head%uint64(len(r.slots))]
		s.tNS, s.id, s.n = tNS, id, uint8(len(data))
		copy(s.data[:], data)
		r.head++
		added = true
	})
	if added {
		close(r.notify)
		r.notify = make(chan struct{})
	}
}

// liveScanMax bounds the slots one read looks at, so a subscription to a rare
// ID never holds the read lock (and so appendLoop) for a whole-ring scan.
const liveScanMax = 8192

// read copies the frames from sequence from onward into dst[:0], keeping those
// whose ID keep accepts, and stops when dst is full, liveScanMax slots have been
// looked at, or the ring has no more.
// next is the sequence after the last frame looked at, so a caller resumes there
// whether or not it kept the frame. oldest is the oldest sequence still in the
// ring. If from is older than that, the caller fell behind: no frames come back
// and next is from. wait is the notify channel, which closes on the next append;
// it is nil when the scan stopped before the head, so the caller reads again.
func (r *liveRing) read(from uint64, dst []liveFrame, keep func(id uint32) bool) (frames []liveFrame, next, oldest uint64, wait <-chan struct{}) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	oldest = r.oldestLocked()
	wait = r.notify
	if from < oldest {
		return nil, from, oldest, wait
	}
	next = from
	frames = dst[:0]
	for s := from; s < r.head && s < from+liveScanMax && len(frames) < cap(frames); s++ {
		f := &r.slots[s%uint64(len(r.slots))]
		if keep(f.id) {
			frames = append(frames, *f)
		}
		next = s + 1
	}
	if next < r.head {
		wait = nil // more to scan: the caller reads again instead of waiting
	}
	return frames, next, oldest, wait
}

// timeAt is the car time of the frame with sequence seq, or 0 when that frame is
// not in the ring.
func (r *liveRing) timeAt(seq uint64) int64 {
	r.mu.RLock()
	defer r.mu.RUnlock()
	if seq < r.oldestLocked() || seq >= r.head {
		return 0
	}
	return r.slots[seq%uint64(len(r.slots))].tNS
}

// oldest is the oldest sequence still in the ring.
// firstAtOrAfter returns the sequence of the oldest frame still in the ring whose
// car time is at or after tNS, or the head when there is none. Frame times only
// roughly increase, so it is a starting point, not an exact cut.
func (r *liveRing) firstAtOrAfter(tNS int64) uint64 {
	r.mu.RLock()
	defer r.mu.RUnlock()
	lo, hi := r.oldestLocked(), r.head
	for lo < hi {
		mid := lo + (hi-lo)/2
		if r.slots[mid%uint64(len(r.slots))].tNS < tNS {
			lo = mid + 1
		} else {
			hi = mid
		}
	}
	return lo
}

func (r *liveRing) oldest() uint64 {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return r.oldestLocked()
}

// stats is the ring's size in frames and the sequence of the next frame.
func (r *liveRing) stats() (capacity int, head uint64) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	return len(r.slots), r.head
}

// bytes is the memory the slots take.
func (r *liveRing) bytes() int {
	return len(r.slots) * int(unsafe.Sizeof(liveFrame{}))
}

// oldestLocked is the oldest sequence still in the ring. r.mu must be held.
func (r *liveRing) oldestLocked() uint64 {
	if n := uint64(len(r.slots)); r.head > n {
		return r.head - n
	}
	return 0
}
