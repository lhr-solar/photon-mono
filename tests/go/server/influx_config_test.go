package main

import (
	"io"
	"testing"
	"time"
)

func TestInfluxWriteSettings(t *testing.T) {
	none := func(string) string { return "" }
	cfg, err := ParseConfigWithEnv(none, nil, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.InfluxFlush != DefaultInfluxFlush || cfg.InfluxNoSync {
		t.Fatalf("defaults: flush=%s noSync=%v", cfg.InfluxFlush, cfg.InfluxNoSync)
	}

	env := map[string]string{"PHOTON_INFLUX_FLUSH": "400ms", "PHOTON_INFLUX_NO_SYNC": "true"}
	cfg, err = ParseConfigWithEnv(func(k string) string { return env[k] }, nil, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.InfluxFlush != 400*time.Millisecond || !cfg.InfluxNoSync {
		t.Fatalf("env: flush=%s noSync=%v", cfg.InfluxFlush, cfg.InfluxNoSync)
	}

	cfg, err = ParseConfigWithEnv(none, []string{"-influx-flush", "0", "-influx-no-sync=false"}, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.InfluxFlush != 0 || cfg.InfluxNoSync {
		t.Fatalf("flags: flush=%s noSync=%v", cfg.InfluxFlush, cfg.InfluxNoSync)
	}

	if _, err := ParseConfigWithEnv(none, []string{"-influx-flush", "-1s"}, io.Discard); err == nil {
		t.Fatal("negative flush interval accepted")
	}
}
