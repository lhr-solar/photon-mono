package main

import (
	"io"
	"math"
	"runtime/debug"
	"strings"
	"testing"
)

func TestParseByteSize(t *testing.T) {
	for _, tc := range []struct {
		in   string
		want int64
	}{
		{"0", 0},
		{"123456", 123456},
		{"96MiB", 96 << 20},
		{"96mib", 96 << 20},
		{" 96 MiB ", 96 << 20},
		{"96M", 96 << 20},
		{"1GiB", 1 << 30},
		{"2g", 2 << 30},
		{"512KiB", 512 << 10},
		{"512k", 512 << 10},
		{"4096b", 4096},
	} {
		got, err := parseByteSize(tc.in)
		if err != nil || got != tc.want {
			t.Errorf("parseByteSize(%q) = %d, %v; want %d", tc.in, got, err, tc.want)
		}
	}
	for _, bad := range []string{"", "MiB", "-1", "1.5GiB", "96MB", "ninety", "99999999999999999999G"} {
		if got, err := parseByteSize(bad); err == nil {
			t.Errorf("parseByteSize(%q) = %d, want an error", bad, got)
		}
	}
}

// The default is 96 MiB, PHOTON_MEM_LIMIT and -mem-limit set it, and a bad value
// is an error that names its source.
func TestMemLimitDefaultEnvAndFlag(t *testing.T) {
	noEnv := func(string) string { return "" }
	cfg, err := ParseConfigWithEnv(noEnv, nil, io.Discard)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.MemLimit != 96<<20 || DefaultMemLimit != 96<<20 {
		t.Fatalf("default mem limit %d, want 96 MiB", cfg.MemLimit)
	}

	env := func(k string) string {
		if k == "PHOTON_MEM_LIMIT" {
			return "64MiB"
		}
		return ""
	}
	if cfg, err := ParseConfigWithEnv(env, nil, io.Discard); err != nil || cfg.MemLimit != 64<<20 {
		t.Fatalf("env limit: %v, %d; want 64 MiB", err, cfg.MemLimit)
	}
	if cfg, err := ParseConfigWithEnv(env, []string{"-mem-limit", "0"}, io.Discard); err != nil || cfg.MemLimit != 0 {
		t.Fatalf("flag 0 over env: %v, %d; want off", err, cfg.MemLimit)
	}
	if cfg, err := ParseConfigWithEnv(noEnv, []string{"-mem-limit=128MiB"}, io.Discard); err != nil || cfg.MemLimit != 128<<20 {
		t.Fatalf("flag limit: %v, %d; want 128 MiB", err, cfg.MemLimit)
	}

	badEnv := func(k string) string {
		if k == "PHOTON_MEM_LIMIT" {
			return "lots"
		}
		return ""
	}
	if _, err := ParseConfigWithEnv(badEnv, nil, io.Discard); err == nil || !strings.Contains(err.Error(), "PHOTON_MEM_LIMIT") {
		t.Fatalf("bad env: %v, want an error naming PHOTON_MEM_LIMIT", err)
	}
	if _, err := ParseConfigWithEnv(noEnv, []string{"-mem-limit", "lots"}, io.Discard); err == nil {
		t.Fatal("bad flag parsed")
	}
	// Below the minimum, and negative, fail validation.
	for _, args := range [][]string{{"-mem-limit", "1MiB"}} {
		if _, err := ParseConfigWithEnv(noEnv, args, io.Discard); err == nil {
			t.Fatalf("%v accepted, want a validation error", args)
		}
	}
	cfg = &Config{Host: "0.0.0.0", IngressPort: 6100, ClientPort: 6500, ObserverPort: 5400,
		VideoIngressPort: 6600, VideoClientPort: 6700, VideoUdpPort: 6800, DataDir: "./data",
		StoreMaxGB: 1, InfluxQueries: 1, LiveSeconds: 1, Ingest: poolSizing{Rate: 1, Latency: 1, Stall: 1}}
	cfg.MemLimit = -1
	if err := cfg.Validate(); err == nil {
		t.Fatal("negative mem limit validates")
	}
	cfg.MemLimit = 0
	if err := cfg.Validate(); err != nil {
		t.Fatalf("0 (off) rejected: %v", err)
	}
	cfg.MemLimit = MinMemLimit
	if err := cfg.Validate(); err != nil {
		t.Fatalf("the minimum rejected: %v", err)
	}
}

// applyMemLimit sets the runtime's soft limit when the limit is above zero, and
// leaves it alone at zero.
func TestApplyMemLimitSetsTheRuntime(t *testing.T) {
	before := debug.SetMemoryLimit(math.MaxInt64)
	defer debug.SetMemoryLimit(before)

	applyMemLimit(0)
	if got := debug.SetMemoryLimit(math.MaxInt64); got != math.MaxInt64 {
		t.Fatalf("limit 0 changed the runtime limit to %d", got)
	}
	applyMemLimit(96 << 20)
	if got := debug.SetMemoryLimit(-1); got != 96<<20 {
		t.Fatalf("runtime limit %d after applyMemLimit(96 MiB), want %d", got, 96<<20)
	}
}
