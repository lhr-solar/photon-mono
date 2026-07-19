package main

import (
	"io"
	"net"
	"time"
)

func main() {
	startedAt := time.Now()

	store, err := OpenStreamStore()
	if err != nil {
		panic(err)
	}
	defer store.Close()

	clients := NewClientTracker()
	ingress := make(chan []byte, 1024)

	go appendLoop(store, ingress)
	go listenForIngress(ingress)
	go listenForClients(store, clients)
	go compactionLoop(store, clients)
	go listenForObservers(startedAt, store, clients, ingress)
	select {}
}

func appendLoop(store *StreamStore, ingress <-chan []byte) {
	for payload := range ingress {
		if store.StoragePressureHigh() {
			started := time.Now()
			queueStats.blockStartedNS.Store(started.UnixNano())
			for store.StoragePressureHigh() {
				time.Sleep(100 * time.Millisecond)
			}
			queueStats.blockStartedNS.Store(0)
			queueStats.blockedNS.Add(time.Since(started).Nanoseconds())
		}
		if _, err := store.Append(payload); err != nil {
			//fmt.Println("append error:", err)
		}
	}
}

func listenForIngress(ingress chan<- []byte) {
	const addr = ":6100"

	listener, err := net.Listen("tcp", addr)
	if err != nil {
		panic(err)
	}
	ingressStats.listening.Store(true)
	defer ingressStats.listening.Store(false)
	defer listener.Close()

	for {
		conn, err := listener.Accept()
		if err != nil {
			ingressStats.acceptFailures.Add(1)
			continue
		}
		ingressStats.active.Add(1)
		ingressStats.total.Add(1)
		go readIngressConnection(conn, ingress)
	}
}

func readIngressConnection(conn net.Conn, ingress chan<- []byte) {
	const readBuffer = 32 * 1024
	defer conn.Close()
	defer ingressStats.active.Add(-1)
	buf := make([]byte, readBuffer)
	for {
		n, err := conn.Read(buf)
		if err != nil {
			if err != io.EOF {
				ingressStats.readFailures.Add(1)
			} //read error }
			return
		}
		ingressStats.bytes.Add(uint64(n))
		ingressStats.reads.Add(1)
		ingressStats.lastReceiveMS.Store(time.Now().UnixMilli())
		payload := make([]byte, n)
		copy(payload, buf[:n])
		ingress <- payload
		observeQueueDepth(len(ingress))
	}
}

func listenForClients(store *StreamStore, tracker *ClientTracker) {
	const addr = ":6500"
	listener, err := net.Listen("tcp", addr)
	if err != nil {
		panic(err)
	}
	tracker.listening.Store(true)
	defer tracker.listening.Store(false)
	defer listener.Close()

	for {
		conn, err := listener.Accept()
		if err != nil {
			tracker.acceptFailures.Add(1)
			continue
		}
		client := tracker.Register(conn.RemoteAddr().String(), store.StartIDForNewClient())
		go writeClient(conn, client, store, tracker)
	}
}

func compactionLoop(store *StreamStore, tracker *ClientTracker) {
	ticker := time.NewTicker(10 * time.Second)
	defer ticker.Stop()
	for range ticker.C {
		if err := store.CompactBefore(tracker.MinNextID(store.LatestID() + 1)); err != nil {
		}
	}
}
