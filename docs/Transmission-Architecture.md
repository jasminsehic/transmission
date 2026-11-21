Transmission is fundamentally a BitTorrent client and communicates with other BitTorrent peers.
In addition to this Transmission supports network-based remote control, whereby an authorised user may control the Transmission core from the same or another machine via the [Transmission JSON RPC protocol](rpc-spec.md).
To make remote control easier from an arbitrary machine, Transmission core can also serve a JavaScript web application to any browser and which in turn makes JSON RPC calls back to the Transmission core.

The core components and methods of control are shown below:

![Architecture](https://transmission.github.io/wiki-images/Transmission_Architecture.gif)

From the above diagram it can be seen that a Transmission core may be controlled by the following:
 * _Local_ directly linked GUI (macOS, GTK+)
 * _Local_ or _remote_ Qt GUI
 * _Local_ or _remote_ command line utility
 * _Local_ or _remote_ Transmission web application running in a web browser

The multiple methods of controlling Transmission and the various native GUIs available result in several different Transmission products as shown in the figure below:

![Products](https://transmission.github.io/wiki-images/Transmission_Products.gif)

The products are:
 * Transmission desktop - macOS
 * Transmission desktop - Windows, Linux/Qt
 * Transmission desktop - Linux/GTK+
 * Transmission daemon (headless)
 * Transmission command line

The Transmission packages available on various distributions may include one or more of these components.
Note. Although the diagram shows "Transmission Desktop Qt" as being a Qt GUI with Transmission core, the Qt component may be packaged on its own as a purely remote tool.

## WebTorrent integration (optional)

The optional WebTorrent integration extends the existing Transmission core with WS/WSS tracker signaling and WebRTC DataChannel peers. It reuses the normal BitTorrent peer protocol and swarm management instead of introducing another torrent engine. This section describes the implementation in this branch; the diagrams use Mermaid so they remain editable and render in GitHub.

### Component view (C4-inspired)

The WS/WSS tracker path carries announces, SDP offers, and SDP answers. Torrent payload data instead flows over the WebRTC DataChannel into the existing peer I/O and swarm components. Conventional TCP and µTP peer sockets continue to use the same peer I/O layer.

```mermaid
flowchart LR
    Remote["Remote WebTorrent peer"]
    Tracker["WS/WSS tracker"]
    subgraph Transmission["Transmission core"]
        Ann["Announcer"]
        WS["WS tracker client<br/>libcurl"]
        Sig["RTC signaling<br/>offer/answer + ICE lifecycle"]
        RTCSock["tr_peer_socket_rtc"]
        Traditional["TCP / µTP peer sockets"]
        PeerIO["Peer I/O, peer manager<br/>and peer messages"]
    end
    subgraph Dependencies["WebRTC dependencies"]
        DC["libdatachannel<br/>DataChannel / DTLS"]
        Juice["libjuice<br/>ICE / UDP"]
        SCTP["usrsctp<br/>SCTP"]
    end
    Ann --> WS
    WS <-->|"Announces and SDP signaling"| Tracker
    WS <-->|"Offers / answers"| Sig
    Sig -->|"Create / configure peer"| DC
    DC -->|"Open DataChannel"| RTCSock
    DC --> Juice
    DC --> SCTP
    RTCSock --> PeerIO
    Traditional --> PeerIO
    Juice <-->|"ICE connectivity checks"| Remote
    DC <-->|"WebRTC DataChannel payloads"| Remote
```

### Outbound connection sequence

Outgoing peers use non-trickle ICE: the local offer is announced only after gathering is complete or the offer batch has been resolved. The incoming-offer path reverses the offer/answer roles. The tracker relays signaling, not piece data.

```mermaid
sequenceDiagram
    participant A as WS announcer
    participant S as RTC signaling
    participant D as libdatachannel
    participant T as WS/WSS tracker
    participant R as Remote WebTorrent peer
    participant P as Transmission peer manager
    A->>S: Request SDP offers
    S->>D: Create peer connections and gather ICE
    D-->>S: Local SDP with gathered candidates
    S-->>A: Offers ready
    A->>T: Announce with offer IDs and SDP
    T->>R: Relay SDP offer
    R-->>T: SDP answer
    T-->>A: Answer associated with offer ID
    A->>S: Route answer
    S->>D: Apply remote description
    D-->>S: DataChannel open
    S->>P: Add RTC peer socket using selected ICE endpoint
    P->>P: Check blocklist at peer admission
    alt Endpoint permitted
        P->>R: BitTorrent handshake and pieces over DataChannel
    else Endpoint blocklisted
        Note over P: RTC socket not admitted
    end
```

### Network paths and privacy boundaries

`proxy_url` controls libcurl's WS/WSS tracker connection, including its authentication and TLS behavior. It does **not** proxy ICE/UDP or SCTP/DTLS peer traffic. With no configured proxy, the WebSocket client follows the applicable libcurl environment proxy rules. The SDP passed through the tracker may include host and server-reflexive addresses. WebRTC connections can therefore disclose network addresses and use direct paths even when tracker signaling is proxied.

```mermaid
flowchart LR
    Settings["proxy_url setting"]
    subgraph Local["Transmission"]
        WS["WS/WSS client<br/>libcurl"]
        ICE["WebRTC ICE<br/>libjuice"]
        Admission["Selected ICE endpoint<br/>blocklist check"]
        Swarm["BitTorrent peer manager"]
    end
    Proxy["HTTP(S) / SOCKS proxy"]
    Tracker["WS/WSS tracker"]
    STUN["STUN server"]
    TURN["TURN server (optional)"]
    Remote["Remote WebRTC peer"]
    Settings -.->|"If set"| WS
    WS -->|"Configured proxy"| Proxy
    Proxy -->|"Tracker signaling / SDP"| Tracker
    WS -->|"Direct or environment proxy when unset"| Tracker
    ICE -->|"STUN discovery"| STUN
    ICE -->|"TURN allocation if configured"| TURN
    ICE <-->|"Direct ICE checks / peer traffic"| Remote
    TURN <-->|"Relayed traffic when selected"| Remote
    ICE -.->|"Selected endpoint after channel opens"| Admission
    Admission -->|"Only if permitted"| Swarm
    Tracker -.->|"SDP forwarded to remote"| Remote
```

**Current policy boundaries:**

- `webtorrent_enabled` controls WebRTC and WS/WSS use at runtime (default `false`). `ENABLE_WEBTORRENT` controls compilation.
- `rtc::Configuration::bindAddress` can bind a libjuice-backed ICE connection to one explicit IPv4 **or** IPv6 address, but this branch currently does not set it from Transmission's `bind_address_ipv4` / `bind_address_ipv6` settings.
- The existing peer-manager IP blocklist applies to the **selected remote ICE endpoint** when the RTC peer socket is admitted, after ICE negotiation. It does not filter each ICE candidate or connectivity check. A TURN-relayed endpoint can represent the relay, not the underlying peer.
- `preferred_transports` currently lists only TCP and µTP. WebRTC is an independently enabled transport, and disabling TCP/µTP does not by itself disable WebRTC.
- Transmission has no general setting to force TCP/µTP/WebRTC peer connections through `proxy_url`; tracker proxy settings cannot guarantee proxy-only WebRTC communication.
- WebRTC DataChannels already use DTLS, so this branch uses a plaintext BitTorrent handshake inside the encrypted DataChannel rather than BitTorrent MSE. The interaction with `encryption = required` is an outstanding policy decision.

See [Configuration Files](Configuration-Files.md) for WebTorrent settings and [Building Transmission](Building-Transmission.md#optional-webtorrent-support) for compile-time dependencies.
