package dbc

import (
	"encoding/hex"
	"encoding/json"
	"os"
	"strconv"
	"testing"
)

type gotRow struct {
	measurement, bus string
	t                int64
	fields           map[string]float64
}

type gotRaw struct {
	bus, reason string
	id          uint32
	data        []byte
	t           int64
}

// recorder is a Sink that keeps copies of everything.
type recorder struct {
	rows []gotRow
	raws []gotRaw
}

func (r *recorder) Row(measurement, bus string, tNS int64, names []string, values []float64) {
	f := make(map[string]float64, len(names))
	for i, n := range names {
		f[n] = values[i]
	}
	r.rows = append(r.rows, gotRow{measurement, bus, tNS, f})
}

func (r *recorder) Raw(bus string, id uint32, data []byte, tNS int64, reason string) {
	r.raws = append(r.raws, gotRaw{bus, reason, id, append([]byte(nil), data...), tNS})
}

func loadSpec(t *testing.T) *Catalog {
	t.Helper()
	c, err := Load("spec/dbc", testRates)
	if err != nil {
		t.Fatal(err)
	}
	return c
}

const ms = int64(1_000_000)

// Every cantools reference frame, decoded whole, gives the same values: plain
// messages in one row, sampled messages spread over one row per sample.
func TestDecodeMatchesCantools(t *testing.T) {
	c := loadSpec(t)
	text, err := os.ReadFile(cantoolsFrames)
	if err != nil {
		t.Fatal(err)
	}
	var cases []cantoolsCase
	if err := json.Unmarshal(text, &cases); err != nil {
		t.Fatal(err)
	}
	checked := 0
	for k, tc := range cases {
		var r recorder
		d := NewDecoder(c, &r)
		data, _ := hex.DecodeString(tc.Data)
		d.Decode(tc.ID, "test", data, int64(k)*1000*ms)
		if len(r.raws) != 0 {
			t.Fatalf("%s data=%s: unexpected raw %+v", tc.Message, tc.Data, r.raws)
		}
		m := c.Lookup(tc.ID)
		for name, text := range tc.Values {
			want, _ := strconv.ParseFloat(text, 64)
			got, ok := findValue(m, r.rows, name)
			if !ok || !same(got, want) {
				t.Fatalf("%s.%s data=%s: got %v (found=%v), want %v", tc.Message, name, tc.Data, got, ok, want)
			}
			checked++
		}
	}
	t.Logf("%d values in %d frames match cantools through the decoder", checked, len(cases))
}

// findValue finds a cantools signal in the rows: under its own name in a plain
// row, or as sample n of a group (AccelerationX3 is AccelerationX in row 3).
func findValue(m *Message, rows []gotRow, name string) (float64, bool) {
	for _, g := range m.Groups {
		for n, si := range g.Signals {
			if m.Signals[si].Name == name {
				if n >= len(rows) {
					return 0, false
				}
				v, ok := rows[n].fields[g.Field]
				return v, ok
			}
		}
	}
	for _, r := range rows {
		if v, ok := r.fields[name]; ok {
			return v, true
		}
	}
	return 0, false
}

func TestDecodeSampledMessage(t *testing.T) {
	c := loadSpec(t)
	var r recorder
	d := NewDecoder(c, &r)
	m := c.Lookup(0x640) // UprightFRAccelerationData: X/Y/Z × 4 samples at 416 Hz
	frame := make([]byte, m.Length)
	frame[0], frame[1] = 0x39, 0x30 // Timestamp raw 12345 → 1234.5
	tFrame := 1_700_000_000_000 * ms

	d.Decode(0x640, "can1", frame, tFrame)
	if len(r.rows) != 4 {
		t.Fatalf("rows=%d, want 4 (one per sample)", len(r.rows))
	}
	period := m.Groups[0].PeriodNS
	for i, row := range r.rows {
		if row.measurement != "UprightFRAccelerationData" || row.bus != "DAqCAN" {
			t.Errorf("row %d: %s on %s", i, row.measurement, row.bus)
		}
		if want := tFrame - int64(3-i)*period; row.t != want {
			t.Errorf("row %d at %d, want %d", i, row.t, want)
		}
		for _, f := range []string{"AccelerationX", "AccelerationY", "AccelerationZ"} {
			if _, ok := row.fields[f]; !ok {
				t.Errorf("row %d has no %s", i, f)
			}
		}
		if row.fields["Timestamp"] != 1234.5 {
			t.Errorf("row %d Timestamp=%v, want 1234.5", i, row.fields["Timestamp"])
		}
		if len(row.fields) != 4 {
			t.Errorf("row %d has %d fields, want X, Y, Z and Timestamp", i, len(row.fields))
		}
	}
	if r.rows[3].t != tFrame {
		t.Errorf("last sample at %d, want the frame time %d", r.rows[3].t, tFrame)
	}
}

func TestDecodeUnknownID(t *testing.T) {
	c := loadSpec(t)
	var unknown uint32 = 1
	for c.Lookup(unknown) != nil {
		unknown++
	}
	var r recorder
	d := NewDecoder(c, &r)
	d.Decode(unknown, "can0", []byte{1, 2, 3}, 5*ms)
	if len(r.rows) != 0 || len(r.raws) != 1 {
		t.Fatalf("rows=%d raws=%d, want 0 and 1", len(r.rows), len(r.raws))
	}
	if g := r.raws[0]; g.reason != RawUnknownID || g.id != unknown || g.bus != "can0" || string(g.data) != "\x01\x02\x03" {
		t.Errorf("raw = %+v", g)
	}
	if d.Stats.UnknownID != 1 {
		t.Errorf("stats = %+v", d.Stats)
	}
}

// A short frame keeps every signal that fits, and is also saved raw.
func TestDecodeShortFrame(t *testing.T) {
	var r recorder
	d := NewDecoder(loadSpec(t), &r)
	d.Decode(0x611, "fpga", make([]byte, 8), 5*ms) // GPSData is 16 bytes
	if len(r.rows) != 1 || len(r.raws) != 1 || r.raws[0].reason != RawShort {
		t.Fatalf("rows=%d raws=%+v, want one row and one short raw", len(r.rows), r.raws)
	}
	fields := r.rows[0].fields
	if _, ok := fields["GPSLongitude"]; !ok { // bits 4..35
		t.Errorf("GPSLongitude missing from the short row")
	}
	if _, ok := fields["GPSAltitude"]; ok { // bits 100..115
		t.Errorf("GPSAltitude decoded from 8 bytes")
	}
}

func TestDecodeMux(t *testing.T) {
	c := loadSpec(t)
	m := c.Lookup(0x9806E6F4) // Elcon_Control_Mux, selector in byte 7
	page := func(p byte) []byte {
		f := make([]byte, m.Length)
		f[7] = p
		return f
	}

	var r recorder
	d := NewDecoder(c, &r)
	d.Decode(m.ID, "can0", page(4), 5*ms)
	if len(r.rows) != 1 || len(r.raws) != 0 {
		t.Fatalf("page 4: rows=%d raws=%d", len(r.rows), len(r.raws))
	}
	for name := range r.rows[0].fields {
		if s := signal(m, name); s.Mux != 4 && s.Mux != -1 {
			t.Errorf("page 4 row has %s from page %d", name, s.Mux)
		}
	}
	if _, ok := r.rows[0].fields["Elcon_BATTERY_SOC"]; !ok {
		t.Errorf("page 4 row is missing Elcon_BATTERY_SOC")
	}

	r = recorder{}
	d = NewDecoder(c, &r)
	d.Decode(m.ID, "can0", page(9), 5*ms)
	if len(r.raws) != 1 || r.raws[0].reason != RawUnknownMux {
		t.Fatalf("page 9: raws=%+v, want one unknown_mux", r.raws)
	}
	if len(r.rows) != 1 || len(r.rows[0].fields) != 1 {
		t.Errorf("page 9: rows=%+v, want only the selector", r.rows)
	}
}

type discard struct{}

func (discard) Row(string, string, int64, []string, []float64) {}
func (discard) Raw(string, uint32, []byte, int64, string)      {}

// Decoding allocates nothing per frame once the decoder has warmed up, so a
// fast IMU stream adds no garbage for the collector.
func TestDecodeDoesNotAllocate(t *testing.T) {
	c := loadSpec(t)
	d := NewDecoder(c, discard{})
	imu := make([]byte, c.Lookup(0x640).Length)
	pedals := make([]byte, 6)
	tNS := int64(0)
	frame := func() {
		tNS += 10 * ms
		d.Decode(0x640, "can1", imu, tNS)
		d.Decode(0x061, "can0", pedals, tNS)
	}
	frame() // first use grows the scratch slices and time maps
	if n := testing.AllocsPerRun(1000, frame); n != 0 {
		t.Errorf("%v allocations per frame, want 0", n)
	}
}

// Two frames of one message stamped in the same millisecond keep both points.
func TestDecodeSameTime(t *testing.T) {
	var r recorder
	d := NewDecoder(loadSpec(t), &r)
	frame := make([]byte, 6)
	d.Decode(0x061, "can0", frame, 10*ms) // PedalsStatus
	d.Decode(0x061, "can0", frame, 10*ms)
	d.Decode(0x061, "can0", frame, 10*ms-ms/2) // half a millisecond back still counts as the same moment
	d.Decode(0x061, "can0", frame, 5*ms)       // a real step back is left alone
	want := []int64{10 * ms, 10*ms + 1, 10*ms + 2, 5 * ms}
	for i, row := range r.rows {
		if row.t != want[i] {
			t.Errorf("row %d at %d, want %d", i, row.t, want[i])
		}
	}
}
