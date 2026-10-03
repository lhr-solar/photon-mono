package cache

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestBucketFoldsOneSecond(t *testing.T) {
	m := New()
	m.Add("IMU_Accel_FR.Accel_X", 100, 1000, 0, false, 0, false)
	m.Add("IMU_Accel_FR.Accel_X", 100, 2000, 0, false, 0, false)
	lines := m.OpenLines("dev")
	if len(lines) != 1 {
		t.Fatalf("lines=%d", len(lines))
	}
	want := "sample,session=dev,signal=IMU_Accel_FR.Accel_X min=1000i,max=2000i,sum=3000i,count=2i 100000"
	if lines[0] != want {
		t.Fatalf("got %s", lines[0])
	}
	if got := m.Len(); got != 1 {
		t.Fatalf("open overwrite dropped the bucket: %d", got)
	}
}

func TestTapAndAgeStayApart(t *testing.T) {
	m := New()
	m.Add("BPS_Temperature_Aggregate_Arr.BPS_Temperature_Tap_Data", 50, 25000, 12, true, 100, true)
	m.Add("BPS_Temperature_Aggregate_Arr.BPS_Temperature_Tap_Data", 50, 26000, 13, true, 80, true)
	lines := m.OpenLines("dev")
	if len(lines) != 2 {
		t.Fatalf("lines=%v", lines)
	}
	if !strings.Contains(lines[0], ",tap=12 ") || !strings.Contains(lines[0], "age=100i") {
		t.Fatalf("tap 12: %s", lines[0])
	}
	if !strings.Contains(lines[1], ",tap=13 ") {
		t.Fatalf("tap 13: %s", lines[1])
	}
}

func TestClosedSecondDrops(t *testing.T) {
	m := New()
	m.Add("IMU_Accel_FR.Accel_X", 100, 1000, 0, false, 0, false)
	m.Add("IMU_Accel_FR.Accel_X", 101, 5, 0, false, 0, false)
	closed := m.TakeClosed("dev", 101)
	if len(closed) != 1 || !strings.Contains(closed[0], " 100000") {
		t.Fatalf("closed=%v", closed)
	}
	if m.Len() != 1 {
		t.Fatalf("open second dropped, len=%d", m.Len())
	}
	if lines := m.OpenLines("dev"); len(lines) != 1 || !strings.Contains(lines[0], " 101000") {
		t.Fatalf("open=%v", lines)
	}
}

func TestCarDBCFoldsIMUAndBPS(t *testing.T) {
	path := filepath.Join("..", "..", "photon-next", "assets", "dbc", "carcan_telemcan.dbc")
	text, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	sha := SHA1Hex(text)
	const dbcSHA = "1a0f0acdac7a0f104a4ce889aad6b1ecf89b66b1"
	if sha != dbcSHA {
		t.Fatalf("dbc sha %s", sha)
	}
	db, err := ParseDBC(string(text))
	if err != nil {
		t.Fatal(err)
	}
	defs := SignalDefLines(db, sha, 1)
	var accel string
	for _, line := range defs {
		if strings.Contains(line, "signal=IMU_Accel_FR.Accel_X ") {
			accel = line
			break
		}
	}
	if !strings.Contains(accel, "factor=-0.000122") {
		t.Fatalf("accel def %s", accel)
	}

	m := New()
	imu := Frame{TimeMs: 100_500, CANID: 4512, DLC: 8}
	imu.Data[6] = 0x03
	imu.Data[7] = 0xE8
	if added, skipped := m.Fold(db, imu); added == 0 || skipped != 0 {
		t.Fatalf("imu added=%d skipped=%d", added, skipped)
	}
	imu.Data[6] = 0x07
	imu.Data[7] = 0xD0
	m.Fold(db, imu)

	bps := Frame{TimeMs: 100_500, CANID: 12, DLC: 8}
	bps.Data[0] = 12
	bps.Data[1] = 0xA8
	bps.Data[2] = 0x61
	bps.Data[5] = 100
	bps.Data[7] = 7
	if added, _ := m.Fold(db, bps); added == 0 {
		t.Fatal("bps added nothing")
	}

	var sawAccel, sawTap, sawFrame bool
	for _, line := range m.OpenLines("dev") {
		if strings.Contains(line, "signal=IMU_Accel_FR.Accel_X ") {
			sawAccel = true
			if !strings.Contains(line, "min=1000i,max=2000i,sum=3000i,count=2i") {
				t.Fatalf("accel %s", line)
			}
			if strings.Contains(line, "tap=") {
				t.Fatalf("accel has a tap: %s", line)
			}
		}
		if strings.Contains(line, "BPS_Temperature_Tap_Data") {
			sawTap = true
			if !strings.Contains(line, ",tap=12 ") || !strings.Contains(line, "min=25000i") || !strings.Contains(line, "age=100i") {
				t.Fatalf("tap %s", line)
			}
		}
		if strings.Contains(line, "FrameID_BPS_Temperature") {
			sawFrame = true
			if !strings.Contains(line, ",tap=12 ") || !strings.Contains(line, "min=7i") {
				t.Fatalf("frame id %s", line)
			}
		}
		if strings.Contains(line, "BPS_Tap_idx") || strings.Contains(line, "Tap_Age") {
			t.Fatalf("index or age stored as a sample: %s", line)
		}
	}
	if !sawAccel || !sawTap || !sawFrame {
		t.Fatalf("accel=%v tap=%v frame=%v", sawAccel, sawTap, sawFrame)
	}
}

func TestFloatSignalsAreSkipped(t *testing.T) {
	path := filepath.Join("..", "..", "photon-next", "assets", "dbc", "carcan_telemcan.dbc")
	db, err := LoadDBC(path)
	if err != nil {
		t.Fatal(err)
	}
	m := New()
	fr := Frame{TimeMs: 1000, CANID: 1059, DLC: 8}
	added, skipped := m.Fold(db, fr)
	if added != 0 || skipped != 2 || m.Len() != 0 {
		t.Fatalf("added=%d skipped=%d len=%d", added, skipped, m.Len())
	}
}

func TestSignedExtract(t *testing.T) {
	db, err := ParseDBC(`
BO_ 1 T: 8 X
 SG_ V : 0|16@1- (1,0) [0|0] "" X
`)
	if err != nil {
		t.Fatal(err)
	}
	m := New()
	fr := Frame{TimeMs: 2000, CANID: 1, DLC: 8}
	fr.Data[0] = 0x18
	fr.Data[1] = 0xFC
	m.Fold(db, fr)
	lines := m.OpenLines("dev")
	if len(lines) != 1 || !strings.Contains(lines[0], "min=-1000i") {
		t.Fatalf("%v", lines)
	}
}
