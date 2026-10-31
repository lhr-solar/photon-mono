package main

import (
	"encoding/binary"
	"encoding/json"
	"math"
	"os"
	"path/filepath"
	"sync"

	"server/influx"
)

var tsdbIndexOnce sync.Mutex
var tsdbIndexCache = map[string][]tsdbRec{}

type tsdbSig struct {
	ID        uint32
	Message   string
	Name      string
	ValueType int
	Factor    float64
	Offset    float64
}

func (s tsdbSig) muxCell() bool {
	if s.Message != "BPS_Voltage_Aggregate_Arr" && s.Message != "BPS_Temperature_Aggregate_Arr" {
		return false
	}
	switch s.Name {
	case "BPS_Tap_idx", "BPS_Voltage_Tap_Data", "BPS_Temperature_Tap_Data":
		return true
	}
	return false
}

type tsdbPoint struct {
	time uint64
	eng  float64
}

func loadTsdbSigMap(dir string) map[uint32]tsdbSig {
	data, err := os.ReadFile(filepath.Join(dir, "signals.json"))
	if err != nil {
		return nil
	}
	var rows []struct {
		ID        uint32  `json:"id"`
		Message   string  `json:"message"`
		Name      string  `json:"name"`
		ValueType int     `json:"value_type"`
		Factor    float64 `json:"factor"`
		Offset    float64 `json:"offset"`
	}
	if json.Unmarshal(data, &rows) != nil {
		return nil
	}
	out := make(map[uint32]tsdbSig, len(rows))
	for _, row := range rows {
		factor := row.Factor
		if factor == 0 {
			factor = 1
		}
		out[row.ID] = tsdbSig{ID: row.ID, Message: row.Message, Name: row.Name, ValueType: row.ValueType, Factor: factor, Offset: row.Offset}
	}
	return out
}

func engineering(raw int64, sig tsdbSig) float64 {
	var num float64
	switch sig.ValueType {
	case 1:
		num = float64(math.Float32frombits(uint32(raw)))
	case 2:
		num = math.Float64frombits(uint64(raw))
	default:
		num = float64(raw)
	}
	return num*sig.Factor + sig.Offset
}

func cachedTsdbIndex(dir string) []tsdbRec {
	tsdbIndexOnce.Lock()
	defer tsdbIndexOnce.Unlock()
	if recs, ok := tsdbIndexCache[dir]; ok {
		return recs
	}
	recs := readTsdbIndex(filepath.Join(dir, "l0.pidx"))
	tsdbIndexCache[dir] = recs
	return recs
}

func queryTsdbHistory(dir string, ids []uint32, from, to uint64, max int) map[uint32][]influx.Row {
	if max <= 0 {
		max = 2000
	}
	sigs := loadTsdbSigMap(dir)
	bySignal := map[uint32][]tsdbRec{}
	for _, rec := range cachedTsdbIndex(dir) {
		if rec.end < from || rec.start > to {
			continue
		}
		bySignal[rec.signal] = append(bySignal[rec.signal], rec)
	}
	files := map[uint32]*os.File{}
	defer func() {
		for _, f := range files {
			f.Close()
		}
	}()
	open := func(file uint32) *os.File {
		if f, ok := files[file]; ok {
			return f
		}
		f, err := os.Open(filepath.Join(dir, "l0", fileName(file)))
		if err != nil {
			return nil
		}
		files[file] = f
		return f
	}
	out := map[uint32][]influx.Row{}
	for _, id := range ids {
		recs := bySignal[id]
		if len(recs) == 0 {
			continue
		}
		sig, ok := sigs[id]
		if !ok {
			sig = tsdbSig{ID: id, Factor: 1}
		}
		bucket := uint32(1000)
		var pts []rawPoint
		short := to > from && to-from <= 180000
		if sig.muxCell() || short {
			bucket = 1
			for _, rec := range recs {
				f := open(rec.file)
				if f == nil {
					continue
				}
				pts = append(pts, decodePageFile(f, rec, from, to)...)
				if len(pts) > 8000 {
					break
				}
			}
		} else {
			for _, rec := range pickRecs(recs, max) {
				f := open(rec.file)
				if f == nil {
					continue
				}
				if pt, ok := firstSample(f, rec); ok && pt.time >= from && pt.time <= to {
					pts = append(pts, pt)
				}
			}
		}
		rows := make([]influx.Row, 0, len(pts))
		for _, pt := range pts {
			eng := engineering(pt.raw, sig)
			rows = append(rows, influx.Row{
				TimeMS: pt.time, Count: "1",
				EngineeringMin: eng, EngineeringMax: eng, EngineeringMean: eng,
				BucketMS: bucket,
			})
		}
		if len(rows) > 0 {
			out[id] = rows
		}
	}
	return out
}

func pickRecs(recs []tsdbRec, max int) []tsdbRec {
	if len(recs) <= max {
		return recs
	}
	out := make([]tsdbRec, 0, max)
	step := float64(len(recs)) / float64(max)
	for i := 0; i < max; i++ {
		out = append(out, recs[int(float64(i)*step)])
	}
	return out
}

type tsdbRec struct {
	signal uint32
	start  uint64
	end    uint64
	file   uint32
	offset uint32
	bytes  uint32
}

func readTsdbIndex(path string) []tsdbRec {
	b, err := os.ReadFile(path)
	if err != nil || len(b) < 64 {
		return nil
	}
	if binary.LittleEndian.Uint32(b) != 0x31584950 {
		return nil
	}
	count := binary.LittleEndian.Uint32(b[12:])
	out := make([]tsdbRec, 0, count)
	for i := uint32(0); i < count; i++ {
		off := 64 + int(i)*48
		if off+48 > len(b) {
			break
		}
		out = append(out, tsdbRec{
			signal: binary.LittleEndian.Uint32(b[off:]),
			start:  binary.LittleEndian.Uint64(b[off+4:]),
			end:    binary.LittleEndian.Uint64(b[off+12:]),
			file:   binary.LittleEndian.Uint32(b[off+20:]),
			offset: binary.LittleEndian.Uint32(b[off+24:]),
			bytes:  binary.LittleEndian.Uint32(b[off+28:]),
		})
	}
	return out
}

func fileName(n uint32) string {
	var buf [9]byte
	for i := 3; i >= 0; i-- {
		buf[i] = byte('0' + n%10)
		n /= 10
	}
	copy(buf[4:], ".pcol")
	return string(buf[:])
}

type rawPoint struct {
	time uint64
	raw  int64
}

func firstSample(f *os.File, rec tsdbRec) (rawPoint, bool) {
	if rec.bytes < 32 {
		return rawPoint{}, false
	}
	buf := make([]byte, rec.bytes)
	if _, err := f.ReadAt(buf, int64(rec.offset)); err != nil {
		return rawPoint{}, false
	}
	pts := decodePageBuf(buf, 1)
	if len(pts) == 0 {
		return rawPoint{}, false
	}
	return pts[0], true
}

func decodeTsdbPage(path string, offset, size uint32) []rawPoint {
	if size < 32 {
		return nil
	}
	f, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer f.Close()
	buf := make([]byte, size)
	if _, err = f.ReadAt(buf, int64(offset)); err != nil {
		return nil
	}
	return decodePageBuf(buf, 0)
}

func decodePageFile(f *os.File, rec tsdbRec, from, to uint64) []rawPoint {
	if rec.bytes < 32 {
		return nil
	}
	buf := make([]byte, rec.bytes)
	if _, err := f.ReadAt(buf, int64(rec.offset)); err != nil {
		return nil
	}
	pts := decodePageBuf(buf, 0)
	out := pts[:0]
	for _, pt := range pts {
		if pt.time >= from && pt.time <= to {
			out = append(out, pt)
		}
	}
	return out
}

func decodePageBuf(buf []byte, limit int) []rawPoint {
	if len(buf) < 32 {
		return nil
	}
	magic := binary.LittleEndian.Uint32(buf)
	if magic != 0x31475050 && magic != 0x32475050 {
		return nil
	}
	sampleCount := int(binary.LittleEndian.Uint32(buf[16:]))
	tEnc := buf[20]
	vEnc := buf[21]
	flags := buf[23]
	pay := int(binary.LittleEndian.Uint32(buf[28:]))
	if flags&1 != 0 || sampleCount == 0 || 32+pay > len(buf) {
		return nil
	}
	n := sampleCount
	if limit > 0 && limit < n {
		n = limit
	}
	base := binary.LittleEndian.Uint64(buf[8:])
	span := binary.LittleEndian.Uint32(buf[24:])
	payload := buf[32 : 32+pay]
	off := 0
	times := make([]uint64, n)
	times[0] = base
	if tEnc == 1 {
		step := int64(0)
		if sampleCount > 1 {
			step = int64(span) / int64(sampleCount-1)
		}
		for i := 1; i < n; i++ {
			times[i] = base + uint64(step)*uint64(i)
		}
	} else if tEnc == 0 && sampleCount >= 2 {
		prev, ok := readZigzag(payload, &off)
		if !ok {
			return nil
		}
		if n > 1 {
			times[1] = times[0] + uint64(prev)
		}
		for i := 2; i < sampleCount; i++ {
			dod, ok := readZigzag(payload, &off)
			if !ok {
				return nil
			}
			prev = wrapAdd(prev, dod)
			if i < n {
				times[i] = times[i-1] + uint64(prev)
			}
		}
	} else if tEnc != 0 {
		return nil
	}
	values := make([]int64, n)
	if vEnc == 3 {
		v, ok := readZigzag(payload, &off)
		if !ok {
			return nil
		}
		for i := range values {
			values[i] = v
		}
	} else if vEnc == 0 {
		v, ok := readZigzag(payload, &off)
		if !ok {
			return nil
		}
		values[0] = v
		prev := v
		for i := 1; i < sampleCount; i++ {
			d, ok := readZigzag(payload, &off)
			if !ok {
				return nil
			}
			prev = wrapAdd(prev, d)
			if i < n {
				values[i] = prev
			}
		}
	} else {
		return nil
	}
	out := make([]rawPoint, n)
	for i := range out {
		out[i] = rawPoint{times[i], values[i]}
	}
	return out
}

func readZigzag(b []byte, off *int) (int64, bool) {
	var u uint64
	shift := 0
	for *off < len(b) {
		x := b[*off]
		*off++
		u |= uint64(x&0x7f) << shift
		if x&0x80 == 0 {
			return int64((u >> 1) ^ uint64(-int64(u&1))), true
		}
		shift += 7
		if shift > 63 {
			return 0, false
		}
	}
	return 0, false
}

func wrapAdd(a, b int64) int64 { return int64(uint64(a) + uint64(b)) }

func thinPoints(pts []tsdbPoint, max int) []tsdbPoint {
	if len(pts) <= max {
		return pts
	}
	out := make([]tsdbPoint, 0, max)
	step := float64(len(pts)) / float64(max)
	for i := 0; i < max; i++ {
		out = append(out, pts[int(float64(i)*step)])
	}
	return out
}
