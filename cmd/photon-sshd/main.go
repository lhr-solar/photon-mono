package main

import (
	"flag"
	"log"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"

	"server/sshd"
)

func main() {
	log.SetFlags(log.Ldate | log.Ltime | log.Lmicroseconds)
	bind := flag.String("bind", sshd.DefaultBind, "listen address (loopback default)")
	keys := flag.String("keys", "", "authorized_keys path (default: next to exe)")
	hostKey := flag.String("host-key", "", "host private key (default: next to exe)")
	password := flag.String("password", os.Getenv("PHOTON_PASSWORD"), "optional password auth")
	shell := flag.String("shell", "", "login shell (default: cmd.exe / /bin/sh)")
	flag.Parse()

	dir := exeDir()
	if strings.TrimSpace(*keys) == "" {
		*keys = firstExisting(
			filepath.Join(dir, "authorized_keys"),
			filepath.Join(os.Getenv("USERPROFILE"), ".ssh", "authorized_keys"),
		)
	}
	if strings.TrimSpace(*hostKey) == "" {
		*hostKey = filepath.Join(dir, "ssh_host_ed25519")
	}

	srv, err := sshd.New(sshd.Config{
		Bind:        *bind,
		HostKeyPath: *hostKey,
		AuthKeys:    *keys,
		Password:    *password,
		Shell:       *shell,
	})
	if err != nil {
		log.Fatalf("[photon-sshd] %v", err)
	}
	if err := srv.Start(); err != nil {
		log.Fatalf("[photon-sshd] listen: %v", err)
	}

	ch := make(chan os.Signal, 1)
	signal.Notify(ch, os.Interrupt, syscall.SIGTERM)
	sig := <-ch
	log.Printf("[photon-sshd] shutting down (%s)", sig)
	_ = srv.Close()
}

func exeDir() string {
	exe, err := os.Executable()
	if err != nil {
		return "."
	}
	return filepath.Dir(exe)
}

func firstExisting(paths ...string) string {
	for _, p := range paths {
		if st, err := os.Stat(p); err == nil && !st.IsDir() {
			return p
		}
	}
	if len(paths) > 0 {
		return paths[0]
	}
	return ""
}
