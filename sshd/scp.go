package sshd

import (
	"fmt"
	"io"
	"log"
	"os"
	"path/filepath"
	"strings"
)

func isSCPCommand(command string) bool {
	fields := strings.Fields(command)
	return len(fields) > 0 && (fields[0] == "scp" || strings.HasSuffix(strings.ToLower(fields[0]), "scp.exe"))
}

func runSCP(ch io.ReadWriter, command string) int {
	from, path, err := parseSCP(command)
	if err != nil {
		log.Printf("[photon-sshd] scp parse %q: %v", command, err)
		scpError(ch, err.Error())
		return 1
	}
	path = windowsPath(path)
	log.Printf("[photon-sshd] scp from=%t path=%s", from, path)
	if !from {
		scpError(ch, "upload not enabled")
		return 1
	}
	if err := scpSendFile(ch, path); err != nil {
		log.Printf("[photon-sshd] scp send: %v", err)
		return 1
	}
	return 0
}

func parseSCP(command string) (from bool, path string, err error) {
	fields := strings.Fields(command)
	if len(fields) < 2 {
		return false, "", fmt.Errorf("scp: missing path")
	}
	for _, f := range fields[1:] {
		if f == "-f" {
			from = true
			continue
		}
		if f == "-t" {
			from = false
			continue
		}
		if strings.HasPrefix(f, "-") {
			continue
		}
		path = f
	}
	if path == "" {
		return false, "", fmt.Errorf("scp: missing path")
	}
	if !from {
		// -t is upload; default to download if only a path was given
		for _, f := range fields[1:] {
			if f == "-t" {
				return false, path, nil
			}
		}
		from = true
	}
	return from, path, nil
}

func windowsPath(path string) string {
	path = strings.TrimSpace(path)
	path = strings.ReplaceAll(path, "\\", "/")
	if strings.HasPrefix(path, "/") && len(path) >= 3 && path[2] == ':' {
		path = path[1:]
	}
	return filepath.Clean(filepath.FromSlash(path))
}

func scpSendFile(ch io.ReadWriter, path string) error {
	st, err := os.Stat(path)
	if err != nil {
		scpError(ch, err.Error())
		return err
	}
	if st.IsDir() {
		scpError(ch, path+" is a directory")
		return fmt.Errorf("directory")
	}
	var start [1]byte
	if _, err := io.ReadFull(ch, start[:]); err != nil {
		return err
	}
	if start[0] != 0 {
		return fmt.Errorf("scp start ack %d", start[0])
	}
	hdr := fmt.Sprintf("C%04o %d %s\n", st.Mode().Perm()&0o777, st.Size(), filepath.Base(path))
	if _, err := io.WriteString(ch, hdr); err != nil {
		return err
	}
	if err := scpAck(ch); err != nil {
		return err
	}
	f, err := os.Open(path)
	if err != nil {
		scpError(ch, err.Error())
		return err
	}
	defer f.Close()
	if _, err := io.Copy(ch, f); err != nil {
		return err
	}
	if _, err := ch.Write([]byte{0}); err != nil {
		return err
	}
	return scpAck(ch)
}

func scpAck(ch io.Reader) error {
	var b [1]byte
	if _, err := io.ReadFull(ch, b[:]); err != nil {
		return err
	}
	if b[0] == 0 {
		return nil
	}
	msg, _ := io.ReadAll(io.LimitReader(ch, 512))
	return fmt.Errorf("scp nack %d %s", b[0], strings.TrimSpace(string(msg)))
}

func scpError(ch io.Writer, msg string) {
	fmt.Fprintf(ch, "\x01%s\n", msg)
}
