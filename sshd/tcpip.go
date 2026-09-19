package sshd

import (
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.org/x/crypto/ssh"
)

func handleDirectTcpip(newCh ssh.NewChannel) {
	var msg struct {
		DestAddr string
		DestPort uint32
		OrigAddr string
		OrigPort uint32
	}
	if err := ssh.Unmarshal(newCh.ExtraData(), &msg); err != nil {
		_ = newCh.Reject(ssh.ConnectionFailed, "bad direct-tcpip")
		return
	}
	dest, err := loopbackTCPAddr(msg.DestAddr, msg.DestPort)
	if err != nil {
		log.Printf("[photon-sshd] direct-tcpip deny %s:%d: %v", msg.DestAddr, msg.DestPort, err)
		_ = newCh.Reject(ssh.Prohibited, err.Error())
		return
	}
	backend, err := net.DialTimeout("tcp", dest, 5*time.Second)
	if err != nil {
		log.Printf("[photon-sshd] direct-tcpip dial %s: %v", dest, err)
		_ = newCh.Reject(ssh.ConnectionFailed, err.Error())
		return
	}
	ch, reqs, err := newCh.Accept()
	if err != nil {
		_ = backend.Close()
		return
	}
	go ssh.DiscardRequests(reqs)
	log.Printf("[photon-sshd] direct-tcpip %s", dest)
	proxyTCP(ch, backend)
}

func proxyTCP(ch ssh.Channel, backend net.Conn) {
	defer ch.Close()
	defer backend.Close()
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		_, _ = io.Copy(backend, ch)
		if tc, ok := backend.(*net.TCPConn); ok {
			_ = tc.CloseWrite()
		}
	}()
	go func() {
		defer wg.Done()
		_, _ = io.Copy(ch, backend)
		_ = ch.CloseWrite()
	}()
	wg.Wait()
}

func loopbackTCPAddr(host string, port uint32) (string, error) {
	if port == 0 || port > 65535 {
		return "", fmt.Errorf("invalid port %d", port)
	}
	host = strings.TrimSpace(host)
	if host == "" {
		return "", errors.New("empty host")
	}
	ip := net.ParseIP(host)
	if ip == nil {
		if !strings.EqualFold(host, "localhost") {
			return "", fmt.Errorf("forward host %q not allowed", host)
		}
		ip = net.IPv4(127, 0, 0, 1)
	}
	if !ip.IsLoopback() {
		return "", fmt.Errorf("forward host %s is not loopback", host)
	}
	return net.JoinHostPort(ip.String(), strconv.FormatUint(uint64(port), 10)), nil
}
