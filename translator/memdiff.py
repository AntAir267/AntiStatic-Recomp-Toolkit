"""Compare two memory dumps written by --dump-memory and list the differing byte ranges.

    memdiff.py A.mem B.mem [--max N] [--skip LO-HI ...]

--max: how many differing ranges to show (60). --skip: leave out an address range (hex), e.g.
memory only drawing writes, or a thread's stack (from Enter the Matrix's tools/rate.sh).
"""
import sys, struct


def load(path):
    regions = {}
    with open(path, 'rb') as f:
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                break
            base, size = struct.unpack('<II', hdr)
            regions[base] = f.read(size)
    return regions


def ranges(a, b, base):
    """Differing ranges between equal-length byte strings (merged when < 16 bytes apart)."""
    out = []
    n = len(a)
    step = 4096
    for off in range(0, n, step):
        if a[off:off + step] == b[off:off + step]:
            continue
        for i in range(off, min(off + step, n)):
            if a[i] != b[i]:
                if out and base + i - out[-1][1] < 16:
                    out[-1][1] = base + i + 1
                else:
                    out.append([base + i, base + i + 1])
    return out


def clip(rs, skips, a, b, base):
    """The differing ranges without the skipped addresses (each part trimmed to bytes that differ)."""
    for lo, hi in rs:
        parts = [(lo, hi)]
        for s_lo, s_hi in skips:
            parts = [p for q_lo, q_hi in parts
                     for p in ((q_lo, min(q_hi, s_lo)), (max(q_lo, s_hi), q_hi)) if p[0] < p[1]]
        for q_lo, q_hi in parts:
            while q_lo < q_hi and a[q_lo - base] == b[q_lo - base]:
                q_lo += 1
            while q_hi > q_lo and a[q_hi - 1 - base] == b[q_hi - 1 - base]:
                q_hi -= 1
            if q_lo < q_hi:
                yield q_lo, q_hi


def main():
    A, B = load(sys.argv[1]), load(sys.argv[2])
    argv = sys.argv
    limit = int(argv[argv.index('--max') + 1]) if '--max' in argv else 60
    skips = [tuple(int(x, 16) for x in argv[k + 1].split('-')) for k in range(len(argv)) if argv[k] == '--skip']
    total = 0
    for base in sorted(set(A) | set(B)):
        if base not in A or base not in B:
            print('region %08x only in %s' % (base, 'A' if base in A else 'B'))
            continue
        a, b = A[base], B[base]
        if len(a) != len(b):
            print('region %08x size differs %x vs %x' % (base, len(a), len(b)))
            continue
        for lo, hi in clip(ranges(a, b, base), skips, a, b, base):
            total += 1
            if total <= limit:
                sa = a[lo - base:min(hi, lo + 32) - base].hex()
                sb = b[lo - base:min(hi, lo + 32) - base].hex()
                print('%08x-%08x (%d bytes) in region %08x\n    A %s\n    B %s' % (lo, hi, hi - lo, base, sa, sb))
    print('%d differing ranges' % total)


if __name__ == '__main__':
    main()
