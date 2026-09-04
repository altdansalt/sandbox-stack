#!/bin/sh
# Serve site/ on 0.0.0.0:8000; re-render every 20s.
cd "$(dirname "$0")/.."
ln -sfn .. site/tree
( while true; do python3 tools/mksite.py; sleep 20; done ) &
exec python3 -m http.server 8000 --bind 0.0.0.0 --directory site
