"""D12 gate number: share of first-attempt failures whose first coded diagnostic is FX-001.
usage: fx_gate.py LABEL=DIR_OR_GLOB ...   (medium-guide B0 units; .json or .json.gz)"""
import collections, glob, gzip, json, os, re, sys
def load(p):
    return json.loads(gzip.decompress(open(p, 'rb').read())) if p.endswith('.gz') else json.load(open(p))
rows = collections.OrderedDict()
for spec in sys.argv[1:]:
    label, pat = spec.split('=', 1)
    for p in sorted(glob.glob(pat)):
        r = load(p)
        dest = r['destinations'][0]['destination_id'] if 'destination_id' in r['destinations'][0] else os.path.basename(p).split('__')[0]
        for v in r['destinations'][0]['variants']:
            if v.get('doc_name') != 'medium': continue
            for c in v['results']:
                a0 = c['attempts'][0] if c.get('attempts') else c
                run = a0.get('run') or {}
                key = (label, dest)
                d = rows.setdefault(key, collections.Counter())
                d['attempts'] += 1
                if run.get('exact_stdout_match'): d['pass'] += 1; continue
                d['fail'] += 1
                m = re.search(r'\[([A-Z]+-\d+)\]', run.get('stderr') or '')
                if m and m.group(1) == 'FX-001': d['fx001'] += 1
                if m and m.group(1) == 'FX-001' and 'toon_parse_file' in (run.get('stderr') or '') + (a0.get('source_code') or ''): d['fx001_file'] += 1
print(f"{'set':10s} {'model':26s} attempts  fail  FX-001  share of failures  share of attempts")
tot = collections.defaultdict(collections.Counter)
for (label, dest), d in rows.items():
    print(f"{label:10s} {dest:26s} {d['attempts']:8d} {d['fail']:5d} {d['fx001']:7d}  {100*d['fx001']/max(d['fail'],1):6.1f}%  {100*d['fx001']/d['attempts']:6.1f}%   (file-read programs {d['fx001_file']})")
    for k in ('all', 'all but prism-coder') if 'prism' not in dest else ('all',):
        tot[k].update(d)
for k, d in tot.items():
    print(f"{'POOLED':10s} {k:26s} {d['attempts']:8d} {d['fail']:5d} {d['fx001']:7d}  {100*d['fx001']/max(d['fail'],1):6.1f}%  {100*d['fx001']/d['attempts']:6.1f}%")
