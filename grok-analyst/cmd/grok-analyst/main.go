package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"

	analyst "grok-analyst"
)

func main() {
	defaultBind := os.Getenv("GROK_ANALYST_BIND")
	if defaultBind == "" {
		defaultBind = analyst.DefaultBind
	}
	bind := flag.String("bind", defaultBind, "loopback host:port (127.0.0.1 only)")
	scratch := flag.String("scratch", os.Getenv("GROK_ANALYST_SCRATCH"), "per-job jail root")
	audit := flag.String("audit", os.Getenv("GROK_ANALYST_AUDIT"), "append-only audit log (no secrets)")
	docker := flag.String("docker", os.Getenv("GROK_ANALYST_DOCKER"), "docker CLI used to spawn per-job containers")
	image := flag.String("job-image", os.Getenv("GROK_ANALYST_JOB_IMAGE"), "per-job runner image")
	flag.Parse()

	s := analyst.New(*bind, *scratch, *audit)
	s.DockerBin = *docker
	s.JobImage = *image
	if err := s.Listen(); err != nil {
		log.Fatalf("[grok-analyst] %v", err)
	}
	fmt.Fprintf(os.Stdout, "[grok-analyst] loopback %s scratch=%s audit=%s\n", s.Addr(), s.Scratch, s.AuditPath)
	fmt.Fprintf(os.Stdout, "[grok-analyst] one container per job image=%s (no ingest ports, no TSDB, no docker.sock on workers)\n", analyst.DefaultJobImage)
	if s.JobImage != "" {
		fmt.Fprintf(os.Stdout, "[grok-analyst] job image override %s\n", s.JobImage)
	}

	ch := make(chan os.Signal, 1)
	signal.Notify(ch, os.Interrupt, syscall.SIGTERM)
	<-ch
	fmt.Fprintf(os.Stdout, "[grok-analyst] shutting down (cancel + destroy containers)\n")
	_ = s.Close()
}
