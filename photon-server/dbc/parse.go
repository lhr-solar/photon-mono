package dbc

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

// Load reads every *.dbc in dir into one Catalog. Each file is one bus, named
// after the file: spec/dbc/MotorCAN.dbc defines the messages of bus "MotorCAN".
// rates give the sample groups their timing.
func Load(dir string, rates []SampleRate) (*Catalog, error) {
	paths, err := filepath.Glob(filepath.Join(dir, "*.dbc"))
	if err != nil {
		return nil, err
	}
	if len(paths) == 0 {
		return nil, fmt.Errorf("no .dbc files in %s", dir)
	}
	c := &Catalog{}
	for _, path := range paths {
		text, err := os.ReadFile(path)
		if err != nil {
			return nil, err
		}
		bus := strings.TrimSuffix(filepath.Base(path), ".dbc")
		msgs, err := parse(bus, string(text))
		if err != nil {
			return nil, fmt.Errorf("%s: %w", path, err)
		}
		for _, m := range msgs {
			if err := c.add(m); err != nil {
				return nil, err
			}
		}
	}
	if err := applyRates(c, rates); err != nil {
		return nil, err
	}
	return c, nil
}

// The DBC lines the decoder needs. Every other line (comments, attributes,
// value tables) is ignored. A BO_ or SG_ line that does not match is an error,
// so a signal can never be dropped without anyone noticing.
var (
	messageLine = regexp.MustCompile(`^BO_ (\d+) (\w+): (\d+) \w+$`)
	// name, mux marker (M or m<page>), start|bits@order sign, (factor,offset), [min|max], "unit"
	signalLine    = regexp.MustCompile(`^ SG_ (\w+)(?: (M|m\d+))? : (\d+)\|(\d+)@([01])([+-]) \(([^,]+),([^)]+)\) \[[^|]*\|[^\]]*\] "([^"]*)" .+$`)
	valueTypeLine = regexp.MustCompile(`^SIG_VALTYPE_ (\d+) (\w+) ?: ?([12]) ?;$`)
	// DBC names stop at 32 characters; a longer name is cut in the BO_ or SG_
	// line and written in full here.
	longNameLine = regexp.MustCompile(`^BA_ "System(?:Signal|Message)LongSymbol" (?:SG_ (\d+) (\w+)|BO_ (\d+)) "([^"]+)";$`)
)

// parse reads one DBC file's messages, all on bus.
func parse(bus, text string) ([]*Message, error) {
	var msgs []*Message
	var cur *Message
	type valueType struct {
		id   uint32
		name string
		kind Kind
		line int
	}
	var valueTypes []valueType
	type longName struct {
		id   uint32
		sig  string // the cut signal name; empty for a message's long name
		full string
		line int
	}
	var longNames []longName

	for n, line := range strings.Split(text, "\n") {
		n++ // line numbers start at 1
		line = strings.TrimRight(line, "\r")
		switch {
		case strings.HasPrefix(line, "BO_ "):
			m, err := parseMessage(line)
			if err != nil {
				return nil, fmt.Errorf("line %d: %w", n, err)
			}
			m.Bus = bus
			msgs = append(msgs, m)
			cur = m
		case strings.HasPrefix(line, " SG_ "):
			if cur == nil {
				return nil, fmt.Errorf("line %d: signal outside a message", n)
			}
			if err := parseSignal(cur, line); err != nil {
				return nil, fmt.Errorf("line %d: %s: %w", n, cur.Name, err)
			}
		case strings.HasPrefix(line, "SIG_VALTYPE_ "):
			p := valueTypeLine.FindStringSubmatch(line)
			if p == nil {
				return nil, fmt.Errorf("line %d: unreadable SIG_VALTYPE_: %q", n, line)
			}
			id, _ := strconv.ParseUint(p[1], 10, 32)
			kind := Float32
			if p[3] == "2" {
				kind = Float64
			}
			valueTypes = append(valueTypes, valueType{uint32(id), p[2], kind, n})
		case strings.HasPrefix(line, "BA_ \"SystemSignalLongSymbol\" ") ||
			strings.HasPrefix(line, "BA_ \"SystemMessageLongSymbol\" "):
			p := longNameLine.FindStringSubmatch(line)
			if p == nil {
				return nil, fmt.Errorf("line %d: unreadable long name: %q", n, line)
			}
			if p[1] != "" {
				id, _ := strconv.ParseUint(p[1], 10, 32)
				longNames = append(longNames, longName{id: uint32(id), sig: p[2], full: p[4], line: n})
			} else {
				id, _ := strconv.ParseUint(p[3], 10, 32)
				longNames = append(longNames, longName{id: uint32(id), full: p[4], line: n})
			}
		case strings.TrimSpace(line) == "":
			cur = nil // a blank line ends the message's signal list
		}
	}

	// Attribute lines come after all the messages, so apply them last.
	// SIG_VALTYPE_ uses the short name, so it goes before the long names.
	for _, vt := range valueTypes {
		sig := findSignal(msgs, vt.id, vt.name)
		if sig == nil {
			return nil, fmt.Errorf("line %d: SIG_VALTYPE_ for unknown signal %d %s", vt.line, vt.id, vt.name)
		}
		if want := map[Kind]uint8{Float32: 32, Float64: 64}[vt.kind]; sig.Bits != want {
			return nil, fmt.Errorf("line %d: %s is a float%d but %d bits long", vt.line, vt.name, want, sig.Bits)
		}
		sig.Kind = vt.kind
	}
	for _, ln := range longNames {
		if ln.sig == "" {
			m := findMessage(msgs, ln.id)
			if m == nil {
				return nil, fmt.Errorf("line %d: long name for unknown message %d", ln.line, ln.id)
			}
			m.Name = ln.full
			continue
		}
		sig := findSignal(msgs, ln.id, ln.sig)
		if sig == nil {
			return nil, fmt.Errorf("line %d: long name for unknown signal %d %s", ln.line, ln.id, ln.sig)
		}
		sig.Name = ln.full
	}

	for _, m := range msgs {
		if err := finish(m); err != nil {
			return nil, fmt.Errorf("%s: %w", m.Name, err)
		}
	}
	return msgs, nil
}

func parseMessage(line string) (*Message, error) {
	p := messageLine.FindStringSubmatch(line)
	if p == nil {
		return nil, fmt.Errorf("unreadable message: %q", line)
	}
	id, err := strconv.ParseUint(p[1], 10, 32)
	if err != nil {
		return nil, fmt.Errorf("message ID %s: %w", p[1], err)
	}
	length, err := strconv.Atoi(p[3])
	if err != nil || length > 64 {
		return nil, fmt.Errorf("%s: length %s is not 0–64 bytes", p[2], p[3])
	}
	return &Message{ID: uint32(id), Name: p[2], Length: uint8(length), Selector: -1, Timestamp: -1}, nil
}

func parseSignal(m *Message, line string) error {
	p := signalLine.FindStringSubmatch(line)
	if p == nil {
		return fmt.Errorf("unreadable signal: %q", line)
	}
	start, _ := strconv.Atoi(p[3])
	bits, _ := strconv.Atoi(p[4])
	if bits < 1 || bits > 64 {
		return fmt.Errorf("%s: %d bits is not 1–64", p[1], bits)
	}
	if start >= 64*8 {
		return fmt.Errorf("%s: start bit %d is past 64 bytes", p[1], start)
	}
	factor, err1 := strconv.ParseFloat(p[7], 64)
	offset, err2 := strconv.ParseFloat(p[8], 64)
	if err1 != nil || err2 != nil {
		return fmt.Errorf("%s: unreadable factor or offset (%s,%s)", p[1], p[7], p[8])
	}
	sig := Signal{
		Name: p[1], Start: uint16(start), Bits: uint8(bits),
		LittleEndian: p[5] == "1", Signed: p[6] == "-",
		Factor: factor, Offset: offset, Unit: p[9], Mux: -1,
	}
	switch mux := p[2]; {
	case mux == "M":
		if m.Selector >= 0 {
			return fmt.Errorf("%s: second mux selector (first is %s)", sig.Name, m.Signals[m.Selector].Name)
		}
		m.Selector = len(m.Signals)
	case mux != "":
		page, err := strconv.ParseInt(mux[1:], 10, 16)
		if err != nil {
			return fmt.Errorf("%s: mux page %s: %w", sig.Name, mux, err)
		}
		sig.Mux = int16(page)
	}
	for _, s := range m.Signals {
		if s.Name == sig.Name {
			return fmt.Errorf("signal %s defined twice", sig.Name)
		}
	}
	m.Signals = append(m.Signals, sig)
	return nil
}

// finish checks a message once all its signals are read, works out how many
// bytes each signal needs, and finds its Timestamp signal and sample groups.
func finish(m *Message) error {
	hasPages := false
	for i := range m.Signals {
		s := &m.Signals[i]
		need := s.span()
		if need > int(m.Length) {
			return fmt.Errorf("%s reaches byte %d of a %d-byte message", s.Name, need, m.Length)
		}
		s.need = uint8(need)
		if s.Mux >= 0 {
			hasPages = true
		}
		if s.Name == "Timestamp" {
			m.Timestamp = i
		}
	}
	if hasPages && m.Selector < 0 {
		return fmt.Errorf("multiplexed signals but no selector (M)")
	}
	return findGroups(m)
}

func findMessage(msgs []*Message, id uint32) *Message {
	for _, m := range msgs {
		if m.ID == id {
			return m
		}
	}
	return nil
}

func findSignal(msgs []*Message, id uint32, name string) *Signal {
	for _, m := range msgs {
		if m.ID != id {
			continue
		}
		for i := range m.Signals {
			if m.Signals[i].Name == name {
				return &m.Signals[i]
			}
		}
	}
	return nil
}
