package analyst

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

// AuditLog is append-only. No secrets. Clients cannot read other clients' lines
// because this file lives next to photon-server logs, not in TSDB.
type AuditLog struct {
	path string
	mu   sync.Mutex
}

type AuditRow struct {
	ClientID    string
	JobID       string
	SessionID   string
	T0Ms        int64
	T1Ms        int64
	SignalCount int
	Recipe      string
	Status      string
	ElapsedMs   int64
	BytesIn     int64
	BytesOut    int64
}

func NewAuditLog(path string) (*AuditLog, error) {
	if strings.TrimSpace(path) == "" {
		return nil, fmt.Errorf("audit path is empty")
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return nil, err
	}
	f, err := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		return nil, err
	}
	_ = f.Close()
	return &AuditLog{path: path}, nil
}

func sanitizeAudit(s string) string {
	s = strings.ReplaceAll(s, ",", " ")
	s = strings.ReplaceAll(s, "\n", " ")
	s = strings.ReplaceAll(s, "\r", " ")
	low := strings.ToLower(s)
	if strings.Contains(low, "key=") || strings.Contains(low, "password") || strings.Contains(low, "secret") {
		return "-"
	}
	if s == "" {
		return "-"
	}
	return s
}

func (a *AuditLog) Append(row AuditRow) error {
	if a == nil {
		return nil
	}
	line := fmt.Sprintf("%s, %s, %s, %s, %d, %d, %d, %s, %s, %d, %d, %d\n",
		time.Now().UTC().Format(time.RFC3339Nano),
		sanitizeAudit(row.ClientID),
		sanitizeAudit(row.JobID),
		sanitizeAudit(row.SessionID),
		row.T0Ms,
		row.T1Ms,
		row.SignalCount,
		sanitizeAudit(row.Recipe),
		sanitizeAudit(row.Status),
		row.ElapsedMs,
		row.BytesIn,
		row.BytesOut,
	)
	a.mu.Lock()
	defer a.mu.Unlock()
	f, err := os.OpenFile(a.path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o600)
	if err != nil {
		return err
	}
	defer f.Close()
	_, err = f.WriteString(line)
	return err
}

func (a *AuditLog) Path() string {
	if a == nil {
		return ""
	}
	return a.path
}
