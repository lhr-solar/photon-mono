package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"math"

	"server/dbc"
)

// v2LiveFrames is the most frames pushLive reads from the ring at once.
const v2LiveFrames = 1300

// liveSub is what a client subscribed to: the wire IDs to read from the ring, and
// the signals to decode out of their messages. It is built once by subscribe and
// never changed after, so the pusher can read it without a lock.
type liveSub struct {
	cat  *dbc.Catalog                  // the catalog the signal indexes belong to
	std  [0x2000]bool                  // wire IDs to read: 11-bit IDs plus the can0 bit (12), indexed directly
	ext  map[uint32]bool               // 29-bit IDs, with bit 31 set as on the wire
	msgs map[*dbc.Message][]liveSignal // the subscribed signals of each message, in catalog order
	any  bool                          // at least one signal; false means nothing is sent
}

// liveSignal is one subscribed signal, and when its samples are taken.
type liveSignal struct {
	index   uint32 // catalog signal index, as the client sent it
	sig     *dbc.Signal
	shiftNS int64 // time from the frame's time to the sample's: 0 unless the signal is in a sample group
}

// newLiveSub builds the subscription to the signals at indexes of cat. sigs is the
// catalog's signals in index order. A repeated index counts once. An index that is
// negative or past the end, and a missing catalog, are errors.
func newLiveSub(cat *dbc.Catalog, sigs []tileSignal, indexes []int64) (*liveSub, error) {
	if cat == nil {
		return nil, errors.New("no catalog")
	}
	s := &liveSub{cat: cat, msgs: make(map[*dbc.Message][]liveSignal)}
	seen := make(map[int64]bool, len(indexes))
	for _, i := range indexes {
		switch {
		case i < 0:
			return nil, fmt.Errorf("signal index %d is negative", i)
		case i >= int64(len(sigs)):
			return nil, fmt.Errorf("signal index %d is past the catalog's %d signals", i, len(sigs))
		case seen[i]:
			continue
		}
		seen[i] = true
		s.add(sigs[i], uint32(i))
	}
	s.any = len(seen) > 0
	return s, nil
}

// add subscribes to signal ts, whose catalog index is index. It also reads the
// wire ID of its message as the DBC has it, and, for a standard ID without the can0
// bit, the same ID with it: a can0 frame carries that bit whether or not the DBC
// does.
func (s *liveSub) add(ts tileSignal, index uint32) {
	m := ts.msg
	s.msgs[m] = append(s.msgs[m], liveSignal{index: index, sig: ts.sig, shiftNS: sampleShift(m, signalPos(m, ts.sig))})
	if m.ID&canpExtended != 0 {
		if s.ext == nil {
			s.ext = make(map[uint32]bool)
		}
		s.ext[m.ID] = true
		return
	}
	s.std[m.ID] = true
	s.std[m.ID|canpBus0Bit] = true
}

// signalPos is the position of sig in m.Signals. sig must point into m.Signals.
func signalPos(m *dbc.Message, sig *dbc.Signal) int {
	for i := range m.Signals {
		if &m.Signals[i] == sig {
			return i
		}
	}
	return -1
}

// sampleShift is how long before its frame a signal's sample is taken. A signal in
// a sample group is sample i of n, which the decoder times n-1-i periods before the
// frame, as dbc's decodeSamples does. Any other signal is at the frame's time.
func sampleShift(m *dbc.Message, pos int) int64 {
	n := 0
	for _, g := range m.Groups {
		n = max(n, len(g.Signals))
	}
	for _, g := range m.Groups {
		for i, si := range g.Signals {
			if si == pos {
				return -int64(n-1-i) * g.PeriodNS
			}
		}
	}
	return 0
}

// has reports whether frames with wire ID id are read. s must not be nil.
func (s *liveSub) has(id uint32) bool {
	if id&canpExtended != 0 {
		return s.ext[id]
	}
	return id < uint32(len(s.std)) && s.std[id]
}

// decode appends to out the samples frame f gives the subscription, and returns
// out. The frame's message is found with the catalog the subscription was built
// on. A signal on a page other than the frame's, a value that does not decode, and
// a NaN or infinite float give no sample.
func (s *liveSub) decode(f liveFrame, out []liveSample) []liveSample {
	m := s.cat.LookupWire(f.id)
	if m == nil {
		return out
	}
	sigs := s.msgs[m]
	if len(sigs) == 0 {
		return out
	}
	data := f.data[:f.n]
	page := int16(-1) // no selector, or it cannot be read: only the signals on every page
	if m.Selector >= 0 {
		if raw, ok := m.Signals[m.Selector].Raw(data); ok {
			page = int16(raw)
		}
	}
	for _, ls := range sigs {
		if ls.sig.Mux >= 0 && ls.sig.Mux != page {
			continue
		}
		v, ok := ls.sig.Value(data)
		if !ok || !finite(v) {
			continue
		}
		out = append(out, liveSample{TNS: f.tNS + ls.shiftNS, Signal: ls.index, Value: v})
	}
	return out
}

// finite reports whether v is neither NaN nor infinite.
func finite(v float64) bool {
	return !math.IsNaN(v) && !math.IsInf(v, 0)
}

// subscribe handles a subscribe message: the catalog signal indexes to send samples
// for, replacing any earlier set. An empty list stops live data. The pusher is told
// through resub, and then starts again from the oldest frame in the ring, so the
// client sees frames it joined late; it drops the ones it already has.
func (c *v2Client) subscribe(data []byte) {
	var msg struct {
		Signals *[]int64 `json:"signals"`
	}
	if err := json.Unmarshal(data, &msg); err != nil || msg.Signals == nil {
		c.enqueueError("bad_subscribe", "subscribe needs a signals list of catalog indexes")
		return
	}
	h := c.hub
	h.mu.Lock()
	sub, err := newLiveSub(h.cat, h.signals, *msg.Signals)
	if err == nil {
		c.sub.Store(sub)
	}
	h.mu.Unlock()
	if err != nil {
		c.enqueueError("bad_subscribe", err.Error())
		return
	}
	select {
	case c.resub <- struct{}{}:
	default: // a signal is already waiting; the pusher will load the new set
	}
}

// pushLive sends the samples a client subscribed to, decoded from the hub's ring
// frames, until the client is done. It never blocks the ingest writer: it only
// reads the ring. A client that falls so far behind that its next frame is
// overwritten is sent a Gap and moved to the newest frame.
func (c *v2Client) pushLive(h *v2Hub) {
	ring := h.ring
	buf := make([]liveFrame, 0, v2LiveFrames)
	var (
		sub     *liveSub     // what the client asked for; nil or empty means nothing
		cursor  uint64       // sequence of the next frame to read
		last    int64        // car time of the last frame read, or the end of the last gap; 0 for none
		samples []liveSample // what the frames read decode to; reused
	)
	// reload takes a new subscription. It starts the live window (-live-seconds)
	// before the newest frame, which covers InfluxDB's write delay, rather than at
	// the oldest frame: at a low frame rate the ring reaches back much further, and
	// every subscribe would resend all of it. With no window set, the oldest frame.
	reload := func() {
		sub = c.sub.Load()
		cursor = ring.oldest()
		if h.liveWindow > 0 {
			if _, head := ring.stats(); head > cursor {
				newest := ring.timeAt(head - 1)
				cursor = ring.firstAtOrAfter(newest - h.liveWindow.Nanoseconds())
			}
		}
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
			samples = samples[:0]
			for _, f := range frames {
				samples = sub.decode(f, samples)
			}
			// Samples of a subscription that was replaced after these frames were read
			// are not sent: its indexes no longer mean these signals. The loop reloads.
			if c.sub.Load() == sub && !c.sendSamples(h, samples) {
				return
			}
			last = frames[len(frames)-1].tNS
			cursor = next
			continue
		}

		cursor = next
		if wait == nil {
			continue // the scan stopped at liveScanMax before the head
		}
		// Nothing new: next is the head. Wait for an append, a new subscription or
		// the end of the client.
		select {
		case <-wait:
		case <-c.resub:
			reload()
		case <-c.done:
			return
		}
	}
}

// sendSamples queues samples as LiveSamples messages and counts them. It returns
// false, without finishing, once the client is done.
func (c *v2Client) sendSamples(h *v2Hub, samples []liveSample) bool {
	for _, m := range liveSampleMessages(samples) {
		if !c.enqueueLive(outMsg{typ: wsBinary, data: m}) {
			return false
		}
	}
	h.liveSamples.Add(uint64(len(samples)))
	return true
}

// dropSubscriptions clears every client's subscription and wakes its pusher, which
// then waits for a new one. The indexes of the old subscriptions no longer mean the
// same signals. h.mu must be held.
func (h *v2Hub) dropSubscriptions() {
	for cl := range h.clients {
		cl.sub.Store(nil)
		select {
		case cl.resub <- struct{}{}:
		default: // a signal is already waiting
		}
	}
}
