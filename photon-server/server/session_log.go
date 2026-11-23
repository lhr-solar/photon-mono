package main

import (
	"bufio"
	"cmp"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"time"
)

// sessionLogName is the file in the segments folder that records when each
// server run started.
const sessionLogName = "sessions.log"

// sessionStart is one server run: its session ID, and the first message ID it
// wrote.
type sessionStart struct {
	SID     string
	FirstID MessageID
}

// sessionID names the server run that started at startedAt. Live frame rows
// carry it as their session tag, and backfill computes it again from
// sessions.log.
func sessionID(startedAt time.Time) string {
	return startedAt.UTC().Format("20060102T150405Z")
}

// appendSessionStart records that run sid starts at message firstID. Each run
// appends one line when it opens the store.
func appendSessionStart(segmentsDir, sid string, firstID MessageID) error {
	f, err := os.OpenFile(filepath.Join(segmentsDir, sessionLogName), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return err
	}
	_, err = fmt.Fprintf(f, "%s %d\n", sid, firstID)
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	return err
}

// readSessionStarts returns the recorded runs, sorted by first message ID. A
// missing file means no runs. Malformed lines are skipped, and so is a last
// line without its newline, which may be half written. When two runs share a
// first ID, the later line wins: that run stopped without writing anything.
func readSessionStarts(segmentsDir string) ([]sessionStart, error) {
	f, err := os.Open(filepath.Join(segmentsDir, sessionLogName))
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	defer f.Close()

	byID := make(map[MessageID]string)
	r := bufio.NewReader(f)
	for {
		line, err := r.ReadString('\n')
		if err != nil && !errors.Is(err, io.EOF) {
			return nil, err
		}
		if !strings.HasSuffix(line, "\n") {
			break // the end of the file, or a last line still being written
		}
		if s, ok := parseSessionStart(line); ok {
			byID[s.FirstID] = s.SID
		}
	}

	starts := make([]sessionStart, 0, len(byID))
	for id, sid := range byID {
		starts = append(starts, sessionStart{SID: sid, FirstID: id})
	}
	slices.SortFunc(starts, func(a, b sessionStart) int { return cmp.Compare(a.FirstID, b.FirstID) })
	return starts, nil
}

// parseSessionStart reads one "<sid> <first id>" line. It reports false for
// anything else, including a first ID of 0, which the store never hands out.
func parseSessionStart(line string) (sessionStart, bool) {
	fields := strings.Fields(line)
	if len(fields) != 2 {
		return sessionStart{}, false
	}
	id, err := strconv.ParseUint(fields[1], 10, 64)
	if err != nil || id == 0 {
		return sessionStart{}, false
	}
	return sessionStart{SID: fields[0], FirstID: MessageID(id)}, true
}
