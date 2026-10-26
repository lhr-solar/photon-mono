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
)

const (
	DefaultHost             = "0.0.0.0"
	DefaultIngressPort      = 6100
	DefaultClientPort       = 6500
	DefaultObserverPort     = 5400
	DefaultVideoIngressPort = 6200
	DefaultVideoClientPort  = 6700
	DefaultVideoUdpPort     = 6800
	DefaultDataDir          = "./data"
)

// Config holds runtime configuration for the photon server.
type Config struct {
	Host             string
	IngressPort      int
	ClientPort       int
	ObserverPort     int
	VideoIngressPort int
	VideoClientPort  int
	VideoUdpPort     int
	DataDir          string
	TsdbDir          string
	DbcPath          string
	LiveWriter       string
	Ui               string
	Password         string
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
		DataDir:          DefaultDataDir,
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
	if dir := getenvFirst(getenv, "PHOTON_TSDB_DIR", "PHOTON_TSDB"); dir != "" {
		cfg.TsdbDir = dir
	}
	if path := getenvFirst(getenv, "PHOTON_DBC", "PHOTON_DBC_PATH"); path != "" {
		cfg.DbcPath = path
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

	fs := flag.NewFlagSet("photon-server", flag.ContinueOnError)
	fs.SetOutput(output)

	var hostFlag string
	var ingressPortFlag int
	var clientPortFlag int
	var observerPortFlag int
	var videoIngressPortFlag int
	var videoClientPortFlag int
	var videoUdpPortFlag int
	var dataDirFlag string
	var tsdbDirFlag string
	var dbcPathFlag string
	var liveWriterFlag string
	var uiFlag string
	var passwordFlag string

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
	fs.StringVar(&dataDirFlag, "data-dir", cfg.DataDir, "Directory path for persistent storage segments")
	fs.StringVar(&dataDirFlag, "storage-path", cfg.DataDir, "Alias for -data-dir")
	fs.StringVar(&tsdbDirFlag, "tsdb-dir", cfg.TsdbDir, "TSDB root (default: <data-dir>/tsdb)")
	fs.StringVar(&dbcPathFlag, "dbc", cfg.DbcPath, "DBC file for the live TSDB writer")
	fs.StringVar(&liveWriterFlag, "live-writer", cfg.LiveWriter, "photon-index binary that writes the TSDB, or 'off'")
	fs.StringVar(&uiFlag, "ui", cfg.Ui, "local UI backend (photon-backend), or off")
	fs.StringVar(&passwordFlag, "password", cfg.Password, "shared client password (env: PHOTON_PASSWORD)")

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
		fmt.Fprintf(output, "  -data-dir, -storage-path string Storage directory (default %q, env: PHOTON_DATA_DIR)\n", DefaultDataDir)
		fmt.Fprintf(output, "  -tsdb-dir string             TSDB root (default <data-dir>/tsdb, env: PHOTON_TSDB_DIR)\n")
		fmt.Fprintf(output, "  -dbc string                  DBC for live TSDB writer (env: PHOTON_DBC)\n")
		fmt.Fprintf(output, "  -live-writer string          photon-index path, or off (env: PHOTON_LIVE_WRITER)\n")
		fmt.Fprintf(output, "  -ui string                   local photon-backend + page, or off (env: PHOTON_UI)\n")
		fmt.Fprintf(output, "  -password string             client password (env: PHOTON_PASSWORD; random if unset)\n")
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
	cfg.DataDir = dataDirFlag
	cfg.TsdbDir = tsdbDirFlag
	cfg.DbcPath = dbcPathFlag
	cfg.LiveWriter = liveWriterFlag
	cfg.Ui = uiFlag
	cfg.Password = passwordFlag

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
