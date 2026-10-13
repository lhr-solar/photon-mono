package main

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

// loadFirstDotEnv reads the first .env file that exists. The process
// environment is applied later and overrides these values.
func loadFirstDotEnv() (map[string]string, error) {
	for _, path := range dotenvCandidates() {
		st, err := os.Stat(path)
		if err != nil || st.IsDir() {
			continue
		}
		return parseDotEnvFile(path)
	}
	return map[string]string{}, nil
}

func dotenvCandidates() []string {
	var paths []string
	if p := strings.TrimSpace(os.Getenv("PHOTON_ENV_FILE")); p != "" {
		paths = append(paths, p)
	}
	if cwd, err := os.Getwd(); err == nil {
		paths = append(paths, filepath.Join(cwd, ".env"))
		paths = append(paths, filepath.Join(cwd, "..", ".env"))
	}
	if exe, err := os.Executable(); err == nil {
		dir := filepath.Dir(exe)
		paths = append(paths, filepath.Join(dir, ".env"))
		paths = append(paths, filepath.Join(dir, "..", ".env"))
	}
	return paths
}

func parseDotEnvFile(path string) (map[string]string, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("read %s: %w", path, err)
	}
	defer f.Close()
	vals, err := parseDotEnv(f)
	if err != nil {
		return nil, fmt.Errorf("parse %s: %w", path, err)
	}
	return vals, nil
}

func parseDotEnv(r io.Reader) (map[string]string, error) {
	sc := bufio.NewScanner(r)
	out := map[string]string{}
	lineNo := 0
	for sc.Scan() {
		lineNo++
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		if strings.HasPrefix(line, "export ") {
			line = strings.TrimSpace(strings.TrimPrefix(line, "export "))
		}
		key, val, ok := strings.Cut(line, "=")
		if !ok {
			return nil, fmt.Errorf("line %d: expected KEY=VALUE", lineNo)
		}
		key = strings.TrimSpace(key)
		if key == "" || strings.ContainsAny(key, " \t") {
			return nil, fmt.Errorf("line %d: invalid key", lineNo)
		}
		val = strings.TrimSpace(val)
		if len(val) >= 2 {
			if (val[0] == '"' && val[len(val)-1] == '"') || (val[0] == '\'' && val[len(val)-1] == '\'') {
				val = val[1 : len(val)-1]
			}
		}
		out[key] = val
	}
	if err := sc.Err(); err != nil {
		return nil, err
	}
	return out, nil
}
