// Package cache is the one-second plot tally.
// Samples stay raw integers. Mean is sum/count at read time.
// appendLoop still writes segment files and does not call this package.
// Prove a change with: go test ./cache
// The copied clip is examples/brainerd-30s. A loopback database named photon_dev
// is the place to try a write. The live database name photon stays on the MacBook.
package cache

import "math/big"

// SignalID identifies a signal within the session's DBC.
type SignalID struct {
	Bus      string
	CANID    uint32
	Extended bool
	Name     string
}

// SignalDef is loaded from the DBC and does not change during a session.
type SignalDef struct {
	ID        SignalID
	Message   string
	DLC       uint8
	ValueType int

	StartBit     uint16
	BitLength    uint16
	LittleEndian bool
	Signed       bool

	Factor   float64
	Offset   float64
	Unit     string
	Number   uint32
	Min, Max float64
}

// SampleKey identifies one aggregate.
type SampleKey struct {
	Signal SignalID
	Tap    int32 // -1 = no tap/channel; 0 is a valid index
	Second int64 // Unix seconds
}

// Sample is updated as readings arrive. Values are unscaled integers.
type Sample struct {
	Min   int64
	Max   int64
	Sum   int64
	Count int64

	AgeMS              int64
	HasAge             bool
	Dirty              bool // Changed since last queued live snapshot
	WideSum            big.Int
	ExactMin, ExactMax big.Int
}

type FloatSample struct {
	Min, Max, Sum float64
	Count         int64
	AgeMS         int64
	HasAge        bool
}
type Value struct {
	Def               SignalDef
	Raw               int64
	Bits              uint64
	Numeric, Physical float64
	Tap               int32
	Age               int64
	HasAge            bool
}

// SessionCache owns the definitions and aggregates for one recording.
type SessionCache struct {
	SessionID string
	DBCSHA    string
	DecodeID  string

	Definitions map[SignalID]SignalDef
	Samples     map[SampleKey]*Sample
	Floats      map[SampleKey]*FloatSample
	byCANID     map[uint32][]SignalDef
}

// New creates the definitions and samples for one recording.
func New() *SessionCache {
	return &SessionCache{
		Definitions: make(map[SignalID]SignalDef),
		Samples:     make(map[SampleKey]*Sample),
		Floats:      make(map[SampleKey]*FloatSample),
	}
}

func (s *SessionCache) Len() int { return len(s.Samples) + len(s.Floats) }

// Add updates one raw tally. raw is the integer from the frame.
func (s *SessionCache) Add(signal string, sec, raw int64, tap int, hasTap bool, age int64, hasAge bool) {
	id := SignalID{Name: signal}
	for k, d := range s.Definitions {
		if d.Message+"."+k.Name == signal {
			id = k
			break
		}
	}
	t := int32(-1)
	if hasTap {
		t = int32(tap)
	}
	s.add(SampleKey{id, t, sec}, raw, age, hasAge)
}

// OpenLines returns the open tallies and leaves them in place.
func (s *SessionCache) OpenLines(session string) []string { return s.lines(session, nil) }

// TakeClosed returns tallies older than keepSec and drops those buckets.
func (s *SessionCache) TakeClosed(session string, keepSec int64) []string {
	lines := s.ClosedLines(session, keepSec)
	s.DropClosed(keepSec)
	return lines
}

// Frame is one decoded CAN frame. TimeMs is the frame clock in milliseconds.
type Frame struct {
	TimeMs uint64
	CANID  uint32
	DLC    int
	Data   [8]byte
	Bus    string
	Flags  uint8
	DT     [8]uint16
}

// Fold adds one frame. An index signal is the tap, not its own row.
// An age signal updates age and is not its own row.
// IEEE float and double signals use the separate float tally map.
func (s *SessionCache) Fold(fr Frame) (added, skippedFloat int) {
	return s.fold(fr)
}
