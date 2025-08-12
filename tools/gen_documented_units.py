#!/usr/bin/env python3
import re
from pathlib import Path

def main():
    doc_path = Path('Docs/pscal_overview.md')
    content = doc_path.read_text(encoding='utf-8')
    try:
        section = content.split('## Included Units', 1)[1]
        section = section.split('## Summary', 1)[0]
    except IndexError:
        raise SystemExit('Could not find "## Included Units" section in documentation.')
    units = re.findall(r'^###\s+(\w+)', section, flags=re.MULTILINE)
    units_lower = [u.lower() for u in units]

    header_path = Path('src/documented_units.h')
    with header_path.open('w', encoding='utf-8') as f:
        f.write('// Generated from Docs/pscal_overview.md; do not edit manually.\n')
        f.write('#ifndef DOCUMENTED_UNITS_H\n#define DOCUMENTED_UNITS_H\n\n')
        f.write('#include <stddef.h>\n\n')
        f.write('static const char *documented_units[] = {\n')
        for u in units_lower:
            f.write(f'    "{u}",\n')
        f.write('};\n')
        f.write('static const size_t documented_units_count = sizeof(documented_units) / sizeof(documented_units[0]);\n\n')
        f.write('#endif // DOCUMENTED_UNITS_H\n')

if __name__ == '__main__':
    main()
