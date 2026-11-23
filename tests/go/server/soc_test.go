package main

import (
	"math"
	"testing"
)

type socRows struct{ soc []float64 }

func (r *socRows) Row(m, _ string, _ int64, names []string, values []float64) {
	if m == "Derived" {
		r.soc = append(r.soc, values[0])
	}
}
func (r *socRows) Raw(string, uint32, []byte, int64, string) {}

func feed(s *socSink, tNS int64, volts, amps float64) {
	s.Row("BPSPackData", "can", tNS, []string{"CellSumVoltage", "BatteryCurrent"}, []float64{volts, amps})
}

func TestSOCParkedThenDriving(t *testing.T) {
	rows := &socRows{}
	s := newSOCSink(rows)
	const sec = int64(1e9)
	volts := 3.7 * packSeries

	// Not trusted until parked for 30 s.
	for i := int64(0); i < 30; i++ {
		feed(s, i*sec, volts, 0)
	}
	if len(rows.soc) != 0 {
		t.Fatalf("SOC before 30 s parked: %v", rows.soc)
	}
	feed(s, 30*sec, volts, 0)
	start := rows.soc[len(rows.soc)-1]
	want := 100 * cellAh(3.7) * packParallel / packAh
	if math.Abs(start-want) > 1e-9 || start < 5 || start > 95 {
		t.Fatalf("parked SOC = %v, want %v", start, want)
	}

	// Drive 6 minutes at packAh amps (sagged voltage ignored): 0.1 h -> -10 points.
	for i := int64(31); i <= 31+360; i++ {
		feed(s, i*sec, volts-10, packAh)
	}
	got := rows.soc[len(rows.soc)-1]
	if math.Abs((start-got)-10) > 0.05 {
		t.Fatalf("after 0.1 h at 1C: %v -> %v, want a 10 point drop", start, got)
	}
}

func TestCellAhTableEnds(t *testing.T) {
	if cellAh(2.4) != 0 || math.Abs(cellAh(4.2)-4.657289) > 1e-9 {
		t.Fatalf("table ends: %v %v", cellAh(2.4), cellAh(4.2))
	}
}
