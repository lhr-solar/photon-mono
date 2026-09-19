package main

import (
	"flag"
	"log"
	"os"
	"os/signal"
	"syscall"

	"server/analyst"
)

func main() {
	log.SetFlags(log.Ldate | log.Ltime | log.Lmicroseconds)
	bind := flag.String("bind", "127.0.0.1:18765", "loopback listen address (refuses non-loopback)")
	scratch := flag.String("scratch", analyst.DefaultScratch(), "per-job jail root (not a TSDB path)")
	audit := flag.String("audit", "", "append-only audit log (default: <scratch>/audit.log)")
	flag.Parse()

	cfg := analyst.Config{
		Bind:    *bind,
		Scratch: *scratch,
		Audit:   *audit,
	}
	srv, err := analyst.NewServer(cfg)
	if err != nil {
		log.Fatalf("[grok-analyst] %v", err)
	}
	if err := srv.Start(); err != nil {
		log.Fatalf("[grok-analyst] listen: %v", err)
	}

	ch := make(chan os.Signal, 1)
	signal.Notify(ch, os.Interrupt, syscall.SIGTERM)
	sig := <-ch
	log.Printf("[grok-analyst] shutting down (%s)", sig)
	_ = srv.Close()
}
