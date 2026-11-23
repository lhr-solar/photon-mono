package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"slices"
	"sort"
	"strings"
	"time"
)

// backfillOptions says which recorded sessions to replay, and how to decode them.
type backfillOptions struct {
	SegmentsDir string
	CAN         CANConfig        // DBCDir and SampleRates; Decode is implied
	Sessions    []string         // session IDs to replay; with All false, only these
	All         bool             // replay every recorded session
	SkipRecent  time.Duration    // skip a session last written within this long ago: it may be live. Zero skips none
	Now         func() time.Time // nil means time.Now
}

// backfillSession is one server run that has records in the segments.
type backfillSession struct {
	SID                   string
	FirstID, LastID       MessageID
	Records               int
	Bytes                 int64
	FirstWrite, LastWrite time.Time
}

// backfillResult is what replaying one session wrote.
type backfillResult struct {
	SID      string
	Records  int64 // records replayed
	Frames   int64 // frames decoded
	BadClock int64 // frames dropped for a bad car clock
	Rows     int64 // rows handed to the sink
}

// sessionMap says which server run wrote each record, from the recorded starts.
type sessionMap struct {
	starts   []sessionStart // sorted by first ID
	fallback string         // the run for records older than starts[0], named from the first of them
}

// sid returns the session of the record id, which was written at writeUnix.
// Records older than the first recorded start (data from before sessions.log
// existed) all go to one fallback session.
func (m *sessionMap) sid(id MessageID, writeUnix uint32) string {
	if i := sort.Search(len(m.starts), func(i int) bool { return m.starts[i].FirstID > id }); i > 0 {
		return m.starts[i-1].SID
	}
	if m.fallback == "" {
		m.fallback = sessionID(time.Unix(int64(writeUnix), 0))
	}
	return m.fallback
}

// scanSessions reads every record once and sums them up per session, in the
// order the sessions first appear.
func scanSessions(segmentsDir string) ([]sessionStart, []backfillSession, error) {
	starts, err := readSessionStarts(segmentsDir)
	if err != nil {
		return nil, nil, err
	}
	m := &sessionMap{starts: starts}
	var sessions []backfillSession
	index := make(map[string]int)
	err = scanSegments(segmentsDir, func(id MessageID, writeUnix uint32, payload []byte) error {
		sid := m.sid(id, writeUnix)
		i, ok := index[sid]
		if !ok {
			i = len(sessions)
			index[sid] = i
			sessions = append(sessions, backfillSession{SID: sid, FirstID: id, FirstWrite: writeTime(writeUnix)})
		}
		s := &sessions[i]
		s.LastID = id
		s.LastWrite = writeTime(writeUnix)
		s.Records++
		s.Bytes += int64(frameHeaderBytes + len(payload))
		return nil
	})
	return starts, sessions, err
}

// writeTime is a record's write time: the seconds the store stamped it with.
func writeTime(writeUnix uint32) time.Time {
	return time.Unix(int64(writeUnix), 0).UTC()
}

// listBackfillSessions lists the sessions that have records, in the order they
// were written. It reads every record once.
func listBackfillSessions(segmentsDir string) ([]backfillSession, error) {
	_, sessions, err := scanSessions(segmentsDir)
	return sessions, err
}

// runBackfill replays the selected sessions into sink, and reports what each
// one wrote. The segments are read twice. The first pass lists the sessions, so
// an unknown name, or a session skipped as recent, is settled before anything is
// written. The second replays the selected records in one pass, with a fresh
// decoder per session, as the live server starts one per run. The rows are the
// ones the live server wrote for the same frames, so a rerun only rewrites the
// same points. It does not close sink; the caller owns it.
func runBackfill(opts backfillOptions, sink lineSink) ([]backfillResult, error) {
	now := opts.Now
	if now == nil {
		now = time.Now
	}
	starts, sessions, err := scanSessions(opts.SegmentsDir)
	if err != nil {
		return nil, err
	}
	for _, name := range opts.Sessions {
		if !slices.ContainsFunc(sessions, func(s backfillSession) bool { return s.SID == name }) {
			return nil, fmt.Errorf("no records for session %q in %s (see -list)", name, opts.SegmentsDir)
		}
	}

	// selected maps a session ID to its index in results.
	selected := make(map[string]int)
	var results []backfillResult
	cutoff := now().Add(-opts.SkipRecent)
	for _, s := range sessions {
		named := slices.Contains(opts.Sessions, s.SID)
		if !named && !opts.All {
			continue
		}
		if !named && opts.SkipRecent > 0 && s.LastWrite.After(cutoff) {
			log.Printf("[photon-server] backfill: skipping session %s, last written %s; it may still be live (name it with -session to replay it)",
				s.SID, s.LastWrite.Format(time.RFC3339))
			continue
		}
		selected[s.SID] = len(results)
		results = append(results, backfillResult{SID: s.SID})
	}
	if len(results) == 0 {
		return nil, nil
	}

	m := &sessionMap{starts: starts}
	var (
		fs  *frameSession // the session being replayed, nil between sessions
		can *canDecoder   // its decoder
		cur int           // its index in results
	)
	finishCurrent := func() {
		if fs == nil {
			return
		}
		fs.finish()
		results[cur].Frames = int64(can.dec.Stats.Frames)
		results[cur].BadClock = int64(can.badClock)
		fs, can = nil, nil
	}
	err = scanSegments(opts.SegmentsDir, func(id MessageID, writeUnix uint32, payload []byte) error {
		i, ok := selected[m.sid(id, writeUnix)]
		if !ok {
			return nil
		}
		if i != cur || fs == nil {
			finishCurrent()
			s := newFrameSessionID(results[i].SID, rowCounter{next: sink, rows: &results[i].Rows})
			cfg := opts.CAN
			cfg.Decode = true
			c, err := newCANDecoder(cfg, s)
			if err != nil {
				return err
			}
			fs, can, cur = s, c, i
		}
		results[i].Records++
		// The live check runs from the clock at decode time, a moment after the
		// buffer was written. The write time is cut to whole seconds, so add one
		// to get back to that moment.
		can.decodeAt(payload, time.Unix(int64(writeUnix), 0).Add(time.Second))
		return nil
	})
	finishCurrent()
	return results, err
}

// rowCounter passes rows on to next, and adds their count to rows.
type rowCounter struct {
	next lineSink
	rows *int64
}

func (c rowCounter) Write(rows []byte, n int) {
	*c.rows += int64(n)
	c.next.Write(rows, n)
}

// Close does nothing: the caller of runBackfill owns the sink.
func (c rowCounter) Close(time.Duration) {}

// dryRunSink takes rows and drops them, for -dry-run.
type dryRunSink struct{}

func (dryRunSink) Write([]byte, int)   {}
func (dryRunSink) Close(time.Duration) {}

// sessionFlag collects -session values: the flag may repeat, and each value may
// be a comma list.
type sessionFlag []string

func (s *sessionFlag) String() string {
	if s == nil {
		return ""
	}
	return strings.Join(*s, ",")
}

func (s *sessionFlag) Set(v string) error {
	for _, name := range strings.Split(v, ",") {
		if name = strings.TrimSpace(name); name != "" {
			*s = append(*s, name)
		}
	}
	return nil
}

// backfillMain runs "photon-server backfill" with the arguments that follow it,
// and returns the exit code: 0 when done, 1 on an error or when rows were
// dropped, 2 on a usage error. Backfill only reads the data directory. Rows go
// to InfluxDB, which keeps one point per series and time, so rerunning a
// backfill rewrites the same points.
func backfillMain(args []string, getenv func(string) string, out io.Writer) int {
	// The defaults are the server's own, from the same environment variables.
	defaults, err := ParseConfigWithEnv(getenv, nil, io.Discard)
	if err != nil {
		fmt.Fprintf(os.Stderr, "backfill: %v\n", err)
		return 2
	}

	fs := flag.NewFlagSet("photon-server backfill", flag.ContinueOnError)
	fs.SetOutput(os.Stderr)
	dataDir := fs.String("data-dir", defaults.DataDir, "storage directory the server writes (env PHOTON_DATA_DIR)")
	dbcDir := fs.String("dbc-dir", defaults.CAN.DBCDir, "folder of DBC files that decode the frames")
	influxURL := fs.String("influx-url", defaults.InfluxURL, "InfluxDB 3 URL to write to, or off (env PHOTON_INFLUX_URL)")
	influxDB := fs.String("influx-db", defaults.InfluxDB, "InfluxDB database (env PHOTON_INFLUX_DB)")
	influxToken := fs.String("influx-token", "", "InfluxDB token (default: env PHOTON_INFLUX_TOKEN or INFLUXDB3_AUTH_TOKEN)")
	var sessions sessionFlag
	fs.Var(&sessions, "session", "session ID to replay; repeat the flag or give a comma list")
	all := fs.Bool("all", false, "replay every recorded session")
	list := fs.Bool("list", false, "list the recorded sessions and exit")
	skipRecent := fs.Duration("skip-recent", 2*time.Minute, "skip a session last written this recently, since it may still be live")
	dryRun := fs.Bool("dry-run", false, "decode and count the rows, without writing to InfluxDB")
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}
	if strings.TrimSpace(*dataDir) == "" {
		fmt.Fprintln(os.Stderr, "backfill: the data directory is empty")
		return 2
	}
	if *influxToken == "" {
		*influxToken = defaults.InfluxToken
	}
	segmentsDir := (&Config{DataDir: *dataDir}).SegmentsDir()

	if *list {
		recorded, err := listBackfillSessions(segmentsDir)
		if err != nil {
			fmt.Fprintf(os.Stderr, "backfill: %v\n", err)
			return 1
		}
		if len(recorded) == 0 {
			fmt.Fprintf(out, "no recorded sessions in %s\n", segmentsDir)
		}
		for _, s := range recorded {
			fmt.Fprintf(out, "%s  records %d  %.1f MiB  first %s  last %s  ids %d-%d\n",
				s.SID, s.Records, float64(s.Bytes)/(1<<20), s.FirstWrite.Format(time.RFC3339),
				s.LastWrite.Format(time.RFC3339), s.FirstID, s.LastID)
		}
		return 0
	}

	if len(sessions) == 0 && !*all {
		fmt.Fprintln(os.Stderr, "backfill: name a session with -session, or pass -all; -list shows the recorded ones")
		return 2
	}
	if !*dryRun && strings.EqualFold(*influxURL, "off") {
		fmt.Fprintln(os.Stderr, "backfill: -influx-url is off, so there is nowhere to write the rows (use -dry-run to only count them)")
		return 2
	}

	opts := backfillOptions{
		SegmentsDir: segmentsDir,
		CAN:         defaults.CAN,
		Sessions:    sessions,
		All:         *all,
		SkipRecent:  *skipRecent,
	}
	opts.CAN.Decode = true
	opts.CAN.DBCDir = *dbcDir

	if *dryRun {
		results, err := runBackfill(opts, dryRunSink{})
		printBackfillResults(out, results)
		if err != nil {
			fmt.Fprintf(os.Stderr, "backfill: %v\n", err)
			return 1
		}
		fmt.Fprintln(out, "dry run: nothing was written")
		return 0
	}

	// Block: a backfill waits for InfluxDB rather than dropping rows.
	w, err := newDBWriter(*influxURL, *influxDB, *influxToken, dbWriterOptions{Flush: DefaultInfluxFlush, Block: true})
	if err != nil {
		fmt.Fprintf(os.Stderr, "backfill: %v\n", err)
		return 2
	}
	started := time.Now()
	results, err := runBackfill(opts, w)
	w.Close(5 * time.Minute)
	printBackfillResults(out, results)
	fmt.Fprintf(out, "rows written %d, dropped %d, elapsed %s\n",
		w.Written(), w.Dropped(), time.Since(started).Round(time.Millisecond))

	// done closes once the sender has delivered its last buffer; Close returns
	// before that only when its wait ran out.
	finished := true
	select {
	case <-w.done:
	default:
		finished = false
	}
	switch {
	case err != nil:
		fmt.Fprintf(os.Stderr, "backfill: %v\n", err)
		return 1
	case !finished:
		fmt.Fprintln(os.Stderr, "backfill: InfluxDB did not take every row within 5 minutes; rerun the same command, it is safe")
		return 1
	case w.Dropped() > 0:
		fmt.Fprintf(os.Stderr, "backfill: %d rows were refused or failed after retries; rerun the same command, it is safe\n", w.Dropped())
		return 1
	}
	return 0
}

// printBackfillResults prints one line per session, then the totals.
func printBackfillResults(out io.Writer, results []backfillResult) {
	var total backfillResult
	for _, r := range results {
		fmt.Fprintf(out, "session %s: %d records, %d frames, %d bad clock, %d rows\n",
			r.SID, r.Records, r.Frames, r.BadClock, r.Rows)
		total.Records += r.Records
		total.Frames += r.Frames
		total.BadClock += r.BadClock
		total.Rows += r.Rows
	}
	fmt.Fprintf(out, "total: %d sessions, %d records, %d frames, %d bad clock, %d rows\n",
		len(results), total.Records, total.Frames, total.BadClock, total.Rows)
}
