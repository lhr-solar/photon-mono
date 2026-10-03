// Package cache is the one-second plot tally.
// Samples stay raw integers. Mean is sum/count at read time.
// appendLoop still writes segment files and does not call this package.
// Prove a change with: go test ./cache
// The copied clip is examples/brainerd-30s. A loopback database named photon_dev
// is the place to try a write. The live database name photon stays on the MacBook.
package cache

import "fmt"

// Map is one open recording. Clients do not read it.
type Map struct{}

func New() *Map { return &Map{} }

func (m *Map) Len() int { return 0 }

// Add updates one raw tally. raw is the integer from the frame.
func (m *Map) Add(signal string, sec, raw int64, tap int, hasTap bool, age int64, hasAge bool) {
}

// OpenLines returns the open tallies and leaves them in place.
func (m *Map) OpenLines(session string) []string { return nil }

// TakeClosed returns tallies older than keepSec and drops those buckets.
func (m *Map) TakeClosed(session string, keepSec int64) []string { return nil }

// DB is the car DBC. Factor and offset stay here. Sample rows keep the raw integer.
type DB struct{}

// Frame is one decoded CAN frame. TimeMs is the frame clock in milliseconds.
type Frame struct {
	TimeMs uint64
	CANID  uint32
	DLC    int
	Data   [8]byte
}

func ParseDBC(text string) (*DB, error) {
	return nil, fmt.Errorf("dbc parse is not written")
}

func LoadDBC(path string) (*DB, error) {
	return nil, fmt.Errorf("dbc load is not written")
}

func SHA1Hex(b []byte) string { return "" }

func SignalDefLines(db *DB, sha string, start int64) []string { return nil }

// Fold adds one frame. An index signal is the tap, not its own row.
// An age signal updates age and is not its own row.
// IEEE float and double signals are skipped.
func (m *Map) Fold(db *DB, fr Frame) (added, skippedFloat int) {
	return 0, 0
}
