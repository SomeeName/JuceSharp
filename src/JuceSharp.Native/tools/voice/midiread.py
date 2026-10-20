"""Minimal SMF reader: returns notes [(track, start_s, end_s, pitch, vel)] honouring tempo map."""
import struct
def _vlq(b, i):
    v = 0
    while True:
        c = b[i]; i += 1
        v = (v << 7) | (c & 0x7f)
        if c < 0x80: return v, i
def read(path):
    b = open(path, 'rb').read()
    assert b[:4] == b'MThd'
    fmt, ntr, div = struct.unpack('>HHH', b[8:14])
    i = 8 + struct.unpack('>I', b[4:8])[0]
    tracks = []
    tempo = []  # (tick, us/qn)
    for t in range(ntr):
        assert b[i:i+4] == b'MTrk', b[i:i+4]
        ln = struct.unpack('>I', b[i+4:i+8])[0]; j = i + 8; end = j + ln; i = end
        tick = 0; run = 0; ev = []
        while j < end:
            d, j = _vlq(b, j); tick += d
            s = b[j]
            if s == 0xFF:
                typ = b[j+1]; l, j2 = _vlq(b, j+2); data = b[j2:j2+l]; j = j2 + l
                if typ == 0x51: tempo.append((tick, int.from_bytes(data, 'big')))
                if typ == 0x03: ev.append((tick, 'name', data.decode('latin1')))
                continue
            if s in (0xF0, 0xF7):
                l, j2 = _vlq(b, j+1); j = j2 + l; continue
            if s & 0x80: run = s; j += 1
            st = run & 0xF0
            if st in (0xC0, 0xD0): d1 = b[j]; j += 1; continue
            d1, d2 = b[j], b[j+1]; j += 2
            if st == 0x90 and d2 > 0: ev.append((tick, 'on', d1, d2, run & 15))
            elif st == 0x80 or (st == 0x90 and d2 == 0): ev.append((tick, 'off', d1, 0, run & 15))
            elif st == 0xE0: ev.append((tick, 'bend', (d2 << 7 | d1) - 8192, 0, run & 15))
        tracks.append(ev)
    tempo.sort()
    if not tempo or tempo[0][0] != 0: tempo.insert(0, (0, 500000))
    def sec(tick):
        s = 0.0; prev_t, prev_us = 0, tempo[0][1]
        for tt, us in tempo[1:]:
            if tt >= tick: break
            s += (tt - prev_t) * prev_us / 1e6 / div; prev_t, prev_us = tt, us
        return s + (tick - prev_t) * prev_us / 1e6 / div
    notes = []; bends = []
    for ti, ev in enumerate(tracks):
        open_ = {}
        for e in ev:
            if e[1] == 'on': open_.setdefault(e[2], []).append((e[0], e[3]))
            elif e[1] == 'off' and open_.get(e[2]):
                t0, v = open_[e[2]].pop(0); notes.append((ti, sec(t0), sec(e[0]), e[2], v))
            elif e[1] == 'bend': bends.append((ti, sec(e[0]), e[2]))
    notes.sort(key=lambda n: n[1])
    return notes, bends
