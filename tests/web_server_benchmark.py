#!/usr/bin/env python3
"""
IMAGINE Web Server Performance Benchmark Suite
Measures throughput (RPS), latency percentiles (p50, p90, p99),
compression ratio (gzip), HTTP caching (ETag / 304 Not Modified),
and concurrency scalability.
"""

import sys
import os
import time
import gzip
import json
import socket
import argparse
import subprocess
import statistics
import urllib.request
import urllib.error
import concurrent.futures

def is_port_open(host, port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.5)
        return s.connect_ex((host, port)) == 0

def wait_for_server(url, timeout=10.0):
    start = time.time()
    while time.time() - start < timeout:
        try:
            req = urllib.request.Request(url + "/api/stats")
            with urllib.request.urlopen(req, timeout=1.0) as resp:
                if resp.status == 200:
                    return True
        except Exception:
            time.sleep(0.1)
    return False

def make_single_request(url, headers=None, method="GET", body=None):
    headers = headers or {}
    req = urllib.request.Request(url, data=body, headers=headers, method=method)
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=10.0) as resp:
            data = resp.read()
            t1 = time.perf_counter()
            return {
                "latency_ms": (t1 - t0) * 1000.0,
                "status": resp.status,
                "size": len(data),
                "headers": {k.lower(): v for k, v in resp.headers.items()}
            }
    except urllib.error.HTTPError as e:
        t1 = time.perf_counter()
        return {
            "latency_ms": (t1 - t0) * 1000.0,
            "status": e.code,
            "size": len(e.read()),
            "headers": {k.lower(): v for k, v in e.headers.items()}
        }
    except Exception as ex:
        t1 = time.perf_counter()
        return {
            "latency_ms": (t1 - t0) * 1000.0,
            "status": 0,
            "size": 0,
            "headers": {},
            "error": str(ex)
        }

def benchmark_endpoint(url, num_requests=100, concurrency=1, headers=None):
    headers = headers or {}
    latencies = []
    statuses = []
    total_bytes = 0

    t_start = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as executor:
        futures = [executor.submit(make_single_request, url, headers) for _ in range(num_requests)]
        for f in concurrent.futures.as_completed(futures):
            res = f.result()
            latencies.append(res["latency_ms"])
            statuses.append(res["status"])
            total_bytes += res["size"]
    total_time = time.perf_counter() - t_start

    latencies.sort()
    success_count = sum(1 for s in statuses if 200 <= s < 300 or s == 304)
    rps = num_requests / total_time if total_time > 0 else 0

    return {
        "url": url,
        "concurrency": concurrency,
        "requests": num_requests,
        "success": success_count,
        "failed": num_requests - success_count,
        "total_time_s": round(total_time, 3),
        "rps": round(rps, 1),
        "mean_ms": round(statistics.mean(latencies), 2) if latencies else 0,
        "p50_ms": round(statistics.median(latencies), 2) if latencies else 0,
        "p90_ms": round(latencies[int(len(latencies) * 0.90)], 2) if latencies else 0,
        "p99_ms": round(latencies[int(len(latencies) * 0.99)], 2) if latencies else 0,
        "total_bytes": total_bytes
    }

def main():
    parser = argparse.ArgumentParser(description="Imagine Web Server Performance Benchmark Suite")
    parser.add_argument("--port", type=int, default=18080, help="Port to test")
    parser.add_argument("--host", default="127.0.0.1", help="Host to test")
    parser.add_argument("--catalog", default="catalog.db", help="Catalog database file")
    parser.add_argument("--web-dir", default="web", help="Static web directory")
    parser.add_argument("--no-spawn", action="store_true", help="Do not spawn server process if already running")
    args = parser.parse_args()

    base_url = f"http://{args.host}:{args.port}"
    server_proc = None

    if not is_port_open(args.host, args.port):
        if args.no_spawn:
            print(f"Error: Server not running on {base_url} and --no-spawn specified.")
            sys.exit(1)
        print(f"Spawning Imagine web server on {base_url}...")
        cmd = ["./build/imagine", "serve", "--catalog", args.catalog, "--port", str(args.port), "--host", args.host, "--web-dir", args.web_dir]
        server_proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not wait_for_server(base_url, timeout=5.0):
            print(f"Error: Failed to connect to spawned server on {base_url}")
            if server_proc:
                server_proc.kill()
            sys.exit(1)
    else:
        print(f"Connecting to existing server on {base_url}...")

    try:
        # Discover sample media item and thumbnail hash
        stats_resp = make_single_request(f"{base_url}/api/stats")
        media_resp = make_single_request(f"{base_url}/api/media?limit=5")
        
        sample_hash = "dummy_hash"
        sample_id = 1
        try:
            # If gzip is encoded, decompress if needed
            content = media_resp.get("headers", {}).get("content-encoding")
            raw = urllib.request.urlopen(f"{base_url}/api/media?limit=5").read()
            if content == "gzip":
                raw = gzip.decompress(raw)
            j = json.loads(raw.decode("utf-8"))
            if j.get("items") and len(j["items"]) > 0:
                sample_hash = j["items"][0]["content_hash"]
                sample_id = j["items"][0]["id"]
        except Exception as ex:
            print(f"Warning: Could not extract sample media item: {ex}")

        print("\n" + "=" * 75)
        print("  1. HTTP PROTOCOL & OPTIMIZATION VERIFICATION")
        print("=" * 75)

        # 1.1 Compression Test
        req_gzip = make_single_request(f"{base_url}/app.js", headers={"Accept-Encoding": "gzip"})
        req_raw = make_single_request(f"{base_url}/app.js")
        
        has_gzip = req_gzip.get("headers", {}).get("content-encoding") == "gzip"
        gzip_size = req_gzip["size"]
        raw_size = req_raw["size"]
        ratio = ((raw_size - gzip_size) / raw_size * 100) if raw_size > 0 else 0
        print(f"  [*] Gzip Compression on /app.js:")
        print(f"      - Header 'content-encoding': {req_gzip.get('headers', {}).get('content-encoding', 'none')} (Pass: {has_gzip})")
        print(f"      - Raw Size: {raw_size:,} bytes | Gzip Size: {gzip_size:,} bytes | Savings: {ratio:.1f}%")

        # 1.2 Compression on API Media
        api_gzip = make_single_request(f"{base_url}/api/media?limit=500", headers={"Accept-Encoding": "gzip"})
        api_raw = make_single_request(f"{base_url}/api/media?limit=500")
        api_has_gzip = api_gzip.get("headers", {}).get("content-encoding") == "gzip"
        api_savings = ((api_raw["size"] - api_gzip["size"]) / api_raw["size"] * 100) if api_raw["size"] > 0 else 0
        print(f"  [*] Gzip Compression on /api/media?limit=500:")
        print(f"      - Raw Size: {api_raw['size']:,} bytes | Gzip Size: {api_gzip['size']:,} bytes | Savings: {api_savings:.1f}%")

        # 1.3 Caching & 304 Test on Thumbnails
        thumb_url = f"{base_url}/api/thumbnails/{sample_hash}/256"
        thumb_req1 = make_single_request(thumb_url)
        thumb_etag = thumb_req1.get("headers", {}).get("etag")
        thumb_cache = thumb_req1.get("headers", {}).get("cache-control")
        print(f"  [*] Thumbnail Caching Headers on /api/thumbnails/{sample_hash[:16]}.../256:")
        print(f"      - Cache-Control: {thumb_cache}")
        print(f"      - ETag: {thumb_etag}")

        if thumb_etag:
            thumb_304 = make_single_request(thumb_url, headers={"If-None-Match": thumb_etag})
            print(f"      - Revalidation with If-None-Match: Status {thumb_304['status']} (Pass: {thumb_304['status'] == 304})")

        # 1.4 Streaming & Range Request on Original Photo
        photo_url = f"{base_url}/api/photos/{sample_id}/original"
        photo_req = make_single_request(photo_url)
        photo_ranges = photo_req.get("headers", {}).get("accept-ranges")
        print(f"  [*] Original Photo Streaming on /api/photos/{sample_id}/original:")
        print(f"      - Accept-Ranges: {photo_ranges} (Pass: {photo_ranges == 'bytes'})")

        print("\n" + "=" * 75)
        print("  2. THROUGHPUT & LATENCY BENCHMARK (CONCURRENCY SCALING)")
        print("=" * 75)

        test_endpoints = [
            ("Static JS (/app.js)", f"{base_url}/app.js"),
            ("Catalog Stats (/api/stats)", f"{base_url}/api/stats"),
            ("Media Page (/api/media?limit=50)", f"{base_url}/api/media?limit=50"),
            ("Full Media List (/api/media?limit=500)", f"{base_url}/api/media?limit=500"),
            ("Search Query (/api/media?search=Egypt)", f"{base_url}/api/media?search=Egypt"),
            ("Thumbnail 256px", thumb_url),
        ]

        print(f"{'Endpoint':<35} | {'c':<2} | {'RPS':>8} | {'p50 (ms)':>8} | {'p90 (ms)':>8} | {'p99 (ms)':>8} | {'Pass Rate':>9}")
        print("-" * 88)

        for name, url in test_endpoints:
            for c in [1, 4, 16, 32]:
                res = benchmark_endpoint(url, num_requests=100, concurrency=c, headers={"Accept-Encoding": "gzip"})
                pass_rate = f"{res['success']}/{res['requests']}"
                print(f"{name:<35} | {c:>2} | {res['rps']:>8.1f} | {res['p50_ms']:>8.2f} | {res['p90_ms']:>8.2f} | {res['p99_ms']:>8.2f} | {pass_rate:>9}")
            print("-" * 88)

        print("\nBenchmark completed successfully!")

    finally:
        if server_proc:
            server_proc.terminate()
            server_proc.wait()

if __name__ == "__main__":
    main()
