#!/bin/bash

# Comprehensive benchmark suite

echo "════════════════════════════════════════════════════════════"
echo "  Epoll Proxy Benchmark Suite"
echo "════════════════════════════════════════════════════════════"
echo ""

# Check if wrk is installed
if ! command -v wrk &> /dev/null; then
    echo "❌ wrk not found. Install with:"
    echo "   Ubuntu: sudo apt-get install wrk"
    echo "   macOS: brew install wrk"
    exit 1
fi

# Start backend
echo "Starting backend server..."
python3 -m http.server 8081 > /tmp/backend.log 2>&1 &
BACKEND_PID=$!
sleep 2

# Start proxy
echo "Starting proxy..."
../../build/bin/epoll-proxy -m http > /tmp/proxy.log 2>&1 &
PROXY_PID=$!
sleep 2

# Cleanup function
cleanup() {
    echo ""
    echo "Cleaning up..."
    kill $PROXY_PID 2>/dev/null
    kill $BACKEND_PID 2>/dev/null
    wait $PROXY_PID 2>/dev/null
    wait $BACKEND_PID 2>/dev/null
}
trap cleanup EXIT

echo ""
echo "════════════════════════════════════════════════════════════"
echo "  Test 1: Light Load (10 connections)"
echo "════════════════════════════════════════════════════════════"
wrk -t2 -c10 -d10s http://localhost:8080

echo ""
echo "════════════════════════════════════════════════════════════"
echo "  Test 2: Medium Load (100 connections)"
echo "════════════════════════════════════════════════════════════"
wrk -t4 -c100 -d30s http://localhost:8080

echo ""
echo "════════════════════════════════════════════════════════════"
echo "  Test 3: High Load (1000 connections)"
echo "════════════════════════════════════════════════════════════"
wrk -t8 -c1000 -d30s http://localhost:8080

echo ""
echo "════════════════════════════════════════════════════════════"
echo "  Benchmark Complete"
echo "════════════════════════════════════════════════════════════"
