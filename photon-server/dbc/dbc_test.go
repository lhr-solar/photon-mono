package dbc

import (
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"testing"
)

// oneMessage is a DBC with one message of one 8-bit signal.
func oneMessage(id int, sig string) string {
	return fmt.Sprintf("BO_ %d Msg%d: 8 Node\n SG_ %s : 0|8@1+ (1,0) [0|255] \"\" Node\n\n", id, id, sig)
}

func writeDBCs(t *testing.T, files map[string]string) string {
	t.Helper()
	dir := t.TempDir()
	for name, text := range files {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(text), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func TestCatalogSHAAndBuses(t *testing.T) {
	files := map[string]string{
		"Zed.dbc":   oneMessage(200, "Zsig"),
		"Alpha.dbc": oneMessage(100, "Asig"),
	}
	dir := writeDBCs(t, files)

	c1, err := Load(dir, nil)
	if err != nil {
		t.Fatal(err)
	}
	c2, err := Load(dir, nil)
	if err != nil {
		t.Fatal(err)
	}
	if c1.SHA == "" || c1.SHA != c2.SHA {
		t.Fatalf("SHA not stable across loads: %q, %q", c1.SHA, c2.SHA)
	}
	if want := []string{"Alpha", "Zed"}; !slices.Equal(c1.Buses, want) {
		t.Fatalf("Buses = %v, want %v", c1.Buses, want)
	}

	// A change to one file's contents changes the SHA.
	if err := os.WriteFile(filepath.Join(dir, "Zed.dbc"), []byte(oneMessage(200, "Renamed")), 0o644); err != nil {
		t.Fatal(err)
	}
	c3, err := Load(dir, nil)
	if err != nil {
		t.Fatal(err)
	}
	if c3.SHA == c1.SHA {
		t.Fatalf("SHA unchanged after editing a DBC: %q", c3.SHA)
	}
	if len(c3.SHA) != 40 {
		t.Fatalf("SHA %q is not 40 hex digits", c3.SHA)
	}
}
