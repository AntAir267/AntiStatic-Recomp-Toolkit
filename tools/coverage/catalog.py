"""Function catalogs for score_recovery.py (pcrecomp's) from AntiStatic's and other tools' output.

    catalog.py gen <gen dir> > candidate.json        what the translator found (gen/extents.txt:
                                                     entry, first and last byte, instructions)
    catalog.py tsv <file> [column] > reference.json  a list with a hex address in a column (0 by
                                                     default): a Ghidra/IDA/REA export, one per line
    catalog.py pdb-or-map: use pdb2_ref.py / map_names.py, which write the same shape

Then: score_recovery.py --reference reference.json --candidate candidate.json
A reference from a disassembler is a second opinion; one from symbols (PDB, MAP) is truth.
"""
import json, os, sys


def from_gen(gen):
    out = []
    for line in open(os.path.join(gen, 'extents.txt')):
        e, lo, hi, n = line.split()
        out.append({'va': int(e, 16), 'end': int(hi, 16), 'entry_kind': 'start'})
    return {'source': 'AntiStatic translator', 'functions': out}


def from_tsv(path, col=0):
    out = []
    for line in open(path):
        f = line.split()
        if len(f) > col:
            try:
                out.append({'va': int(f[col], 16)})
            except ValueError:
                pass                                    # a header line
    return {'source': path, 'functions': out}


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'gen':
        doc = from_gen(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == 'tsv':
        doc = from_tsv(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0)
    else:
        sys.exit(__doc__)
    json.dump(doc, sys.stdout)
