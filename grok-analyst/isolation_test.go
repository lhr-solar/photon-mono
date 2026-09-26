package analyst

import (
	"bytes"
	"context"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"testing"
	"time"
)

var (
	imageOnce sync.Once
	imageErr  error
)

func moduleDir() string {
	_, file, _, _ := runtime.Caller(0)
	return filepath.Dir(file)
}

func requireDocker(t *testing.T) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	if err := exec.CommandContext(ctx, "docker", "info").Run(); err != nil {
		t.Fatalf("docker daemon required: a job must not rely on Windows path jail: %v", err)
	}
}

func ensureJobImage(t *testing.T) {
	t.Helper()
	imageOnce.Do(func() {
		ctx, cancel := context.WithTimeout(context.Background(), 4*time.Minute)
		defer cancel()
		cmd := exec.CommandContext(ctx, "docker", "build", "-t", DefaultJobImage, "-f", "Dockerfile.job", ".")
		cmd.Dir = moduleDir()
		var buf bytes.Buffer
		cmd.Stdout = &buf
		cmd.Stderr = &buf
		if err := cmd.Run(); err != nil {
			imageErr = fmt.Errorf("docker build: %v\n%s", err, buf.String())
		}
	})
	if imageErr != nil {
		t.Fatal(imageErr)
	}
}

func startContainerSidecar(t *testing.T) (*Sidecar, string) {
	t.Helper()
	requireDocker(t)
	ensureJobImage(t)
	return startTestSidecar(t)
}

func waitJobStatus(t *testing.T, base, id, client, want string, d time.Duration) JobView {
	t.Helper()
	deadline := time.Now().Add(d)
	var view JobView
	for time.Now().Before(deadline) {
		_, view = getJob(t, base, id, client)
		if view.Status == want {
			return view
		}
		if terminal(view.Status) && view.Status != want {
			t.Fatalf("status=%s code=%s msg=%s want=%s", view.Status, view.Code, view.Message, want)
		}
		time.Sleep(80 * time.Millisecond)
	}
	t.Fatalf("timeout waiting for %s last=%+v", want, view)
	return view
}

func waitContainerRunning(t *testing.T, name string, d time.Duration) {
	t.Helper()
	deadline := time.Now().Add(d)
	for time.Now().Before(deadline) {
		if containerRunning("docker", name) {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("container %s not running", name)
}

func containerExists(name string) bool {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	return exec.CommandContext(ctx, "docker", "inspect", name).Run() == nil
}

func dockerExec(t *testing.T, name string, args ...string) string {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	cmdArgs := append([]string{"exec", "-u", JobUser, name}, args...)
	cmd := exec.CommandContext(ctx, "docker", cmdArgs...)
	out, _ := cmd.CombinedOutput()
	return string(out)
}

func hostPathGuesses(p string) []string {
	abs, err := filepath.Abs(p)
	if err != nil {
		return []string{p}
	}
	slash := filepath.ToSlash(abs)
	out := []string{abs, slash, p}
	if len(abs) >= 2 && abs[1] == ':' {
		drive := strings.ToLower(string(abs[0]))
		rest := filepath.ToSlash(abs[2:])
		out = append(out,
			"/mnt/"+drive+rest,
			"/host_mnt/"+drive+rest,
			"/run/desktop/mnt/host/"+drive+rest,
			"/mnt/host/"+drive+rest,
			"/host/"+drive+rest,
		)
	}
	return unique(out)
}

func unique(in []string) []string {
	seen := map[string]struct{}{}
	var out []string
	for _, s := range in {
		if s == "" {
			continue
		}
		if _, ok := seen[s]; ok {
			continue
		}
		seen[s] = struct{}{}
		out = append(out, s)
	}
	return out
}

func TestJobCannotReadForeignScratchOrHostTsdb(t *testing.T) {
	s, base := startContainerSidecar(t)

	hostRoot := t.TempDir()
	tsdbDir := filepath.Join(hostRoot, "data", "tsdb")
	videoDir := filepath.Join(hostRoot, "data", "video")
	repoDir := filepath.Join(hostRoot, "photon-next", "frontend", "src", "screens")
	for _, d := range []string{tsdbDir, videoDir, repoDir} {
		if err := os.MkdirAll(d, 0o700); err != nil {
			t.Fatal(err)
		}
	}
	const (
		tsdbSecret    = "TSDB_SECRET_DO_NOT_LEAK"
		videoSecret   = "VIDEO_SECRET_DO_NOT_LEAK"
		repoSecret    = "PIT_CPP_SECRET_DO_NOT_LEAK"
		sockSecret    = "DOCKER_SOCK_SECRET_DO_NOT_LEAK"
		foreignSecret = "FOREIGN_CLIENT_B_SECRET_DO_NOT_LEAK"
	)
	mustWrite := func(path, body string) {
		t.Helper()
		if err := os.WriteFile(path, []byte(body), 0o600); err != nil {
			t.Fatal(err)
		}
	}
	mustWrite(filepath.Join(tsdbDir, "catalog.json"), tsdbSecret)
	mustWrite(filepath.Join(videoDir, "clip.mp4"), videoSecret)
	mustWrite(filepath.Join(repoDir, "pit.cpp"), repoSecret)
	mustWrite(filepath.Join(hostRoot, "docker.sock"), sockSecret)

	codeB, viewB := postJob(t, base, JobRequest{
		JobID: "job-b", ClientID: "client-b", Recipe: "freeform",
		TimeoutS: 90, HoldMs: 60000,
	})
	if codeB != http.StatusAccepted {
		t.Fatalf("client-b POST %d %+v", codeB, viewB)
	}
	nameB := ContainerName("client-b", "job-b")
	waitContainerRunning(t, nameB, 45*time.Second)
	bJail := filepath.Join(s.Scratch, "client-b", "job-b")
	mustWrite(filepath.Join(bJail, "secret.txt"), foreignSecret)

	codeA, viewA := postJob(t, base, JobRequest{
		JobID: "job-a", ClientID: "client-a", Recipe: "freeform",
		TimeoutS: 90, HoldMs: 60000,
	})
	if codeA != http.StatusAccepted {
		t.Fatalf("client-a POST %d %+v", codeA, viewA)
	}
	nameA := ContainerName("client-a", "job-a")
	waitContainerRunning(t, nameA, 45*time.Second)

	own := dockerExec(t, nameA, "/bin/sh", "-c", "cat /job/request.json")
	if !strings.Contains(own, "client-a") {
		t.Fatalf("job A cannot read its own scratch (exec broken?): %s", own)
	}

	inspect := dockerExecInspect(t, nameA)
	if !strings.Contains(inspect["NetworkMode"], "none") {
		t.Fatalf("network not default-deny: %v", inspect)
	}
	if inspect["Memory"] != fmt.Sprintf("%d", JobMemoryBytes) {
		t.Fatalf("memory limit: %v", inspect)
	}
	if inspect["PidsLimit"] != fmt.Sprintf("%d", JobPIDs) {
		t.Fatalf("pids limit: %v", inspect)
	}
	mounts := inspect["Mounts"]
	if !strings.Contains(mounts, "/job") {
		t.Fatalf("expected /job mount, got %s", mounts)
	}
	lowMounts := strings.ToLower(mounts)
	for _, bad := range []string{"docker.sock", "data/tsdb", "data/video", "photon-next", "photon-server", "client-b"} {
		if strings.Contains(lowMounts, bad) {
			t.Fatalf("forbidden mount leaked %q: %s", bad, mounts)
		}
	}

	nets := dockerExec(t, nameA, "/bin/sh", "-c", "ls /sys/class/net")
	if strings.Contains(nets, "eth") {
		t.Fatalf("job has egress interface: %s", nets)
	}

	secrets := []string{tsdbSecret, videoSecret, repoSecret, sockSecret, foreignSecret}
	probe := []string{
		"/job/secret.txt",
		"/job/../client-b/job-b/secret.txt",
		"/job/../../client-b/job-b/secret.txt",
		"/data/tsdb/catalog.json",
		"/data/video/clip.mp4",
		"/var/run/docker.sock",
		"/run/docker.sock",
		"//./pipe/docker_engine",
		"/job/bundle.json",
	}
	probe = append(probe, hostPathGuesses(filepath.Join(bJail, "secret.txt"))...)
	probe = append(probe, hostPathGuesses(filepath.Join(tsdbDir, "catalog.json"))...)
	probe = append(probe, hostPathGuesses(filepath.Join(videoDir, "clip.mp4"))...)
	probe = append(probe, hostPathGuesses(filepath.Join(repoDir, "pit.cpp"))...)
	probe = append(probe, hostPathGuesses(filepath.Join(hostRoot, "docker.sock"))...)

	var leaked []string
	for _, p := range probe {
		out := dockerExec(t, nameA, "/bin/sh", "-c", "cat -- "+shellSingle(p)+" 2>/dev/null || true")
		for _, secret := range secrets {
			if strings.Contains(out, secret) {
				leaked = append(leaked, p+":"+secret)
			}
		}
	}
	if len(leaked) > 0 {
		t.Fatalf("job A read forbidden data via %v", leaked)
	}

	code, _ := cancelJob(t, base, "job-a", "client-a")
	if code != http.StatusOK {
		t.Fatalf("cancel A %d", code)
	}
	_, _ = cancelJob(t, base, "job-b", "client-b")
	deadline := time.Now().Add(15 * time.Second)
	for time.Now().Before(deadline) {
		if !containerExists(nameA) && !containerExists(nameB) {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	t.Fatalf("leftover containers A=%v B=%v", containerExists(nameA), containerExists(nameB))
}

func dockerExecInspect(t *testing.T, name string) map[string]string {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	format := "{{.HostConfig.NetworkMode}}|{{.HostConfig.Memory}}|{{.HostConfig.PidsLimit}}|{{range .Mounts}}{{.Destination}}={{.Source}};{{end}}"
	out, err := exec.CommandContext(ctx, "docker", "inspect", "-f", format, name).Output()
	if err != nil {
		t.Fatalf("inspect %s: %v", name, err)
	}
	parts := strings.Split(strings.TrimSpace(string(out)), "|")
	got := map[string]string{}
	if len(parts) > 0 {
		got["NetworkMode"] = parts[0]
	}
	if len(parts) > 1 {
		got["Memory"] = parts[1]
	}
	if len(parts) > 2 {
		got["PidsLimit"] = parts[2]
	}
	if len(parts) > 3 {
		got["Mounts"] = parts[3]
	}
	return got
}

func shellSingle(s string) string {
	return "'" + strings.ReplaceAll(s, "'", `'"'"'`) + "'"
}
