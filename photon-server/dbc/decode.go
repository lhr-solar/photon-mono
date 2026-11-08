package dbc

// Sink receives what the Decoder makes of each frame. names, values and data
// are reused for the next frame; a Sink that keeps them must copy them.
type Sink interface {
	// Row is one point: the message name, the bus it is defined on, the time
	// in Unix nanoseconds, and its signals' physical values.
	Row(measurement, bus string, tNS int64, names []string, values []float64)
	// Raw is a frame, or the part of one, that could not be fully decoded.
	// bus is the interface the frame arrived on, since there may be no
	// message to name one.
	Raw(bus string, id uint32, data []byte, tNS int64, reason string)
}

// Reasons a frame is passed to Sink.Raw.
const (
	RawUnknownID  = "unknown_id"  // no DBC defines the ID
	RawShort      = "short"       // too short for some signals; the rest were decoded
	RawUnknownMux = "unknown_mux" // the selector names a page the DBC does not define
)

// Stats counts what the Decoder has done. Only the Decoder's goroutine writes it.
type Stats struct {
	Frames     uint64
	Rows       uint64
	UnknownID  uint64
	Short      uint64
	UnknownMux uint64
}

// Decoder turns frames into rows using a Catalog. It is not safe for
// concurrent use; the parser owns one.
type Decoder struct {
	catalog *Catalog
	sink    Sink
	Stats   Stats

	names  []string
	values []float64
	last   map[*Message]int64 // time of the last row per message
	lastID map[uint32]int64   // time of the last raw frame per ID
}

func NewDecoder(c *Catalog, sink Sink) *Decoder {
	return &Decoder{catalog: c, sink: sink, last: map[*Message]int64{}, lastID: map[uint32]int64{}}
}

// Decode handles one frame: its wire ID, the interface it arrived on, its data
// bytes, and its capture time in Unix nanoseconds.
func (d *Decoder) Decode(id uint32, bus string, data []byte, tNS int64) {
	d.Stats.Frames++
	m := d.catalog.Lookup(id)
	if m == nil {
		d.Stats.UnknownID++
		d.raw(bus, id, data, tNS, RawUnknownID)
		return
	}
	if len(m.Groups) > 0 {
		d.decodeSamples(m, bus, id, data, tNS)
	} else {
		d.decodePlain(m, bus, id, data, tNS)
	}
}

// decodePlain writes one row with every signal that applies to this frame.
func (d *Decoder) decodePlain(m *Message, bus string, id uint32, data []byte, tNS int64) {
	// page stays -1 when there is no selector or it cannot be read; then only
	// the signals present on every page are decoded.
	page, unknownPage := int16(-1), false
	if m.Selector >= 0 {
		if raw, ok := m.Signals[m.Selector].Raw(data); ok {
			page = int16(raw)
			unknownPage = !m.hasPage(page)
		}
	}
	d.names, d.values = d.names[:0], d.values[:0]
	short := false
	for i := range m.Signals {
		s := &m.Signals[i]
		if s.Mux >= 0 && s.Mux != page {
			continue
		}
		v, ok := s.Value(data)
		if !ok {
			short = true
			continue
		}
		d.names = append(d.names, s.Name)
		d.values = append(d.values, v)
	}
	if len(d.names) > 0 {
		d.row(m, d.time(m, tNS))
	}
	if short {
		d.Stats.Short++
		d.raw(bus, id, data, tNS, RawShort)
	}
	if unknownPage {
		d.Stats.UnknownMux++
		d.raw(bus, id, data, tNS, RawUnknownMux)
	}
}

// decodeSamples writes one row per sample number. Sample i of every group is
// taken at the same moment, so they share a row. The last sample is at the
// frame's capture time and the earlier ones are one period apart before it.
// Every row carries the frame's Timestamp, the sensor's own time of sample 1,
// so rows can be re-timed once that clock can be mapped to wall time.
func (d *Decoder) decodeSamples(m *Message, bus string, id uint32, data []byte, tNS int64) {
	n := 0
	for _, g := range m.Groups {
		n = max(n, len(g.Signals))
	}
	ts, tsOK := m.Signals[m.Timestamp].Value(data)
	short := !tsOK
	for i := 0; i < n; i++ {
		d.names, d.values = d.names[:0], d.values[:0]
		var period int64
		for _, g := range m.Groups {
			if i >= len(g.Signals) {
				continue
			}
			period = max(period, g.PeriodNS)
			s := &m.Signals[g.Signals[i]]
			v, ok := s.Value(data)
			if !ok {
				short = true
				continue
			}
			d.names = append(d.names, g.Field)
			d.values = append(d.values, v)
		}
		if i == n-1 {
			d.appendOthers(m, data, &short)
		}
		if len(d.names) == 0 {
			continue
		}
		if tsOK {
			d.names = append(d.names, m.Signals[m.Timestamp].Name)
			d.values = append(d.values, ts)
		}
		d.row(m, d.time(m, tNS-int64(n-1-i)*period))
	}
	if short {
		d.Stats.Short++
		d.raw(bus, id, data, tNS, RawShort)
	}
}

// appendOthers adds the signals of a sampled message that are neither a sample
// nor its Timestamp. They go on the last row, at the frame's capture time.
func (d *Decoder) appendOthers(m *Message, data []byte, short *bool) {
	for i := range m.Signals {
		if i == m.Timestamp || m.inGroup(i) {
			continue
		}
		v, ok := m.Signals[i].Value(data)
		if !ok {
			*short = true
			continue
		}
		d.names = append(d.names, m.Signals[i].Name)
		d.values = append(d.values, v)
	}
}

func (d *Decoder) row(m *Message, tNS int64) {
	d.Stats.Rows++
	d.sink.Row(m.Name, m.Bus, tNS, d.names, d.values)
}

func (d *Decoder) raw(bus string, id uint32, data []byte, tNS int64, reason string) {
	tNS = bump(d.lastID, id, tNS)
	d.sink.Raw(bus, id, data, tNS, reason)
}

func (d *Decoder) time(m *Message, tNS int64) int64 {
	return bump(d.last, m, tNS)
}

// sameTimeWindow is how far behind the last row a time may be and still be
// treated as the same moment. Influx keeps one point per series and time, so
// two frames of a message stamped in the same millisecond would overwrite each
// other. A time within the window is moved to 1 ns after the last one; an
// older time is left alone, so a clock that steps back is not dragged forward.
const sameTimeWindow = 1_000_000

func bump[K comparable](last map[K]int64, key K, tNS int64) int64 {
	if prev, ok := last[key]; ok && tNS <= prev && tNS > prev-sameTimeWindow {
		tNS = prev + 1
	}
	last[key] = tNS
	return tNS
}

// hasPage reports whether any signal is on mux page page.
func (m *Message) hasPage(page int16) bool {
	for i := range m.Signals {
		if m.Signals[i].Mux == page {
			return true
		}
	}
	return false
}

func (m *Message) inGroup(i int) bool {
	for _, g := range m.Groups {
		for _, si := range g.Signals {
			if si == i {
				return true
			}
		}
	}
	return false
}
