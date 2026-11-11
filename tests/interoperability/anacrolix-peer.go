// Stage 19 sidecar, compiled inside a pinned jasminsehic/torrent checkout.
// The tracker is intentionally local and forwards only WebTorrent signaling.
package main

import (
    "context"
    "encoding/json"
    "flag"
    "fmt"
    "log"
    "net/http"
    "os"
    "path/filepath"
    "sync"
    "time"

    "github.com/anacrolix/torrent"
    "github.com/anacrolix/torrent/metainfo"
    "github.com/coder/websocket"
)

type socketPeer struct {
    conn *websocket.Conn
    mu sync.Mutex
}

func (p *socketPeer) write(v any) error {
    data, err := json.Marshal(v)
    if err != nil { return err }
    p.mu.Lock()
    defer p.mu.Unlock()
    ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
    defer cancel()
    return p.conn.Write(ctx, websocket.MessageText, data)
}

type trackerState struct {
    mu sync.Mutex
    peers map[string]map[string]*socketPeer
    offers int
    answers int
}

func field(m map[string]json.RawMessage, key string) string {
    var s string
    _ = json.Unmarshal(m[key], &s)
    return s
}

func (s *trackerState) forget(p *socketPeer) {
    s.mu.Lock()
    defer s.mu.Unlock()
    for hash, peers := range s.peers {
        for id, registered := range peers {
            if registered == p { delete(peers, id) }
        }
        if len(peers) == 0 { delete(s.peers, hash) }
    }
}

func (s *trackerState) handle(p *socketPeer, m map[string]json.RawMessage) {
    if field(m, "action") != "announce" { return }
    hash, id := field(m, "info_hash"), field(m, "peer_id")
    if hash == "" || id == "" { return }

    event := field(m, "event")
    s.mu.Lock()
    peers := s.peers[hash]
    if peers == nil {
        peers = make(map[string]*socketPeer)
        s.peers[hash] = peers
    }
    if event == "stopped" {
        if peers[id] == p { delete(peers, id) }
    } else {
        peers[id] = p
    }
    var recipients []*socketPeer
    if target := field(m, "to_peer_id"); target != "" {
        if dst := peers[target]; dst != nil && dst != p { recipients = append(recipients, dst) }
    } else {
        for peerID, dst := range peers {
            if peerID != id && dst != p { recipients = append(recipients, dst) }
        }
    }
    s.mu.Unlock()

    // A normal announce response is required even when no other peer exists.
    if len(m["answer"]) == 0 {
        _ = p.write(map[string]any{
            "action": "announce", "info_hash": hash,
            "interval": 60, "complete": 1, "incomplete": 1,
        })
    }

    if raw := m["answer"]; len(raw) != 0 {
        for _, dst := range recipients {
            err := dst.write(map[string]any{
                "action": "announce", "info_hash": hash, "peer_id": id,
                "to_peer_id": field(m, "to_peer_id"),
                "offer_id": field(m, "offer_id"), "answer": raw,
            })
            if err == nil {
                s.mu.Lock()
                s.answers++
                s.mu.Unlock()
                log.Print("TRACKER forwarded answer")
            }
        }
        return
    }

    var offers []map[string]json.RawMessage
    if err := json.Unmarshal(m["offers"], &offers); err != nil { return }
    for _, offer := range offers {
        for _, dst := range recipients {
            err := dst.write(map[string]any{
                "action": "announce", "info_hash": hash, "peer_id": id,
                "offer_id": field(offer, "offer_id"), "offer": offer["offer"],
            })
            if err == nil {
                s.mu.Lock()
                s.offers++
                s.mu.Unlock()
                log.Print("TRACKER forwarded offer")
            }
        }
    }
}

func runTracker(address string) error {
    state := &trackerState{peers: make(map[string]map[string]*socketPeer)}
    mux := http.NewServeMux()
    mux.HandleFunc("/status", func(w http.ResponseWriter, r *http.Request) {
        state.mu.Lock()
        count := 0
        for _, peers := range state.peers { count += len(peers) }
        status := map[string]int{"registrations": count, "offers": state.offers, "answers": state.answers}
        state.mu.Unlock()
        w.Header().Set("Content-Type", "application/json")
        _ = json.NewEncoder(w).Encode(status)
    })
    // Transmission canonicalizes tracker /announce URLs to the WS root;
    // accept both its root path and anacrolix\x27s explicit /announce path.
    mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
        conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{InsecureSkipVerify: true})
        if err != nil { log.Printf("tracker accept: %v", err); return }
        p := &socketPeer{conn: conn}
        defer state.forget(p)
        defer conn.CloseNow()
        for {
            _, data, err := conn.Read(r.Context())
            if err != nil { return }
            var m map[string]json.RawMessage
            if err := json.Unmarshal(data, &m); err == nil { state.handle(p, m) }
        }
    })
    server := &http.Server{Addr: address, Handler: mux, ReadHeaderTimeout: 10*time.Second}
    fmt.Println("READY TRACKER")
    return server.ListenAndServe()
}

func runPeer(mode, torrentPath, dataDir string, expected int64) error {
    if err := os.MkdirAll(dataDir, 0755); err != nil { return err }
    mi, err := metainfo.LoadFromFile(torrentPath)
    if err != nil { return fmt.Errorf("metainfo: %w", err) }
    cfg := torrent.NewDefaultClientConfig()
    cfg.DataDir = dataDir
    cfg.Seed = true
    cfg.DisableTCP = true
    cfg.DisableUTP = true
    cfg.NoDHT = true
    cfg.DisablePEX = true
    cfg.DisableWebseeds = true
    cfg.NoDefaultPortForwarding = true
    cfg.DisableIPv6 = true
    cfg.DisableWebtorrent = false
    cfg.ListenPort = 0
    cfg.LocalServiceDiscovery = nil
    cfg.ICEServers = nil
    cfg.ICEServerList = nil

    client, err := torrent.NewClient(cfg)
    if err != nil { return fmt.Errorf("new anacrolix client: %w", err) }
    defer client.Close()
    tor, err := client.AddTorrent(mi)
    if err != nil { return fmt.Errorf("add torrent: %w", err) }
    defer tor.Drop()

    if mode == "seed" {
        ctx, cancel := context.WithTimeout(context.Background(), 45*time.Second)
        defer cancel()
        if err := tor.VerifyDataContext(ctx); err != nil { return fmt.Errorf("verify seed: %w", err) }
        if got := tor.BytesCompleted(); got != expected {
            return fmt.Errorf("seed not verified: %d / %d bytes (%s)", got, expected, filepath.Join(dataDir, "interop.bin"))
        }
    } else if mode == "download" {
        tor.DownloadAll()
    } else {
        return fmt.Errorf("invalid peer mode %q", mode)
    }
    fmt.Printf("READY %s\n", mode)
    deadline := time.NewTimer(180*time.Second)
    defer deadline.Stop()
    ticker := time.NewTicker(time.Second)
    defer ticker.Stop()
    for {
        select {
        case <-deadline.C:
            return fmt.Errorf("%s timed out with %d of %d bytes", mode, tor.BytesCompleted(), expected)
        case <-ticker.C:
            if mode == "download" && tor.BytesCompleted() >= expected {
                fmt.Printf("COMPLETE %d\n", tor.BytesCompleted())
                return nil
            }
        }
    }
}

func main() {
    mode := flag.String("mode", "", "tracker, seed or download")
    address := flag.String("address", "", "tracker listen address")
    torrentPath := flag.String("torrent", "", "torrent metadata")
    dataDir := flag.String("data", "", "download directory")
    size := flag.Int64("size", 0, "expected payload length")
    flag.Parse()
    var err error
    switch *mode {
    case "tracker": err = runTracker(*address)
    case "seed", "download": err = runPeer(*mode, *torrentPath, *dataDir, *size)
    default: err = fmt.Errorf("invalid mode %q", *mode)
    }
    if err != nil { log.Fatal(err) }
}
