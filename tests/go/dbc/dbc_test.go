package dbc

import (
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"testing"
)

// oneMessage is a DBC with one message of one 8-bit signal.
func oneMessage(id int, sig string) string {
	return fmt.Sprintf("BO_ %d Msg%d: 8 Node\n SG_ %s : 0|8@1+ (1,0) [0|255] \"\" Node\n\n", id, id, sig)
}

func writeDBCs(t *testing.T, files map[string]string) string {
	t.Helper()
	dir := t.TempDir()
	for name, text := range files {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(text), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func TestCatalogSHAAndBuses(t *testing.T) {
	files := map[string]string{
		"Zed.dbc":   oneMessage(200, "Zsig"),
		"Alpha.dbc": oneMessage(100, "Asig"),
	}
	dir := writeDBCs(t, files)

	c1, err := Load(dir, nil)
	if err != nil {
		t.Fatal(err)
	}
	c2, err := Load(dir, nil)
	if err != nil {
		t.Fatal(err)
	}
	if c1.SHA == "" || c1.SHA != c2.SHA {
		t.Fatalf("SHA not stable across loads: %q, %q", c1.SHA, c2.SHA)
	}
	if want := []string{"Alpha", "Zed"}; !slices.Equal(c1.Buses, want) {
		t.Fatalf("Buses = %v, want %v", c1.Buses, want)
	}

	// A change to one file's contents changes the SHA.
	if err := os.WriteFile(filepath.Join(dir, "Zed.dbc"), []byte(oneMessage(200, "Renamed")), 0o644); err != nil {
		t.Fatal(err)
	}
	c3, err := Load(dir, nil)
	if err != nil {
		t.Fatal(err)
	}
	if c3.SHA == c1.SHA {
		t.Fatalf("SHA unchanged after editing a DBC: %q", c3.SHA)
	}
	if len(c3.SHA) != 40 {
		t.Fatalf("SHA %q is not 40 hex digits", c3.SHA)
	}
}

// Signal lines may be indented with a tab, as some tools write them; they must
// not be skipped. The HighNoon 2026 client DBC lost most of its signals this way.
func TestTabIndentedSignals(t *testing.T) {
	dir := writeDBCs(t, map[string]string{
		"Car.dbc": "BO_ 100 Tabbed: 8 Node\n\tSG_ A : 0|8@1+ (1,0) [0|255] \"\" Node\n\tSG_ B : 8|8@1+ (1,0) [0|255] \"\" Node\n\n",
	})
	c, err := Load(dir, nil)
	if err != nil {
		t.Fatal(err)
	}
	m := c.Lookup(100)
	if m == nil || len(m.Signals) != 2 {
		t.Fatalf("Tabbed = %+v, want 2 signals", m)
	}
}

// The HighNoon 2026 DBC defines its can0 messages with bit 12 set (0x1xxx).
func TestCan0BitIDs(t *testing.T) {
	dir := writeDBCs(t, map[string]string{"Car.dbc": oneMessage(0x1123, "A") + oneMessage(0x123, "B")})
	c, err := Load(dir, nil)
	if err != nil {
		t.Fatal(err)
	}
	if c.Lookup(0x1123) == nil || c.Lookup(0x123) == nil || c.Lookup(0x1123) == c.Lookup(0x123) {
		t.Fatalf("0x1123 and 0x123 should be two messages")
	}
	if _, err := Load(writeDBCs(t, map[string]string{"Car.dbc": oneMessage(0x2000, "C")}), nil); err == nil {
		t.Fatalf("a 14-bit standard ID loaded")
	}
}

// A signal's [min|max] range is kept, with negative and decimal bounds.
func TestSignalRangeKept(t *testing.T) {
	c, err := Load(writeDBCs(t, map[string]string{"Car.dbc": "BO_ 100 R: 8 Node\n" +
		" SG_ Neg : 0|8@1- (1,0) [-655.36|-0.5] \"\" Node\n" +
		" SG_ Dec : 8|16@1+ (0.01,0) [0.0|655.35] \"V\" Node\n" +
		" SG_ Whole : 24|8@1+ (1,0) [0|255] \"\" Node\n\n"}), nil)
	if err != nil {
		t.Fatal(err)
	}
	want := map[string][2]float64{"Neg": {-655.36, -0.5}, "Dec": {0, 655.35}, "Whole": {0, 255}}
	m := c.Lookup(100)
	if m == nil || len(m.Signals) != len(want) {
		t.Fatalf("message = %+v, want 3 signals", m)
	}
	for _, s := range m.Signals {
		if r := want[s.Name]; s.Min != r[0] || s.Max != r[1] {
			t.Errorf("%s range [%v|%v], want [%v|%v]", s.Name, s.Min, s.Max, r[0], r[1])
		}
	}
	if unit := m.Signals[1].Unit; unit != "V" {
		t.Errorf("unit %q after the range, want V", unit)
	}
}

// LookupWire falls back to the ID without the can0 bit when the DBC does not
// define the ID with it, and prefers the exact ID when it does.
func TestLookupWire(t *testing.T) {
	only, err := Load(writeDBCs(t, map[string]string{"Car.dbc": oneMessage(0x123, "A")}), nil)
	if err != nil {
		t.Fatal(err)
	}
	both, err := Load(writeDBCs(t, map[string]string{"Car.dbc": oneMessage(0x123, "A") + oneMessage(0x1123, "B")}), nil)
	if err != nil {
		t.Fatal(err)
	}
	if m := only.LookupWire(0x1123); m == nil || m.ID != 0x123 {
		t.Errorf("LookupWire(0x1123) without 0x1123 defined = %+v, want 0x123", m)
	}
	if m := both.LookupWire(0x1123); m == nil || m.ID != 0x1123 {
		t.Errorf("LookupWire(0x1123) with 0x1123 defined = %+v, want 0x1123", m)
	}
	if m := both.LookupWire(0x123); m == nil || m.ID != 0x123 {
		t.Errorf("LookupWire(0x123) = %+v, want 0x123", m)
	}
	if m := only.LookupWire(0x1124); m != nil {
		t.Errorf("LookupWire(0x1124) = %s, want nil", m.Name)
	}
	if m := only.LookupWire(0x80000000 | 0x1123); m != nil {
		t.Errorf("an extended ID with bit 12 fell back to %s", m.Name)
	}
}

// The same message on two buses may differ in a signal's [min|max]: that is only
// a plot range, so it still loads as one message.
func TestDuplicateIgnoresRange(t *testing.T) {
	a := "BO_ 100 Dup: 8 Node\n SG_ A : 0|8@1+ (1,0) [0|255] \"\" Node\n\n"
	b := "BO_ 100 Dup: 8 Node\n SG_ A : 0|8@1+ (1,0) [0|100] \"\" Node\n\n"
	c, err := Load(writeDBCs(t, map[string]string{"CarCAN.dbc": a, "ChargerCAN.dbc": b}), nil)
	if err != nil {
		t.Fatal(err)
	}
	if c.Lookup(100) == nil {
		t.Fatal("Dup missing")
	}
}

// GenMsgCycleTime gives a message's frame period in ms. A message without one
// has 0.
func TestCycleTimeParsed(t *testing.T) {
	dir := writeDBCs(t, map[string]string{"Car.dbc": oneMessage(100, "A") + oneMessage(101, "B") +
		"BA_ \"GenMsgCycleTime\" BO_ 100 20;\n"})
	c, err := Load(dir, nil)
	if err != nil {
		t.Fatal(err)
	}
	m100, m101 := c.Lookup(100), c.Lookup(101)
	if m100 == nil || m101 == nil {
		t.Fatal("messages 100 and 101 should both load")
	}
	if m100.CycleMS != 20 {
		t.Errorf("message 100 CycleMS = %d, want 20", m100.CycleMS)
	}
	if m101.CycleMS != 0 {
		t.Errorf("message 101 without a cycle time: CycleMS = %d, want 0", m101.CycleMS)
	}
}

// The 2026 HighNoon DBC has no cycle times, so every message has 0.
func TestCycleTimeAbsentIsZero(t *testing.T) {
	c, err := Load("highnoon2026", nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(c.Messages) == 0 {
		t.Fatal("HighNoon has no messages")
	}
	for _, m := range c.Messages {
		if m.CycleMS != 0 {
			t.Errorf("%s CycleMS = %d, want 0", m.Name, m.CycleMS)
		}
	}
}

// A cycle time line that does not read, or names no message, is an error.
func TestCycleTimeBadLineIsError(t *testing.T) {
	for _, line := range []string{
		"BA_ \"GenMsgCycleTime\" BO_ 100 soon;\n",
		"BA_ \"GenMsgCycleTime\" BO_ 100 20\n",
		"BA_ \"GenMsgCycleTime\" BO_ 100 99999999999;\n",
		"BA_ \"GenMsgCycleTime\" BO_ 999 20;\n",
	} {
		dir := writeDBCs(t, map[string]string{"Car.dbc": oneMessage(100, "A") + line})
		if _, err := Load(dir, nil); err == nil {
			t.Errorf("%q loaded without an error", line)
		}
	}
}

// A duplicate that differs only in its cycle time still loads, and the copy on a
// direct bus (CarCAN) keeps its own cycle time.
func TestDuplicateIgnoresCycleTime(t *testing.T) {
	a := oneMessage(100, "A") + "BA_ \"GenMsgCycleTime\" BO_ 100 10;\n"
	b := oneMessage(100, "A") + "BA_ \"GenMsgCycleTime\" BO_ 100 50;\n"
	c, err := Load(writeDBCs(t, map[string]string{"CarCAN.dbc": a, "ChargerCAN.dbc": b}), nil)
	if err != nil {
		t.Fatal(err)
	}
	m := c.Lookup(100)
	if m == nil || m.Bus != "CarCAN" || m.CycleMS != 10 {
		t.Fatalf("100 = %+v, want the CarCAN copy with cycle time 10", m)
	}
}

// The 2027 spec gives a cycle time to 25 of CarCAN's 30 messages; the five External
// command messages have none. BPSStatus (0x060) has 200 ms.
func TestCycleTimeInSpec(t *testing.T) {
	c, err := Load("spec/dbc", testRates)
	if err != nil {
		t.Fatal(err)
	}
	withCycle := 0
	for _, m := range c.Messages {
		if m.Bus == "CarCAN" && m.CycleMS > 0 {
			withCycle++
		}
	}
	if withCycle != 25 {
		t.Errorf("CarCAN messages with a cycle time = %d, want 25", withCycle)
	}
	if m := c.Lookup(0x060); m == nil || m.CycleMS != 200 {
		t.Errorf("BPSStatus CycleMS = %+v, want 200", m)
	}
}
