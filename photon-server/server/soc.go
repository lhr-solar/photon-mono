package main

import (
	"math"

	"server/dbc"
)

// Pack SOC. The car does not send it, so the decoder works it out:
//   - parked (current near zero for a while): read it off the resting cell
//     voltage with the M50T table, so charging between runs shows up;
//   - driving: voltage sags under load, so count amp-hours from the last
//     parked reading instead.
//
// Pack: 32s9p LG 21700 M50T (lhr-solar/BPS config.h MODULE_CELLS_IN_PARALLEL 9).
const (
	packSeries   = 32
	packParallel = 9
	packAh       = packParallel * 4.657289 // ~41.9 Ah, the table's last entry

	socParkedAmps = 1.0          // |current| below this counts as parked
	socParkedNS   = 30 * 1e9     // parked this long before the voltage is trusted
	socMaxGapNS   = 5 * 60 * 1e9 // longer gaps are not integrated across
	socEmitNS     = int64(1e9)   // one Derived row per second
)

// socSink passes every row on to next and adds a "Derived" row with PackSOC
// (%) once a second. Inputs are BPSPackData.CellSumVoltage and
// BPSPackData.BatteryCurrent (positive = discharge). Only the decoder's
// goroutine calls it.
type socSink struct {
	next dbc.Sink

	volts, amps  float64
	haveV, haveI bool
	lastNS       int64 // time of the last current sample used
	parked       bool
	parkedSince  int64
	ah           float64
	known        bool // a parked reading has set ah
	lastEmitNS   int64

	names  []string
	values []float64
}

func newSOCSink(next dbc.Sink) *socSink {
	return &socSink{next: next, names: []string{"PackSOC"}, values: []float64{0}}
}

func (s *socSink) Raw(bus string, id uint32, data []byte, tNS int64, reason string) {
	s.next.Raw(bus, id, data, tNS, reason)
}

func (s *socSink) Row(measurement, bus string, tNS int64, names []string, values []float64) {
	s.next.Row(measurement, bus, tNS, names, values)
	if measurement != "BPSPackData" {
		return
	}
	for i, n := range names {
		switch n {
		case "CellSumVoltage":
			s.volts, s.haveV = values[i], true
		case "BatteryCurrent":
			s.amps, s.haveI = values[i], true
		}
	}
	s.update(tNS)
}

func (s *socSink) update(tNS int64) {
	if !s.haveV || !s.haveI {
		return
	}
	if s.known && s.lastNS > 0 {
		if dt := tNS - s.lastNS; dt > 0 && dt <= socMaxGapNS {
			s.ah -= s.amps * float64(dt) / 3600e9
		}
	}
	s.lastNS = tNS

	if math.Abs(s.amps) < socParkedAmps {
		if !s.parked {
			s.parked, s.parkedSince = true, tNS
		}
		if tNS-s.parkedSince >= socParkedNS {
			s.ah = cellAh(s.volts/packSeries) * packParallel
			s.known = true
		}
	} else {
		s.parked = false
	}

	if s.known && tNS-s.lastEmitNS >= socEmitNS {
		s.lastEmitNS = tNS
		s.values[0] = math.Max(0, math.Min(100, 100*s.ah/packAh))
		s.next.Row("Derived", "derived", tNS, s.names, s.values)
	}
}

// cellAh is the charge left in one cell at a resting voltage, interpolated
// from the M50T table.
func cellAh(volts float64) float64 {
	pos := (volts*1000 - cellTableMinMV) / cellTableStepMV
	last := len(cellChargeUAh) - 1
	switch {
	case !(pos > 0):
		return 0
	case pos >= float64(last):
		return float64(cellChargeUAh[last]) / 1e6
	}
	i := int(pos)
	f := pos - float64(i)
	return (float64(cellChargeUAh[i])*(1-f) + float64(cellChargeUAh[i+1])*f) / 1e6
}
