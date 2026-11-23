package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"server/dbc"
)

// can0BitDBC defines the same low ID on both buses of a CANP v3 car, the way the
// HighNoon 2026 DBC does: 0x1123 (bit 12 set) is the can0 message, 0x123 the
// can1 one.
const can0BitDBC = `BO_ 4387 OnCan0: 1 Node
 SG_ A : 0|8@1+ (1,0) [0|255] "" Node

BO_ 291 OnCan1: 1 Node
 SG_ B : 0|8@1+ (1,0) [0|255] "" Node
`

// The default DBC folder is HighNoon 2026, the car running now, and every DBC
// folder the server ships loads.
func TestDBCFoldersLoad(t *testing.T) {
	if got := DefaultCANConfig().DBCDir; got != "dbc/highnoon2026" {
		t.Errorf("default DBC dir = %q, want dbc/highnoon2026", got)
	}
	for _, c := range []struct {
		dir  string
		want int
	}{
		{"../dbc/highnoon2026", 130},
		{"../dbc/spec/dbc", 117},
		{"../dbc/spec-cdr/dbc", 115},
	} {
		cat, err := dbc.Load(c.dir, DefaultCANConfig().SampleRates)
		if err != nil {
			t.Errorf("%s: %v", c.dir, err)
			continue
		}
		if len(cat.Messages) != c.want {
			t.Errorf("%s: %d messages, want %d", c.dir, len(cat.Messages), c.want)
		}
	}
}

// -dbc-dir and PHOTON_DBC_DIR pick the decoder's DBC folder; the flag wins.
func TestDBCDirSetting(t *testing.T) {
	env := func(k string) string {
		if k == "PHOTON_DBC_DIR" {
			return "dbc/spec-cdr/dbc"
		}
		return ""
	}
	cfg, err := ParseConfigWithEnv(env, nil, os.Stderr)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.CAN.DBCDir != "dbc/spec-cdr/dbc" {
		t.Errorf("env: DBC dir = %q", cfg.CAN.DBCDir)
	}
	cfg, err = ParseConfigWithEnv(env, []string{"-dbc-dir", "dbc/spec/dbc"}, os.Stderr)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.CAN.DBCDir != "dbc/spec/dbc" {
		t.Errorf("flag: DBC dir = %q", cfg.CAN.DBCDir)
	}
}

// With a DBC that marks can0 messages with bit 12, a can0 frame and a can1
// frame with the same low ID decode as their own messages.
func TestCan0BitDBCKeepsBusesApart(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "Car.dbc"), []byte(can0BitDBC), 0o644); err != nil {
		t.Fatal(err)
	}
	sink := &recordSink{}
	now := testNow
	c := newTestDecoder(t, dir, newTestSession(sink, &now))
	c.decode(frameBatch(testTSMS,
		testFrame{id: 0x123 | canpBus0Bit, data: []byte{7}},
		testFrame{id: 0x123, data: []byte{9}},
	))
	rows := rowsWithPrefix(sink.lines(t), "frame,")
	if len(rows) != 2 ||
		!strings.Contains(rows[0], ",msg=OnCan0 A=7i ") ||
		!strings.Contains(rows[1], ",msg=OnCan1 B=9i ") {
		t.Fatalf("rows = %q, want OnCan0 A=7 then OnCan1 B=9", rows)
	}
	if s := c.dec.Stats; s.Frames != 2 || s.UnknownID != 0 {
		t.Errorf("stats = %+v, want 2 frames, none unknown", s)
	}
}

// The live ring keeps a can0 frame's ID as sent (bit 12 set), so a client can
// tell it from a can1 frame with the same low ID, and can subscribe to it.
func TestLiveRingKeepsCan0Bit(t *testing.T) {
	r := newLiveRing(64)
	r.appendBuffer(frameBatch(uint64(time.Now().UnixMilli()),
		testFrame{id: 0x123 | canpBus0Bit, data: []byte{1}},
		testFrame{id: 0x123, data: []byte{2}},
	))
	frames, _, _, _ := r.read(0, make([]liveFrame, 0, 8), func(uint32) bool { return true })
	if len(frames) != 2 || frames[0].id != 0x123|canpBus0Bit || frames[1].id != 0x123 {
		t.Fatalf("ring frames = %+v, want ids 0x1123 then 0x123", frames)
	}
	sub, err := newLiveSub([]uint32{0x123 | canpBus0Bit})
	if err != nil {
		t.Fatal(err)
	}
	if !sub.has(0x123|canpBus0Bit) || sub.has(0x123) {
		t.Errorf("subscription to 0x1123 should match it and not 0x123")
	}
}
