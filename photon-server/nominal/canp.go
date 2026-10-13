package nominal

import (
	"encoding/binary"
	"strconv"
	"time"
)

const (
	canpMagic       = 0x43414E31
	canpHeaderBytes = 20
	canpV2Packet    = 21
	canpV3Packet    = 29
	canpMaxBatch    = 64
)

// Point is one decoded signal sample.
type Point struct {
	Channel string
	Tap     string
	TimeNs  int64
	Value   float64
}

// Decoder reassembles CANP v2/v3 batches from TCP chunks and decodes them
// with a DBC. The batch timestamp is unix milliseconds, matching the observer.
type Decoder struct {
	db  *Database
	buf []byte
}

func NewDecoder(db *Database) *Decoder {
	return &Decoder{db: db}
}

func (d *Decoder) Push(payload []byte, now time.Time) []Point {
	if d == nil || d.db == nil || len(payload) == 0 {
		return nil
	}
	d.buf = append(d.buf, payload...)
	var out []Point
	for len(d.buf) >= canpHeaderBytes {
		if binary.BigEndian.Uint32(d.buf[0:4]) != canpMagic {
			d.buf = d.buf[1:]
			continue
		}
		version := binary.BigEndian.Uint16(d.buf[4:6])
		packetBytes := canpV3Packet
		if version == 2 {
			packetBytes = canpV2Packet
		} else if version != 3 {
			d.buf = d.buf[1:]
			continue
		}
		count := int(binary.BigEndian.Uint16(d.buf[6:8]))
		if count == 0 || count > canpMaxBatch {
			d.buf = d.buf[1:]
			continue
		}
		size := canpHeaderBytes + count*packetBytes
		if len(d.buf) < size {
			break
		}
		ms := unixMillis(binary.BigEndian.Uint64(d.buf[12:20]))
		if ms == 0 {
			ms = uint64(now.UnixMilli())
		}
		ts := int64(ms) * int64(time.Millisecond)
		for i := 0; i < count; i++ {
			pkt := d.buf[canpHeaderBytes+i*packetBytes:]
			out = append(out, decodePacket(d.db, pkt, ts)...)
		}
		d.buf = d.buf[size:]
	}
	if len(d.buf) > 1<<20 {
		d.buf = append([]byte(nil), d.buf[len(d.buf)-64:]...)
	}
	return out
}

func decodePacket(db *Database, pkt []byte, timeNs int64) []Point {
	canID := binary.BigEndian.Uint32(pkt[0:4]) & 0x1fffffff
	msg := db.message(canID)
	if msg == nil {
		return nil
	}
	dlc := int(pkt[4])
	if dlc > 8 {
		dlc = 8
	}
	var data [8]byte
	copy(data[:], pkt[5:5+dlc])

	tap := ""
	hasTap := false
	type row struct {
		name  string
		value float64
	}
	var rows []row
	for _, sig := range msg.signals {
		bits, ok := extractRaw(sig, data[:], dlc)
		if !ok {
			continue
		}
		if sig.isIndex {
			tap = strconv.FormatInt(rawInt(sig, bits), 10)
			hasTap = true
			continue
		}
		v, ok := sig.physical(bits)
		if !ok {
			continue
		}
		rows = append(rows, row{name: msg.name + "." + sig.name, value: v})
	}
	if !hasTap {
		tap = ""
	}
	points := make([]Point, 0, len(rows))
	for _, row := range rows {
		points = append(points, Point{
			Channel: row.name,
			Tap:     tap,
			TimeNs:  timeNs,
			Value:   row.value,
		})
	}
	return points
}

// unixMillis accepts microseconds, milliseconds, or seconds. CANP batch
// headers on the car are unix milliseconds (~1e12).
func unixMillis(ts uint64) uint64 {
	switch {
	case ts == 0:
		return 0
	case ts > 1_000_000_000_000_000:
		return ts / 1000
	case ts > 1_000_000_000_000:
		return ts
	case ts > 1_000_000_000:
		return ts * 1000
	default:
		return 0
	}
}
