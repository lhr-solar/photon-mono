package sshd

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/binary"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"

	"github.com/pkg/sftp"
	"golang.org/x/crypto/ssh"
)

const DefaultBind = "127.0.0.1:2222"

type Config struct {
	Bind        string
	HostKeyPath string
	AuthKeys    string
	Password    string
	Shell       string
}

type Server struct {
	cfg      Config
	listener net.Listener
	host     ssh.Signer
	keys     map[string]ssh.PublicKey
	password []byte
	shell    []string
}

func New(cfg Config) (*Server, error) {
	cfg.Bind = strings.TrimSpace(cfg.Bind)
	if cfg.Bind == "" {
		cfg.Bind = DefaultBind
	}
	host, _, err := net.SplitHostPort(cfg.Bind)
	if err != nil {
		return nil, fmt.Errorf("ssh bind: %w", err)
	}
	if ip := net.ParseIP(host); ip != nil && !ip.IsLoopback() && !ip.IsUnspecified() {
		return nil, fmt.Errorf("ssh bind must be loopback or 0.0.0.0, got %s", host)
	}

	signer, err := loadOrCreateHostKey(cfg.HostKeyPath)
	if err != nil {
		return nil, err
	}
	keys, err := loadAuthorizedKeys(cfg.AuthKeys)
	if err != nil {
		return nil, err
	}
	password := strings.TrimSpace(cfg.Password)
	if len(keys) == 0 && password == "" {
		return nil, errors.New("ssh: set authorized_keys or PHOTON_PASSWORD")
	}
	return &Server{
		cfg:      cfg,
		host:     signer,
		keys:     keys,
		password: []byte(password),
		shell:    defaultShell(cfg.Shell),
	}, nil
}

func (s *Server) Start() error {
	ln, err := net.Listen("tcp", s.cfg.Bind)
	if err != nil {
		return err
	}
	s.listener = ln
	log.Printf("[photon-sshd] listening on %s keys=%d password=%t", s.cfg.Bind, len(s.keys), len(s.password) > 0)
	go s.accept()
	return nil
}

func (s *Server) Close() error {
	if s.listener == nil {
		return nil
	}
	return s.listener.Close()
}

func (s *Server) Addr() string {
	if s.listener == nil {
		return s.cfg.Bind
	}
	return s.listener.Addr().String()
}

func (s *Server) accept() {
	for {
		conn, err := s.listener.Accept()
		if err != nil {
			if !errors.Is(err, net.ErrClosed) {
				log.Printf("[photon-sshd] accept: %v", err)
			}
			return
		}
		go s.handleConn(conn)
	}
}

func (s *Server) handleConn(nConn net.Conn) {
	defer nConn.Close()
	cfg := &ssh.ServerConfig{
		PublicKeyCallback: s.checkKey,
	}
	if len(s.password) > 0 {
		cfg.PasswordCallback = s.checkPassword
	}
	cfg.AddHostKey(s.host)

	conn, chans, reqs, err := ssh.NewServerConn(nConn, cfg)
	if err != nil {
		log.Printf("[photon-sshd] handshake %s: %v", nConn.RemoteAddr(), err)
		return
	}
	defer conn.Close()
	log.Printf("[photon-sshd] %s user=%s", conn.RemoteAddr(), conn.User())
	go ssh.DiscardRequests(reqs)
	for newCh := range chans {
		switch newCh.ChannelType() {
		case "session":
			ch, reqs, err := newCh.Accept()
			if err != nil {
				continue
			}
			go handleSession(ch, reqs, s.shell)
		case "direct-tcpip":
			go handleDirectTcpip(newCh)
		default:
			_ = newCh.Reject(ssh.UnknownChannelType, newCh.ChannelType())
		}
	}
}

func (s *Server) checkKey(_ ssh.ConnMetadata, key ssh.PublicKey) (*ssh.Permissions, error) {
	if _, ok := s.keys[ssh.FingerprintSHA256(key)]; ok {
		return nil, nil
	}
	return nil, errors.New("unknown public key")
}

func (s *Server) checkPassword(_ ssh.ConnMetadata, password []byte) (*ssh.Permissions, error) {
	if len(s.password) == 0 || !subtleConstantEq(s.password, password) {
		return nil, errors.New("bad password")
	}
	return nil, nil
}

func subtleConstantEq(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	var v byte
	for i := range a {
		v |= a[i] ^ b[i]
	}
	return v == 0
}

func handleSession(ch ssh.Channel, reqs <-chan *ssh.Request, shell []string) {
	defer ch.Close()
	for req := range reqs {
		switch req.Type {
		case "pty-req", "env", "window-change", "signal":
			if req.WantReply {
				_ = req.Reply(true, nil)
			}
		case "shell":
			if req.WantReply {
				_ = req.Reply(true, nil)
			}
			code := runCommand(ch, shell, nil)
			sendExit(ch, code)
			return
		case "exec":
			var msg struct{ Command string }
			if err := ssh.Unmarshal(req.Payload, &msg); err != nil || strings.TrimSpace(msg.Command) == "" {
				if req.WantReply {
					_ = req.Reply(false, nil)
				}
				sendExit(ch, 1)
				return
			}
			if req.WantReply {
				_ = req.Reply(true, nil)
			}
			log.Printf("[photon-sshd] exec %q", msg.Command)
			var code int
			if isSCPCommand(msg.Command) {
				code = runSCP(ch, msg.Command)
			} else {
				code = runExec(ch, msg.Command)
			}
			_ = ch.CloseWrite()
			sendExit(ch, code)
			return
		case "subsystem":
			var msg struct{ Name string }
			if err := ssh.Unmarshal(req.Payload, &msg); err != nil || msg.Name != "sftp" {
				if req.WantReply {
					_ = req.Reply(false, nil)
				}
				continue
			}
			if req.WantReply {
				_ = req.Reply(true, nil)
			}
			_ = serveSFTP(ch)
			return
		default:
			if req.WantReply {
				_ = req.Reply(false, nil)
			}
		}
	}
}

func runExec(ch ssh.Channel, command string) int {
	var cmd *exec.Cmd
	if runtime.GOOS == "windows" {
		cmd = exec.Command("cmd.exe", "/c", command)
	} else {
		cmd = exec.Command("/bin/sh", "-c", command)
	}
	cmd.Env = os.Environ()
	out, err := cmd.CombinedOutput()
	if len(out) > 0 {
		_, _ = ch.Write(out)
	}
	if err != nil {
		if ee, ok := err.(*exec.ExitError); ok {
			return ee.ExitCode()
		}
		fmt.Fprintf(ch.Stderr(), "%v\n", err)
		return 1
	}
	return 0
}

func runCommand(ch ssh.Channel, argv []string, extraEnv []string) int {
	if len(argv) == 0 {
		return 1
	}
	cmd := exec.Command(argv[0], argv[1:]...)
	cmd.Env = append(os.Environ(), extraEnv...)
	stdin, err := cmd.StdinPipe()
	if err != nil {
		fmt.Fprintf(ch.Stderr(), "%v\n", err)
		return 1
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		fmt.Fprintf(ch.Stderr(), "%v\n", err)
		return 1
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		fmt.Fprintf(ch.Stderr(), "%v\n", err)
		return 1
	}
	if err := cmd.Start(); err != nil {
		fmt.Fprintf(ch.Stderr(), "%v\n", err)
		return 1
	}
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer stdin.Close()
		_, _ = io.Copy(stdin, ch)
	}()
	go func() {
		defer wg.Done()
		_, _ = io.Copy(ch, stdout)
	}()
	go func() {
		defer wg.Done()
		_, _ = io.Copy(ch.Stderr(), stderr)
	}()
	waitErr := cmd.Wait()
	_ = stdin.Close()
	wg.Wait()
	if waitErr != nil {
		if ee, ok := waitErr.(*exec.ExitError); ok {
			return ee.ExitCode()
		}
		fmt.Fprintf(ch.Stderr(), "%v\n", waitErr)
		return 1
	}
	return 0
}

func sendExit(ch ssh.Channel, code int) {
	var buf [4]byte
	if code < 0 {
		code = 1
	}
	binary.BigEndian.PutUint32(buf[:], uint32(code))
	_, _ = ch.SendRequest("exit-status", false, buf[:])
}

func serveSFTP(ch ssh.Channel) error {
	srv, err := sftp.NewServer(ch)
	if err != nil {
		return err
	}
	defer srv.Close()
	return srv.Serve()
}

func defaultShell(override string) []string {
	if s := strings.TrimSpace(override); s != "" {
		return []string{s}
	}
	if runtime.GOOS == "windows" {
		return []string{"powershell.exe", "-NoLogo", "-NoProfile"}
	}
	return []string{"/bin/sh"}
}

func loadOrCreateHostKey(path string) (ssh.Signer, error) {
	path = strings.TrimSpace(path)
	if path == "" {
		return nil, errors.New("ssh host key path is empty")
	}
	if raw, err := os.ReadFile(path); err == nil {
		return ssh.ParsePrivateKey(raw)
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil && !errors.Is(err, os.ErrExist) {
		if filepath.Dir(path) != "." {
			return nil, err
		}
	}
	_, priv, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return nil, err
	}
	signer, err := ssh.NewSignerFromKey(priv)
	if err != nil {
		return nil, err
	}
	block, err := ssh.MarshalPrivateKey(priv, "")
	if err != nil {
		return nil, err
	}
	if err := os.WriteFile(path, pem.EncodeToMemory(block), 0o600); err != nil {
		return nil, err
	}
	return signer, nil
}

func loadAuthorizedKeys(path string) (map[string]ssh.PublicKey, error) {
	out := make(map[string]ssh.PublicKey)
	path = strings.TrimSpace(path)
	if path == "" {
		return out, nil
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return out, nil
		}
		return nil, err
	}
	for len(raw) > 0 {
		key, _, _, rest, err := ssh.ParseAuthorizedKey(raw)
		if err != nil {
			break
		}
		out[ssh.FingerprintSHA256(key)] = key
		raw = rest
	}
	return out, nil
}
