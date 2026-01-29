# High-Performance Epoll Proxy

A production-ready, high-performance HTTP/TCP proxy built in C using Linux epoll.

## Features

- ⚡ **10M+ concurrent connections** capability
- 🚀 **Edge-triggered epoll** for maximum efficiency  
- 🔄 **HTTP/1.1 keep-alive** support
- 🎯 **Zero-copy forwarding** where possible
- 📊 **Built-in metrics** and statistics
- 🛡️ **Robust error handling**

## Quick Start

```bash
# Build
make

# Run with test backend
./scripts/run-with-backend.sh

# Test
curl http://localhost:8080

# Benchmark
make benchmark
```

## Architecture

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for detailed design documentation.

## Building

See [docs/BUILDING.md](docs/BUILDING.md) for build instructions.

## Performance

Expected performance on modern hardware:
- **100,000+ req/sec** for small responses
- **Sub-millisecond latency** (p50)
- **10M concurrent connections** (with proper tuning)

## Directory Structure

```
.
├── src/
│   ├── core/         # Event loop, main
│   ├── network/      # Sockets, connections, buffers
│   ├── http/         # HTTP parsing and handling
│   ├── proxy/        # Proxy logic
│   └── ipc/          # Inter-process communication
├── include/          # Public headers
├── tests/            # Unit and integration tests
├── docs/             # Documentation
├── scripts/          # Helper scripts
└── build/            # Build artifacts
```

## License

MIT
