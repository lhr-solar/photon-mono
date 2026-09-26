package analyst

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"time"
)

const (
	DefaultJobImage = "grok-analyst-job:local"
	JobCPUs         = "1.0"
	JobMemory       = "1g"
	JobMemoryBytes  = 1 << 30
	JobPIDs         = 64
	JobNetwork      = "none"
	JobUser         = "65534:65534"
	containerLabel  = "grok-analyst=job"
)

// ContainerName is unique per client+job. Docker names cannot contain ':'.
func ContainerName(clientID, jobID string) string {
	return "gaj-" + jailSegment(clientID) + "-" + jailSegment(jobID)
}

func (s *Sidecar) dockerBin() string {
	if s.DockerBin != "" {
		return s.DockerBin
	}
	if v := os.Getenv("GROK_ANALYST_DOCKER"); v != "" {
		return v
	}
	return "docker"
}

func (s *Sidecar) jobImage() string {
	if s.JobImage != "" {
		return s.JobImage
	}
	if v := os.Getenv("GROK_ANALYST_JOB_IMAGE"); v != "" {
		return v
	}
	return DefaultJobImage
}

// DockerRunArgs is the only allowed docker-run shape for a compute job.
// No publish, no docker.sock, no TSDB/video/repo mounts, default-deny net.
func DockerRunArgs(name, image, jail string) ([]string, error) {
	if name == "" || image == "" {
		return nil, fmt.Errorf("container name and image required")
	}
	abs, err := filepath.Abs(jail)
	if err != nil {
		return nil, err
	}
	if err := rejectMount(abs); err != nil {
		return nil, err
	}
	src := dockerBindSrc(abs)
	mount := "type=bind,source=" + src + ",target=/job"
	if strings.Contains(strings.ToLower(mount), "docker.sock") {
		return nil, fmt.Errorf("refusing docker.sock mount")
	}
	args := []string{
		"run",
		"--rm",
		"--name", name,
		"--cpus=" + JobCPUs,
		"--memory=" + JobMemory,
		"--memory-swap=" + JobMemory,
		"--pids-limit=" + strconv.Itoa(JobPIDs),
		"--network=" + JobNetwork,
		"--read-only",
		"--tmpfs", "/tmp:rw,nosuid,nodev,size=16m",
		"--tmpfs", "/run:rw,nosuid,nodev,size=8m",
		"--cap-drop=ALL",
		"--security-opt", "no-new-privileges:true",
		"--user", JobUser,
		"--pull=never",
		"--label", containerLabel,
		"--label", "grok-analyst.job=" + name,
		"--mount", mount,
		image,
	}
	return args, nil
}

func dockerBindSrc(abs string) string {
	if runtime.GOOS == "windows" {
		return filepath.ToSlash(abs)
	}
	return abs
}

func rejectMount(path string) error {
	if path == "" {
		return fmt.Errorf("empty mount")
	}
	if forbiddenMount(path) {
		return fmt.Errorf("mount path not allowed: %s", path)
	}
	return nil
}

func forbiddenMount(path string) bool {
	norm := strings.ToLower(filepath.ToSlash(path))
	if strings.Contains(norm, "docker.sock") {
		return true
	}
	if strings.Contains(norm, "npipe:////./pipe/docker") {
		return true
	}
	parts := strings.Split(norm, "/")
	for _, p := range parts {
		if p == "tsdb" || p == "video" {
			return true
		}
	}
	if strings.Contains(norm, "data/tsdb") || strings.Contains(norm, "data/video") {
		return true
	}
	if strings.Contains(norm, "/photon-next/") || strings.HasSuffix(norm, "/photon-next") {
		return true
	}
	if strings.Contains(norm, "/photon-server/") || strings.HasSuffix(norm, "/photon-server") {
		return true
	}
	return false
}

func (s *Sidecar) runContainer(ctx context.Context, j *job) error {
	args, err := DockerRunArgs(j.container, s.jobImage(), j.jail)
	if err != nil {
		return err
	}
	cmd := exec.CommandContext(ctx, s.dockerBin(), args...)
	var buf bytes.Buffer
	cmd.Stdout = &buf
	cmd.Stderr = &buf
	err = cmd.Run()
	s.mu.Lock()
	if buf.Len() > 0 {
		j.logs = buf.String()
		if len(j.logs) > 8<<10 {
			j.logs = j.logs[len(j.logs)-8<<10:]
		}
	}
	s.mu.Unlock()
	return err
}

func (s *Sidecar) killContainer(name string) {
	if name == "" {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, s.dockerBin(), "rm", "-f", name)
	_ = cmd.Run()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		check, checkCancel := context.WithTimeout(context.Background(), 2*time.Second)
		err := exec.CommandContext(check, s.dockerBin(), "inspect", name).Run()
		checkCancel()
		if err != nil {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
}

func (s *Sidecar) reapJobContainers() {
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, s.dockerBin(), "ps", "-aq", "--filter", "label="+containerLabel).Output()
	if err != nil {
		return
	}
	for _, id := range strings.Fields(string(out)) {
		_ = exec.CommandContext(ctx, s.dockerBin(), "rm", "-f", id).Run()
	}
}

func containerRunning(dockerBin, name string) bool {
	if name == "" {
		return false
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	out, err := exec.CommandContext(ctx, dockerBin, "inspect", "-f", "{{.State.Running}}", name).Output()
	if err != nil {
		return false
	}
	return strings.TrimSpace(string(out)) == "true"
}
