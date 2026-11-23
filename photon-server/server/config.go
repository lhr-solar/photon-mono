package main

import (
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"server/dbc"
)

const (
	DefaultHost             = "0.0.0.0"
	DefaultIngressPort      = 6100
	DefaultClientPort       = 6500
	DefaultObserverPort     = 5400
	DefaultVideoIngressPort = 6200
	DefaultVideoClientPort  = 6700
	DefaultVideoUdpPort     = 6800
	DefaultV2Port           = 6900
	DefaultV2CFPort         = 6901
	DefaultDataDir          = "./data"
	DefaultInfluxURL        = "http://127.0.0.1:8181"
	DefaultInfluxDB         = "photon_dev"
	// DefaultInfluxQueries is how many history queries run at once across all
	// clients. 4 to 8 keeps InfluxDB busy without piling up work.
	DefaultInfluxQueries = 6
	// DefaultInfluxFlush is how long the DB writer holds rows before sending them.
	// Together with InfluxDB's own --wal-flush-interval (run it at 250ms too),
	// rows are queryable within about 500 ms of arriving. At 1 MiB/s of CAN a
	// 250 ms batch is still large enough to post efficiently.
	DefaultInfluxFlush = 250 * time.Millisecond

	// Ingest pool inputs; batch_pool.go derives the pool from them.
	// DefaultIngestRate is the upper bound from dbc/spec/topology.yaml: 8 buses,
	// 4.5 Mbit/s total; an 8-byte classic frame is ~111 bits, so ~40k frames/s
	// at 100% bus load, x 29 B per CANP v3 record.
	DefaultIngestRate = 40_000 * canpFrameSize
	// DefaultIngestLatency is half the car's own 20 ms batching (FLUSH_INTERVAL_US).
	DefaultIngestLatency = 10 * time.Millisecond
	// DefaultIngestStall is a choice: how long the decoder may stall (disk
	// append, GC) before readers stop and TCP pushes back on the car.
	DefaultIngestStall = 10 * time.Second
	// DefaultLiveSeconds is how far back the live ring reaches. It covers
	// InfluxDB's ~0.5 s write delay, so a client that joins live loses no frames
	// to that delay, plus slack for a client that reads slowly. A choice: a
	// longer window costs memory, a shorter one gives gaps sooner.
	DefaultLiveSeconds = 5 * time.Second
)

// DefaultStoreMaxGB is how many GiB may be on disk under segments/ before appends
// wait. A choice: the old fixed limit was 8 GiB, and 64 leaves room for a longer
// recording.
const DefaultStoreMaxGB = 64

// DefaultArchive is whether sealed chunks are compressed in the background.
const DefaultArchive = true

// DefaultRclone is the rclone program the archiver runs.
const DefaultRclone = "rclone"

// Config holds runtime configuration for the photon server.
type Config struct {
	Host             string
	IngressPort      int
	ClientPort       int
	ObserverPort     int
	VideoIngressPort int
	VideoClientPort  int
	VideoUdpPort     int
	V2Port           int    // protocol v2 WebSocket port on the LAN; 0 = off
	V2CFPort         int    // protocol v2 WebSocket port behind Cloudflare Access, 127.0.0.1 only; 0 = off
	CFTeam           string // Cloudflare Access team name
	CFAud            string // Cloudflare Access application AUD tag
	WebDir           string // static web bundle served at /; empty = none
	DataDir          string
	StoreMaxGB       int // GiB on disk under segments/ before appends wait, at least 1
	TsdbDir          string
	DbcPath          string
	LiveWriter       string
	Ui               string
	Password         string
	InfluxURL        string // InfluxDB 3 base URL for decoded rows, or "off"
	InfluxDB         string
	InfluxToken      string        // env only, so it stays out of process listings
	InfluxFlush      time.Duration // send buffered rows this long after the first; 0 sends each Write at once
	InfluxNoSync     bool          // ask InfluxDB to acknowledge before its WAL is saved (no_sync=true)
	InfluxQueries    int           // history queries to InfluxDB in flight at once, 1 to 64
	CAN              CANConfig
	Ingest           poolSizing    // sizes the ingest buffer pool
	LiveSeconds      time.Duration // how far back the live ring for protocol v2 clients reaches
	Archive          bool          // compress sealed chunks to .segz files in the background
	ArchiveRemote    string        // rclone destination for the .segz files, e.g. sharepoint:photon/segments; empty = no upload
	Rclone           string        // the rclone program used for uploads
}

// CANConfig is how the server decodes the car's CAN frames. It is set here, not
// by flags; Photon web will edit it later.
type CANConfig struct {
	Decode      bool             // decode ingested frames with the DBCs
	DBCDir      string           // folder of *.dbc files, one per bus, named after the bus
	SampleRates []dbc.SampleRate // how fast the sensors behind multi-sample messages sample
}

// DefaultCANConfig is the car's CAN setup: the spec copied from the firmware
// repo, and the sample rates of its DAqCAN sensors.
func DefaultCANConfig() CANConfig {
	return CANConfig{
		Decode: false,
		// HighNoon 2026, the car running now. HighNoon 2027: dbc/spec/dbc or dbc/spec-cdr/dbc.
		DBCDir: "dbc/highnoon2026",
		SampleRates: []dbc.SampleRate{
			{Messages: `Chassis(Acceleration|Gyro)Data`, Hz: 208},
			{Messages: `Upright\w+(Acceleration|Gyro)Data`, Hz: 416},
			{Messages: `Upright\w+WheelSpeedData`, Hz: 100},
			{Messages: `Suspension\w+Data`, Hz: 500},
		},
	}
}

// IngressAddr returns the TCP listen address for the ingress service.
func (c *Config) IngressAddr() string {
	return net.JoinHostPort(c.Host, strconv.Itoa(c.IngressPort))
}

// ClientAddr returns the TCP listen address for the client fan-out service.
func (c *Config) ClientAddr() string {
	return net.JoinHostPort(c.Host, strconv.Itoa(c.ClientPort))
}

// ObserverAddr returns the TCP listen address for the observer status service.
func (c *Config) ObserverAddr() string {
	return net.JoinHostPort(c.Host, strconv.Itoa(c.ObserverPort))
}

// VideoIngressAddr returns the UDP+TCP listen address for RTP/H.264 ingress.
func (c *Config) VideoIngressAddr() string {
	return net.JoinHostPort(c.Host, strconv.Itoa(c.VideoIngressPort))
}

// VideoClientAddr returns the TCP listen address for video live/replay clients.
func (c *Config) VideoClientAddr() string {
	return net.JoinHostPort(c.Host, strconv.Itoa(c.VideoClientPort))
}

// VideoUdpAddr returns the UDP listen address for live video fan-out.
func (c *Config) VideoUdpAddr() string {
	return net.JoinHostPort(c.Host, strconv.Itoa(c.VideoUdpPort))
}

// VideoRTCPAddr returns the UDP listen address for RTCP sender reports.
//
// RTCP conventionally sits on the RTP port plus one (RFC 3550 §11), so it is
// derived rather than configured: a separate flag would let the two drift apart
// and every sender already assumes the convention.
func (c *Config) VideoRTCPAddr() string {
	return net.JoinHostPort(c.Host, strconv.Itoa(c.VideoIngressPort+1))
}

// SegmentsDir returns the path where log segments are stored.
func (c *Config) SegmentsDir() string {
	return filepath.Join(c.DataDir, "segments")
}

// VideoSegmentsDir returns the path where compressed video segments are stored.
func (c *Config) VideoSegmentsDir() string {
	return filepath.Join(c.DataDir, "video", "segments")
}

// TsdbRoot is the telemetry store. Empty TsdbDir means <data-dir>/tsdb.
func (c *Config) TsdbRoot() string {
	if strings.TrimSpace(c.TsdbDir) != "" {
		return c.TsdbDir
	}
	return filepath.Join(c.DataDir, "tsdb")
}

// VideoVptPath is the video pointer table photon-server already writes.
func (c *Config) VideoVptPath() string {
	return filepath.Join(c.DataDir, "video", "video.vpt")
}

// LiveFanoutAddr is where the TSDB writer connects. Always loopback: the writer
// runs on this PC next to photon-server, even when the listen host is 0.0.0.0.
func (c *Config) LiveFanoutAddr() string {
	return net.JoinHostPort("127.0.0.1", strconv.Itoa(c.ClientPort))
}

// Validate checks that the configuration values are valid.
func (c *Config) Validate() error {
	ports := []struct {
		name string
		port int
	}{
		{"ingress", c.IngressPort},
		{"client", c.ClientPort},
		{"observer", c.ObserverPort},
		{"video-ingress", c.VideoIngressPort},
		{"video-client", c.VideoClientPort},
	}
	if c.VideoUdpPort != 0 {
		ports = append(ports, struct {
			name string
			port int
		}{"video-udp", c.VideoUdpPort})
	}
	if c.V2Port != 0 {
		ports = append(ports, struct {
			name string
			port int
		}{"v2", c.V2Port})
	}
	if c.V2CFPort != 0 {
		ports = append(ports, struct {
			name string
			port int
		}{"v2-cf", c.V2CFPort})
	}
	for _, p := range ports {
		if p.port < 1 || p.port > 65535 {
			return fmt.Errorf("invalid %s port: %d (must be between 1 and 65535)", p.name, p.port)
		}
	}
	for i := 0; i < len(ports); i++ {
		for j := i + 1; j < len(ports); j++ {
			if ports[i].port == ports[j].port {
				return fmt.Errorf("%s port (%d) and %s port (%d) must not be identical",
					ports[i].name, ports[i].port, ports[j].name, ports[j].port)
			}
		}
	}
	if strings.TrimSpace(c.DataDir) == "" {
		return fmt.Errorf("data directory cannot be empty")
	}
	if c.StoreMaxGB < 1 {
		return fmt.Errorf("invalid store max: %d GiB (must be at least 1)", c.StoreMaxGB)
	}
	if c.InfluxFlush < 0 {
		return fmt.Errorf("invalid influx flush interval: %s (must not be negative)", c.InfluxFlush)
	}
	if c.InfluxQueries < 1 || c.InfluxQueries > 64 {
		return fmt.Errorf("invalid influx queries: %d (must be between 1 and 64)", c.InfluxQueries)
	}
	if c.Ingest.Rate <= 0 || c.Ingest.Latency <= 0 || c.Ingest.Stall < c.Ingest.Latency {
		return fmt.Errorf("invalid ingest pool: rate %d B/s, latency %s, stall %s (need rate and latency above zero, stall at least latency)",
			c.Ingest.Rate, c.Ingest.Latency, c.Ingest.Stall)
	}
	if c.LiveSeconds <= 0 {
		return fmt.Errorf("invalid live window: %s (must be above zero)", c.LiveSeconds)
	}
	if c.CAN.Decode && strings.TrimSpace(c.CAN.DBCDir) == "" {
		return fmt.Errorf("CAN decoding is on but no DBC directory is set")
	}
	return nil
}

// ParseConfig builds a Config from CLI arguments and environment variables.
func ParseConfig(args []string) (*Config, error) {
	return ParseConfigWithEnv(os.Getenv, args, os.Stderr)
}

// ParseConfigWithEnv parses configuration with custom environment lookup and output writer.
func ParseConfigWithEnv(getenv func(string) string, args []string, output io.Writer) (*Config, error) {
	cfg := &Config{
		Host:             DefaultHost,
		IngressPort:      DefaultIngressPort,
		ClientPort:       DefaultClientPort,
		ObserverPort:     DefaultObserverPort,
		VideoIngressPort: DefaultVideoIngressPort,
		VideoClientPort:  DefaultVideoClientPort,
		VideoUdpPort:     DefaultVideoUdpPort,
		V2Port:           DefaultV2Port,
		V2CFPort:         DefaultV2CFPort,
		DataDir:          DefaultDataDir,
		StoreMaxGB:       DefaultStoreMaxGB,
		InfluxURL:        DefaultInfluxURL,
		InfluxDB:         DefaultInfluxDB,
		InfluxFlush:      DefaultInfluxFlush,
		InfluxQueries:    DefaultInfluxQueries,
		CAN:              DefaultCANConfig(),
		Ingest:           poolSizing{Rate: DefaultIngestRate, Latency: DefaultIngestLatency, Stall: DefaultIngestStall},
		LiveSeconds:      DefaultLiveSeconds,
		Archive:          DefaultArchive,
		Rclone:           DefaultRclone,
	}

	// Environment variable fallbacks
	if host := getenvFirst(getenv, "PHOTON_HOST", "PHOTON_BIND", "PHOTON_BIND_ADDR"); host != "" {
		cfg.Host = host
	}
	if portStr := getenvFirst(getenv, "PHOTON_INGRESS_PORT"); portStr != "" {
		if p, err := strconv.Atoi(portStr); err == nil {
			cfg.IngressPort = p
		}
	}
	if portStr := getenvFirst(getenv, "PHOTON_CLIENT_PORT"); portStr != "" {
		if p, err := strconv.Atoi(portStr); err == nil {
			cfg.ClientPort = p
		}
	}
	if portStr := getenvFirst(getenv, "PHOTON_OBSERVER_PORT"); portStr != "" {
		if p, err := strconv.Atoi(portStr); err == nil {
			cfg.ObserverPort = p
		}
	}
	if dir := getenvFirst(getenv, "PHOTON_STORAGE_PATH", "PHOTON_DATA_DIR"); dir != "" {
		cfg.DataDir = dir
	}
	if v := getenvFirst(getenv, "PHOTON_STORE_MAX_GB"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil {
			return nil, fmt.Errorf("PHOTON_STORE_MAX_GB: %w", err)
		}
		cfg.StoreMaxGB = n
	}
	if portStr := getenvFirst(getenv, "PHOTON_VIDEO_INGRESS_PORT"); portStr != "" {
		if p, err := strconv.Atoi(portStr); err == nil {
			cfg.VideoIngressPort = p
		}
	}
	if portStr := getenvFirst(getenv, "PHOTON_VIDEO_CLIENT_PORT"); portStr != "" {
		if p, err := strconv.Atoi(portStr); err == nil {
			cfg.VideoClientPort = p
		}
	}
	if portStr := getenvFirst(getenv, "PHOTON_VIDEO_UDP_PORT"); portStr != "" {
		if p, err := strconv.Atoi(portStr); err == nil {
			cfg.VideoUdpPort = p
		}
	}
	if portStr := getenvFirst(getenv, "PHOTON_V2_PORT"); portStr != "" {
		if p, err := strconv.Atoi(portStr); err == nil {
			cfg.V2Port = p
		}
	}
	if portStr := getenvFirst(getenv, "PHOTON_V2_CF_PORT"); portStr != "" {
		if p, err := strconv.Atoi(portStr); err == nil {
			cfg.V2CFPort = p
		}
	}
	if v := getenvFirst(getenv, "PHOTON_CF_TEAM"); v != "" {
		cfg.CFTeam = v
	}
	if v := getenvFirst(getenv, "PHOTON_CF_AUD"); v != "" {
		cfg.CFAud = v
	}
	if v := getenvFirst(getenv, "PHOTON_WEB_DIR"); v != "" {
		cfg.WebDir = v
	}
	if dir := getenvFirst(getenv, "PHOTON_TSDB_DIR", "PHOTON_TSDB"); dir != "" {
		cfg.TsdbDir = dir
	}
	if path := getenvFirst(getenv, "PHOTON_DBC", "PHOTON_DBC_PATH"); path != "" {
		cfg.DbcPath = path
	}
	if dir := getenvFirst(getenv, "PHOTON_DBC_DIR"); dir != "" {
		cfg.CAN.DBCDir = dir
	}
	if path := getenvFirst(getenv, "PHOTON_LIVE_WRITER"); path != "" {
		cfg.LiveWriter = path
	}
	if path := getenvFirst(getenv, "PHOTON_UI"); path != "" {
		cfg.Ui = path
	}
	if secret := getenvFirst(getenv, "PHOTON_PASSWORD"); secret != "" {
		cfg.Password = secret
	}
	if u := getenvFirst(getenv, "PHOTON_INFLUX_URL"); u != "" {
		cfg.InfluxURL = u
	}
	if db := getenvFirst(getenv, "PHOTON_INFLUX_DB"); db != "" {
		cfg.InfluxDB = db
	}
	if v := getenvFirst(getenv, "PHOTON_INFLUX_FLUSH"); v != "" {
		if d, err := time.ParseDuration(v); err == nil {
			cfg.InfluxFlush = d
		}
	}
	if v := getenvFirst(getenv, "PHOTON_INFLUX_NO_SYNC"); v != "" {
		if b, err := strconv.ParseBool(v); err == nil {
			cfg.InfluxNoSync = b
		}
	}
	if v := getenvFirst(getenv, "PHOTON_INFLUX_QUERIES"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			cfg.InfluxQueries = n
		}
	}
	cfg.InfluxToken = getenvFirst(getenv, "PHOTON_INFLUX_TOKEN", "INFLUXDB3_AUTH_TOKEN")
	if v := getenvFirst(getenv, "PHOTON_INGEST_RATE"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			cfg.Ingest.Rate = n
		}
	}
	if v := getenvFirst(getenv, "PHOTON_INGEST_LATENCY"); v != "" {
		if d, err := time.ParseDuration(v); err == nil {
			cfg.Ingest.Latency = d
		}
	}
	if v := getenvFirst(getenv, "PHOTON_INGEST_STALL"); v != "" {
		if d, err := time.ParseDuration(v); err == nil {
			cfg.Ingest.Stall = d
		}
	}
	if v := getenvFirst(getenv, "PHOTON_LIVE_SECONDS"); v != "" {
		if d, err := time.ParseDuration(v); err == nil {
			cfg.LiveSeconds = d
		}
	}
	if v := getenvFirst(getenv, "PHOTON_ARCHIVE"); v != "" {
		b, err := strconv.ParseBool(v)
		if err != nil {
			return nil, fmt.Errorf("PHOTON_ARCHIVE: %w", err)
		}
		cfg.Archive = b
	}
	if v := getenvFirst(getenv, "PHOTON_ARCHIVE_REMOTE"); v != "" {
		cfg.ArchiveRemote = v
	}
	if v := getenvFirst(getenv, "PHOTON_RCLONE"); v != "" {
		cfg.Rclone = v
	}

	fs := flag.NewFlagSet("photon-server", flag.ContinueOnError)
	fs.SetOutput(output)

	var hostFlag string
	var ingressPortFlag int
	var clientPortFlag int
	var observerPortFlag int
	var videoIngressPortFlag int
	var videoClientPortFlag int
	var videoUdpPortFlag int
	var v2PortFlag, v2CFPortFlag int
	var cfTeamFlag, cfAudFlag, webDirFlag string
	var dataDirFlag string
	var storeMaxGBFlag int
	var tsdbDirFlag string
	var dbcPathFlag string
	var dbcDirFlag string
	var liveWriterFlag string
	var uiFlag string
	var passwordFlag string
	var influxURLFlag string
	var influxDBFlag string
	var influxFlushFlag time.Duration
	var influxNoSyncFlag bool
	var influxQueriesFlag int
	var ingestRateFlag int
	var ingestLatencyFlag, ingestStallFlag time.Duration
	var liveSecondsFlag time.Duration
	var archiveFlag bool
	var archiveRemoteFlag, rcloneFlag string

	fs.StringVar(&hostFlag, "host", cfg.Host, "Bind host/IP address (e.g. 0.0.0.0 for all interfaces, 127.0.0.1 for local only)")
	fs.StringVar(&hostFlag, "bind", cfg.Host, "Alias for -host")
	fs.IntVar(&ingressPortFlag, "ingress-port", cfg.IngressPort, "TCP port for CANP ingress stream")
	fs.IntVar(&ingressPortFlag, "ingress", cfg.IngressPort, "Alias for -ingress-port")
	fs.IntVar(&clientPortFlag, "client-port", cfg.ClientPort, "TCP port for client fan-out stream")
	fs.IntVar(&clientPortFlag, "client", cfg.ClientPort, "Alias for -client-port")
	fs.IntVar(&observerPortFlag, "observer-port", cfg.ObserverPort, "TCP port for observer JSON telemetry")
	fs.IntVar(&observerPortFlag, "observer", cfg.ObserverPort, "Alias for -observer-port")
	fs.IntVar(&videoIngressPortFlag, "video-ingress-port", cfg.VideoIngressPort, "UDP+TCP port for RTP/H.264 video ingress")
	fs.IntVar(&videoIngressPortFlag, "video-ingress", cfg.VideoIngressPort, "Alias for -video-ingress-port")
	fs.IntVar(&videoClientPortFlag, "video-client-port", cfg.VideoClientPort, "TCP port for video live/replay clients")
	fs.IntVar(&videoClientPortFlag, "video-client", cfg.VideoClientPort, "Alias for -video-client-port")
	fs.IntVar(&videoUdpPortFlag, "video-udp-port", cfg.VideoUdpPort, "UDP port for live video fan-out")
	fs.IntVar(&videoUdpPortFlag, "video-udp", cfg.VideoUdpPort, "Alias for -video-udp-port")
	fs.IntVar(&v2PortFlag, "v2-port", cfg.V2Port, "WebSocket port for protocol v2 on the LAN; 0 = off")
	fs.IntVar(&v2CFPortFlag, "v2-cf-port", cfg.V2CFPort, "WebSocket port for protocol v2 behind Cloudflare Access (127.0.0.1); 0 = off")
	fs.StringVar(&cfTeamFlag, "cf-team", cfg.CFTeam, "Cloudflare Access team name (issuer https://<team>.cloudflareaccess.com)")
	fs.StringVar(&cfAudFlag, "cf-aud", cfg.CFAud, "Cloudflare Access application AUD tag")
	fs.StringVar(&webDirFlag, "web-dir", cfg.WebDir, "static web bundle served at / (empty = none)")
	fs.StringVar(&dataDirFlag, "data-dir", cfg.DataDir, "Directory path for persistent storage segments")
	fs.StringVar(&dataDirFlag, "storage-path", cfg.DataDir, "Alias for -data-dir")
	fs.IntVar(&storeMaxGBFlag, "store-max-gb", cfg.StoreMaxGB, "GiB on disk under segments/ before appends wait, at least 1 (env PHOTON_STORE_MAX_GB)")
	fs.StringVar(&tsdbDirFlag, "tsdb-dir", cfg.TsdbDir, "TSDB root (default: <data-dir>/tsdb)")
	fs.StringVar(&dbcPathFlag, "dbc", cfg.DbcPath, "DBC file for the live TSDB writer")
	fs.StringVar(&dbcDirFlag, "dbc-dir", cfg.CAN.DBCDir, "folder of *.dbc files the CAN decoder uses, one per bus: dbc/highnoon2026 (HighNoon 2026), dbc/spec/dbc or dbc/spec-cdr/dbc (HighNoon 2027)")
	fs.StringVar(&liveWriterFlag, "live-writer", cfg.LiveWriter, "photon-index binary that writes the TSDB, or 'off'")
	fs.StringVar(&uiFlag, "ui", cfg.Ui, "local UI backend (photon-backend), or off")
	fs.StringVar(&passwordFlag, "password", cfg.Password, "shared client password (env: PHOTON_PASSWORD)")
	fs.StringVar(&influxURLFlag, "influx-url", cfg.InfluxURL, "InfluxDB 3 URL for decoded rows, or 'off'")
	fs.StringVar(&influxDBFlag, "influx-db", cfg.InfluxDB, "InfluxDB database for decoded rows")
	fs.DurationVar(&influxFlushFlag, "influx-flush", cfg.InfluxFlush, "send buffered rows to InfluxDB this long after the first one (0 = each batch at once)")
	fs.BoolVar(&influxNoSyncFlag, "influx-no-sync", cfg.InfluxNoSync, "InfluxDB acknowledges before its WAL is saved (faster ack, may lose the last WAL interval on crash, hides some write errors)")
	fs.IntVar(&influxQueriesFlag, "influx-queries", cfg.InfluxQueries, "history queries to InfluxDB in flight at once, 1 to 64 (env PHOTON_INFLUX_QUERIES)")
	fs.IntVar(&ingestRateFlag, "ingest-rate", cfg.Ingest.Rate, "peak CAN ingest in bytes/s; sizes the ingest pool")
	fs.DurationVar(&ingestLatencyFlag, "ingest-latency", cfg.Ingest.Latency, "hand a buffer to the decoder at most this long after its first batch")
	fs.DurationVar(&ingestStallFlag, "ingest-stall", cfg.Ingest.Stall, "decoder stall the ingest pool absorbs before TCP pushes back")
	fs.DurationVar(&liveSecondsFlag, "live-seconds", cfg.LiveSeconds, "how far back the live ring for protocol v2 clients reaches; a client that falls further behind gets a gap")
	fs.BoolVar(&archiveFlag, "archive", cfg.Archive, "compress sealed chunks to .segz files in the background (env PHOTON_ARCHIVE)")
	fs.StringVar(&archiveRemoteFlag, "archive-remote", cfg.ArchiveRemote, "rclone destination for the compressed chunks, e.g. sharepoint:photon/segments; empty = no upload (env PHOTON_ARCHIVE_REMOTE)")
	fs.StringVar(&rcloneFlag, "rclone", cfg.Rclone, "rclone program used to upload compressed chunks (env PHOTON_RCLONE)")

	fs.Usage = func() {
		fmt.Fprintf(output, "Usage of photon-server:\n")
		fmt.Fprintf(output, "  photon-server [flags]\n\n")
		fmt.Fprintf(output, "Flags:\n")
		fmt.Fprintf(output, "  -host, -bind string          Bind host/IP address (default %q, env: PHOTON_HOST)\n", DefaultHost)
		fmt.Fprintf(output, "  -ingress-port, -ingress int  TCP ingress port (default %d, env: PHOTON_INGRESS_PORT)\n", DefaultIngressPort)
		fmt.Fprintf(output, "  -client-port, -client int    TCP client fan-out port (default %d, env: PHOTON_CLIENT_PORT)\n", DefaultClientPort)
		fmt.Fprintf(output, "  -observer-port, -observer int TCP observer port (default %d, env: PHOTON_OBSERVER_PORT)\n", DefaultObserverPort)
		fmt.Fprintf(output, "  -video-ingress-port, -video-ingress int UDP+TCP RTP/H.264 ingress port (default %d, env: PHOTON_VIDEO_INGRESS_PORT)\n", DefaultVideoIngressPort)
		fmt.Fprintf(output, "  -video-client-port, -video-client int TCP video live/replay port (default %d, env: PHOTON_VIDEO_CLIENT_PORT)\n", DefaultVideoClientPort)
		fmt.Fprintf(output, "  -video-udp-port, -video-udp int UDP live video fan-out port (default %d, env: PHOTON_VIDEO_UDP_PORT)\n", DefaultVideoUdpPort)
		fmt.Fprintf(output, "  -v2-port int                 WebSocket port for protocol v2 on the LAN (default %d, env: PHOTON_V2_PORT; 0 = off)\n", DefaultV2Port)
		fmt.Fprintf(output, "  -v2-cf-port int              WebSocket port behind Cloudflare Access, 127.0.0.1 (default %d, env: PHOTON_V2_CF_PORT; 0 = off)\n", DefaultV2CFPort)
		fmt.Fprintf(output, "  -cf-team string              Cloudflare Access team name (env: PHOTON_CF_TEAM)\n")
		fmt.Fprintf(output, "  -cf-aud string               Cloudflare Access application AUD tag (env: PHOTON_CF_AUD)\n")
		fmt.Fprintf(output, "  -web-dir string              static web bundle served at / (env: PHOTON_WEB_DIR; empty = none)\n")
		fmt.Fprintf(output, "  -data-dir, -storage-path string Storage directory (default %q, env: PHOTON_DATA_DIR)\n", DefaultDataDir)
		fmt.Fprintf(output, "  -store-max-gb int            GiB on disk under segments/ before appends wait (default %d, env: PHOTON_STORE_MAX_GB)\n", DefaultStoreMaxGB)
		fmt.Fprintf(output, "  -tsdb-dir string             TSDB root (default <data-dir>/tsdb, env: PHOTON_TSDB_DIR)\n")
		fmt.Fprintf(output, "  -dbc string                  DBC for the live TSDB writer (env: PHOTON_DBC)\n")
		fmt.Fprintf(output, "  -dbc-dir string              DBC folder for the CAN decoder (default %q, env: PHOTON_DBC_DIR;\n", DefaultCANConfig().DBCDir)
		fmt.Fprintf(output, "                               HighNoon 2027: dbc/spec/dbc, or dbc/spec-cdr/dbc for the newer CAN/cdr spec)\n")
		fmt.Fprintf(output, "  -live-writer string          photon-index path, or off (env: PHOTON_LIVE_WRITER)\n")
		fmt.Fprintf(output, "  -ui string                   local photon-backend + page, or off (env: PHOTON_UI)\n")
		fmt.Fprintf(output, "  -password string             client password (env: PHOTON_PASSWORD; random if unset)\n")
		fmt.Fprintf(output, "  -influx-url string           InfluxDB 3 URL for decoded rows, or off (default %q, env: PHOTON_INFLUX_URL)\n", DefaultInfluxURL)
		fmt.Fprintf(output, "  -influx-db string            InfluxDB database (default %q, env: PHOTON_INFLUX_DB)\n", DefaultInfluxDB)
		fmt.Fprintf(output, "  -influx-queries int          history queries in flight at once, 1 to 64 (default %d, env: PHOTON_INFLUX_QUERIES)\n", DefaultInfluxQueries)
		fmt.Fprintf(output, "                               token: env PHOTON_INFLUX_TOKEN or INFLUXDB3_AUTH_TOKEN\n")
		fmt.Fprintf(output, "  -ingest-rate int             peak CAN bytes/s for pool sizing (default %d, env: PHOTON_INGEST_RATE)\n", DefaultIngestRate)
		fmt.Fprintf(output, "  -ingest-latency duration     max wait before a buffer goes to the decoder (default %s, env: PHOTON_INGEST_LATENCY)\n", DefaultIngestLatency)
		fmt.Fprintf(output, "  -ingest-stall duration       decoder stall the pool absorbs (default %s, env: PHOTON_INGEST_STALL)\n", DefaultIngestStall)
		fmt.Fprintf(output, "  -live-seconds duration       live ring window for protocol v2 clients (default %s, env: PHOTON_LIVE_SECONDS)\n", DefaultLiveSeconds)
		fmt.Fprintf(output, "  -archive                     compress sealed chunks to .segz in the background (default %t, env: PHOTON_ARCHIVE)\n", true)
		fmt.Fprintf(output, "  -archive-remote string       rclone destination for compressed chunks, empty = no upload (env: PHOTON_ARCHIVE_REMOTE)\n")
		fmt.Fprintf(output, "  -rclone string               rclone program for uploads (default %q, env: PHOTON_RCLONE)\n", DefaultRclone)
		fmt.Fprintf(output, "\nPHOTON_NO_BROWSER=1 starts the local UI without opening a browser.\n")
	}

	if err := fs.Parse(args); err != nil {
		return nil, err
	}

	cfg.Host = hostFlag
	cfg.IngressPort = ingressPortFlag
	cfg.ClientPort = clientPortFlag
	cfg.ObserverPort = observerPortFlag
	cfg.VideoIngressPort = videoIngressPortFlag
	cfg.VideoClientPort = videoClientPortFlag
	cfg.VideoUdpPort = videoUdpPortFlag
	cfg.V2Port = v2PortFlag
	cfg.V2CFPort = v2CFPortFlag
	cfg.CFTeam = cfTeamFlag
	cfg.CFAud = cfAudFlag
	cfg.WebDir = webDirFlag
	cfg.DataDir = dataDirFlag
	cfg.StoreMaxGB = storeMaxGBFlag
	cfg.TsdbDir = tsdbDirFlag
	cfg.DbcPath = dbcPathFlag
	cfg.CAN.DBCDir = dbcDirFlag
	cfg.LiveWriter = liveWriterFlag
	cfg.Ui = uiFlag
	cfg.Password = passwordFlag
	cfg.InfluxURL = influxURLFlag
	cfg.InfluxDB = influxDBFlag
	cfg.InfluxFlush = influxFlushFlag
	cfg.InfluxNoSync = influxNoSyncFlag
	cfg.InfluxQueries = influxQueriesFlag
	cfg.Ingest = poolSizing{Rate: ingestRateFlag, Latency: ingestLatencyFlag, Stall: ingestStallFlag}
	cfg.LiveSeconds = liveSecondsFlag
	cfg.Archive = archiveFlag
	cfg.ArchiveRemote = archiveRemoteFlag
	cfg.Rclone = rcloneFlag

	if err := cfg.Validate(); err != nil {
		return nil, err
	}

	return cfg, nil
}

func getenvFirst(getenv func(string) string, keys ...string) string {
	for _, key := range keys {
		if val := strings.TrimSpace(getenv(key)); val != "" {
			return val
		}
	}
	return ""
}
