package cache

import (
	"os"
	"testing"
)

func TestCarcanValidates(t *testing.T) {
	b, err := os.ReadFile(`F:\lhr\photon-mono\photon-next\assets\dbc\carcan_telemcan.dbc`)
	if err != nil {
		t.Skip(err)
	}
	s, diag, err := ValidateDBC(string(b))
	if err != nil {
		if len(diag) > 0 {
			t.Log(diag[0])
		}
		t.Fatal(err)
	}
	if s == nil || len(s.Definitions) == 0 {
		t.Fatal("no signals")
	}
}
