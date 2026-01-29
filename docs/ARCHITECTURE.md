# Epoll Proxy Architecture

## Design Goals
- Handle 10M+ concurrent connections
- Sub-millisecond latency
- Zero-copy data forwarding
- HTTP/1.1 with keep-alive support
- Minimal memory footprint per connection

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                     Event Loop (epoll)                      │
│                                                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  │
│  │ Client 1 │  │ Client 2 │  │ Client N │  │ Listen   │  │
│  │  Socket  │  │  Socket  │  │  Socket  │  │  Socket  │  │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘  │
│       │             │             │             │         │
│       └─────────────┴─────────────┴─────────────┘         │
│                         │                                  │
└─────────────────────────┼──────────────────────────────────┘
                          │
                          ▼
              ┌───────────────────────┐
              │  Connection Manager   │
              │   (Object Pool)       │
              └───────────┬───────────┘
                          │
        ┌─────────────────┼─────────────────┐
        │                 │                 │
        ▼                 ▼                 ▼
   ┌────────┐        ┌────────┐       ┌────────┐
   │ Client │◄──────►│Backend │       │ Client │
   │  Conn  │  Pair  │  Conn  │       │  Conn  │
   └────────┘        └────────┘       └────────┘
        │                 │
        ▼                 ▼
   ┌────────┐        ┌────────┐
   │ Buffers│        │ Buffers│
   └────────┘        └────────┘
```

## Key Components

### 1. Event Loop (epoll)
- Edge-triggered mode for efficiency
- Single-threaded (can be multi-process with SO_REUSEPORT)
- Events: EPOLLIN, EPOLLOUT, EPOLLERR, EPOLLRDHUP

### 2. Connection Pool
- Pre-allocated array of connections
- O(1) allocation/deallocation
- Free list for available connections
- Paired connections (client ↔ backend)

### 3. Buffer Management
- Fixed-size buffers per connection
- Zero-copy forwarding when possible
- Backpressure handling (stop reading when peer buffer full)

### 4. HTTP Parser
- Streaming parser (handles incomplete requests)
- Supports keep-alive
- Request validation
- Error handling (400, 413, 502, 503)

## Data Flow

### TCP Mode (Simple)
```
Client → Read → Forward → Backend
Backend → Read → Forward → Client
```

### HTTP Mode (Smarter)
```
1. Client → Read HTTP Request → Parse
2. Validate → Connect to Backend
3. Forward Request → Backend
4. Backend → Read Response → Forward → Client
5. If keep-alive: goto 1, else: close
```

## State Machine

```
Client Connection States:
┌──────────────┐
│ CONN_CLOSED  │ (Initial state)
└──────┬───────┘
       │ accept()
       ▼
┌────────────────────┐
│ CONN_READING_      │ (HTTP mode only)
│ REQUEST            │
└──────┬─────────────┘
       │ Request complete
       ▼
┌────────────────────┐
│ CONN_REQUEST_      │ (Transitional)
│ COMPLETE           │
└──────┬─────────────┘
       │ Backend connected
       ▼
┌────────────────────┐
│ CONN_WRITING_      │ (Writing response)
│ RESPONSE           │
└──────┬─────────────┘
       │ Response done
       ├─► Keep-alive: goto READING_REQUEST
       └─► No keep-alive: CONN_CLOSED

Backend Connection States:
┌──────────────┐
│ CONN_CLOSED  │
└──────┬───────┘
       │ connect()
       ▼
┌────────────────┐
│ CONN_          │ (Async connect in progress)
│ CONNECTING     │
└──────┬─────────┘
       │ EPOLLOUT (connect complete)
       ▼
┌────────────────┐
│ CONN_CONNECTED │ (Active forwarding)
└────────────────┘
```

## Performance Optimizations

1. **Edge-Triggered Epoll**
   - Fewer syscalls
   - Must drain sockets completely

2. **Non-Blocking I/O**
   - Never blocks event loop
   - Handles EAGAIN/EWOULDBLOCK

3. **TCP_NODELAY**
   - Disables Nagle's algorithm
   - Lower latency for small packets

4. **SO_REUSEPORT**
   - Multi-process load balancing
   - Kernel distributes connections

5. **Object Pooling**
   - Pre-allocated connections
   - No malloc/free in hot path

6. **Backpressure**
   - Stop reading when peer buffer full
   - TCP flow control prevents bufferbloat

## Scalability

### Vertical Scaling (Single Machine)
- Use all CPU cores: run N processes with SO_REUSEPORT
- Increase ulimit: `ulimit -n 1048576`
- Tune kernel: `/etc/sysctl.conf`

### Horizontal Scaling (Multiple Machines)
- Run proxy on each machine
- Load balancer in front (HAProxy, nginx)
- Or DNS round-robin

## Future Enhancements

1. **IPC with Go Backend**
   - Shared memory ring buffers
   - Unix domain sockets
   - Zero-copy with splice/vmsplice

2. **HTTP/2 Support**
   - Multiplexing
   - Server push
   - Binary framing

3. **TLS/SSL**
   - OpenSSL integration
   - Session resumption
   - SNI support

4. **Connection Pooling**
   - Reuse backend connections
   - Persistent connections to backend

5. **Real-time Metrics**
   - Prometheus exporter
   - Grafana dashboard
   - Per-second statistics
