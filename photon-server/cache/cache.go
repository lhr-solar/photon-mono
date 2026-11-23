// Package cache is the one-second plot tally.
// Samples stay raw integers. Mean is sum/count at read time.
// appendLoop still writes segment files and does not call this package.
// From the repository root, prove a change with: python tests/go/run.py test ./cache
// The copied clip is examples/brainerd-30s. A loopback database named photon_dev
// is the place to try a write. The live database name photon stays on the MacBook.
package cache

import (
	"crypto/sha1"
	"encoding/hex"
	"fmt"
	"sort"
	"strconv"
	"strings"
)

// SignalID identifies a signal within the session's DBC.
type SignalID struct {
	Bus      string
	CANID    uint32
	Extended bool
	Name     string
}

// is loaded from the DBC and does not change during a session.
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

	// AgeMS is the worst DBC _Age signal seen in the second. It is not
	// transport delay: the car's δt age is already applied to Frame.TimeMs.
	AgeMS  int64
	HasAge bool
}

// SessionCache owns the definitions and aggregates for one recording.
type SessionCache struct {
	SessionID string
	DBCSHA    string
	DecodeID  string

	Definitions map[SignalID]SignalDef
	Samples     map[SampleKey]*Sample
	byCANID     map[uint32][]SignalDef
	layouts     map[uint32]*frameLayout // frame rows, keyed like byCANID
}

// New creates the definitions and samples for one recording.
func New() *SessionCache {
	return &SessionCache{
		Definitions: make(map[SignalID]SignalDef),
		Samples:     make(map[SampleKey]*Sample),
	}
}

func (s *SessionCache) Len() int {
	return len(s.Samples)
}

// Add updates one raw tally. raw is the integer from the frame.
func (s *SessionCache) Add(signal string, sec, raw int64, tap int, hasTap bool, age int64, hasAge bool) {
	key := SampleKey{Signal: SignalID{Name: signal}, Tap: -1, Second: sec}
	if hasTap {
		key.Tap = int32(tap)
	}

	smp, ok := s.Samples[key]
	if !ok {
		smp = &Sample{Min: raw, Max: raw}
		s.Samples[key] = smp
	}

	if raw < smp.Min {
		smp.Min = raw
	}
	if raw > smp.Max {
		smp.Max = raw
	}
	smp.Sum += raw
	smp.Count++

	if hasAge && (!smp.HasAge || age > smp.AgeMS) {
		smp.AgeMS = age
		smp.HasAge = true
	}
}

// OpenLines returns the open tallies and leaves them in place.
func (s *SessionCache) OpenLines(session string) []string {
	keys := make([]SampleKey, 0, len(s.Samples))
	for k := range s.Samples {
		keys = append(keys, k)
	}
	sortKeys(keys)

	lines := make([]string, 0, len(keys))
	for _, k := range keys {
		lines = append(lines, sampleLine(session, k, s.Samples[k]))
	}
	return lines
}

// sortKeys orders keys by signal name, then second, then tap.
func sortKeys(keys []SampleKey) {
	sort.Slice(keys, func(i, j int) bool {
		a, b := keys[i], keys[j]
		if a.Signal.Name != b.Signal.Name {
			return a.Signal.Name < b.Signal.Name
		}
		if a.Second != b.Second {
			return a.Second < b.Second
		}
		return a.Tap < b.Tap
	})
}

// Line protocol escaping: tag values escape , = and space; string fields
// escape \ and ".
var (
	tagEscaper = strings.NewReplacer(",", `\,`, "=", `\=`, " ", `\ `)
	strEscaper = strings.NewReplacer(`\`, `\\`, `"`, `\"`)
)

// nsPerSec makes row timestamps nanoseconds, the line protocol default.
const nsPerSec = 1_000_000_000

// sampleLine formats one tally as a line-protocol row.
func sampleLine(session string, k SampleKey, smp *Sample) string {
	line := "sample,session=" + tagEscaper.Replace(session) + ",signal=" + tagEscaper.Replace(k.Signal.Name)
	if k.Tap >= 0 {
		line += fmt.Sprintf(",tap=%d", k.Tap)
	}
	line += fmt.Sprintf(" min=%di,max=%di,sum=%di,count=%di", smp.Min, smp.Max, smp.Sum, smp.Count)
	if smp.HasAge {
		line += fmt.Sprintf(",age=%di", smp.AgeMS)
	}
	line += fmt.Sprintf(" %d", k.Second*nsPerSec)
	return line
}

// TakeClosed returns tallies older than keepSec and drops those buckets.
func (s *SessionCache) TakeClosed(session string, keepSec int64) []string {
	var keys []SampleKey
	for k := range s.Samples {
		if k.Second < keepSec {
			keys = append(keys, k)
		}
	}
	sortKeys(keys)

	lines := make([]string, 0, len(keys))
	for _, k := range keys {
		lines = append(lines, sampleLine(session, k, s.Samples[k]))
		delete(s.Samples, k)
	}
	return lines
}

// Frame is one decoded CAN frame. TimeMs is the frame clock in milliseconds.
type Frame struct {
	TimeMs uint64
	CANID  uint32
	DLC    int
	Data   [8]byte
}

func SHA1Hex(b []byte) string {
	sum := sha1.Sum(b)
	return hex.EncodeToString(sum[:])
}

// nsPerMs turns frame-clock milliseconds into line protocol nanoseconds.
const nsPerMs = 1_000_000

// SignalDefLines returns one signal_def row per DBC signal, so stored raw
// integers can be scaled later. startMs is the session start on the frame
// clock; the rows match what the photon database already holds.
func SignalDefLines(db *SessionCache, sha string, startMs int64) []string {
	// Build each "Message.Signal" name once; the sort compares them many times.
	type named struct {
		name string
		def  SignalDef
	}
	defs := make([]named, 0, len(db.Definitions))
	for _, def := range db.Definitions {
		defs = append(defs, named{def.Message + "." + def.ID.Name, def})
	}
	sort.Slice(defs, func(i, j int) bool {
		if defs[i].name != defs[j].name {
			return defs[i].name < defs[j].name
		}
		return defs[i].def.ID.CANID < defs[j].def.ID.CANID
	})

	lines := make([]string, 0, len(defs))
	for _, d := range defs {
		def := d.def
		line := "signal_def,dbc_sha=" + tagEscaper.Replace(sha) + ",signal=" + tagEscaper.Replace(d.name)
		line += " factor=" + strconv.FormatFloat(def.Factor, 'g', -1, 64)
		line += ",offset=" + strconv.FormatFloat(def.Offset, 'g', -1, 64)
		line += `,unit="` + strEscaper.Replace(def.Unit) + `"`
		line += fmt.Sprintf(" %d", startMs*nsPerMs)
		lines = append(lines, line)
	}
	return lines
}

// SessionLine returns the session row. Its time is startMs, so a later call
// with a newer endMs overwrites the same row.
func SessionLine(id, dbcLabel, sha string, startMs, endMs int64) string {
	return fmt.Sprintf(`session,id=%s dbc_label="%s",dbc_sha="%s",start=%di,end=%di %d`,
		tagEscaper.Replace(id), strEscaper.Replace(dbcLabel), strEscaper.Replace(sha),
		startMs, endMs, startMs*nsPerMs)
}

// frameLayout is how one CAN ID becomes a row, worked out once at DBC load:
// names are already escaped, and tap and field signals already sorted out.
type frameLayout struct {
	msgTag string      // ",msg=<message>"
	taps   []SignalDef // _idx signals; the first that decodes is the tap tag
	fields []frameField
}

type frameField struct {
	def SignalDef
	key string // "<signal>="
}

func newFrameLayout(defs []SignalDef) *frameLayout {
	l := &frameLayout{msgTag: ",msg=" + tagEscaper.Replace(defs[0].Message)}
	for _, def := range defs {
		switch {
		case isTapIndex(def):
			l.taps = append(l.taps, def)
		case def.ValueType == 1 || def.ValueType == 2: // IEEE float/double
		default:
			l.fields = append(l.fields, frameField{def: def, key: tagEscaper.Replace(def.ID.Name) + "="})
		}
	}
	return l
}

// FramePrefix is the start of every frame row in a session. Build it once
// and pass it to AppendFrameLine.
func FramePrefix(session string) string {
	return "frame,session=" + tagEscaper.Replace(session)
}

// AppendFrameLine appends one row for a decoded frame to dst, ending in '\n':
// prefix (from FramePrefix), the message as a tag, and every integer signal
// as a raw field. An index signal is the tap tag, so multiplexed frames at the
// same instant stay apart. IEEE float and double signals are skipped. ok is
// false, and dst is returned unchanged, when the frame has no signal to write.
// It appends straight into the caller's buffer, so a row costs no allocation.
func (s *SessionCache) AppendFrameLine(dst []byte, prefix string, fr Frame) (out []byte, ok bool) {
	l := s.layouts[fr.CANID]
	if l == nil {
		return dst, false
	}
	start := len(dst)
	dst = append(dst, prefix...)
	dst = append(dst, l.msgTag...)
	for _, def := range l.taps {
		if raw, ok := extractRaw(def, fr.Data, fr.DLC); ok {
			dst = append(dst, ",tap="...)
			dst = strconv.AppendInt(dst, raw, 10)
			break
		}
	}

	sep := byte(' ')
	for i := range l.fields {
		f := &l.fields[i]
		raw, ok := extractRaw(f.def, fr.Data, fr.DLC)
		if !ok {
			continue
		}
		dst = append(dst, sep)
		sep = ','
		dst = append(dst, f.key...)
		dst = strconv.AppendInt(dst, raw, 10)
		dst = append(dst, 'i')
	}
	if sep == ' ' {
		return dst[:start], false
	}
	dst = append(dst, ' ')
	dst = strconv.AppendUint(dst, fr.TimeMs*nsPerMs, 10)
	dst = append(dst, '\n')
	return dst, true
}

// Fold adds one frame. An index signal is the tap, not its own row.
// An age signal updates age and is not its own row.
// IEEE float and double signals are skipped.
func (s *SessionCache) Fold(fr Frame) (added, skippedFloat int) {
	defs := s.byCANID[fr.CANID]
	sec := int64(fr.TimeMs / 1000)

	// Pass 1: the tap index and the age describe the other signals in this frame.
	var tap int
	var age int64
	var hasTap, hasAge bool
	for _, def := range defs {
		switch {
		case isTapIndex(def):
			if raw, ok := extractRaw(def, fr.Data, fr.DLC); ok {
				tap, hasTap = int(raw), true
			}
		case isAge(def):
			if raw, ok := extractRaw(def, fr.Data, fr.DLC); ok {
				age, hasAge = raw, true
			}
		}
	}

	// Pass 2: every other signal becomes a sample.
	for _, def := range defs {
		if isTapIndex(def) || isAge(def) {
			continue
		}
		if def.ValueType == 1 || def.ValueType == 2 {
			skippedFloat++
			continue
		}
		raw, ok := extractRaw(def, fr.Data, fr.DLC)
		if !ok {
			continue
		}
		s.Add(def.Message+"."+def.ID.Name, sec, raw, tap, hasTap, age, hasAge)
		added++
	}
	return added, skippedFloat
}

func isTapIndex(def SignalDef) bool { return strings.HasSuffix(def.ID.Name, "_idx") }
func isAge(def SignalDef) bool      { return strings.HasSuffix(def.ID.Name, "_Age") }
func extractRaw(def SignalDef, data [8]byte, dlc int) (int64, bool) {
	n := int(def.BitLength)
	start := int(def.StartBit)
	if n <= 0 || n > 64 || dlc < 0 || dlc > 8 {
		return 0, false
	}
	avail := dlc * 8

	var raw uint64
	if def.LittleEndian {
		if start+n > avail {
			return 0, false
		}
		for i := 0; i < n; i++ {
			bit := start + i
			raw |= uint64((data[bit/8]>>(bit%8))&1) << i
		}
	} else {
		bit := start
		for i := 0; i < n; i++ {
			if bit < 0 || bit >= avail {
				return 0, false
			}
			raw = raw<<1 | uint64((data[bit/8]>>(bit%8))&1)
			if bit%8 == 0 {
				bit += 15
			} else {
				bit--
			}
		}
	}

	if def.Signed {
		shift := 64 - n
		return int64(raw<<shift) >> shift, true
	}
	return int64(raw), true
}
