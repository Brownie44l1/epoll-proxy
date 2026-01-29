# Building and Running

## Prerequisites

```bash
# Ubuntu/Debian
sudo apt-get install build-essential

# macOS
xcode-select --install
```

## Building

```bash
# Production build (optimized)
make

# Debug build (with sanitizers)
make debug

# Profile-guided optimization
make pgo
# ... run representative workload ...
make pgo-use
```

## Running

```bash
# Start a test backend
python3 -m http.server 8081 &

# Run proxy in HTTP mode
./build/bin/epoll-proxy -m http

# Run proxy in TCP mode
./build/bin/epoll-proxy -m tcp -p 3306 -P 3307
```

## Testing

```bash
# Unit tests
make test

# Benchmark
make benchmark

# Quick performance check
make perf

# Memory leak check
make valgrind
```

## System Tuning

For high-performance testing, tune your system:

```bash
# Increase file descriptor limit
ulimit -n 1048576

# Kernel tuning (add to /etc/sysctl.conf)
net.core.somaxconn = 65535
net.ipv4.tcp_max_syn_backlog = 65535
net.ipv4.ip_local_port_range = 1024 65535
net.ipv4.tcp_fin_timeout = 15
net.ipv4.tcp_tw_reuse = 1
```

Apply with: `sudo sysctl -p`
