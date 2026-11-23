package main

import (
	"bytes"
	"testing"
)

// -store-max-gb and PHOTON_STORE_MAX_GB: the default is 64 GiB, the flag and the
// environment set it, and anything under 1 is refused.
func TestStoreMaxGBParsingAndDefault(t *testing.T) {
	noEnv := func(string) string { return "" }

	cfg, err := ParseConfigWithEnv(noEnv, []string{}, &bytes.Buffer{})
	if err != nil {
		t.Fatal(err)
	}
	if cfg.StoreMaxGB != DefaultStoreMaxGB || DefaultStoreMaxGB != 64 {
		t.Fatalf("default store max %d GiB, want 64", cfg.StoreMaxGB)
	}

	cfg, err = ParseConfigWithEnv(noEnv, []string{"-store-max-gb", "8"}, &bytes.Buffer{})
	if err != nil || cfg.StoreMaxGB != 8 {
		t.Fatalf("-store-max-gb 8: %v, got %d", err, cfg.StoreMaxGB)
	}

	env := map[string]string{"PHOTON_STORE_MAX_GB": "16"}
	cfg, err = ParseConfigWithEnv(func(k string) string { return env[k] }, []string{}, &bytes.Buffer{})
	if err != nil || cfg.StoreMaxGB != 16 {
		t.Fatalf("PHOTON_STORE_MAX_GB=16: %v, got %d", err, cfg.StoreMaxGB)
	}
	cfg, err = ParseConfigWithEnv(func(k string) string { return env[k] }, []string{"-store-max-gb", "4"}, &bytes.Buffer{})
	if err != nil || cfg.StoreMaxGB != 4 {
		t.Fatalf("flag over env: %v, got %d", err, cfg.StoreMaxGB)
	}

	for _, args := range [][]string{{"-store-max-gb", "0"}, {"-store-max-gb", "-3"}, {"-store-max-gb", "lots"}} {
		if _, err := ParseConfigWithEnv(noEnv, args, &bytes.Buffer{}); err == nil {
			t.Fatalf("%v was accepted", args)
		}
	}
	bad := map[string]string{"PHOTON_STORE_MAX_GB": "lots"}
	if _, err := ParseConfigWithEnv(func(k string) string { return bad[k] }, []string{}, &bytes.Buffer{}); err == nil {
		t.Fatal("PHOTON_STORE_MAX_GB=lots was accepted")
	}
}
