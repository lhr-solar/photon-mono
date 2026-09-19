package main

import (
	"bytes"
	"path/filepath"
	"testing"
)

func TestDefaultConfig(t *testing.T) {
	emptyEnv := func(string) string { return "" }
	var buf bytes.Buffer
	cfg, err := ParseConfigWithEnv(emptyEnv, []string{}, &buf)
	if err != nil {
		t.Fatalf("unexpected error parsing default config: %v", err)
	}

	if cfg.Host != DefaultHost {
		t.Errorf("expected host %q, got %q", DefaultHost, cfg.Host)
	}
	if cfg.IngressPort != DefaultIngressPort {
		t.Errorf("expected ingress port %d, got %d", DefaultIngressPort, cfg.IngressPort)
	}
	if cfg.ClientPort != DefaultClientPort {
		t.Errorf("expected client port %d, got %d", DefaultClientPort, cfg.ClientPort)
	}
	if cfg.ObserverPort != DefaultObserverPort {
		t.Errorf("expected observer port %d, got %d", DefaultObserverPort, cfg.ObserverPort)
	}
	if cfg.VideoIngressPort != DefaultVideoIngressPort {
		t.Errorf("expected video ingress port %d, got %d", DefaultVideoIngressPort, cfg.VideoIngressPort)
	}
	if cfg.VideoClientPort != DefaultVideoClientPort {
		t.Errorf("expected video client port %d, got %d", DefaultVideoClientPort, cfg.VideoClientPort)
	}
	if cfg.VideoUdpPort != DefaultVideoUdpPort {
		t.Errorf("expected video udp port %d, got %d", DefaultVideoUdpPort, cfg.VideoUdpPort)
	}
	if cfg.DataDir != DefaultDataDir {
		t.Errorf("expected data dir %q, got %q", DefaultDataDir, cfg.DataDir)
	}
	if got := cfg.TsdbRoot(); got != filepath.Join(DefaultDataDir, "tsdb") {
		t.Errorf("expected tsdb root %q, got %q", filepath.Join(DefaultDataDir, "tsdb"), got)
	}
	if got := cfg.LiveFanoutAddr(); got != "127.0.0.1:6500" {
		t.Errorf("expected live fan-out %q, got %q", "127.0.0.1:6500", got)
	}
	if cfg.Ui != "" {
		t.Errorf("expected empty -ui (auto), got %q", cfg.Ui)
	}
}

func TestUiOffFlag(t *testing.T) {
	emptyEnv := func(string) string { return "" }
	var buf bytes.Buffer
	cfg, err := ParseConfigWithEnv(emptyEnv, []string{"-ui", "off"}, &buf)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if cfg.Ui != "off" {
		t.Fatalf("expected -ui off, got %q", cfg.Ui)
	}
	if got := resolveUiBackend(cfg); got != "" {
		t.Fatalf("off should resolve empty, got %q", got)
	}
}

func TestEnvFallback(t *testing.T) {
	mockEnv := map[string]string{
		"PHOTON_HOST":               "127.0.0.1",
		"PHOTON_INGRESS_PORT":       "7100",
		"PHOTON_CLIENT_PORT":        "7500",
		"PHOTON_OBSERVER_PORT":      "7400",
		"PHOTON_VIDEO_INGRESS_PORT": "7600",
		"PHOTON_VIDEO_CLIENT_PORT":  "7700",
		"PHOTON_DATA_DIR":           "/custom/data",
	}
	lookupEnv := func(k string) string { return mockEnv[k] }

	var buf bytes.Buffer
	cfg, err := ParseConfigWithEnv(lookupEnv, []string{}, &buf)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if cfg.Host != "127.0.0.1" {
		t.Errorf("expected host 127.0.0.1, got %q", cfg.Host)
	}
	if cfg.IngressPort != 7100 {
		t.Errorf("expected ingress port 7100, got %d", cfg.IngressPort)
	}
	if cfg.ClientPort != 7500 {
		t.Errorf("expected client port 7500, got %d", cfg.ClientPort)
	}
	if cfg.ObserverPort != 7400 {
		t.Errorf("expected observer port 7400, got %d", cfg.ObserverPort)
	}
	if cfg.VideoIngressPort != 7600 {
		t.Errorf("expected video ingress port 7600, got %d", cfg.VideoIngressPort)
	}
	if cfg.VideoClientPort != 7700 {
		t.Errorf("expected video client port 7700, got %d", cfg.VideoClientPort)
	}
	if cfg.DataDir != "/custom/data" {
		t.Errorf("expected data dir /custom/data, got %q", cfg.DataDir)
	}
}

func TestFlagOverridesEnv(t *testing.T) {
	mockEnv := map[string]string{
		"PHOTON_HOST":         "10.0.0.1",
		"PHOTON_INGRESS_PORT": "7100",
	}
	lookupEnv := func(k string) string { return mockEnv[k] }

	args := []string{
		"-host", "0.0.0.0",
		"-ingress-port", "8100",
		"-client-port", "8500",
		"-observer-port", "8400",
		"-video-ingress-port", "8600",
		"-video-client-port", "8700",
		"-data-dir", "./test-data",
	}

	var buf bytes.Buffer
	cfg, err := ParseConfigWithEnv(lookupEnv, args, &buf)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if cfg.Host != "0.0.0.0" {
		t.Errorf("expected host 0.0.0.0, got %q", cfg.Host)
	}
	if cfg.IngressPort != 8100 {
		t.Errorf("expected ingress port 8100, got %d", cfg.IngressPort)
	}
	if cfg.ClientPort != 8500 {
		t.Errorf("expected client port 8500, got %d", cfg.ClientPort)
	}
	if cfg.ObserverPort != 8400 {
		t.Errorf("expected observer port 8400, got %d", cfg.ObserverPort)
	}
	if cfg.VideoIngressPort != 8600 {
		t.Errorf("expected video ingress port 8600, got %d", cfg.VideoIngressPort)
	}
	if cfg.VideoClientPort != 8700 {
		t.Errorf("expected video client port 8700, got %d", cfg.VideoClientPort)
	}
	if cfg.DataDir != "./test-data" {
		t.Errorf("expected data dir ./test-data, got %q", cfg.DataDir)
	}
}

func TestFlagAliases(t *testing.T) {
	emptyEnv := func(string) string { return "" }
	args := []string{
		"-bind", "127.0.0.2",
		"-ingress", "6200",
		"-client", "6600",
		"-observer", "5500",
		"-video-ingress", "7600",
		"-video-client", "7700",
		"-storage-path", "/mnt/logs",
	}

	var buf bytes.Buffer
	cfg, err := ParseConfigWithEnv(emptyEnv, args, &buf)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}

	if cfg.Host != "127.0.0.2" {
		t.Errorf("expected host 127.0.0.2, got %q", cfg.Host)
	}
	if cfg.IngressPort != 6200 {
		t.Errorf("expected ingress port 6200, got %d", cfg.IngressPort)
	}
	if cfg.ClientPort != 6600 {
		t.Errorf("expected client port 6600, got %d", cfg.ClientPort)
	}
	if cfg.ObserverPort != 5500 {
		t.Errorf("expected observer port 5500, got %d", cfg.ObserverPort)
	}
	if cfg.VideoIngressPort != 7600 {
		t.Errorf("expected video ingress port 7600, got %d", cfg.VideoIngressPort)
	}
	if cfg.VideoClientPort != 7700 {
		t.Errorf("expected video client port 7700, got %d", cfg.VideoClientPort)
	}
	if cfg.DataDir != "/mnt/logs" {
		t.Errorf("expected data dir /mnt/logs, got %q", cfg.DataDir)
	}
}

func TestValidationErrors(t *testing.T) {
	tests := []struct {
		name string
		cfg  Config
	}{
		{
			name: "invalid ingress port (0)",
			cfg: Config{
				Host:             "0.0.0.0",
				IngressPort:      0,
				ClientPort:       6500,
				ObserverPort:     5400,
				VideoIngressPort: 6600,
				VideoClientPort:  6700,
				VideoUdpPort:     6800,
				DataDir:          "./data",
			},
		},
		{
			name: "invalid client port (>65535)",
			cfg: Config{
				Host:             "0.0.0.0",
				IngressPort:      6100,
				ClientPort:       70000,
				ObserverPort:     5400,
				VideoIngressPort: 6600,
				VideoClientPort:  6700,
				VideoUdpPort:     6800,
				DataDir:          "./data",
			},
		},
		{
			name: "duplicate ingress and client ports",
			cfg: Config{
				Host:             "0.0.0.0",
				IngressPort:      6100,
				ClientPort:       6100,
				ObserverPort:     5400,
				VideoIngressPort: 6600,
				VideoClientPort:  6700,
				VideoUdpPort:     6800,
				DataDir:          "./data",
			},
		},
		{
			name: "duplicate ingress and observer ports",
			cfg: Config{
				Host:             "0.0.0.0",
				IngressPort:      6100,
				ClientPort:       6500,
				ObserverPort:     6100,
				VideoIngressPort: 6600,
				VideoClientPort:  6700,
				VideoUdpPort:     6800,
				DataDir:          "./data",
			},
		},
		{
			name: "duplicate video ingress and video client ports",
			cfg: Config{
				Host:             "0.0.0.0",
				IngressPort:      6100,
				ClientPort:       6500,
				ObserverPort:     5400,
				VideoIngressPort: 6600,
				VideoClientPort:  6600,
				VideoUdpPort:     6800,
				DataDir:          "./data",
			},
		},
		{
			name: "invalid video ingress port",
			cfg: Config{
				Host:             "0.0.0.0",
				IngressPort:      6100,
				ClientPort:       6500,
				ObserverPort:     5400,
				VideoIngressPort: 0,
				VideoClientPort:  6700,
				VideoUdpPort:     6800,
				DataDir:          "./data",
			},
		},
		{
			name: "empty data dir",
			cfg: Config{
				Host:             "0.0.0.0",
				IngressPort:      6100,
				ClientPort:       6500,
				ObserverPort:     5400,
				VideoIngressPort: 6600,
				VideoClientPort:  6700,
				VideoUdpPort:     6800,
				DataDir:          "  ",
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if err := tt.cfg.Validate(); err == nil {
				t.Errorf("expected validation error for %s, got nil", tt.name)
			}
		})
	}
}
