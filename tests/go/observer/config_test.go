package main

import (
	"bytes"
	"testing"
)

func TestNormalizeAddress(t *testing.T) {
	tests := []struct {
		name        string
		input       string
		defaultHost string
		defaultPort int
		expected    string
	}{
		{
			name:        "empty string",
			input:       "",
			defaultHost: "127.0.0.1",
			defaultPort: 5400,
			expected:    "127.0.0.1:5400",
		},
		{
			name:        "port only with colon",
			input:       ":5400",
			defaultHost: "127.0.0.1",
			defaultPort: 5400,
			expected:    "127.0.0.1:5400",
		},
		{
			name:        "port only digits",
			input:       "5400",
			defaultHost: "127.0.0.1",
			defaultPort: 5400,
			expected:    "127.0.0.1:5400",
		},
		{
			name:        "host only IP",
			input:       "192.168.1.50",
			defaultHost: "127.0.0.1",
			defaultPort: 5400,
			expected:    "192.168.1.50:5400",
		},
		{
			name:        "host only hostname",
			input:       "photon.local",
			defaultHost: "127.0.0.1",
			defaultPort: 5400,
			expected:    "photon.local:5400",
		},
		{
			name:        "host and port",
			input:       "192.168.1.50:5401",
			defaultHost: "127.0.0.1",
			defaultPort: 5400,
			expected:    "192.168.1.50:5401",
		},
		{
			name:        "localhost with custom port",
			input:       "localhost:6000",
			defaultHost: "127.0.0.1",
			defaultPort: 5400,
			expected:    "localhost:6000",
		},
		{
			name:        "ipv6 bracketed with port",
			input:       "[::1]:5400",
			defaultHost: "127.0.0.1",
			defaultPort: 5400,
			expected:    "[::1]:5400",
		},
		{
			name:        "ipv6 without port",
			input:       "::1",
			defaultHost: "127.0.0.1",
			defaultPort: 5400,
			expected:    "[::1]:5400",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := normalizeAddress(tt.input, tt.defaultHost, tt.defaultPort)
			if got != tt.expected {
				t.Errorf("normalizeAddress(%q, %q, %d) = %q, expected %q",
					tt.input, tt.defaultHost, tt.defaultPort, got, tt.expected)
			}
		})
	}
}

func TestParseObserverConfig(t *testing.T) {
	emptyEnv := func(string) string { return "" }
	var buf bytes.Buffer

	// Default
	addr, err := parseObserverConfig([]string{}, emptyEnv, &buf)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if addr != "127.0.0.1:5400" {
		t.Errorf("expected default 127.0.0.1:5400, got %q", addr)
	}

	// Flag -addr
	addr, err = parseObserverConfig([]string{"-addr", "10.0.0.5:5400"}, emptyEnv, &buf)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if addr != "10.0.0.5:5400" {
		t.Errorf("expected 10.0.0.5:5400, got %q", addr)
	}

	// Flag -server
	addr, err = parseObserverConfig([]string{"-server", "10.0.0.6:5400"}, emptyEnv, &buf)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if addr != "10.0.0.6:5400" {
		t.Errorf("expected 10.0.0.6:5400, got %q", addr)
	}

	// Positional argument
	addr, err = parseObserverConfig([]string{"192.168.1.200:5400"}, emptyEnv, &buf)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if addr != "192.168.1.200:5400" {
		t.Errorf("expected 192.168.1.200:5400, got %q", addr)
	}

	// Env fallback PHOTON_OBSERVER_ADDR
	mockEnv := map[string]string{
		"PHOTON_OBSERVER_ADDR": "192.168.1.80:5400",
	}
	addr, err = parseObserverConfig([]string{}, func(k string) string { return mockEnv[k] }, &buf)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if addr != "192.168.1.80:5400" {
		t.Errorf("expected 192.168.1.80:5400, got %q", addr)
	}
}
