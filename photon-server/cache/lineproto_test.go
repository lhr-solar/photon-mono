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
	defs := SignalDefLines(db, "sha", 2)
	if len(defs) != 1 || !strings.HasSuffix(defs[0], `,unit="°C\\s" 2000000000`) {
		t.Fatalf("got %v", defs)
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
