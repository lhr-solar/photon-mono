// Package cache is the one-second plot tally.
// Samples stay raw integers. Mean is sum/count at read time.
// appendLoop still writes segment files and does not call this package.
// From the repository root, prove a change with: python tests/go/run.py test ./cache
// The copied clip is examples/brainerd-30s. A loopback database named photon_dev
// is the place to try a write. The live database name photon stays on the MacBook.
package cache

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
	ValueType int

	StartBit     uint16
	BitLength    uint16
	LittleEndian bool
	Signed       bool

	Factor float64
	Offset float64
	Unit   string
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

	AgeMS  int64
	HasAge bool
	Dirty  bool // Changed since last queued live snapshot
}

// SessionCache owns the definitions and aggregates for one recording.
type SessionCache struct {
	SessionID string
	DBCSHA    string
	DecodeID  string

	Definitions map[SignalID]SignalDef
	Samples     map[SampleKey]*Sample
	byCANID     map[uint32][]SignalDef
}

// New creates the definitions and samples for one recording.
func New() *SessionCache {
	return &SessionCache{
		Definitions: make(map[SignalID]SignalDef),
		Samples:     make(map[SampleKey]*Sample),
	}
}

func (s *SessionCache) Len() int { return 0 }

// Add updates one raw tally. raw is the integer from the frame.
func (s *SessionCache) Add(signal string, sec, raw int64, tap int, hasTap bool, age int64, hasAge bool) {

}

// OpenLines returns the open tallies and leaves them in place.
func (s *SessionCache) OpenLines(session string) []string { return nil }

// TakeClosed returns tallies older than keepSec and drops those buckets.
func (s *SessionCache) TakeClosed(session string, keepSec int64) []string { return nil }

// Frame is one decoded CAN frame. TimeMs is the frame clock in milliseconds.
type Frame struct {
	TimeMs uint64
	CANID  uint32
	DLC    int
	Data   [8]byte
}

func SHA1Hex(b []byte) string { return "" }

func SignalDefLines(db *SessionCache, sha string, start int64) []string { return nil }

// Fold adds one frame. An index signal is the tap, not its own row.
// An age signal updates age and is not its own row.
// IEEE float and double signals are skipped.
func (s *SessionCache) Fold(fr Frame) (added, skippedFloat int) {
	return 0, 0
}
