package analyst

import (
	"net/http"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestDockerRunArgsDefaultDeny(t *testing.T) {
	jail := t.TempDir()
	args, err := DockerRunArgs("gaj-alice-job-a", DefaultJobImage, jail)
	if err != nil {
		t.Fatal(err)
	}
	joined := strings.Join(args, " ")
	for _, need := range []string{
		"--network=none",
		"--cpus=1.0",
		"--memory=1g",
		"--pids-limit=64",
		"--cap-drop=ALL",
		"--read-only",
		"--pull=never",
		"--user",
		"65534:65534",
		"target=/job",
		"--label",
		"grok-analyst=job",
	} {
		if !strings.Contains(joined, need) {
			t.Fatalf("missing %q in %v", need, args)
		}
	}
	for _, a := range args {
		if a == "-p" || a == "--publish" || strings.HasPrefix(a, "-p=") || strings.HasPrefix(a, "--publish") {
			t.Fatalf("job container must not publish ports: %v", args)
		}
	}
	for _, bad := range []string{
		"6100", "6500", "6700",
		"docker.sock", "data/tsdb", "data/video", "photon-next", "photon-server",
	} {
		if strings.Contains(joined, bad) {
			t.Fatalf("forbidden %q in docker args: %v", bad, args)
		}
	}
	if strings.Count(joined, "--mount") != 1 {
		t.Fatalf("expected exactly one mount, got %v", args)
	}
}

func TestDockerRunArgsRejectForbiddenJail(t *testing.T) {
	for _, jail := range []string{
		filepath.Join(t.TempDir(), "data", "tsdb", "job"),
		filepath.Join(t.TempDir(), "data", "video", "job"),
		filepath.Join(t.TempDir(), "docker.sock"),
		filepath.Join("F:", "lhr", "photon-next", "frontend"),
		filepath.Join("F:", "lhr", "photon-server", "data"),
	} {
		if _, err := DockerRunArgs("gaj-x-y", DefaultJobImage, jail); err == nil {
			t.Fatalf("expected reject for %s", jail)
		}
	}
}

func TestForbiddenMount(t *testing.T) {
	if !forbiddenMount(`F:\lhr\photon-server\data\tsdb\catalog.json`) {
		t.Fatal("tsdb must be forbidden")
	}
	if !forbiddenMount(`/var/run/docker.sock`) {
		t.Fatal("docker.sock must be forbidden")
	}
	if !forbiddenMount(`C:\Users\x\data\video\clip.mp4`) {
		t.Fatal("video must be forbidden")
	}
	if forbiddenMount(filepath.Join(t.TempDir(), "grok-analyst-jails", "client-a", "job-1")) {
		t.Fatal("job scratch must be allowed")
	}
}

func TestNoInProcessFallback(t *testing.T) {
	root := t.TempDir()
	s := New("127.0.0.1:0", filepath.Join(root, "jails"), filepath.Join(root, "audit.log"))
	s.DockerBin = filepath.Join(root, "definitely-not-docker")
	if err := s.Listen(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = s.Close() })
	base := "http://" + s.Addr()
	code, view := postJob(t, base, JobRequest{
		JobID: "job-inline", ClientID: "client-a", Recipe: "summarize_window", HoldMs: 5,
	})
	if code != http.StatusAccepted && code != http.StatusInternalServerError {
		t.Fatalf("POST status %d", code)
	}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) && !terminal(view.Status) {
		time.Sleep(20 * time.Millisecond)
		_, view = getJob(t, base, "job-inline", "client-a")
	}
	if view.Status != "failed" || view.Code != "container" {
		t.Fatalf("in-process path jail still running jobs: %+v", view)
	}
	if view.Summary != "" {
		t.Fatalf("host stub summary leaked: %q", view.Summary)
	}
}

func TestListenRejectsStoreScratch(t *testing.T) {
	s := New("127.0.0.1:0", filepath.Join(t.TempDir(), "data", "tsdb"), "")
	if err := s.Listen(); err == nil {
		_ = s.Close()
		t.Fatal("expected scratch reject for tsdb")
	}
}
