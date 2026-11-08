package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"

	"server/dbc"
)

// The spec as the server package sees it; tests run in photon-server/server.
const testDBCDir = "../dbc/spec/dbc"

type canpTestFrame struct {
	id    uint32
	data  []byte
	ageMS uint16
}

// canp3Batch encodes one CANP v3 batch the way the car's sender does.
func canp3Batch(tsMS uint64, frames ...canpTestFrame) []byte {
	out := binary.BigEndian.AppendUint32(nil, canpMagic)
	out = binary.BigEndian.AppendUint16(out, canpVersion)
	out = binary.BigEndian.AppendUint16(out, uint16(len(frames)))
	out = binary.BigEndian.AppendUint32(out, 7) // seq
	out = binary.BigEndian.AppendUint64(out, tsMS)
	for _, f := range frames {
		rec := make([]byte, canpRecordSize)
		binary.BigEndian.PutUint32(rec[0:4], f.id)
		rec[4] = byte(len(f.data))
		copy(rec[5:13], f.data)
		binary.BigEndian.PutUint16(rec[13:15], f.ageMS)
		out = append(out, rec...)
	}
	return out
}

type walkedFrame struct {
	id   uint32
	bus  string
	data string
	tNS  int64
}

func walk(buf []byte) []walkedFrame {
	var got []walkedFrame
	walkCANP3(buf, func(id uint32, bus string, data []byte, tNS int64) {
		got = append(got, walkedFrame{id, bus, string(data), tNS})
	})
	return got
}

func TestWalkCANP3(t *testing.T) {
	buf := canp3Batch(10_000,
		canpTestFrame{id: 0x061 | canpBus0Bit, data: []byte{1, 2, 3, 4, 5, 6}, ageMS: 250},
		canpTestFrame{id: 0x640, data: []byte{9}},
		canpTestFrame{id: 0x1806E6F4 | canpExtended, data: []byte{1, 2, 3, 4, 5, 6, 7, 8}, ageMS: 20_000},
	)
	buf = append(buf, canp3Batch(20_000, canpTestFrame{id: 0x123, data: nil})...)

	got := walk(buf)
	want := []walkedFrame{
		{0x061, "can0", "\x01\x02\x03\x04\x05\x06", 9_750 * 1e6},                             // bit 12 cleared; 250 ms old
		{0x640, "can1", "\x09", 10_000 * 1e6},                                                // dlc 1 gives one byte
		{0x1806E6F4 | canpExtended, "can", "\x01\x02\x03\x04\x05\x06\x07\x08", 10_000 * 1e6}, // age past the timestamp is ignored
		{0x123, "can1", "", 20_000 * 1e6},                                                    // second batch
	}
	if len(got) != len(want) {
		t.Fatalf("got %d frames, want %d: %+v", len(got), len(want), got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("frame %d = %+v, want %+v", i, got[i], want[i])
		}
	}
}

// A dlc above 8 must not read into the dt field.
func TestWalkCANP3ClampsDLC(t *testing.T) {
	buf := canp3Batch(1, canpTestFrame{id: 0x10, data: make([]byte, 8)})
	buf[canpHeaderSize+4] = 15
	if got := walk(buf); len(got) != 1 || len(got[0].data) != 8 {
		t.Fatalf("got %+v, want one frame with 8 data bytes", got)
	}
}

func TestCANDecodingOffByDefault(t *testing.T) {
	cfg, err := ParseConfigWithEnv(func(string) string { return "" }, nil, os.Stderr)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.CAN.Decode {
		t.Errorf("CAN decoding is on by default")
	}
	if startCANDecoder(cfg.CAN) != nil {
		t.Errorf("startCANDecoder returned a decoder with decoding off")
	}
	cfg.CAN.Decode, cfg.CAN.DBCDir = true, ""
	if cfg.Validate() == nil {
		t.Errorf("Validate accepted decoding with no DBC directory")
	}
}

func TestStartCANDecoderBadDir(t *testing.T) {
	cfg := DefaultCANConfig()
	cfg.Decode, cfg.DBCDir = true, filepath.Join(t.TempDir(), "missing")
	if startCANDecoder(cfg) != nil {
		t.Errorf("startCANDecoder returned a decoder for a missing DBC directory")
	}
}

// The configured sample rates must cover every multi-sample message in the
// spec. When the spec gains a sensor message, this fails until
// DefaultCANConfig has its rate.
func TestDefaultSampleRatesCoverSpec(t *testing.T) {
	cfg := DefaultCANConfig()
	c, err := dbc.Load(testDBCDir, cfg.SampleRates)
	if err != nil {
		t.Fatal(err)
	}
	groups := 0
	for _, m := range c.Messages {
		for _, g := range m.Groups {
			groups++
			if g.PeriodNS == 0 {
				t.Errorf("%s.%s has no sample rate; add it to DefaultCANConfig", m.Name, g.Field)
			}
		}
	}
	if groups == 0 {
		t.Fatalf("the spec has no sample groups")
	}
}

// End to end: a v3 buffer through the decoder the server builds, counted in canStats.
func TestCANDecoderCountsFrames(t *testing.T) {
	cfg := DefaultCANConfig()
	cfg.Decode, cfg.DBCDir = true, testDBCDir
	can := startCANDecoder(cfg)
	if can == nil {
		t.Fatal("startCANDecoder returned nil for the spec")
	}
	before := canStats.frames.Load()
	imu := make([]byte, 32)
	buf := canp3Batch(1_000,
		canpTestFrame{id: 0x061 | canpBus0Bit, data: make([]byte, 6)}, // PedalsStatus: one row
		canpTestFrame{id: 0x640, data: imu[:8]},                       // UprightFRAccelerationData, short: v3 carries 8 of its 32 bytes
		canpTestFrame{id: 0x7FD | canpBus0Bit, data: make([]byte, 8)}, // Sig_Wave
	)
	can.decode(buf)
	s := can.dec.Stats
	if s.Frames != 3 || s.UnknownID != 0 || s.Short != 1 {
		t.Errorf("stats = %+v, want 3 frames, 0 unknown, 1 short", s)
	}
	if canStats.frames.Load() != before+3 || !canStats.enabled.Load() {
		t.Errorf("canStats not published: frames %d → %d", before, canStats.frames.Load())
	}
}
