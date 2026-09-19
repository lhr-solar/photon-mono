package analyst

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"unicode"
)

const (
	maxBundleBytes = 32 * 1024 * 1024
	maxIDLen       = 128
)

var (
	errBadID       = errors.New("job or client id is not a jail-safe identifier")
	errBundleCap   = errors.New("bundle exceeds 32 MB cap")
	errTSDBPath    = errors.New("bundle path must not be a TSDB or video store")
	errNoBundle    = errors.New("bundle path is empty")
	errOutsideJail = errors.New("path is outside the job jail")
)

// Jail is a per-job scratch directory. Two jobs never share a dir.
// Destroy removes the tree; it is the only cleanup path.
type Jail struct {
	Root     string
	Dir      string
	ClientID string
	JobID    string
}

func safeID(id string) bool {
	if id == "" || len(id) > maxIDLen {
		return false
	}
	for _, r := range id {
		if unicode.IsLetter(r) || unicode.IsDigit(r) || r == '-' || r == '_' || r == '.' || r == ':' {
			continue
		}
		return false
	}
	if strings.Contains(id, "..") {
		return false
	}
	return true
}

func looksLikeStore(path string) bool {
	norm := strings.ToLower(filepath.ToSlash(filepath.Clean(path)))
	parts := strings.Split(norm, "/")
	for _, p := range parts {
		if p == "tsdb" || p == "video" {
			return true
		}
	}
	return strings.Contains(norm, "/data/tsdb") || strings.HasSuffix(norm, "/catalog.json")
}

func NewJail(root, clientID, jobID string) (*Jail, error) {
	if !safeID(clientID) || !safeID(jobID) {
		return nil, errBadID
	}
	if strings.TrimSpace(root) == "" {
		return nil, fmt.Errorf("scratch root is empty")
	}
	dir := filepath.Join(root, "jobs", clientID, jobID)
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, err
	}
	return &Jail{Root: root, Dir: dir, ClientID: clientID, JobID: jobID}, nil
}

func (j *Jail) BundlePath() string {
	if j == nil {
		return ""
	}
	return filepath.Join(j.Dir, "bundle.json")
}

func (j *Jail) Confine(path string) (string, error) {
	if j == nil {
		return "", errOutsideJail
	}
	absJail, err := filepath.Abs(j.Dir)
	if err != nil {
		return "", err
	}
	absPath, err := filepath.Abs(path)
	if err != nil {
		return "", err
	}
	rel, err := filepath.Rel(absJail, absPath)
	if err != nil || strings.HasPrefix(rel, "..") {
		return "", errOutsideJail
	}
	return absPath, nil
}

func (j *Jail) InstallBundle(src string) (int64, error) {
	if j == nil {
		return 0, errOutsideJail
	}
	if strings.TrimSpace(src) == "" {
		return 0, errNoBundle
	}
	if looksLikeStore(src) {
		return 0, errTSDBPath
	}
	info, err := os.Stat(src)
	if err != nil {
		return 0, err
	}
	if info.IsDir() {
		return 0, fmt.Errorf("bundle path is a directory")
	}
	if info.Size() > maxBundleBytes {
		return 0, errBundleCap
	}
	in, err := os.Open(src)
	if err != nil {
		return 0, err
	}
	defer in.Close()

	dst := j.BundlePath()
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o400)
	if err != nil {
		return 0, err
	}
	defer out.Close()

	n, err := io.Copy(out, io.LimitReader(in, maxBundleBytes+1))
	if err != nil {
		return 0, err
	}
	if n > maxBundleBytes {
		_ = os.Remove(dst)
		return 0, errBundleCap
	}
	return n, nil
}

func (j *Jail) Destroy() error {
	if j == nil || j.Dir == "" {
		return nil
	}
	err := os.RemoveAll(j.Dir)
	parent := filepath.Dir(j.Dir)
	if parent != "" && filepath.Base(filepath.Dir(parent)) == "jobs" {
		_ = os.Remove(parent) // remove empty client dir
	}
	j.Dir = ""
	return err
}
