package dbc

import "testing"

// testRates are the car's sensor rates. The server's CAN configuration holds the
// real list; a server test checks that it covers every group in the spec.
var testRates = []SampleRate{
	{`Chassis(Acceleration|Gyro)Data`, 208},
	{`Upright\w+(Acceleration|Gyro)Data`, 416},
	{`Upright\w+WheelSpeedData`, 100},
	{`Suspension\w+Data`, 500},
}

func TestGroupsInSpec(t *testing.T) {
	c, err := Load("spec/dbc", testRates)
	if err != nil {
		t.Fatal(err)
	}
	type want struct {
		fields  []string
		samples int
		hz      float64
	}
	imu := func(hz float64, axis string) want {
		return want{[]string{axis + "X", axis + "Y", axis + "Z"}, 4, hz}
	}
	expect := map[string]want{
		"ChassisAccelerationData":     imu(208, "Acceleration"),
		"ChassisGyroData":             imu(208, "Gyro"),
		"UprightFRAccelerationData":   imu(416, "Acceleration"),
		"UprightFLAccelerationData":   imu(416, "Acceleration"),
		"UprightRearAccelerationData": imu(416, "Acceleration"),
		"UprightFRGyroData":           imu(416, "Gyro"),
		"UprightFLGyroData":           imu(416, "Gyro"),
		"UprightRearGyroData":         imu(416, "Gyro"),
		"UprightFRWheelSpeedData":     {[]string{"WheelRPM"}, 5, 100},
		"UprightFLWheelSpeedData":     {[]string{"WheelRPM"}, 5, 100},
		"UprightRearWheelSpeedData":   {[]string{"WheelRPM"}, 5, 100},
		"SuspensionFRData":            {[]string{"LinpotPosition"}, 5, 500},
		"SuspensionFLData":            {[]string{"LinpotPosition"}, 5, 500},
		"SuspensionRearData":          {[]string{"LinpotPosition"}, 5, 500},
	}

	seen := 0
	for _, m := range c.Messages {
		w, sensor := expect[m.Name]
		if !sensor {
			// Numbered channels read together, like CellVoltage0..31, are not samples.
			if len(m.Groups) > 0 {
				t.Errorf("%s has groups %+v; only sensor messages should", m.Name, m.Groups)
			}
			continue
		}
		seen++
		if len(m.Groups) != len(w.fields) {
			t.Errorf("%s: %d groups, want %d", m.Name, len(m.Groups), len(w.fields))
			continue
		}
		for i, g := range m.Groups {
			if g.Field != w.fields[i] || len(g.Signals) != w.samples {
				t.Errorf("%s group %d: %s with %d samples, want %s with %d", m.Name, i, g.Field, len(g.Signals), w.fields[i], w.samples)
			}
			for n, si := range g.Signals {
				if want := g.Field + string(rune('1'+n)); m.Signals[si].Name != want {
					t.Errorf("%s sample %d is %s, want %s", m.Name, n+1, m.Signals[si].Name, want)
				}
			}
			if wantNS := int64(1e9/w.hz + 0.5); g.PeriodNS != wantNS {
				t.Errorf("%s.%s period %d ns, want %d (%v Hz)", m.Name, g.Field, g.PeriodNS, wantNS, w.hz)
			}
		}
	}
	if seen != len(expect) {
		t.Errorf("found %d of %d sensor messages", seen, len(expect))
	}
}

// Without a rate, a group's samples have no spacing and PeriodNS stays 0.
func TestGroupsWithoutRates(t *testing.T) {
	c, err := Load("spec/dbc", nil)
	if err != nil {
		t.Fatal(err)
	}
	if m := c.Lookup(0x670); len(m.Groups) != 1 || m.Groups[0].PeriodNS != 0 {
		t.Errorf("SuspensionFRData groups = %+v, want one group with no period", m.Groups)
	}
}

func TestBadRates(t *testing.T) {
	for _, rates := range [][]SampleRate{
		{{`Suspension(`, 500}}, // not a regular expression
		{{`Suspension\w+Data`, 0}},
		{{`Suspension\w+Data`, -5}},
	} {
		if _, err := Load("spec/dbc", rates); err == nil {
			t.Errorf("rates %+v loaded without error", rates)
		}
	}
}

func TestGroupsRejectGaps(t *testing.T) {
	const head = "BO_ 1 S: 8 X\n SG_ Timestamp : 0|16@1+ (0.1,0) [0|1] \"\" X\n"
	cases := map[string]string{
		"gap":       head + " SG_ A1 : 16|8@1+ (1,0) [0|1] \"\" X\n SG_ A3 : 24|8@1+ (1,0) [0|1] \"\" X\n",
		"from zero": head + " SG_ A0 : 16|8@1+ (1,0) [0|1] \"\" X\n SG_ A1 : 24|8@1+ (1,0) [0|1] \"\" X\n",
		"encoding":  head + " SG_ A1 : 16|8@1+ (1,0) [0|1] \"\" X\n SG_ A2 : 24|8@1+ (2,0) [0|1] \"\" X\n",
	}
	for name, text := range cases {
		if _, err := parse("Test", text); err == nil {
			t.Errorf("%s: parsed without error", name)
		}
	}
	// Without a Timestamp, numbered signals are channels and need no checks.
	if _, err := parse("Test", "BO_ 1 S: 8 X\n SG_ A0 : 0|8@1+ (1,0) [0|1] \"\" X\n SG_ A3 : 8|8@1+ (2,0) [0|1] \"\" X\n"); err != nil {
		t.Errorf("channels without Timestamp: %v", err)
	}
}
