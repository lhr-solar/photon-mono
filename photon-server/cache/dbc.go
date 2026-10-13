package cache

import (
	"bufio"
	"fmt"
	"os"
	"regexp"
	"strconv"
	"strings"
)

var (
	messageLine   = regexp.MustCompile(`^BO_\s+(\d+)\s+(\S+):\s*(\d+)\s+\S+`)
	signalLine    = regexp.MustCompile(`^SG_\s+(\S+)\s*:\s*(\d+)\|(\d+)@(0|1)([+-])\s*\(([^,]+),([^)]+)\)\s*\[([^|]+)\|([^]]+)\]\s*"([^"]*)"`)
	valueTypeLine = regexp.MustCompile(`^SIG_VALTYPE_\s+(\d+)\s+(\S+)\s*:\s*([12])\s*;`)
)

func ParseDBC(text string) (*SessionCache, error) {
	cache := New()
	var currentID uint32
	var currentExtended bool
	var currentMessage string
	var currentDLC uint8
	var hasMessage bool
	valueTypes := make(map[SignalID]int)
	scanner := bufio.NewScanner(strings.NewReader(text))
	scanner.Buffer(make([]byte, 4096), 4*1024*1024)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		switch {
		case strings.HasPrefix(line, "BO_ "):
			parts := messageLine.FindStringSubmatch(line)
			hasMessage = false
			if parts == nil {
				continue
			}
			id, errID := strconv.ParseUint(parts[1], 10, 32)
			dlc, errDLC := strconv.Atoi(parts[3])
			if errID != nil || errDLC != nil || dlc < 0 || dlc > 8 {
				continue
			}
			currentID = uint32(id) & 0x1fffffff
			currentExtended = uint32(id)&0x80000000 != 0
			currentMessage = parts[2]
			currentDLC = uint8(dlc)
			hasMessage = true
		case strings.HasPrefix(line, "SG_ ") && hasMessage:
			parts := signalLine.FindStringSubmatch(line)
			if parts == nil { // Includes multiplexed signals, as in the C++ parser.
				continue
			}
			start, _ := strconv.Atoi(parts[2])
			length, _ := strconv.Atoi(parts[3])
			if length < 1 || length > 64 {
				continue
			}
			factor, e1 := strconv.ParseFloat(strings.TrimSpace(parts[6]), 64)
			offset, e2 := strconv.ParseFloat(strings.TrimSpace(parts[7]), 64)
			min, e3 := strconv.ParseFloat(strings.TrimSpace(parts[8]), 64)
			max, e4 := strconv.ParseFloat(strings.TrimSpace(parts[9]), 64)
			if e1 != nil || e2 != nil || e3 != nil || e4 != nil || start < 0 || start > 65535 || length > 65535 {
				continue
			}
			id := SignalID{CANID: currentID, Extended: currentExtended, Name: parts[1]}
			cache.Definitions[id] = SignalDef{
				ID: id, Message: currentMessage, DLC: currentDLC, StartBit: uint16(start), BitLength: uint16(length),
				LittleEndian: parts[4] == "1", Signed: parts[5] == "-",
				Factor: factor, Offset: offset, Unit: parts[10],
				Min: min, Max: max,
			}
		case strings.HasPrefix(line, "SIG_VALTYPE_ "):
			parts := valueTypeLine.FindStringSubmatch(line)
			if parts == nil {
				continue
			}
			id, err := strconv.ParseUint(parts[1], 10, 32)
			if err != nil {
				continue
			}
			key := SignalID{CANID: uint32(id) & 0x1fffffff, Extended: uint32(id)&0x80000000 != 0, Name: parts[2]}
			valueTypes[key], _ = strconv.Atoi(parts[3])
		}
	}
	if err := scanner.Err(); err != nil {
		return nil, fmt.Errorf("scan DBC: %w", err)
	}
	if len(cache.Definitions) == 0 {
		return nil, fmt.Errorf("DBC contains no supported signals")
	}
	cache.byCANID = make(map[uint32][]SignalDef)
	for id, def := range cache.Definitions {
		def.ValueType = valueTypes[id]
		cache.Definitions[id] = def
		cache.byCANID[id.CANID] = append(cache.byCANID[id.CANID], def)
	}
	cache.BuildIndex()
	return cache, nil
}

func LoadDBC(path string) (*SessionCache, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read DBC %q: %w", path, err)
	}
	cache, err := ParseDBC(string(data))
	if err != nil {
		return nil, fmt.Errorf("parse DBC %q: %w", path, err)
	}
	return cache, nil
}
