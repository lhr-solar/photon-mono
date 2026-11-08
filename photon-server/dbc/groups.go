package dbc

import (
	"fmt"
	"math"
	"regexp"
	"strconv"
)

// SampleRate is how fast the sensors behind some messages sample. The DBCs only
// say how many samples a frame holds, not how far apart they are;
// GenMsgCycleTime is rounded to whole milliseconds and is the frame period, not
// the sample period. So the rates come from the server's configuration.
type SampleRate struct {
	Messages string  // regular expression matched against the whole message name
	Hz       float64 // samples per second
}

// applyRates sets every group's PeriodNS from the first rate whose pattern
// matches its message. A group no rate matches keeps PeriodNS 0.
func applyRates(c *Catalog, rates []SampleRate) error {
	patterns := make([]*regexp.Regexp, len(rates))
	for i, r := range rates {
		p, err := regexp.Compile("^(?:" + r.Messages + ")$")
		if err != nil {
			return fmt.Errorf("sample rate %q: %w", r.Messages, err)
		}
		if !(r.Hz > 0) {
			return fmt.Errorf("sample rate %q: %v Hz is not positive", r.Messages, r.Hz)
		}
		patterns[i] = p
	}
	for _, m := range c.Messages {
		var period int64
		for i, p := range patterns {
			if p.MatchString(m.Name) {
				period = int64(math.Round(1e9 / rates[i].Hz))
				break
			}
		}
		for g := range m.Groups {
			m.Groups[g].PeriodNS = period
		}
	}
	return nil
}

// numbered splits a name like AccelerationX3 into its base and sample number.
var numbered = regexp.MustCompile(`^(.*\D)(\d+)$`)

// findGroups finds the quantities sampled several times per frame. Only a
// message with a Timestamp signal has them: its Timestamp is the time of
// sample 1, and Base1..BaseN are samples 1..N. Numbered signals in other
// messages, such as CellVoltage0..31, are channels read together, not samples.
func findGroups(m *Message) error {
	m.Groups = nil
	if m.Timestamp < 0 {
		return nil
	}
	samples := map[string]map[int]int{} // base → sample number → signal index
	var bases []string                  // in order of first appearance
	for i, s := range m.Signals {
		p := numbered.FindStringSubmatch(s.Name)
		if p == nil {
			continue
		}
		n, _ := strconv.Atoi(p[2])
		if samples[p[1]] == nil {
			samples[p[1]] = map[int]int{}
			bases = append(bases, p[1])
		}
		samples[p[1]][n] = i
	}

	for _, base := range bases {
		byNumber := samples[base]
		if len(byNumber) < 2 {
			continue // a lone numbered signal is just a name
		}
		g := Group{Field: base} // PeriodNS is set by applyRates
		for n := 1; n <= len(byNumber); n++ {
			i, ok := byNumber[n]
			if !ok {
				return fmt.Errorf("samples of %s are not numbered 1..%d", base, len(byNumber))
			}
			if first := m.Signals[byNumber[1]]; !sameEncoding(first, m.Signals[i]) {
				return fmt.Errorf("%s is encoded differently from %s", m.Signals[i].Name, first.Name)
			}
			g.Signals = append(g.Signals, i)
		}
		m.Groups = append(m.Groups, g)
	}
	return nil
}

// sameEncoding reports whether two samples of one quantity are read and scaled alike.
func sameEncoding(a, b Signal) bool {
	return a.Bits == b.Bits && a.LittleEndian == b.LittleEndian && a.Signed == b.Signed &&
		a.Kind == b.Kind && a.Factor == b.Factor && a.Offset == b.Offset && a.Unit == b.Unit && a.Mux == b.Mux
}
