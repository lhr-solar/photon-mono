package main

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"time"
)

// sessionMap says which server run wrote each record of the old files, from the
// starts recorded in sessions.log.
type sessionMap struct {
	starts   []sessionStart // sorted by first ID
	fallback string         // the run for records older than starts[0], named from the first of them
}

// sid returns the run that wrote message id, which was written at writeUnix.
// Records older than the first recorded start (data from before sessions.log
// existed) all go to one fallback run.
func (m *sessionMap) sid(id MessageID, writeUnix uint32) string {
	if i := sort.Search(len(m.starts), func(i int) bool { return m.starts[i].FirstID > id }); i > 0 {
		return m.starts[i-1].SID
	}
	if m.fallback == "" {
		m.fallback = sessionID(time.Unix(int64(writeUnix), 0))
	}
	return m.fallback
}

// legacyMigration says what migrateLegacySegments did.
type legacyMigration struct {
	Runs    int
	Records int64
	Bytes   int64 // bytes of records, headers included
	Moved   int   // old files moved into legacy
	Elapsed time.Duration
}

// legacyFiles returns the names of the old-format segment files in segmentsDir,
// in number order, and whether sessions.log is there.
func legacyFiles(segmentsDir string) (segs []string, hasLog bool, err error) {
	entries, err := os.ReadDir(segmentsDir)
	if errors.Is(err, fs.ErrNotExist) {
		// A new data directory: the server creates segments/ after this runs.
		return nil, false, nil
	}
	if err != nil {
		return nil, false, err
	}
	type numbered struct {
		n    int
		name string
	}
	var found []numbered
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		if e.Name() == sessionLogName {
			hasLog = true
			continue
		}
		if n, ok := legacySegmentNumber(e.Name()); ok {
			found = append(found, numbered{n, e.Name()})
		}
	}
	sort.Slice(found, func(i, j int) bool { return found[i].n < found[j].n })
	for _, f := range found {
		segs = append(segs, f.name)
	}
	return segs, hasLog, nil
}

// migrateLegacySegments moves the old layout, one file per 256 MiB with all runs
// mixed and a sessions.log, into run folders in the new layout. Records are
// copied unchanged: same IDs, payloads and write times. Then the old files move
// into segments/legacy. It does nothing when there are no old files.
//
// If it fails, the run folders it made are removed and the old files stay where
// they are. A later start tries again.
func migrateLegacySegments(segmentsDir string) (legacyMigration, error) {
	started := time.Now()
	segs, hasLog, err := legacyFiles(segmentsDir)
	if err != nil {
		return legacyMigration{}, err
	}
	if len(segs) == 0 && !hasLog {
		return legacyMigration{}, nil
	}

	legacyDir := filepath.Join(segmentsDir, legacyFolder)
	moves := slices.Clone(segs)
	if hasLog {
		moves = append(moves, sessionLogName)
	}
	for _, name := range moves {
		if _, err := os.Stat(filepath.Join(legacyDir, name)); err == nil {
			return legacyMigration{}, fmt.Errorf("%s is already in %s; move it aside and start again", name, legacyFolder)
		}
	}
	starts, err := readSessionStarts(segmentsDir)
	if err != nil {
		return legacyMigration{}, err
	}

	m := &sessionMap{starts: starts}
	var (
		mig     legacyMigration
		created []string // run folders this attempt made
		cur     *runWriter
		curRun  string
		prevID  MessageID
	)
	fail := func(err error) (legacyMigration, error) {
		if cur != nil {
			cur.abandon()
		}
		for _, dir := range created {
			if rerr := os.RemoveAll(dir); rerr != nil {
				err = errors.Join(err, rerr)
			}
		}
		return legacyMigration{}, err
	}

	err = scanSegments(segmentsDir, func(id MessageID, writeUnix uint32, payload []byte) error {
		if id <= prevID {
			return fmt.Errorf("message %d follows %d in the old files", id, prevID)
		}
		prevID = id
		sid := m.sid(id, writeUnix)
		if cur == nil || sid != curRun {
			if cur != nil {
				if _, err := cur.sealActive(); err != nil {
					return err
				}
			}
			if !validRunID(sid) {
				return fmt.Errorf("sessions.log names run %q, which cannot be a folder", sid)
			}
			dir := filepath.Join(segmentsDir, sid)
			if _, err := os.Stat(dir); err == nil {
				return fmt.Errorf("run folder %s already exists", sid)
			} else if !errors.Is(err, fs.ErrNotExist) {
				return err
			}
			cur = &runWriter{id: sid, dir: dir}
			curRun = sid
			created = append(created, dir)
			mig.Runs++
		}
		frame := encodeFrameAt(id, writeUnix, payload)
		if _, _, _, err := cur.appendFrame(frame, id); err != nil {
			return err
		}
		mig.Records++
		mig.Bytes += int64(len(frame))
		return nil
	})
	if err != nil {
		return fail(err)
	}
	if cur != nil {
		if _, err := cur.sealActive(); err != nil {
			return fail(err)
		}
	}

	moved, err := moveFiles(segmentsDir, legacyDir, moves)
	if err != nil {
		// Put back the files already moved, then undo the copies.
		var back error
		for _, name := range moved {
			if rerr := os.Rename(filepath.Join(legacyDir, name), filepath.Join(segmentsDir, name)); rerr != nil {
				back = errors.Join(back, rerr)
			}
		}
		return fail(errors.Join(err, back))
	}
	mig.Moved = len(moves)
	mig.Elapsed = time.Since(started)
	return mig, nil
}

// moveFiles moves the named files from segmentsDir into legacyDir and returns the
// names it moved before any failure.
func moveFiles(segmentsDir, legacyDir string, names []string) (moved []string, err error) {
	if err := os.MkdirAll(legacyDir, 0o755); err != nil {
		return nil, err
	}
	for _, name := range names {
		if err := os.Rename(filepath.Join(segmentsDir, name), filepath.Join(legacyDir, name)); err != nil {
			return moved, err
		}
		moved = append(moved, name)
	}
	return moved, nil
}

// legacyNextID returns the ID after the newest record in the old files, or 0 when
// there are none. Records are numbered upward, so the newest is the last record
// of the last file that has one. The server uses it when migration fails, so
// that new messages are never numbered over old ones.
func legacyNextID(segmentsDir string) (MessageID, error) {
	segs, _, err := legacyFiles(segmentsDir)
	if err != nil {
		return 0, err
	}
	for i := len(segs) - 1; i >= 0; i-- {
		var last MessageID
		found := false
		_, err := scanRecordFile(filepath.Join(segmentsDir, segs[i]), 0, -1, scanBufferBytes, func(_ int64, id MessageID, _ uint32, _ []byte) error {
			last, found = id, true
			return nil
		})
		if err != nil {
			return 0, err
		}
		if found {
			return last + 1, nil
		}
	}
	return 0, nil
}
