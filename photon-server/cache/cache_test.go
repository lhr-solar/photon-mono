package cache

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestBucketFoldsOneSecond(t *testing.T) {
	session := New()
	session.Add("IMU_Accel_FR.Accel_X", 100, 1000, 0, false, 0, false)
	session.Add("IMU_Accel_FR.Accel_X", 100, 2000, 0, false, 0, false)
	lines := session.OpenLines("dev")
	if len(lines) != 1 {
		t.Fatalf("lines=%d", len(lines))
	}
	want := "sample,session=dev,signal=IMU_Accel_FR.Accel_X min=1000i,max=2000i,sum=3000i,count=2i 100000000000"
	if lines[0] != want {
		t.Fatalf("got %s", lines[0])
	}
	if got := session.Len(); got != 1 {
		t.Fatalf("open overwrite dropped the bucket: %d", got)
	}
}

func TestTapAndAgeStayApart(t *testing.T) {
	session := New()
	session.Add("BPS_Temperature_Aggregate_Arr.BPS_Temperature_Tap_Data", 50, 25000, 12, true, 100, true)
	session.Add("BPS_Temperature_Aggregate_Arr.BPS_Temperature_Tap_Data", 50, 26000, 13, true, 80, true)
	lines := session.OpenLines("dev")
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
	session := New()
	session.Add("IMU_Accel_FR.Accel_X", 100, 1000, 0, false, 0, false)
	session.Add("IMU_Accel_FR.Accel_X", 101, 5, 0, false, 0, false)
	closed := session.TakeClosed("dev", 101)
	if len(closed) != 1 || !strings.Contains(closed[0], " 100000000000") {
		t.Fatalf("closed=%v", closed)
	}
	if session.Len() != 1 {
		t.Fatalf("open second dropped, len=%d", session.Len())
	}
	if lines := session.OpenLines("dev"); len(lines) != 1 || !strings.Contains(lines[0], " 101000000000") {
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

	session := db
	imu := Frame{TimeMs: 100_500, CANID: 4512, DLC: 8}
	imu.Data[6] = 0x03
	imu.Data[7] = 0xE8
	if added, skipped := session.Fold(imu); added == 0 || skipped != 0 {
		t.Fatalf("imu added=%d skipped=%d", added, skipped)
	}
	imu.Data[6] = 0x07
	imu.Data[7] = 0xD0
	session.Fold(imu)

	bps := Frame{TimeMs: 100_500, CANID: 12, DLC: 8}
	bps.Data[0] = 12
	bps.Data[1] = 0xA8
	bps.Data[2] = 0x61
	bps.Data[5] = 100
	bps.Data[7] = 7
	if added, _ := session.Fold(bps); added == 0 {
		t.Fatal("bps added nothing")
	}

	var sawAccel, sawTap, sawFrame bool
	for _, line := range session.OpenLines("dev") {
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
	session := db
	fr := Frame{TimeMs: 1000, CANID: 1059, DLC: 8}
	added, skipped := session.Fold(fr)
	if added != 0 || skipped != 2 || session.Len() != 0 {
		t.Fatalf("added=%d skipped=%d len=%d", added, skipped, session.Len())
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
	session := db
	fr := Frame{TimeMs: 2000, CANID: 1, DLC: 8}
	fr.Data[0] = 0x18
	fr.Data[1] = 0xFC
	session.Fold(fr)
	lines := session.OpenLines("dev")
	if len(lines) != 1 || !strings.Contains(lines[0], "min=-1000i") {
		t.Fatalf("%v", lines)
	}
}

func TestFoldRespectsDLCAndBigEndian(t *testing.T) {
	db, err := ParseDBC("BO_ 7 T: 8 X\n SG_ V : 7|16@0- (1,0) [0|0] \"\" X\n")
	if err != nil {
		t.Fatal(err)
	}
	session := db
	fr := Frame{TimeMs: 2500, CANID: 7, DLC: 1, Data: [8]byte{0xff, 0xfe}}
	if added, _ := session.Fold(fr); added != 0 || session.Len() != 0 {
		t.Fatalf("short DLC added=%d len=%d", added, session.Len())
	}
	fr.DLC = 2
	if added, _ := session.Fold(fr); added != 1 {
		t.Fatalf("added=%d", added)
	}
	if lines := session.OpenLines("dev"); len(lines) != 1 || !strings.Contains(lines[0], "min=-2i") {
		t.Fatalf("lines=%v", lines)
	}
}
