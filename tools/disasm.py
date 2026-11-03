"""dis.py EXE addr [count] : disassemble with import names resolved."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE
from capstone import *
p = PE(sys.argv[1]); a = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 60
iat = {x: name for x, d, name in p.imports}
md = Cs(CS_ARCH_X86, CS_MODE_32)
for i in md.disasm(p.bytes_at(a, n * 8), a):
    s = i.op_str
    for x, nm in iat.items():
        if hex(x) in s: s = s.replace(hex(x), nm)
    print('%08x  %-6s %s' % (i.address, i.mnemonic, s)); n -= 1
    if n <= 0: break
