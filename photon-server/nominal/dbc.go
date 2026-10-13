package nominal

import (
	"bufio"
	"math"
	"os"
	"regexp"
	"strconv"
	"strings"
)

type valueKind int

const (
	kindInt valueKind = iota
	kindFloat
	kindDouble
)

type signalDef struct {
	name    string
	start   int
	length  int
	little  bool
	signed  bool
	kind    valueKind
	factor  float64
	offset  float64
	isIndex bool
}

type messageDef struct {
	canID   uint32
	name    string
	signals []signalDef
}

// Database is a CAN database keyed by the 29-bit id in the DBC.
type Database struct {
	byID map[uint32]*messageDef
}

var (
	boRe = regexp.MustCompile(`^BO_\s+(\d+)\s+(\w+)\s*:`)
	sgRe = regexp.MustCompile(`^SG_\s+(\w+)\s*(?:[mM]\d+)?\s*:\s*(\d+)\|(\d+)@([01])([+-])\s*\(([^,]+),([^)]+)\)\s*\[[^\]]*\]\s*"([^"]*)"`)
	vtRe = regexp.MustCompile(`^SIG_VALTYPE_\s+\d+\s+(\w+)\s*:\s*(\d+)\s*;`)
)

// LoadDBC reads a Vector DBC file. A missing factor is treated as 1 so a
// parse miss cannot zero every sample.
func LoadDBC(path string) (*Database, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	db := &Database{byID: map[uint32]*messageDef{}}
	var cur *messageDef
	kinds := map[string]valueKind{}
	sc := bufio.NewScanner(f)
	buf := make([]byte, 0, 64*1024)
	sc.Buffer(buf, 1024*1024)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if m := boRe.FindStringSubmatch(line); m != nil {
			id, _ := strconv.ParseUint(m[1], 10, 32)
			cur = &messageDef{canID: uint32(id), name: m[2]}
			db.byID[cur.canID] = cur
			continue
		}
		if strings.HasPrefix(line, "BO_") {
			cur = nil
			continue
		}
		if m := vtRe.FindStringSubmatch(line); m != nil {
			k := kindInt
			if m[2] == "1" {
				k = kindFloat
			} else if m[2] == "2" {
				k = kindDouble
			}
			kinds[m[1]] = k
			continue
		}
		if cur == nil {
			continue
		}
		if m := sgRe.FindStringSubmatch(line); m != nil {
			start, _ := strconv.Atoi(m[2])
			length, _ := strconv.Atoi(m[3])
			factor, err := strconv.ParseFloat(m[6], 64)
			if err != nil {
				factor = 1
			}
			offset, _ := strconv.ParseFloat(m[7], 64)
			sig := signalDef{
				name:    m[1],
				start:   start,
				length:  length,
				little:  m[4] == "1",
				signed:  m[5] == "-",
				factor:  factor,
				offset:  offset,
				isIndex: strings.HasSuffix(m[1], "_idx"),
			}
			cur.signals = append(cur.signals, sig)
		}
	}
	if err := sc.Err(); err != nil {
		return nil, err
	}
	for _, msg := range db.byID {
		for i := range msg.signals {
			if k, ok := kinds[msg.signals[i].name]; ok {
				msg.signals[i].kind = k
			}
		}
	}
	return db, nil
}

func (db *Database) message(canID uint32) *messageDef {
	if db == nil {
		return nil
	}
	return db.byID[canID&0x1fffffff]
}

func extractRaw(sig signalDef, data []byte, dlc int) (uint64, bool) {
	if sig.start < 0 || sig.length <= 0 || sig.length > 64 || dlc > 8 || dlc < 0 {
		return 0, false
	}
	available := dlc * 8
	var raw uint64
	if sig.little {
		if sig.start+sig.length > available {
			return 0, false
		}
		for i := 0; i < sig.length; i++ {
			bit := sig.start + i
			raw |= uint64((data[bit/8]>>(bit%8))&1) << uint(i)
		}
		return raw, true
	}
	bit := sig.start
	for i := 0; i < sig.length; i++ {
		if bit < 0 || bit >= available {
			return 0, false
		}
		raw = (raw << 1) | uint64((data[bit/8]>>(bit%8))&1)
		if bit%8 == 0 {
			bit += 15
		} else {
			bit--
		}
	}
	return raw, true
}

func rawInt(sig signalDef, bits uint64) int64 {
	if sig.kind == kindFloat || sig.kind == kindDouble || !sig.signed {
		return int64(bits)
	}
	if sig.length >= 64 {
		return int64(bits)
	}
	sign := uint64(1) << uint(sig.length-1)
	if bits&sign == 0 {
		return int64(bits)
	}
	return int64(bits | ^((uint64(1) << uint(sig.length)) - 1))
}

// physical is the engineering value Nominal should store.
func (sig signalDef) physical(bits uint64) (float64, bool) {
	var v float64
	switch sig.kind {
	case kindFloat:
		if sig.length != 32 {
			return 0, false
		}
		v = float64(math.Float32frombits(uint32(bits)))
	case kindDouble:
		if sig.length != 64 {
			return 0, false
		}
		v = math.Float64frombits(bits)
	default:
		v = float64(rawInt(sig, bits))
	}
	v = v*sig.factor + sig.offset
	if math.IsNaN(v) || math.IsInf(v, 0) {
		return 0, false
	}
	return v, true
}
