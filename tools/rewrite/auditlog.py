#!/usr/bin/env python3
"""auditlog.py PREFIX [--all]  (or: auditlog.py T_FILE R_FILE [--all])

Compares the register audit's two-run log (runtime/rewrite/audit.c, <PORT_ENV>_AUDIT_LOG=PREFIX;
docs/REWRITE.md "The register audit"): PREFIX.a.txt, written by a run of the translation
(<PORT_ENV>_DECOMP_OFF=all), against PREFIX.b.txt, written by a run of the rewrites. For each
function the two runs' calls are compared in order: what each call left (eax; ebx, esi, edi, ebp
kept or their new value; esp; the return address kept or not; df; the x87 values pushed; the
argument slots, kept or their new value). One line per finding, T the translation, R the rewrite:

  ADDR DIFF call J: eax T=.. R=.. slot1 T=.. R=..   call J left something else, with the same
                                                    inputs (eax, ecx, edx) coming in
  ADDR DIFF calls A-B: ...                          the same past the first 16 calls (digests)
  ADDR SLOTS call J: slot1 T=.. R=.. (moved)        only argument slots differ, with the registers
                                                    the same: "moved" (the value is in another slot of
                                                    the other run: the callers' frames are laid out
                                                    differently) or "one run only" (kept in the other):
                                                    often a callee writing through a pointer into its
                                                    caller's frame, no leftover; counted apart from DIFF
  ADDR INPUTS call J: ...                           left otherwise, but the inputs differed too (a
                                                    caller's leftovers: often the effect of another
                                                    finding); listed with --all, else only counted
  ADDR COUNT T n=.. R n=..                          called a different number of times (a rewrite
                                                    that does a callee's work itself, or a route
                                                    that went elsewhere)
  ADDR ONLY-T / ONLY-R n=..                         called in one run only

and a totals line. Values: '=' kept as it came in, 'stk' a stack address (the runs' frames differ).
Exit status 1 when there is a DIFF (a SLOTS line alone is 0), else 0 (2: a file missing).
"""
import sys

OUT = ['eax', 'ebx', 'esi', 'edi', 'ebp', 'esp', 'ret', 'df', 'fpu']


def load(path):
    funcs = {}
    try:
        f = open(path)
    except OSError:
        return None
    with f:
        for line in f:
            w = line.split()
            if len(w) < 3 or line.startswith('#'):
                continue
            a = w[0]
            if w[1].startswith('n='):
                kv = dict(x.split('=', 1) for x in w[1:])
                cps = []
                if kv['cp'] != '-':
                    for c in kv['cp'].split(','):
                        n, i, o, *g = c.split(':')
                        cps.append((int(n), i, o, g[0] if g else o))
                funcs[a] = {'n': int(kv['n']), 'declined': int(kv['declined']), 'in': kv['in'],
                            'out': kv['out'], 'reg': kv.get('reg', kv['out']), 'cp': cps, 'rec': [], 'kv': kv}
            elif w[1].startswith('#') and a in funcs:
                kv = dict(x.split('=', 1) for x in w[2:])
                funcs[a]['rec'].append({'in': kv['in'].split(','), 'out': kv['out'].split(','),
                                        'slots': kv['slots'].split(',')})
    return funcs


def slot_name(k):
    return 'retslot' if k == 0 else 'slot%d' % (k - 1)


def what_differs(r, q):
    parts = []
    for k, name in enumerate(OUT):
        a, b = r['out'][k], q['out'][k]
        if a != b:
            if name == 'esp':
                a, b = 'entry%+d' % to_signed(a), 'entry%+d' % to_signed(b)
            parts.append('%s T=%s R=%s' % (name, a, b))
    ns = max(len(r['slots']), len(q['slots']))
    for k in range(ns):
        a = r['slots'][k] if k < len(r['slots']) else '-'
        b = q['slots'][k] if k < len(q['slots']) else '-'
        if a != b:
            parts.append('%s T=%s R=%s' % (slot_name(k), a, b))
    return ' '.join(parts)


def slot_kind(r, q):
    """a slot-only difference described slot by slot: moved, or written in one run only"""
    parts = []
    ns = max(len(r['slots']), len(q['slots']))
    for k in range(ns):
        a = r['slots'][k] if k < len(r['slots']) else '-'
        b = q['slots'][k] if k < len(q['slots']) else '-'
        if a == b:
            continue
        # moved first: a value written in this slot in one run sits in another slot in the other
        # (each half of a move is "kept" in one run, so the kept test alone would hide it)
        if (a not in ('=', '-') and a in q['slots']) or (b not in ('=', '-') and b in r['slots']):
            kind = 'moved'
        elif a == '=' or b == '=':
            kind = 'one run only'
        else:
            kind = 'other value'
        parts.append('%s T=%s R=%s (%s)' % (slot_name(k), a, b, kind))
    return ' '.join(parts)


def to_signed(h):
    v = int(h, 16)
    return v - (1 << 32) if v & 0x80000000 else v


def main(argv):
    show_all = '--all' in argv
    args = [a for a in argv if a != '--all']
    if len(args) == 1:
        tpath, rpath = args[0] + '.a.txt', args[0] + '.b.txt'
    elif len(args) == 2:
        tpath, rpath = args
    else:
        print(__doc__.strip())
        return 2
    T, R = load(tpath), load(rpath)
    if T is None or R is None:
        print('auditlog: missing log (%s, %s)' % (tpath if T is None else '', rpath if R is None else ''))
        return 2
    same = diff = inputs = count = only = slots = 0
    for a in sorted(set(T) | set(R)):
        x, y = T.get(a), R.get(a)
        if not x or not y:
            only += 1
            print('%s ONLY-%s n=%d' % (a, 'T' if x else 'R', (x or y)['n']))
            continue
        if x['n'] != y['n']:
            count += 1
            print('%s COUNT T n=%d R n=%d' % (a, x['n'], y['n']))
            continue
        if x['out'] == y['out']:
            same += 1
            continue
        found = None
        weak = None
        # the registers' digests agree and the full ones don't: only slots differ, whatever came in
        slot_only = x['reg'] == y['reg'] and 'reg' in x['kv'] and 'reg' in y['kv']
        if slot_only:
            first = next(('call %d: %s' % (j, slot_kind(r, q)) for j, (r, q) in enumerate(zip(x['rec'], y['rec']))
                          if r['slots'] != q['slots']), 'past the first %d calls' % len(x['rec']))
            slots += 1
            print('%s SLOTS %s' % (a, first))
            continue
        for j, (r, q) in enumerate(zip(x['rec'], y['rec'])):
            if r['out'] == q['out'] and r['slots'] == q['slots']:
                continue
            if r['out'] == q['out'] and x['reg'] == y['reg']:
                continue                    # (slots only: counted with the registers below if they differ later)
            if r['in'] == q['in']:
                found = 'call %d: %s' % (j, what_differs(r, q))
                break
            if weak is None:
                weak = 'call %d: %s (in T=%s R=%s)' % (j, what_differs(r, q), ','.join(r['in']), ','.join(q['in']))
        if found is None and weak is None:
            # past the full records: the first checkpoint whose digests part
            prev = len(x['rec'])          # (calls are counted from 0; a checkpoint n covers calls 0..n-1)
            for (n, ti, to, tg), (_, ri, ro, rg) in zip(x['cp'], y['cp']):
                if n <= prev:
                    continue                # (within the full records, compared above)
                if (tg != rg) if x['reg'] != y['reg'] else (to != ro):
                    if ti == ri:
                        found = 'calls %d-%d: something left otherwise, the same inputs so far' % (prev, n - 1)
                    else:
                        weak = 'calls %d-%d: something left otherwise, the inputs differ too' % (prev, n - 1)
                    break
                prev = n
            else:
                if x['in'] == y['in']:
                    found = 'after call %d: something left otherwise, the same inputs' % prev
                else:
                    weak = 'after call %d: something left otherwise, the inputs differ too' % prev
        if found is not None:
            diff += 1
            print('%s DIFF %s%s' % (a, found, '' if not y['declined'] else ' (the rewrite declined %d calls)' % y['declined']))
        else:
            inputs += 1
            if show_all:
                print('%s INPUTS %s' % (a, weak))
    total = len(set(T) | set(R))
    print('auditlog: %d functions: %d same, %d differ, %d differ in argument slots only, %d differ with other '
          'inputs coming in, %d call counts differ, %d in one run only' % (total, same, diff, slots, inputs, count, only))
    return 1 if diff else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
