#!/bin/sh
# tinyhttp against nginx, one worker each, serving the same files over keep-alive connections.
#   sh bench/run.sh > results/bench.txt
# Needs nginx and wrk (brew install nginx wrk). Both servers listen on 127.0.0.1 only.
set -e
cd "$(dirname "$0")/.."
B=bench/www
mkdir -p $B bench/tmp
head -c 1024 /dev/urandom > $B/1k.bin
head -c 102400 /dev/urandom > $B/100k.bin
cat > bench/tmp/nginx.conf <<CONF
worker_processes 1;
daemon off;
pid $(pwd)/bench/tmp/nginx.pid;
error_log $(pwd)/bench/tmp/error.log;
events { worker_connections 4096; }
http {
    access_log off;
    sendfile on;
    tcp_nopush on;
    keepalive_requests 10000000;
    server { listen 127.0.0.1:18081; root $(pwd)/$B; }
}
CONF
./build/tinyhttp -p 18080 -r $B 2>/dev/null & T=$!
nginx -c "$(pwd)/bench/tmp/nginx.conf" & N=$!
trap 'kill $T $N 2>/dev/null' EXIT
sleep 1
for f in 1k.bin 100k.bin; do
  for c in 1 50; do
    for name in tinyhttp:18080 nginx:18081; do
      echo "== ${name%%:*} $f connections=$c"
      wrk -t1 -c$c -d10s --latency "http://127.0.0.1:${name##*:}/$f" | grep -E "Requests/sec|Transfer/sec|50%|99%|Socket errors|Non-2xx"
    done
  done
done
