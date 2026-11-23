package main

import (
	"fmt"
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
// ("...i"), IEEE floats as floats.
type frameField struct {
	sig *dbc.Signal
	key string // "<signal name>="
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
		l.fields = append(l.fields, frameField{sig: sig, key: tagEscaper.Replace(sig.Name) + "="})
	}
	slices.SortFunc(l.fields, func(a, b frameField) int { return strings.Compare(a.sig.Name, b.sig.Name) })
	return l
}

// frameSession turns decoded frames into rows for the sink: one frame row per
// frame, plus the signal_def and session rows. Every decoder the server runs
// shares it, and only the appendLoop goroutine touches it.
type frameSession struct {
	sid    string // session ID: the server start time, UTC
	prefix string // "frame,session=<escaped sid>", built once
	sink   lineSink
	now    func() time.Time

	// The session's span on the frame clock, in ms, and the end that the last
	// session row carried.
	startMs     int64
	endMs       int64
	sentEndMs   int64
	lastSession time.Time

	// The DBC the session row names. appendSignalDefs sets them.
	dbcSHA   string
	dbcLabel string

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
		sid:    sid,
		prefix: "frame,session=" + tagEscaper.Replace(sid),
		sink:   sink,
		now:    time.Now,
		buf:    make([]byte, 0, 64<<10),
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

	// page stays -1 when there is no selector or it does not read, so only the
	// signals on every page are written, as the decoder writes them.
	page := int16(-1)
	start := len(s.buf)
	b := append(s.buf, s.prefix...)
	b = append(b, l.msgTag...)
	if l.sel != nil {
		if raw, ok := l.sel.Raw(data); ok {
			page = int16(raw)
			b = append(b, ",tap="...)
			b = strconv.AppendUint(b, raw, 10)
		}
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

// appendSession queues the session row with the span so far. Its time is the
// session start, so a later row overwrites the earlier one in InfluxDB.
func (s *frameSession) appendSession() {
	s.buf = fmt.Appendf(s.buf, "session,id=%s dbc_label=\"%s\",dbc_sha=\"%s\",start=%di,end=%di %d\n",
		tagEscaper.Replace(s.sid), strEscaper.Replace(s.dbcLabel), strEscaper.Replace(s.dbcSHA),
		s.startMs, s.endMs, s.startMs*nsPerMs)
	s.n++
	s.sentEndMs = s.endMs
}

// flush queues the session row when its end has moved and it is due, then
// hands every queued row to the sink in one Write. appendLoop calls it once per
// ingest buffer.
func (s *frameSession) flush() {
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

// finish queues the final session row if the end has moved, and sends every
// queued row. It leaves the sink open, so backfill can run more sessions into
// it.
func (s *frameSession) finish() {
	if s.endMs != s.sentEndMs {
		s.appendSession()
	}
	s.flush()
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
