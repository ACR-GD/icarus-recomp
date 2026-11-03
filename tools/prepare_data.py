#!/usr/bin/env python3
"""Build the runtime data folder from the original Icarus / Droiyan disc images (BIN/CUE).

usage: prepare_data.py CD1.cue CD2.cue outdir [--is3unpack path]

  outdir/                 game install (what SETUP.EXE would copy): ICARUS.EXE, ROOT.Z files,
  outdir/edata, tdata     EDATA.Z / TDATA.Z contents
  outdir/droiyan.ini      CD drive path ("D:\\") the game reads at start-up
  outdir/cd/              CD contents the game reads at run time (MAP\\*.MAP, OPEN.AVI, ENDING.AVI)
  outdir/cd/audio/        CD-DA music tracks as 44.1 kHz stereo WAV files (track02.wav ...)

Standard library only, so it runs anywhere Python 3 does.
"""
import os, sys, struct, subprocess, shlex

def parse_cue(path):
    d = os.path.dirname(os.path.abspath(path))
    tracks, binf = [], None
    for line in open(path, encoding='latin-1'):
        w = shlex.split(line.strip())
        if not w: continue
        if w[0] == 'FILE': binf = os.path.join(d, w[1])
        elif w[0] == 'TRACK': tracks.append({'no': int(w[1]), 'mode': w[2], 'file': binf, 'start': None})
        elif w[0] == 'INDEX' and w[1] == '01':
            m, s, f = map(int, w[2].split(':')); tracks[-1]['start'] = (m * 60 + s) * 75 + f
    size = os.path.getsize(binf)
    for k, t in enumerate(tracks):
        t['end'] = tracks[k + 1]['start'] if k + 1 < len(tracks) else size // 2352
    return tracks

class Iso:
    """ISO9660 reader over a raw MODE1/2352 track."""
    def __init__(self, track):
        self.f = open(track['file'], 'rb'); self.base = track['start']
        pvd = self.sector(16)
        assert pvd[1:6] == b'CD001', 'no ISO9660 volume'
        self.root = pvd[156:156 + 34]
    def sector(self, lba, n=1):
        out = bytearray()
        for k in range(n):
            self.f.seek((self.base + lba + k) * 2352 + 16)
            out += self.f.read(2048)
        return bytes(out)
    def read_extent(self, lba, size, out):
        left = size; s = lba
        while left > 0:
            chunk = min(left, 2048 * 256)
            n = (chunk + 2047) // 2048
            out.write(self.sector(s, n)[:chunk]); left -= chunk; s += n
    def walk(self, rec=None, path=''):
        rec = rec or self.root
        lba, size = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
        data = self.sector(lba, (size + 2047) // 2048)
        p = 0
        while p < size:
            n = data[p]
            if n == 0: p = (p // 2048 + 1) * 2048; continue
            r = data[p:p + n]; p += n
            nl = r[32]; name = r[33:33 + nl]
            if name in (b'\0', b'\1'): continue
            name = name.decode('latin-1').split(';')[0].rstrip('.')
            if r[25] & 2: yield from self.walk(r, path + name + '/')
            else: yield path + name, struct.unpack_from('<I', r, 2)[0], struct.unpack_from('<I', r, 10)[0]

def extract_iso(track, out, only=None):
    iso = Iso(track)
    for name, lba, size in iso.walk():
        if only and not only(name): continue
        dst = os.path.join(out, *name.split('/'))
        if os.path.exists(dst) and os.path.getsize(dst) == size: continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as o: iso.read_extent(lba, size, o)
        print('  ', name, size)

def extract_audio(track, dst):
    frames = track['end'] - track['start']
    n = frames * 2352
    if os.path.exists(dst) and os.path.getsize(dst) == n + 44: return
    with open(track['file'], 'rb') as f, open(dst, 'wb') as o:
        o.write(b'RIFF' + struct.pack('<I', 36 + n) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 2, 44100, 176400, 4, 16)
                + b'data' + struct.pack('<I', n))
        f.seek(track['start'] * 2352)
        left = n
        while left:
            b = f.read(min(left, 2352 * 1000)); o.write(b); left -= len(b)
    print('   audio track %02d (%d s)' % (track['no'], frames // 75))

def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    if len(a) != 3: print(__doc__); sys.exit(1)
    cd1, cd2, out = a
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'is3unpack')
    if '--is3unpack' in sys.argv: tool = sys.argv[sys.argv.index('--is3unpack') + 1]
    t1, t2 = parse_cue(cd1), parse_cue(cd2)
    os.makedirs(out, exist_ok=True)
    stage = os.path.join(out, '_disc1')
    print('CD1: extracting installer archives')
    extract_iso(t1[0], stage, lambda n: n.upper().endswith('.Z'))
    print('CD1: unpacking InstallShield archives')
    for z, sub in (('EXE.Z', ''), ('ROOT.Z', ''), ('EDATA.Z', 'edata'), ('TDATA.Z', 'tdata')):
        dst = os.path.join(out, sub) if sub else out
        os.makedirs(dst, exist_ok=True)
        subprocess.check_call([tool, os.path.join(stage, z), dst])
    for f in os.listdir(stage): os.remove(os.path.join(stage, f))
    os.rmdir(stage)
    with open(os.path.join(out, 'droiyan.ini'), 'wb') as f: f.write(b'D:\\')
    print('CD2: copying run-time CD files')
    cd = os.path.join(out, 'cd')
    extract_iso(t2[0], cd)
    print('CD2: extracting music tracks')
    os.makedirs(os.path.join(cd, 'audio'), exist_ok=True)
    for t in t2:
        if t['mode'] == 'AUDIO': extract_audio(t, os.path.join(cd, 'audio', 'track%02d.wav' % t['no']))
    print('done:', out)

if __name__ == '__main__':
    main()
