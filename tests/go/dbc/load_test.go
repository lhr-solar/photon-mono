package dbc

import (
	"strings"
	"testing"
)

// The counts come from the spec copied in spec/dbc, worked out separately from
// this parser. Update them when the spec is updated.
func TestLoadSpec(t *testing.T) {
	c, err := Load("spec/dbc", testRates)
	if err != nil {
		t.Fatal(err)
	}
	// 121 messages in the files; the 4 BPS messages in both CarCAN and ChargerCAN count once.
	if len(c.Messages) != 117 {
		t.Errorf("messages=%d, want 117", len(c.Messages))
	}
	wantMessages := map[string]int{"CarCAN": 30, "ChargerCAN": 1, "DAqCAN": 20, "ElconCAN": 3, "LightingCAN": 3,
		"MPPTCAN": 33, "MotorCAN": 19, "SteeringCAN": 2, "TelemetryLink": 6}
	wantSignals := map[string]int{"CarCAN": 474, "ChargerCAN": 13, "DAqCAN": 144, "ElconCAN": 30, "LightingCAN": 15,
		"MPPTCAN": 54, "MotorCAN": 47, "SteeringCAN": 6, "TelemetryLink": 98}
	gotMessages, gotSignals := map[string]int{}, map[string]int{}
	floats := 0
	for _, m := range c.Messages {
		gotMessages[m.Bus]++
		gotSignals[m.Bus] += len(m.Signals)
		for _, s := range m.Signals {
			if s.Kind == Float32 {
				floats++
			}
		}
		if c.Lookup(m.ID) != m {
			t.Errorf("Lookup(0x%X) does not return %s", m.ID, m.Name)
		}
	}
	for bus, want := range wantMessages {
		if gotMessages[bus] != want || gotSignals[bus] != wantSignals[bus] {
			t.Errorf("%s: %d messages, %d signals; want %d, %d", bus, gotMessages[bus], gotSignals[bus], want, wantSignals[bus])
		}
	}
	// 24 motor controller values and TelemetryLink's Sine and Cosine.
	if floats != 26 {
		t.Errorf("float32 signals=%d, want 26", floats)
	}
}

func TestLoadSpecDetails(t *testing.T) {
	c, err := Load("spec/dbc", testRates)
	if err != nil {
		t.Fatal(err)
	}

	// The BPS duplicates keep CarCAN, the bus the sender is on.
	if m := c.Lookup(0x060); m == nil || m.Name != "BPSStatus" || m.Bus != "CarCAN" {
		t.Errorf("0x060 = %+v, want BPSStatus on CarCAN", m)
	}

	// CAN FD lengths are kept.
	if m := c.Lookup(0x611); m == nil || m.Name != "GPSData" || m.Length != 16 {
		t.Errorf("0x611 = %+v, want GPSData, 16 bytes", m)
	}
	if m := c.Lookup(0x7FF); m == nil || m.Length != 64 {
		t.Errorf("0x7FF = %+v, want 64 bytes", m)
	}

	// Elcon: extended ID with bit 31 set, a selector, and pages m1–m5.
	// The DBC writes it as 2550589172 = 0x9806E6F4: 29-bit ID 0x1806E6F4 plus bit 31.
	mux := c.Lookup(0x9806E6F4)
	if mux == nil || mux.Name != "Elcon_Control_Mux" {
		t.Fatalf("Elcon_Control_Mux not found by its wire ID")
	}
	if mux.Selector < 0 || mux.Signals[mux.Selector].Name != "Elcon_MuxPage" {
		t.Errorf("selector = %d, want Elcon_MuxPage", mux.Selector)
	}
	pages := 0
	for _, s := range mux.Signals {
		if s.Mux >= 1 && s.Mux <= 5 {
			pages++
		}
	}
	// 19 signals on pages m1–m5, plus the selector itself.
	if pages != 19 {
		t.Errorf("paged signals=%d, want 19", pages)
	}
	if c.Lookup(0x1806E6F4) != nil {
		t.Errorf("an extended ID was found without bit 31")
	}

	// Names over 32 characters come from BA_ "SystemSignalLongSymbol", not the cut SG_ name.
	if m := c.Lookup(0x061); m == nil || signal(m, "WarningMainAcceleratorDisconnected") == nil ||
		signal(m, "WarningRedundantAcceleratorDisconnected") == nil {
		t.Errorf("PedalsStatus is missing its long signal names")
	}

	// Sensor messages know where their Timestamp is.
	if m := c.Lookup(0x640); m == nil || m.Name != "UprightFRAccelerationData" || m.Timestamp != 0 {
		t.Errorf("0x640 = %+v, want UprightFRAccelerationData with Timestamp first", m)
	}

	// Unknown IDs are nil, not a panic.
	for _, id := range []uint32{0x7FE, 0x800, 0xFFFF, 0x80000001} {
		if m := c.Lookup(id); m != nil && m.ID != id {
			t.Errorf("Lookup(0x%X) returned %s", id, m.Name)
		}
	}
}

func TestParseRejects(t *testing.T) {
	cases := map[string]string{
		"unreadable signal": "BO_ 1 A: 8 X\n SG_ broken line\n",
		"signal outside":    " SG_ S : 0|8@1+ (1,0) [0|255] \"\" X\n",
		"too long":          "BO_ 1 A: 65 X\n",
		"no selector":       "BO_ 1 A: 8 X\n SG_ S m1 : 0|8@1+ (1,0) [0|255] \"\" X\n",
		"bad float width":   "BO_ 1 A: 8 X\n SG_ S : 0|16@1+ (1,0) [0|1] \"\" X\n\nSIG_VALTYPE_ 1 S : 1;\n",
		"duplicate signal":  "BO_ 1 A: 8 X\n SG_ S : 0|8@1+ (1,0) [0|255] \"\" X\n SG_ S : 8|8@1+ (1,0) [0|255] \"\" X\n",
	}
	for name, text := range cases {
		if _, err := parse("Test", text); err == nil {
			t.Errorf("%s: parsed without error", name)
		}
	}
}

func TestAddRejectsConflictingDuplicate(t *testing.T) {
	a, err := parse("CarCAN", "BO_ 16 A: 8 X\n SG_ S : 0|8@1+ (1,0) [0|255] \"\" X\n")
	if err != nil {
		t.Fatal(err)
	}
	b, err := parse("MotorCAN", "BO_ 16 A: 8 X\n SG_ S : 0|16@1+ (1,0) [0|255] \"\" X\n")
	if err != nil {
		t.Fatal(err)
	}
	var c Catalog
	if err := c.add(a[0]); err != nil {
		t.Fatal(err)
	}
	err = c.add(b[0])
	if err == nil || !strings.Contains(err.Error(), "defined differently") {
		t.Fatalf("err=%v, want a conflict", err)
	}
}
