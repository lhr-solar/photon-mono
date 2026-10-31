package cache

import (
	"crypto/sha1"
	"fmt"
	"math"
	"math/big"
	"sort"
	"strconv"
	"strings"
)

func (s *SessionCache) Clone() *SessionCache {
	c := New()
	c.DBCSHA = s.DBCSHA
	for k, v := range s.Definitions {
		c.Definitions[k] = v
	}
	c.BuildIndex()
	return c
}
func SignalNumber(name string) uint32 {
	h := uint32(2166136261)
	for i := 0; i < len(name); i++ {
		h ^= uint32(name[i])
		h *= 16777619
	}
	if h == 0 {
		h = 1
	}
	return h
}
func (s *SessionCache) BuildIndex() {
	s.byCANID = make(map[uint32][]SignalDef)
	used := make(map[uint32]bool)
	for _, d := range s.SortedDefinitions() {
		id := SignalNumber(d.Message + "." + d.ID.Name)
		for used[id] {
			id++
			if id == 0 {
				id = 1
			}
		}
		used[id] = true
		d.Number = id
		s.Definitions[d.ID] = d
		s.byCANID[d.ID.CANID] = append(s.byCANID[d.ID.CANID], d)
	}
}
func (s *SessionCache) SortedDefinitions() []SignalDef {
	defs := make([]SignalDef, 0, len(s.Definitions))
	for _, d := range s.Definitions {
		defs = append(defs, d)
	}
	sort.Slice(defs, func(i, j int) bool {
		a, b := defs[i], defs[j]
		if a.ID.CANID != b.ID.CANID {
			return a.ID.CANID < b.ID.CANID
		}
		return a.ID.Name < b.ID.Name
	})
	return defs
}
func (s *SessionCache) add(k SampleKey, raw, age int64, hasAge bool) {
	v := s.Samples[k]
	if v == nil {
		v = &Sample{Min: raw, Max: raw}
		v.ExactMin.SetInt64(raw)
		v.ExactMax.SetInt64(raw)
		s.Samples[k] = v
	}
	if raw < v.Min {
		v.Min = raw
	}
	if raw > v.Max {
		v.Max = raw
	}
	exact := big.NewInt(raw)
	if exact.Cmp(&v.ExactMin) < 0 {
		v.ExactMin.Set(exact)
	}
	if exact.Cmp(&v.ExactMax) > 0 {
		v.ExactMax.Set(exact)
	}
	v.WideSum.Add(&v.WideSum, big.NewInt(raw))
	v.Sum = v.WideSum.Int64()
	v.Count++
	v.Dirty = true
	if hasAge && (!v.HasAge || age > v.AgeMS) {
		v.AgeMS = age
		v.HasAge = true
	}
}

// AddIntegerBits imports an already extracted integer without losing unsigned width.
func (s *SessionCache) AddIntegerBits(id SignalID, sec int64, bits uint64, tap int32) error {
	d, ok := s.Definitions[id]
	if !ok || d.ValueType != 0 {
		return fmt.Errorf("unknown integer signal")
	}
	k := SampleKey{id, tap, sec}
	if d.Signed {
		s.add(k, int64(bits), 0, false)
	} else {
		s.addUnsigned(k, bits, 0, false)
	}
	return nil
}
func Extract(d SignalDef, data [8]byte, dlc int) (uint64, bool) {
	n, bit := int(d.BitLength), int(d.StartBit)
	if dlc < 0 || dlc > 8 || n < 1 || n > 64 {
		return 0, false
	}
	var bits uint64
	for i := 0; i < n; i++ {
		if bit < 0 || bit >= dlc*8 {
			return 0, false
		}
		v := uint64(data[bit/8] >> uint(bit%8) & 1)
		if d.LittleEndian {
			bits |= v << uint(i)
			bit++
		} else {
			bits = bits<<1 | v
			if bit%8 == 0 {
				bit += 15
			} else {
				bit--
			}
		}
	}
	return bits, true
}
func isTap(n string) bool {
	return n == "BPS_Tap_idx" || n == "HSS_Channel_idx" || strings.HasSuffix(n, "_Tap_idx") || strings.HasSuffix(n, "_Channel_idx")
}
func isAge(n string) bool {
	return strings.HasSuffix(n, "_Tap_Age") || strings.HasSuffix(n, "_Channel_Age")
}
func (s *SessionCache) Decode(fr Frame) []Value {
	defs := s.byCANID[fr.CANID&0x1fffffff]
	values := make([]Value, 0, len(defs))
	tap := int32(-1)
	var age int64
	hasAge := false
	for _, d := range defs {
		if strings.HasPrefix(fr.Bus, "pcan:") && ((fr.Flags&1 != 0) != d.ID.Extended) {
			continue
		}
		if d.ID.Bus != "" && d.ID.Bus != fr.Bus {
			continue
		}
		frameExt := fr.Flags&1 != 0 || fr.CANID&0x80000000 != 0
		if frameExt != d.ID.Extended {
			continue
		}
		bits, ok := Extract(d, fr.Data, fr.DLC)
		if !ok {
			continue
		}
		raw := int64(bits)
		if d.Signed && d.BitLength < 64 && bits&(uint64(1)<<(d.BitLength-1)) != 0 {
			raw = int64(bits | ^((uint64(1) << d.BitLength) - 1))
		}
		numeric := float64(raw)
		if !d.Signed {
			numeric = float64(bits)
		}
		if d.ValueType == 1 {
			numeric = float64(math.Float32frombits(uint32(bits)))
		}
		if d.ValueType == 2 {
			numeric = math.Float64frombits(bits)
		}
		if math.IsNaN(numeric) || math.IsInf(numeric, 0) {
			continue
		}
		if isTap(d.ID.Name) {
			tap = int32(raw)
			continue
		}
		if isAge(d.ID.Name) {
			age = raw
			hasAge = true
			continue
		}
		values = append(values, Value{Def: d, Raw: raw, Bits: bits, Numeric: numeric, Physical: numeric*d.Factor + d.Offset})
	}
	for i := range values {
		values[i].Tap = tap
		values[i].Age = age
		values[i].HasAge = hasAge
	}
	return values
}
func (s *SessionCache) fold(fr Frame) (added, skipped int) {
	for _, v := range s.Decode(fr) {
		k := SampleKey{v.Def.ID, v.Tap, int64(fr.TimeMs / 1000)}
		if v.Def.ValueType == 0 {
			if !v.Def.Signed && v.Bits > math.MaxInt64 {
				s.addUnsigned(k, v.Bits, v.Age, v.HasAge)
			} else {
				s.add(k, v.Raw, v.Age, v.HasAge)
			}
		} else {
			f := s.Floats[k]
			if f == nil {
				f = &FloatSample{Min: v.Numeric, Max: v.Numeric}
				s.Floats[k] = f
			}
			f.Min = math.Min(f.Min, v.Numeric)
			f.Max = math.Max(f.Max, v.Numeric)
			f.Sum += v.Numeric
			f.Count++
			if v.HasAge && (!f.HasAge || v.Age > f.AgeMS) {
				f.AgeMS = v.Age
				f.HasAge = true
			}
		}
		added++
	}
	return
}
func EscapeTag(s string) string {
	return strings.NewReplacer("\\", "\\\\", ",", "\\,", " ", "\\ ", "=", "\\=").Replace(s)
}
func (s *SessionCache) keyName(k SampleKey) string {
	if d, ok := s.Definitions[k.Signal]; ok {
		return d.Message + "." + d.ID.Name
	}
	return k.Signal.Name
}
func (s *SessionCache) lines(session string, before *int64) []string {
	var lines []string
	label := func(table string, k SampleKey) string {
		t := table + ",session=" + EscapeTag(session)
		if s.DecodeID != "" {
			t += ",decode_id=" + EscapeTag(s.DecodeID)
		}
		t += ",signal=" + EscapeTag(s.keyName(k))
		if k.Tap >= 0 {
			t += fmt.Sprintf(",tap=%d", k.Tap)
		}
		return t
	}
	for k, v := range s.Samples {
		if before != nil && k.Second >= *before {
			continue
		}
		fields := fmt.Sprintf("min=%di,max=%di,sum=%di,count=%di", v.Min, v.Max, v.Sum, v.Count)
		if s.DecodeID != "" {
			fields += fmt.Sprintf(",min_exact=%s,max_exact=%s,sum_exact=%s,has_age=%t", strconv.Quote(v.ExactMin.String()), strconv.Quote(v.ExactMax.String()), strconv.Quote(v.WideSum.String()), v.HasAge)
		}
		if v.HasAge {
			fields += fmt.Sprintf(",age=%di", v.AgeMS)
		}
		lines = append(lines, fmt.Sprintf("%s %s %d", label("sample", k), fields, k.Second*1_000_000_000))
	}
	for k, v := range s.Floats {
		if before != nil && k.Second >= *before {
			continue
		}
		fields := fmt.Sprintf("min=%g,max=%g,sum=%g,count=%di", v.Min, v.Max, v.Sum, v.Count)
		if s.DecodeID != "" {
			fields += fmt.Sprintf(",min_exact=%s,max_exact=%s,sum_exact=%s,has_age=%t", strconv.Quote(strconv.FormatFloat(v.Min, 'g', -1, 64)), strconv.Quote(strconv.FormatFloat(v.Max, 'g', -1, 64)), strconv.Quote(strconv.FormatFloat(v.Sum, 'g', -1, 64)), v.HasAge)
		}
		if v.HasAge {
			fields += fmt.Sprintf(",age=%di", v.AgeMS)
		}
		lines = append(lines, fmt.Sprintf("%s %s %d", label("float_sample", k), fields, k.Second*1_000_000_000))
	}
	sort.Strings(lines)
	return lines
}
func (s *SessionCache) addUnsigned(k SampleKey, bits uint64, age int64, hasAge bool) {
	raw := new(big.Int).SetUint64(bits)
	v := s.Samples[k]
	if v == nil {
		v = &Sample{}
		v.ExactMin.Set(raw)
		v.ExactMax.Set(raw)
		s.Samples[k] = v
	}
	if raw.Cmp(&v.ExactMin) < 0 {
		v.ExactMin.Set(raw)
	}
	if raw.Cmp(&v.ExactMax) > 0 {
		v.ExactMax.Set(raw)
	}
	v.Min = v.ExactMin.Int64()
	v.Max = v.ExactMax.Int64()
	v.WideSum.Add(&v.WideSum, raw)
	v.Sum = v.WideSum.Int64()
	v.Count++
	v.Dirty = true
	if hasAge && (!v.HasAge || age > v.AgeMS) {
		v.AgeMS = age
		v.HasAge = true
	}
}
func (s *SessionCache) ClosedLines(session string, keepSec int64) []string {
	return s.lines(session, &keepSec)
}
func (s *SessionCache) DropClosed(keepSec int64) {
	for k := range s.Samples {
		if k.Second < keepSec {
			delete(s.Samples, k)
		}
	}
	for k := range s.Floats {
		if k.Second < keepSec {
			delete(s.Floats, k)
		}
	}
}
func SHA1Hex(b []byte) string { return fmt.Sprintf("%x", sha1.Sum(b)) }
func SignalDefLines(s *SessionCache, sha string, start int64) []string {
	var lines []string
	for _, d := range s.SortedDefinitions() {
		unit := strings.NewReplacer(`\`, `\\`, `"`, `\"`).Replace(d.Unit)
		lines = append(lines, fmt.Sprintf("signal_def,dbc_sha=%s,signal=%s factor=%g,offset=%g,signal_id=%di,value_type=%di,unit=\"%s\" %d", EscapeTag(sha), EscapeTag(d.Message+"."+d.ID.Name), d.Factor, d.Offset, d.Number, d.ValueType, unit, start*1_000_000_000))
	}
	return lines
}
