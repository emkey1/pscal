#!/usr/bin/env python3
"""Collapse short multi-line control/effect blocks in Aether corpus positives to
the compact one-liner form (valid now that the compiler expands one-liners).

Targets blocks led by if/else/while/for/loop/fx whose body is small (<=2
statements, <=90 chars collapsed, no comments). Collapses innermost-first so a
guard like:

    if !has_toon() {
        fx {
            println("no");
        }
        ret;
    }

becomes:

    if !has_toon() { fx { println("no"); } ret; }

Semantics are identical (the compiler re-expands), which the differential check
verifies. Leaves fn/type blocks and longer bodies multi-line.
"""
import os, re, sys

OPENER = re.compile(r'^(if|else|while|for|loop|fx)\b')
MAX_LEN = 90
MAX_STMTS = 2


def indent_of(line):
    return len(line) - len(line.lstrip())


def collapse_text(text):
    lines = text.split('\n')
    changed = True
    n_collapsed = 0
    while changed:
        changed = False
        i = 0
        while i < len(lines):
            s = lines[i].strip()
            if s.endswith('{') and OPENER.match(s):
                indent = indent_of(lines[i])
                j = i + 1
                body = []
                nested = False
                closed = False
                while j < len(lines):
                    bs = lines[j].strip()
                    if bs == '}' and indent_of(lines[j]) == indent:
                        closed = True
                        break
                    if bs.endswith('{'):        # an un-collapsed nested opener
                        nested = True
                    body.append(bs)
                    j += 1
                stmts = [b for b in body if b]
                if (closed and not nested and 0 < len(stmts) <= MAX_STMTS
                        and not any(b.startswith('//') for b in stmts)):
                    collapsed = lines[i].rstrip() + ' ' + ' '.join(stmts) + ' }'
                    if len(collapsed) <= MAX_LEN:
                        lines[i:j + 1] = [collapsed]
                        changed = True
                        n_collapsed += 1
                        continue
            i += 1
    return '\n'.join(lines), n_collapsed


def main():
    src_dir = sys.argv[1] if len(sys.argv) > 1 else "Tests/aether_specialization/corpus_candidates"
    dst_dir = sys.argv[2] if len(sys.argv) > 2 else "Tests/aether_specialization/corpus_candidates_oneliner"
    os.makedirs(dst_dir, exist_ok=True)
    total_files = total_collapsed = touched = 0
    for name in sorted(os.listdir(src_dir)):
        sp = os.path.join(src_dir, name)
        if not os.path.isfile(sp) or name.endswith('.json'):
            continue
        text = open(sp).read()
        new, nc = collapse_text(text)
        open(os.path.join(dst_dir, name), 'w').write(new)
        total_files += 1
        total_collapsed += nc
        touched += (nc > 0)
    print(f"files={total_files} touched={touched} blocks_collapsed={total_collapsed}")


if __name__ == "__main__":
    main()
