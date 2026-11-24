package main

import (
	"fmt"
	"log"
	"math"
	"slices"
	"strconv"
	"strings"
	"time"

	"server/dbc"
)

const (
	// nsPerMs turns frame times in ms into line protocol nanoseconds.
	nsPerMs = 1_000_000
	// sessionRowEvery is how often the session row's end is rewritten.
	sessionRowEvery = time.Second
	// rollupNS is the width of a rollup_1s bucket, in ns.
	rollupNS = 1_000_000_000
	// rollupLagNS is how far behind the newest frame a second's bucket waits before
	// it is written. A frame for a second already written is late: it is in the
	// frame rows only.
	rollupLagNS = 2 * rollupNS
	// rollupIdle is how long wall time may pass without a frame before the open
	// buckets are written, so the last seconds of a run that stopped sending reach
	// rollup_1s too.
	rollupIdle = 2 * time.Second
	// rollupIdleCheck is how often appendLoop checks for idle rollup buckets.
	rollupIdleCheck = 500 * time.Millisecond
)

// Line protocol escaping: tag keys and values escape , = and space; string
// fields escape \ and ".
var (
	tagEscaper = strings.NewReplacer(",", `\,`, "=", `\=`, " ", `\ `)
	strEscaper = strings.NewReplacer(`\`, `\\`, `"`, `\"`)
)

// lineSink takes finished line-protocol rows. dbWriter is the real one.
type lineSink interface {
	// Write adds n rows; rows holds them back to back, each ending in '\n'.
	// Write must not keep rows after it returns; the caller reuses it.
	Write(rows []byte, n int)
	Close(wait time.Duration)
}

// frameLayout is how one CAN ID becomes a frame row. buildLayouts works it out
// once per DBC load, with the names already escaped.
type frameLayout struct {
	msgTag string       // ",msg=<message name>"
	sel    *dbc.Signal  // the mux selector, written as the tap tag; nil when there is none
	fields []frameField // the integer signals other than the selector, sorted by name

	// last is the time of the last row per mux page (-1: no tap). Each page is
	// its own InfluxDB series, which keeps one point per time, so rows of a
	// page within dbc.SameTimeWindow are moved apart (see bump).
	last map[int16]int64
}

// bump moves tNS to 1 ns after the page's last row when it falls within
// dbc.SameTimeWindow of it, as the dbc Decoder does: CANP v3 stamps whole
// milliseconds, and two frames of a series in the same millisecond would
// otherwise overwrite each other. An older time is left alone.
func (l *frameLayout) bump(page int16, tNS int64) int64 {
	if prev, ok := l.last[page]; ok && tNS <= prev && tNS > prev-dbc.SameTimeWindow {
		tNS = prev + 1
	}
	l.last[page] = tNS
	return tNS
}

// frameField is one signal, written as a field: integers as raw integers
// ("...i"), IEEE floats as floats. The rollup keys are the field names of the
// rollup_1s row: min, max, sum and count of the second.
type frameField struct {
	sig                          *dbc.Signal
	key                          string // "<signal name>="
	minKey, maxKey, sumKey, nKey string
}

// buildLayouts makes the frame layout of every message in c, keyed by wire ID.
func buildLayouts(c *dbc.Catalog) map[uint32]*frameLayout {
	layouts := make(map[uint32]*frameLayout, len(c.Messages))
	for _, m := range c.Messages {
		layouts[m.ID] = newFrameLayout(m)
	}
	return layouts
}

func newFrameLayout(m *dbc.Message) *frameLayout {
	l := &frameLayout{msgTag: ",msg=" + tagEscaper.Replace(m.Name), last: map[int16]int64{-1: 0}}
	if m.Selector >= 0 {
		l.sel = &m.Signals[m.Selector]
	}
	for i := range m.Signals {
		sig := &m.Signals[i]
		if sig.Mux >= 0 {
			l.last[sig.Mux] = 0 // every known page has its slot, so bump does not allocate
		}
		if i == m.Selector {
			continue
		}
		name := tagEscaper.Replace(sig.Name)
		l.fields = append(l.fields, frameField{sig: sig, key: name + "=",
			minKey: name + "_min=", maxKey: name + "_max=", sumKey: name + "_sum=", nKey: name + "_n="})
	}
	slices.SortFunc(l.fields, func(a, b frameField) int { return strings.Compare(a.sig.Name, b.sig.Name) })
	return l
}

// frameSession turns decoded frames into rows for the sink: one frame row per
// frame, a rollup_1s row per message, page and second, plus the signal_def and
// session rows. Every decoder the server runs shares it, and only the appendLoop
// goroutine touches it.
type frameSession struct {
	sid    string // session ID: the server start time, UTC
	prefix string // "frame,session=<escaped sid>", built once
	sink   lineSink
	now    func() time.Time

	// rollups holds the buckets not yet written, per message and page. The
	// rollup_1s prefix is the session part of their rows.
	rollups      map[rollupKey]*rollupSeries
	rollupPrefix string
	framed       bool      // frames were added since the last flush
	lastFrame    time.Time // when frames were last flushed, by now
	rollupLate   int64     // frames too late for their second's rollup bucket

	// The session's span on the frame clock, in ms, and the end that the last
	// session row carried.
	startMs     int64
	endMs       int64
	sentEndMs   int64
	lastSession time.Time

	// The DBC the session row names. appendSignalDefs sets them.
	dbcSHA   string
	dbcLabel string

	// rev is the run's revision on its session row. The live server leaves it 0;
	// a backfill sets it to the time it started, so a replay always raises it.
	rev int64

	buf []byte // rows waiting for the next flush
	n   int    // rows in buf
}

// newFrameSession starts a session for the server that started at startedAt.
func newFrameSession(startedAt time.Time, sink lineSink) *frameSession {
	return newFrameSessionID(sessionID(startedAt), sink)
}

// newFrameSessionID starts a session with the given ID, as sessionID makes it.
// Backfill replays a past session under its own ID.
func newFrameSessionID(sid string, sink lineSink) *frameSession {
	return &frameSession{
		sid:          sid,
		prefix:       "frame,session=" + tagEscaper.Replace(sid),
		sink:         sink,
		now:          time.Now,
		rollups:      make(map[rollupKey]*rollupSeries),
		rollupPrefix: "rollup_1s,session=" + tagEscaper.Replace(sid),
		buf:          make([]byte, 0, 64<<10),
	}
}

// appendFrame adds one row for a frame to the buffer, and widens the session's
// span to the frame's time. The row has the message as a tag, the mux page as
// the tap tag, and every signal on that page as a field: integer signals raw,
// IEEE float signals as floats (a NaN or infinity is left out, since line
// protocol cannot carry it). No row is written when no signal is left. Once buf
// has room, it does not allocate.
func (s *frameSession) appendFrame(l *frameLayout, data []byte, tNS int64) {
	ms := tNS / nsPerMs
	if s.startMs == 0 || ms < s.startMs {
		s.startMs = ms
	}
	if ms > s.endMs {
		s.endMs = ms
	}
	s.framed = true

	// page stays -1 when there is no selector or it does not read, so only the
	// signals on every page are written, as the decoder writes them.
	page := int16(-1)
	var tap uint64
	hasTap := false
	if l.sel != nil {
		if raw, ok := l.sel.Raw(data); ok {
			page, tap, hasTap = int16(raw), raw, true
		}
	}
	// The rollup bucket is found first: closing buckets writes rows into buf, and
	// the frame's own row goes after them.
	acc := s.rollupBucketFor(l, page, tNS)

	start := len(s.buf)
	b := append(s.buf, s.prefix...)
	b = append(b, l.msgTag...)
	if hasTap {
		b = append(b, ",tap="...)
		b = strconv.AppendUint(b, tap, 10)
	}
	sep := byte(' ')
	for i := range l.fields {
		f := &l.fields[i]
		if f.sig.Mux >= 0 && f.sig.Mux != page {
			continue
		}
		if f.sig.Kind == dbc.Integer {
			v, ok := f.sig.RawInt(data)
			if !ok {
				continue
			}
			b = append(b, sep)
			sep = ','
			b = append(b, f.key...)
			b = strconv.AppendInt(b, v, 10)
			b = append(b, 'i')
			if acc != nil {
				acc[i].addInt(v)
			}
			continue
		}
		raw, ok := f.sig.Raw(data)
		if !ok {
			continue
		}
		v, bits := math.Float64frombits(raw), 64
		if f.sig.Kind == dbc.Float32 {
			v, bits = float64(math.Float32frombits(uint32(raw))), 32
		}
		if math.IsNaN(v) || math.IsInf(v, 0) {
			continue
		}
		b = append(b, sep)
		sep = ','
		b = append(b, f.key...)
		b = strconv.AppendFloat(b, v, 'g', -1, bits)
		if acc != nil {
			acc[i].addFloat(v)
		}
	}
	if sep == ' ' {
		s.buf = b[:start]
		return
	}
	b = append(b, ' ')
	b = strconv.AppendInt(b, l.bump(page, tNS), 10)
	b = append(b, '\n')
	s.buf = b
	s.n++
}

// rollupKey names the rollup series of one message on one page of a layout.
type rollupKey struct {
	l    *frameLayout
	page int16
}

// rollupSeries is the rollup of one message on one page: its buckets that are not
// written yet, and the point up to which it is written.
type rollupSeries struct {
	l      *frameLayout
	prefix string // "rollup_1s,session=<sid>,msg=<msg>[,tap=<page>]", built once
	newest int64  // newest frame time of this series, ns
	done   int64  // buckets at or before this time, ns, are written; a frame at or before it is late
	open   []rollupBucket
	spare  [][]rollupAcc // accumulators of written buckets, for reuse
}

// rollupBucket is one second of a series: sec is its start, ns.
type rollupBucket struct {
	sec int64
	acc []rollupAcc // one per field of the layout
}

// rollupAcc is what one signal had in a second: its count, sum, and the lowest and
// highest value. Integer signals keep their min and max as integers (ilo, ihi);
// float signals keep them as floats (lo, hi).
type rollupAcc struct {
	n        int64
	sum      float64
	lo, hi   float64
	ilo, ihi int64
}

// addInt adds one integer value.
func (a *rollupAcc) addInt(v int64) {
	if a.n == 0 || v < a.ilo {
		a.ilo = v
	}
	if a.n == 0 || v > a.ihi {
		a.ihi = v
	}
	a.n++
	a.sum += float64(v)
}

// addFloat adds one float value.
func (a *rollupAcc) addFloat(v float64) {
	if a.n == 0 || v < a.lo {
		a.lo = v
	}
	if a.n == 0 || v > a.hi {
		a.hi = v
	}
	a.n++
	a.sum += v
}

// rollupSecond is the start, ns, of the second that holds tNS.
func rollupSecond(tNS int64) int64 {
	return tNS - ((tNS%rollupNS)+rollupNS)%rollupNS
}

// rollupBucketFor returns the accumulators of the bucket that a frame at tNS on
// page of layout l goes into. It writes the buckets that the frame closes first.
// It returns nil for a frame too late for its bucket, which counts as rollupLate.
// The frame row is written either way.
func (s *frameSession) rollupBucketFor(l *frameLayout, page int16, tNS int64) []rollupAcc {
	key := rollupKey{l: l, page: page}
	ser := s.rollups[key]
	if ser == nil {
		ser = s.newRollupSeries(l, page)
	}
	sec := rollupSecond(tNS)
	if sec <= ser.done {
		s.rollupLate++
		return nil
	}
	if tNS > ser.newest {
		ser.newest = tNS
		s.closeRollups(ser, tNS-rollupLagNS)
	}
	for i := range ser.open {
		if ser.open[i].sec == sec {
			return ser.open[i].acc
		}
	}
	acc := ser.spareAcc(len(l.fields))
	ser.open = append(ser.open, rollupBucket{sec: sec, acc: acc})
	return acc
}

// newRollupSeries starts the series of page of l.
func (s *frameSession) newRollupSeries(l *frameLayout, page int16) *rollupSeries {
	prefix := s.rollupPrefix + l.msgTag
	if page >= 0 {
		prefix += ",tap=" + strconv.Itoa(int(page))
	}
	ser := &rollupSeries{l: l, prefix: prefix}
	s.rollups[rollupKey{l: l, page: page}] = ser
	return ser
}

// spareAcc returns n empty accumulators, reusing those of a bucket already written.
func (ser *rollupSeries) spareAcc(n int) []rollupAcc {
	if k := len(ser.spare); k > 0 {
		acc := ser.spare[k-1]
		ser.spare = ser.spare[:k-1]
		clear(acc)
		return acc
	}
	return make([]rollupAcc, n)
}

// closeRollups writes the open buckets of ser at or before cut, and sets cut as the
// point up to which ser is written.
func (s *frameSession) closeRollups(ser *rollupSeries, cut int64) {
	if cut > ser.done {
		ser.done = cut
	}
	keep := ser.open[:0]
	for _, b := range ser.open {
		if b.sec > cut {
			keep = append(keep, b)
			continue
		}
		s.appendRollupRow(ser, b)
		ser.spare = append(ser.spare, b.acc)
	}
	ser.open = keep
}

// closeIdleRollups writes every open bucket once no frame has come for rollupIdle
// of wall time. This is how the last seconds of a run reach rollup_1s when the car
// stops sending: a message's buckets are otherwise closed only by its own later
// frames, or by finish. The car's clock is not compared with the wall clock. It runs
// on appendLoop's timer, so it shares the session with the frames.
func (s *frameSession) closeIdleRollups() {
	if s == nil || s.lastFrame.IsZero() || s.now().Sub(s.lastFrame) < rollupIdle {
		return
	}
	open := false
	for _, ser := range s.rollups {
		if len(ser.open) > 0 {
			open = true
			s.closeRollups(ser, ser.newest)
		}
	}
	if open {
		s.flush()
	}
}

// appendRollupRow queues the rollup_1s row of bucket b of ser. Each signal with a
// value in the second gets its min, max, sum and count: integer min and max stay
// integers, and the sum is a float. A signal with no value is left out, and a
// bucket with no signal writes no row.
func (s *frameSession) appendRollupRow(ser *rollupSeries, b rollupBucket) {
	start := len(s.buf)
	buf := append(s.buf, ser.prefix...)
	sep := byte(' ')
	for i := range ser.l.fields {
		a := &b.acc[i]
		if a.n == 0 {
			continue
		}
		f := &ser.l.fields[i]
		buf = append(buf, sep)
		sep = ','
		if f.sig.Kind == dbc.Integer {
			buf = append(buf, f.minKey...)
			buf = strconv.AppendInt(buf, a.ilo, 10)
			buf = append(buf, 'i', ',')
			buf = append(buf, f.maxKey...)
			buf = strconv.AppendInt(buf, a.ihi, 10)
			buf = append(buf, 'i', ',')
		} else {
			bits := 64
			if f.sig.Kind == dbc.Float32 {
				bits = 32
			}
			buf = append(buf, f.minKey...)
			buf = strconv.AppendFloat(buf, a.lo, 'g', -1, bits)
			buf = append(buf, ',')
			buf = append(buf, f.maxKey...)
			buf = strconv.AppendFloat(buf, a.hi, 'g', -1, bits)
			buf = append(buf, ',')
		}
		buf = append(buf, f.sumKey...)
		buf = strconv.AppendFloat(buf, a.sum, 'g', -1, 64)
		buf = append(buf, ',')
		buf = append(buf, f.nKey...)
		buf = strconv.AppendInt(buf, a.n, 10)
		buf = append(buf, 'i')
	}
	if sep == ' ' {
		s.buf = buf[:start]
		return
	}
	buf = append(buf, ' ')
	buf = strconv.AppendInt(buf, b.sec, 10)
	buf = append(buf, '\n')
	s.buf = buf
	s.n++
}

// appendSignalDefs queues one signal_def row for every signal in c, so the
// stored raw values can be scaled later; kind says int, f32 or f64. startMs is the frame time, in ms,
// of the decoder's first frame, and the rows carry it. It also makes c the DBC
// that the session row names.
func (s *frameSession) appendSignalDefs(c *dbc.Catalog, startMs int64) {
	s.dbcSHA = c.SHA
	s.dbcLabel = strings.Join(c.Buses, "+")

	type signalDef struct {
		name string // "<message>.<signal>"
		sig  *dbc.Signal
	}
	var defs []signalDef
	for _, m := range c.Messages {
		for i := range m.Signals {
			defs = append(defs, signalDef{name: m.Name + "." + m.Signals[i].Name, sig: &m.Signals[i]})
		}
	}
	slices.SortStableFunc(defs, func(a, b signalDef) int { return strings.Compare(a.name, b.name) })

	for _, d := range defs {
		s.buf = fmt.Appendf(s.buf, "signal_def,dbc_sha=%s,signal=%s factor=%s,offset=%s,unit=\"%s\",kind=\"%s\" %d\n",
			tagEscaper.Replace(c.SHA), tagEscaper.Replace(d.name),
			strconv.FormatFloat(d.sig.Factor, 'g', -1, 64), strconv.FormatFloat(d.sig.Offset, 'g', -1, 64),
			strEscaper.Replace(d.sig.Unit), signalKind(d.sig.Kind), startMs*nsPerMs)
		s.n++
	}
}

// appendSession queues the session row with the span so far and the run's rev.
// Its time is the session start, so a later row overwrites the earlier one in
// InfluxDB.
func (s *frameSession) appendSession() {
	s.buf = fmt.Appendf(s.buf, "session,id=%s dbc_label=\"%s\",dbc_sha=\"%s\",start=%di,end=%di,rev=%di %d\n",
		tagEscaper.Replace(s.sid), strEscaper.Replace(s.dbcLabel), strEscaper.Replace(s.dbcSHA),
		s.startMs, s.endMs, s.rev, s.startMs*nsPerMs)
	s.n++
	s.sentEndMs = s.endMs
}

// flush queues the session row when its end has moved and it is due, then hands
// every queued row to the sink in one Write. appendLoop calls it once per ingest
// buffer.
func (s *frameSession) flush() {
	if s.framed {
		s.framed = false
		s.lastFrame = s.now()
	}
	if s.endMs != s.sentEndMs {
		if now := s.now(); now.Sub(s.lastSession) >= sessionRowEvery {
			s.lastSession = now
			s.appendSession()
		}
	}
	if s.n == 0 {
		return
	}
	s.sink.Write(s.buf, s.n)
	s.buf, s.n = s.buf[:0], 0
}

// finish writes every rollup bucket still open, queues the final session row if
// the end has moved, and sends every queued row. It leaves the sink open, so
// backfill can run more sessions into it.
func (s *frameSession) finish() {
	for _, ser := range s.rollups {
		s.closeRollups(ser, ser.newest)
	}
	if s.endMs != s.sentEndMs {
		s.appendSession()
	}
	s.flush()
	if s.rollupLate > 0 {
		log.Printf("[photon-server] rollup_1s: %d frames arrived after their second was written; they are in the frame rows only", s.rollupLate)
	}
}

// Close finishes the session, then gives the sink up to wait to deliver its
// rows. It does nothing on a nil session.
func (s *frameSession) Close(wait time.Duration) {
	if s == nil {
		return
	}
	s.finish()
	s.sink.Close(wait)
}

// signalKind names a signal's encoding for signal_def rows.
func signalKind(k dbc.Kind) string {
	switch k {
	case dbc.Float32:
		return "f32"
	case dbc.Float64:
		return "f64"
	}
	return "int"
}
