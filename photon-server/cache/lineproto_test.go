package cache

import (
	"strings"
	"testing"
)

func TestLineProtocolEscaping(t *testing.T) {
	session := New()
	session.Add("Msg.Temp A,B=C", 1, 5, 0, false, 0, false)
	lines := session.OpenLines("run 1")
	want := `sample,session=run\ 1,signal=Msg.Temp\ A\,B\=C min=5i,max=5i,sum=5i,count=1i 1000000000`
	if len(lines) != 1 || lines[0] != want {
		t.Fatalf("got %v\nwant %s", lines, want)
	}

	// DBC units cannot hold a quote, so a backslash is the case to escape.
	db, err := ParseDBC("BO_ 1 T: 8 X\n SG_ V : 0|8@1+ (1,0) [0|0] \"°C\\s\" X\n")
	if err != nil {
		t.Fatal(err)
	}
	defs := SignalDefLines(db, "sha", 2000)
	want = `signal_def,dbc_sha=sha,signal=T.V factor=1,offset=0,unit="°C\\s" 2000000000`
	if len(defs) != 1 || defs[0] != want {
		t.Fatalf("got %v\nwant %s", defs, want)
	}
}

func TestSessionLine(t *testing.T) {
	got := SessionLine("2026-07-23-03", "carcan_telemcan", "abc", 1784825289056, 1784832504212)
	want := `session,id=2026-07-23-03 dbc_label="carcan_telemcan",dbc_sha="abc",start=1784825289056i,end=1784832504212i 1784825289056000000`
	if got != want {
		t.Fatalf("got  %s\nwant %s", got, want)
	}
}

func TestAgeKeepsWorstInSecond(t *testing.T) {
	session := New()
	session.Add("M.S", 1, 1, 0, false, 300, true)
	session.Add("M.S", 1, 1, 0, false, 20, true)
	lines := session.OpenLines("dev")
	if len(lines) != 1 || !strings.Contains(lines[0], ",age=300i ") {
		t.Fatalf("got %v", lines)
	}
}

func TestExtendedAndStandardIDsStayApart(t *testing.T) {
	db, err := ParseDBC("BO_ 256 Std: 8 X\n SG_ A : 0|8@1+ (1,0) [0|0] \"\" X\n" +
		"BO_ 2147483904 Ext: 8 X\n SG_ B : 0|8@1+ (1,0) [0|0] \"\" X\n")
	if err != nil {
		t.Fatal(err)
	}
	db.Fold(Frame{TimeMs: 1000, CANID: 0x100, DLC: 8})
	db.Fold(Frame{TimeMs: 1000, CANID: 0x80000100, DLC: 8})
	lines := db.OpenLines("dev")
	if len(lines) != 2 || !strings.Contains(lines[0], "signal=Ext.B ") || !strings.Contains(lines[1], "signal=Std.A ") {
		t.Fatalf("got %v", lines)
	}
}

func TestFrameLine(t *testing.T) {
	db, err := ParseDBC(`BO_ 16 Cell: 8 X
 SG_ Cell_idx : 0|8@1+ (1,0) [0|0] "" X
 SG_ Volt : 8|16@1+ (0.001,0) [0|0] "V" X
 SG_ Neg : 24|8@1- (1,0) [0|0] "" X
`)
	if err != nil {
		t.Fatal(err)
	}
	fr := Frame{TimeMs: 1784825000123, CANID: 16, DLC: 8, Data: [8]byte{3, 0x10, 0x0e, 0xff}}
	prev := []byte("prev\n")
	got, ok := db.AppendFrameLine(prev, FramePrefix("run 1"), fr)
	want := "prev\n" + `frame,session=run\ 1,msg=Cell,tap=3 Neg=-1i,Volt=3600i 1784825000123000000` + "\n"
	if !ok || string(got) != want {
		t.Fatalf("got  %q\nwant %q", got, want)
	}

	if out, ok := db.AppendFrameLine(prev, FramePrefix("s"), Frame{CANID: 99, DLC: 8}); ok || string(out) != "prev\n" {
		t.Fatalf("unknown CAN ID made a row: %q", out)
	}
	// Cell_idx and every field fail to extract from an empty frame, after the
	// tags are written; nothing of the partial row may stay in dst.
	if out, ok := db.AppendFrameLine(prev, FramePrefix("s"), Frame{CANID: 16, DLC: 0}); ok || string(out) != "prev\n" {
		t.Fatalf("empty frame left a partial row: %q", out)
	}
}
