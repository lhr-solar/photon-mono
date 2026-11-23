package main

import (
	"encoding/json"
	"errors"
)

// v2LiveFrames is the most frames in one LiveFrames message: about 23 KiB at the
// largest frame size.
const v2LiveFrames = 1300

// liveSub is the set of wire IDs a client subscribed to. It is built once by
// subscribe and never changed after, so the pusher can read it without a lock.
type liveSub struct {
	std [2048]bool      // 11-bit IDs, indexed directly
	ext map[uint32]bool // 29-bit IDs, with bit 31 set as on the wire
	any bool            // at least one ID; false means nothing is sent
}

// newLiveSub builds the set for ids. An ID that is neither 11-bit nor 29-bit
// (bit 31 set) is an error.
func newLiveSub(ids []uint32) (*liveSub, error) {
	s := &liveSub{}
	for _, id := range ids {
		switch {
		case id&canpExtended != 0:
			if id&^canpExtended > 0x1FFFFFFF {
				return nil, errors.New("message ID is wider than 29 bits")
			}
			if s.ext == nil {
				s.ext = make(map[uint32]bool)
			}
			s.ext[id] = true
		case id < uint32(len(s.std)):
			s.std[id] = true
		default:
			return nil, errors.New("message ID is wider than 11 bits and has no extended bit (31)")
		}
	}
	s.any = len(ids) > 0
	return s, nil
}

// has reports whether frames with wire ID id are sent. s must not be nil.
func (s *liveSub) has(id uint32) bool {
	if id&canpExtended != 0 {
		return s.ext[id]
	}
	return id < uint32(len(s.std)) && s.std[id]
}

// subscribe handles a subscribe message: the wire IDs to send live frames for,
// replacing any earlier set. An empty list stops live data. The pusher is told
// through resub, and then starts again from the oldest frame in the ring, so the
// client sees frames it joined late; it drops the ones it already has.
func (c *v2Client) subscribe(data []byte) {
	var msg struct {
		Messages []uint32 `json:"messages"`
	}
	if err := json.Unmarshal(data, &msg); err != nil {
		c.enqueueError("bad_subscribe", "subscribe needs a messages list of wire IDs")
		return
	}
	sub, err := newLiveSub(msg.Messages)
	if err != nil {
		c.enqueueError("bad_subscribe", err.Error())
		return
	}
	c.sub.Store(sub)
	select {
	case c.resub <- struct{}{}:
	default: // a signal is already waiting; the pusher will load the new set
	}
}

// busLookup returns a function that gives the bus index of a wire ID in the
// current catalog, or v2BusUnknown. It reads the catalog under h.mu once; the
// function keeps that catalog, so it is safe to use after a catalog change.
func (h *v2Hub) busLookup() func(id uint32) uint8 {
	h.mu.Lock()
	cat, idx := h.cat, h.busIdx
	h.mu.Unlock()
	if cat == nil {
		return func(uint32) uint8 { return v2BusUnknown }
	}
	return func(id uint32) uint8 {
		m := cat.Lookup(id)
		if m == nil {
			return v2BusUnknown
		}
		if i, ok := idx[m.Bus]; ok {
			return i
		}
		return v2BusUnknown
	}
}

// pushLive sends the live frames a client subscribed to, from the hub's ring,
// until the client is done. It never blocks the ingest writer: it only reads the
// ring. A client that falls so far behind that its next frame is overwritten is
// sent a Gap and moved to the newest frame.
func (c *v2Client) pushLive(h *v2Hub) {
	ring := h.ring
	buf := make([]liveFrame, 0, v2LiveFrames)
	var (
		sub    *liveSub // what the client asked for; nil or empty means nothing
		cursor uint64   // sequence of the next frame to read
		last   int64    // car time of the last frame sent, or the end of the last gap; 0 for none
	)
	// reload takes a new subscription, which starts at the oldest frame in the ring.
	reload := func() {
		sub = c.sub.Load()
		cursor = ring.oldest()
	}
	for {
		select {
		case <-c.resub:
			reload()
		default:
		}
		if sub == nil || !sub.any {
			select {
			case <-c.done:
				return
			case <-c.resub:
				reload()
			}
			continue
		}

		frames, next, oldest, wait := ring.read(cursor, buf, sub.has)
		if cursor < oldest {
			// The frames from cursor on were overwritten before this client read
			// them. Say so, and continue from the newest frame.
			_, head := ring.stats()
			from := last
			// Nothing sent yet: the cursor's frame is gone, so the gap starts at the
			// oldest frame still in the ring instead.
			if from == 0 {
				from = ring.timeAt(oldest)
			}
			to := ring.timeAt(head - 1)
			h.gaps.Add(1)
			if !c.enqueueLive(outMsg{typ: wsBinary, data: encodeGap(from, to)}) {
				return
			}
			last = to
			cursor = head
			continue
		}
		if len(frames) > 0 {
			lookup := h.busLookup()
			for rest := frames; len(rest) > 0; {
				n := liveSpanFrames(rest)
				if !c.enqueueLive(outMsg{typ: wsBinary, data: encodeLiveFrames(rest[:n], lookup)}) {
					return
				}
				last = rest[n-1].tNS
				rest = rest[n:]
			}
			cursor = next
			continue
		}

		// Nothing new: next is the head. Wait for an append, a new subscription or
		// the end of the client.
		cursor = next
		select {
		case <-wait:
		case <-c.resub:
			reload()
		case <-c.done:
			return
		}
	}
}
