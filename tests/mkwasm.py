#!/usr/bin/env python3
"""Hand-assemble minimal wasm modules that exercise the runtime's safety checks.
No deps. Each imports env.read/env.write/env.exit and exports _start (func).
Usage: python3 tests/mkwasm.py <outdir>
Emits: oob_load, mem_init_oob, elem_oob, callind_typemismatch, positive."""
import struct, sys, os

def u(n):
    out = b''
    while True:
        b = n & 0x7f; n >>= 7
        if n: out += bytes([b | 0x80])
        else: return out + bytes([b])

def s(n):
    out = b''
    while True:
        b = n & 0x7f; n >>= 7
        if (n == 0 and not (b & 0x40)) or (n == -1 and (b & 0x40)):
            return out + bytes([b])
        out += bytes([b | 0x80])

def vec(items): return u(len(items)) + b''.join(items)
def sect(sid, body): return bytes([sid]) + u(len(body)) + body
def name(x): b = x.encode(); return u(len(b)) + b

I32 = 0x7f
# opcodes
BLOCK_VOID=b'\x40'
END=b'\x0b'
def i32c(n): return b'\x41'+s(n)
CALL=b'\x10'; CALL_INDIRECT=b'\x11'; DROP=b'\x1a'
I32_LOAD=b'\x28'; RETURN=b'\x0f'

# type section shared shape: type0 = (i32,i32,i32)->i32 (read/write), type1=(i32)->void (exit),
# type2 = ()->void (_start), type3 = (i32)->i32
def types():
    t0 = b'\x60'+vec([bytes([I32])]*3)+vec([bytes([I32])])   # (i32,i32,i32)->i32
    t1 = b'\x60'+vec([bytes([I32])])+vec([])                 # (i32)->()
    t2 = b'\x60'+vec([])+vec([])                             # ()->()
    t3 = b'\x60'+vec([bytes([I32])])+vec([bytes([I32])])     # (i32)->i32
    return sect(1, vec([t0,t1,t2,t3]))

def imports():
    # env.read:type0 env.write:type0 env.exit:type1  -> function indices 0,1,2
    imp = lambda m,n,ti: name(m)+name(n)+b'\x00'+u(ti)
    return sect(2, vec([imp("env","read",0), imp("env","write",0), imp("env","exit",1)]))

MAGIC = b'\x00asm\x01\x00\x00\x00'

def module(funcsecs):
    return MAGIC + b''.join(funcsecs)

def write_data_segment_passive(data):
    # data section: 1 passive segment (flag 1) with bytes
    return sect(11, vec([b'\x01'+vec([bytes([c]) for c in data])]))

def emit(outdir):
    os.makedirs(outdir, exist_ok=True)
    mods = {}

    # ---- positive: _start writes "ok\n" to fd1, exits 0 ----
    # need a data segment at mem offset 0 with "ok\n"; memory min1
    body = (u(0)  # locals
            + i32c(1)+i32c(0)+i32c(3)+CALL+u(1)  # write(1,0,3)
            + DROP
            + i32c(0)+CALL+u(2)  # exit(0)
            + END)
    m = module([
        types(), imports(),
        sect(3, vec([u(2)])),                # funcs: local func0 -> type2
        sect(5, vec([b'\x00'+u(1)])),        # memory min 1
        sect(7, vec([name("_start")+b'\x00'+u(3)])),  # export _start = func idx 3
        sect(10, vec([u(len(body))+body])),
        sect(11, vec([b'\x00'+i32c(0)+END+vec([bytes([c]) for c in b'ok\n'])])),  # active data at 0
    ])
    mods['positive'] = m

    # ---- oob_load: load i32 from a huge address ----
    body = (u(0)
            + i32c(0x7fffff00)+I32_LOAD+b'\x02'+u(0)  # align2 offset0
            + DROP
            + i32c(0)+CALL+u(2) + END)
    mods['oob_load'] = module([
        types(), imports(),
        sect(3, vec([u(2)])),
        sect(5, vec([b'\x00'+u(1)])),
        sect(7, vec([name("_start")+b'\x00'+u(3)])),
        sect(10, vec([u(len(body))+body])),
    ])

    # ---- mem_init_oob: passive segment of 3 bytes, memory.init src=0 len=100 (past segment) ----
    # memory.init: 0xfc 0x08 dataidx memidx ; args dest,src,len on stack
    body = (u(0)
            + i32c(0)      # dest
            + i32c(0)      # src
            + i32c(100)    # len (> segment length 3)
            + b'\xfc\x08'+u(0)+u(0)
            + i32c(0)+CALL+u(2) + END)
    mods['mem_init_oob'] = module([
        types(), imports(),
        sect(3, vec([u(2)])),
        sect(5, vec([b'\x00'+u(1)])),
        sect(7, vec([name("_start")+b'\x00'+u(3)])),
        # datacount section (12) required for memory.init
        sect(12, u(1)),
        sect(10, vec([u(len(body))+body])),
        write_data_segment_passive(b'abc'),
    ])

    # ---- elem_oob: table size 2, active elem segment at offset 5 (past end) ----
    # func3=_start, need a target func4:type3. table min2. elem: offset 5, [func4]
    start = (u(0) + i32c(0)+CALL+u(2) + END)
    tgt = (u(0) + i32c(0) + END)  # (i32)->i32 returns 0 (ignores arg -> just push 0)
    elem = b'\x00'+i32c(5)+END+vec([u(4)])  # active, table0, offset 5, funcidx 4
    mods['elem_oob'] = module([
        types(), imports(),
        sect(3, vec([u(2), u(3)])),          # func3:type2(_start), func4:type3
        sect(4, vec([b'\x70'+b'\x00'+u(2)])),# table: funcref, min2
        sect(5, vec([b'\x00'+u(1)])),
        sect(7, vec([name("_start")+b'\x00'+u(3)])),
        sect(9, vec([elem])),
        sect(10, vec([u(len(start))+start, u(len(tgt))+tgt])),
    ])

    # ---- callind_typemismatch: table[0]=func with type3 (i32)->i32, call_indirect as type0 (i32,i32,i32)->i32 ----
    # _start: push 3 i32 args, push table index 0, call_indirect type0
    start = (u(0)
             + i32c(0)+i32c(0)+i32c(0)   # 3 dummy args
             + i32c(0)                    # table index
             + CALL_INDIRECT+u(0)+u(0)    # type index 0, table 0
             + DROP
             + i32c(0)+CALL+u(2) + END)
    tgt = (u(0) + i32c(0) + END)  # type3
    elem = b'\x00'+i32c(0)+END+vec([u(4)])  # table0 offset0 [func4]
    mods['callind_typemismatch'] = module([
        types(), imports(),
        sect(3, vec([u(2), u(3)])),
        sect(4, vec([b'\x70'+b'\x00'+u(1)])),  # table min1
        sect(5, vec([b'\x00'+u(1)])),
        sect(7, vec([name("_start")+b'\x00'+u(3)])),
        sect(9, vec([elem])),
        sect(10, vec([u(len(start))+start, u(len(tgt))+tgt])),
    ])

    for k, v in mods.items():
        open(os.path.join(outdir, k+'.wasm'), 'wb').write(v)
        print(k, len(v))

if __name__ == '__main__':
    emit(sys.argv[1] if len(sys.argv) > 1 else 'build/adv')
