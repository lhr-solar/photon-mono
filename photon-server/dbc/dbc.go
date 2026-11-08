package dbc

import "fmt"

// Kind is how a signal's raw bits are interpreted.
type Kind uint8

const (
	Integer Kind = iota // signed or unsigned per Signal.Signed
	Float32             // SIG_VALTYPE_ 1
	Float64             // SIG_VALTYPE_ 2
)

// extendedFlag marks a 29-bit ID, as the DBC and the wire write it.
const extendedFlag = 0x80000000

type Signal struct {
	Name         string
	Start        uint16 // DBC start bit; for Motorola, the most significant bit
	Bits         uint8  // 1–64
	LittleEndian bool   // @1 Intel, @0 Motorola
	Signed       bool
	Kind         Kind
	Factor       float64
	Offset       float64
	Unit         string
	Mux          int16 // mux page this signal belongs to, or -1 if always present
	need         uint8 // bytes a frame must have to hold this signal; set by Load
}

type Message struct {
	ID        uint32 // as on the wire: extendedFlag set for 29-bit IDs
	Name      string
	Bus       string // the DBC it is defined in, e.g. "MotorCAN"
	Length    uint8  // bytes, 0–64
	Signals   []Signal
	Selector  int     // index of the mux selector in Signals, or -1
	Timestamp int     // index of the "Timestamp" signal, or -1
	Groups    []Group // multi-sample groups; empty for most messages
}

// Group is one quantity sampled several times per frame, such as
// AccelerationX1..AccelerationX4.
type Group struct {
	Field    string // base name, e.g. "AccelerationX"
	Signals  []int  // indexes into Message.Signals, samples 1..N in order
	PeriodNS int64  // time between samples; 0 if the rate is unknown
}

// Catalog is every message the car can send, looked up by wire ID.
// It is built once from the DBCs at startup.
type Catalog struct {
	Messages []*Message
	standard [2048]*Message      // 11-bit IDs, indexed directly
	extended map[uint32]*Message // 29-bit IDs, keyed with extendedFlag set
}

// Lookup returns the message for a wire ID, or nil if no DBC defines it.
func (c *Catalog) Lookup(id uint32) *Message {
	if id&extendedFlag != 0 {
		return c.extended[id]
	}
	if id < uint32(len(c.standard)) {
		return c.standard[id]
	}
	return nil
}

// directBuses are on the sender itself. When two DBCs define the same ID, the
// frames the server sees are the native ones on these buses, not forwarded copies.
var directBuses = map[string]bool{"CarCAN": true, "DAqCAN": true, "TelemetryLink": true}

// add registers m. A duplicate ID is allowed only with an identical layout;
// the copy on a direct bus keeps its bus name.
func (c *Catalog) add(m *Message) error {
	if m.ID&extendedFlag != 0 {
		if m.ID&^extendedFlag > 0x1FFFFFFF {
			return fmt.Errorf("%s: extended ID 0x%X is wider than 29 bits", m.Name, m.ID&^extendedFlag)
		}
	} else if m.ID >= uint32(len(c.standard)) {
		return fmt.Errorf("%s: standard ID 0x%X is wider than 11 bits", m.Name, m.ID)
	}

	old := c.Lookup(m.ID)
	if old == nil {
		c.set(m)
		c.Messages = append(c.Messages, m)
		return nil
	}
	if !sameLayout(old, m) {
		return fmt.Errorf("ID 0x%X is defined differently in %s (%s) and %s (%s)",
			m.ID&^extendedFlag, old.Bus, old.Name, m.Bus, m.Name)
	}
	if directBuses[old.Bus] || !directBuses[m.Bus] {
		return nil
	}
	// The new copy is on a direct bus; it replaces the forwarded one.
	c.set(m)
	for i, x := range c.Messages {
		if x == old {
			c.Messages[i] = m
		}
	}
	return nil
}

func (c *Catalog) set(m *Message) {
	if m.ID&extendedFlag != 0 {
		if c.extended == nil {
			c.extended = make(map[uint32]*Message)
		}
		c.extended[m.ID] = m
		return
	}
	c.standard[m.ID] = m
}

// sameLayout reports whether a and b decode a frame identically. Bus is left
// out; it is the one thing a duplicate is expected to differ in.
func sameLayout(a, b *Message) bool {
	if a.Name != b.Name || a.Length != b.Length || a.Selector != b.Selector || len(a.Signals) != len(b.Signals) {
		return false
	}
	for i := range a.Signals {
		if a.Signals[i] != b.Signals[i] {
			return false
		}
	}
	return true
}
