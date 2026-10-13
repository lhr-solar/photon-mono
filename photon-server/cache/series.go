package cache

import "fmt"

type Series struct {
	Def  SignalDef
	ID   uint32
	Tap  *int32
	Name string
}

func TapNumber(d SignalDef, tap int32) uint32 {
	return SignalNumber(fmt.Sprintf("%s.%s[tap=%d]", d.Message, d.ID.Name, tap))
}
func (s *SessionCache) Series() []Series {
	var out []Series
	for _, d := range s.SortedDefinitions() {
		out = append(out, Series{Def: d, ID: d.Number, Name: d.ID.Name})
		count := 0
		for _, index := range s.byCANID[d.ID.CANID] {
			if index.Message == d.Message && isTap(index.ID.Name) {
				count = 1 << min(int(index.BitLength), 8)
			}
		}
		if isTap(d.ID.Name) || isAge(d.ID.Name) {
			continue
		}
		for i := 0; i < count; i++ {
			tap := int32(i)
			out = append(out, Series{Def: d, ID: TapNumber(d, tap), Tap: &tap, Name: fmt.Sprintf("%s[tap=%d]", d.ID.Name, i)})
		}
	}
	return out
}
