package cache

import (
	"math"
	"os"
	"testing"
)

func TestMotorVelocityFloatBits(t *testing.T) {
	b, err := os.ReadFile(`F:\lhr\_handoff_logs_dbc\HighNoon.dbc`)
	if err != nil {
		t.Skip(err)
	}
	s, diag, err := ValidateDBC(string(b))
	if err != nil {
		for _, d := range diag {
			t.Log(d)
		}
		t.Fatalf("validate: %v (%d)", err, len(diag))
	}
	var def SignalDef
	found := false
	for _, d := range s.Definitions {
		if d.Message == "MC_VelocityMeasurement" && d.ID.Name == "MC_MotorVelocity" {
			def = d
			found = true
		}
	}
	if !found {
		t.Fatal("missing MC_MotorVelocity")
	}
	if def.ValueType != 1 {
		t.Fatalf("value type %d, want float32", def.ValueType)
	}
	bits := uint32(1127270016)
	var data [8]byte
	data[0] = byte(bits)
	data[1] = byte(bits >> 8)
	data[2] = byte(bits >> 16)
	data[3] = byte(bits >> 24)
	var got float64
	for _, v := range s.Decode(Frame{CANID: 1059, DLC: 8, Data: data, TimeMs: 1}) {
		if v.Def.ID.Name == "MC_MotorVelocity" {
			got = v.Physical
		}
	}
	want := float64(math.Float32frombits(bits))
	if math.Abs(got-want) > 0.01 {
		t.Fatalf("physical %g, want %g", got, want)
	}
}
