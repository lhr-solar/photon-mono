package cache

import (
	"fmt"
	"math"
	"strconv"
	"strings"
)

// ValidateDBC is the upload/apply boundary: unsupported signals are never silently lost.
func ValidateDBC(text string) (*SessionCache, []string, error) {
	var diagnostics []string
	dlc := 0
	messageKey := ""
	have := false
	seen := make(map[string]bool)
	messages := make(map[string]bool)
	annotations := make(map[SignalID]bool)
	for n, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		fields := strings.Fields(line)
		if len(fields) == 0 {
			continue
		}
		switch fields[0] {
		case "BO_":
			p := messageLine.FindStringSubmatch(line)
			have = p != nil
			if !have {
				diagnostics = append(diagnostics, fmt.Sprintf("line %d: invalid message", n+1))
				continue
			}
			dlc, _ = strconv.Atoi(p[3])
			messageKey = p[1]
			id, err := strconv.ParseUint(p[1], 10, 32)
			if err != nil || id&0x60000000 != 0 || id&0x80000000 == 0 && id > 0x7ff {
				diagnostics = append(diagnostics, fmt.Sprintf("line %d: invalid CAN identifier", n+1))
			}
			if messages[messageKey] {
				diagnostics = append(diagnostics, fmt.Sprintf("line %d: duplicate message identifier", n+1))
			}
			messages[messageKey] = true
			if dlc > 8 {
				diagnostics = append(diagnostics, fmt.Sprintf("line %d: CAN FD is not supported", n+1))
			}
		case "SG_":
			if line == "SG_" {
				continue
			}
			p := signalLine.FindStringSubmatch(line)
			if !have || p == nil {
				diagnostics = append(diagnostics, fmt.Sprintf("line %d: malformed or multiplexed signal is unsupported", n+1))
				continue
			}
			start, _ := strconv.Atoi(p[2])
			key := messageKey + ":" + p[1]
			if seen[key] {
				diagnostics = append(diagnostics, fmt.Sprintf("line %d: duplicate signal", n+1))
			}
			seen[key] = true
			length, _ := strconv.Atoi(p[3])
			for _, field := range p[6:10] {
				value, err := strconv.ParseFloat(strings.TrimSpace(field), 64)
				if err != nil || math.IsNaN(value) || math.IsInf(value, 0) {
					diagnostics = append(diagnostics, fmt.Sprintf("line %d: invalid scaling or range", n+1))
					break
				}
			}
			d := SignalDef{StartBit: uint16(start), BitLength: uint16(length), LittleEndian: p[4] == "1"}
			if _, ok := Extract(d, [8]byte{}, dlc); !ok || start > 65535 || length > 64 {
				diagnostics = append(diagnostics, fmt.Sprintf("line %d: signal exceeds message DLC", n+1))
			}
		case "SG_MUL_VAL_":
			if len(fields) > 1 {
				diagnostics = append(diagnostics, fmt.Sprintf("line %d: multiplexing is unsupported", n+1))
			}
		case "SIG_VALTYPE_":
			if len(fields) == 1 {
				continue
			}
			p := valueTypeLine.FindStringSubmatch(line)
			if p == nil {
				diagnostics = append(diagnostics, fmt.Sprintf("line %d: unsupported signal value type (use IEEE 32 or 64)", n+1))
				continue
			}
			id, err := strconv.ParseUint(p[1], 10, 32)
			if err != nil {
				diagnostics = append(diagnostics, fmt.Sprintf("line %d: invalid signal value type identifier", n+1))
				continue
			}
			key := SignalID{CANID: uint32(id) & 0x1fffffff, Extended: uint32(id)&0x80000000 != 0, Name: p[2]}
			if annotations[key] {
				diagnostics = append(diagnostics, fmt.Sprintf("line %d: repeated signal value type", n+1))
			}
			annotations[key] = true
		}
	}
	s, err := ParseDBC(text)
	if err != nil {
		return nil, diagnostics, err
	}
	for key := range annotations {
		if _, ok := s.Definitions[key]; !ok {
			diagnostics = append(diagnostics, "value type refers to unknown signal "+key.Name)
		}
	}
	for _, d := range s.SortedDefinitions() {
		name := d.Message + "." + d.ID.Name
		if seen[name] {
			diagnostics = append(diagnostics, "duplicate signal "+name)
		}
		seen[name] = true
		if math.IsNaN(d.Factor) || math.IsInf(d.Factor, 0) || math.IsNaN(d.Offset) || math.IsInf(d.Offset, 0) {
			diagnostics = append(diagnostics, "non-finite scaling for "+name)
		}
		if d.ValueType == 1 && d.BitLength != 32 || d.ValueType == 2 && d.BitLength != 64 {
			diagnostics = append(diagnostics, "invalid IEEE signal width for "+name)
		}
	}
	if len(diagnostics) > 0 {
		return nil, diagnostics, fmt.Errorf("DBC has %d unsupported or invalid definitions", len(diagnostics))
	}
	return s, nil, nil
}
