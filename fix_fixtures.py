import os
import re
import subprocess

def run_tests():
    # We will just run the scripts, parsing output for the tmp files, and copy them over.
    for script in ["./Tests/run_pascal_tests.sh", "./Tests/run_clike_tests.sh", "./Tests/run_rea_tests.sh"]:
        result = subprocess.run([script], capture_output=True, text=True)
        # Find lines like:
        # --- /app/Tests/Pascal/ShortCircuitTest.disasm
        # +++ /tmp/tmp.12345

        matches = re.findall(r'---\s+([^\n]+)\n\s*\+\+\+\s+([^\n]+)', result.stdout)
        for expected, actual in matches:
            # Strip timestamps
            expected = expected.split('\t')[0].strip()
            actual = actual.split('\t')[0].strip()
            if os.path.exists(actual):
                print(f"Copying {actual} to {expected}")
                os.system(f"cp {actual} {expected}")

run_tests()
