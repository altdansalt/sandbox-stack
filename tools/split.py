#!/usr/bin/env python3
"""Split the guest w2c2 stdout stream ("@@FILE <name> <len>\\n" + bytes, repeated) into files in a directory."""
import os, sys
data = open(sys.argv[1], 'rb').read(); outdir = sys.argv[2]; os.makedirs(outdir, exist_ok=True)
i = 0
while i < len(data):
    assert data.startswith(b'@@FILE ', i), (i, data[i:i+40])
    nl = data.index(b'\n', i); name, n = data[i+7:nl].rsplit(b' ', 1); n = int(n)
    open(os.path.join(outdir, os.path.basename(name.decode())), 'wb').write(data[nl+1:nl+1+n]); i = nl + 1 + n
    print(name.decode(), n)
