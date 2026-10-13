package nominal

import (
	"encoding/binary"
	"math"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestPhysicalScale(t *testing.T) {
	sig := signalDef{start: 16, length: 24, little: true, factor: 0.001, offset: 0}
	data := []byte{0, 0, 0x10, 0x27, 0x00, 0, 0, 0}
	bits, ok := extractRaw(sig, data, 8)
	if !ok || bits != 10000 {
		t.Fatalf("bits=%d ok=%v", bits, ok)
	}
	got, ok := sig.physical(bits)
	if !ok || math.Abs(got-10) > 1e-9 {
		t.Fatalf("physical=%v ok=%v", got, ok)
	}
}

func TestSignedPhysical(t *testing.T) {
	sig := signalDef{start: 0, length: 16, little: true, signed: true, factor: 0.1, offset: 5}
	data := []byte{0x18, 0xFC, 0, 0, 0, 0, 0, 0}
	bits, ok := extractRaw(sig, data, 8)
	if !ok {
		t.Fatal("extract")
	}
	got, ok := sig.physical(bits)
	if !ok || math.Abs(got-(-95)) > 1e-9 {
		t.Fatalf("physical=%v", got)
	}
}

func TestCANPSplitChunks(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "mini.dbc")
	body := "BO_ 256 PACK_State: 8 Vector__XXX\n" +
		" SG_ Pack_Voltage : 16|24@1+ (0.001,0) [0|0] \"V\" Vector__XXX\n" +
		" SG_ Cell_idx : 0|8@1+ (1,0) [0|0] \"\" Vector__XXX\n"
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	db, err := LoadDBC(path)
	if err != nil {
		t.Fatal(err)
	}
	frame := canpFrame(256, 8, []byte{3, 0, 0x10, 0x27, 0x00, 0, 0, 0}, 1_700_000_000_000)
	dec := NewDecoder(db)
	if pts := dec.Push(frame[:10], time.UnixMilli(1)); len(pts) != 0 {
		t.Fatalf("partial header produced %d points", len(pts))
	}
	pts := dec.Push(frame[10:], time.UnixMilli(1))
	if len(pts) != 1 {
		t.Fatalf("points=%d", len(pts))
	}
	if pts[0].Channel != "PACK_State.Pack_Voltage" || pts[0].Tap != "3" {
		t.Fatalf("point %+v", pts[0])
	}
	if math.Abs(pts[0].Value-10) > 1e-9 {
		t.Fatalf("value %v", pts[0].Value)
	}
	if pts[0].TimeNs != 1_700_000_000_000*int64(time.Millisecond) {
		t.Fatalf("time %d", pts[0].TimeNs)
	}
}

func TestCarDBCLoads(t *testing.T) {
	candidates := []string{
		filepath.Join("..", ".external", "carcan_telemcan.dbc"),
		filepath.Join("..", "..", "photon-next", "assets", "dbc", "carcan_telemcan.dbc"),
	}
	path := ""
	for _, c := range candidates {
		if _, err := os.Stat(c); err == nil {
			path = c
			break
		}
	}
	if path == "" {
		t.Skip("car DBC is not in this checkout")
	}
	db, err := LoadDBC(path)
	if err != nil {
		t.Fatal(err)
	}
	msg := db.message(1)
	if msg == nil || msg.name != "BPS_Status" || len(msg.signals) == 0 {
		t.Fatalf("BPS_Status missing: %+v", msg)
	}
}

func canpFrame(canID uint32, dlc uint8, data []byte, timeMs uint64) []byte {
	pkt := make([]byte, canpV3Packet)
	binary.BigEndian.PutUint32(pkt[0:4], canID)
	pkt[4] = dlc
	copy(pkt[5:], data)
	buf := make([]byte, canpHeaderBytes+len(pkt))
	binary.BigEndian.PutUint32(buf[0:4], canpMagic)
	binary.BigEndian.PutUint16(buf[4:6], 3)
	binary.BigEndian.PutUint16(buf[6:8], 1)
	binary.BigEndian.PutUint32(buf[8:12], 1)
	binary.BigEndian.PutUint64(buf[12:20], timeMs)
	copy(buf[canpHeaderBytes:], pkt)
	return buf
}
