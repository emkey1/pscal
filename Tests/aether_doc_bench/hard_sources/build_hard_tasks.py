import json, subprocess, os
D = "/tmp/hardprobe"
AE = "/home/claw/aether-current/build/aether"
DEST = "/home/claw/pscal-bench/Tests/aether_doc_bench/tasks_hard.json"

GUARD = "checks `has_toon()`; if TOON support is missing, print `yyjson unavailable` and return. Otherwise"

TASKS = [
    dict(id="hard_expense_outliers", title="Two-pass expense outlier report",
         mechanisms=["hard", "two_pass", "toon_file", "real_format", "type_method"],
         ae="hard_a.ae", data="expenses_hard_a.json",
         prompt=('A JSON file named `expenses_hard_a.json` is provided in the working directory, shaped '
                 '`{"expenses": [{"name": Text, "amount": Int}, ...]}`. Write an Aether program that ' + GUARD +
                 ' first compute the mean of all `amount` values; then, for each expense in order, compute its '
                 'absolute deviation from the mean and mark it an outlier when that deviation exceeds half the '
                 'mean. Print one line per expense as `expense <i>: <name> / <amount> / dev <deviation> / '
                 '<isOutlier>` where `<i>` is the zero-based index, `<deviation>` has exactly two decimals, and '
                 '`<isOutlier>` is `true`/`false`. Then print `count = <n>`, `mean = <mean>` (two decimals), '
                 '`outliers = <count>`, and `maxDev = <largest deviation>` (two decimals), each on its own line. '
                 'Close the document and return.')),
    dict(id="hard_log_levels", title="Log-level grouping with dominant level",
         mechanisms=["hard", "group", "argmax", "first_match", "toon_file"],
         ae="hard_b.ae", data="events_hard_b.json",
         prompt=('A JSON file named `events_hard_b.json` is provided in the working directory, shaped '
                 '`{"events": [{"level": Text, "msg": Text}, ...]}` where `level` is one of `info`, `warn`, or '
                 '`error`. Write an Aether program that ' + GUARD + ' print one line per event as '
                 '`event <i>: <level> / <msg>`. Track the count of each level and the index of the first `error` '
                 'event. After the events print `info = <n>`, `warn = <n>`, `error = <n>`, then `dominant = '
                 '<level>` where dominant is the level with the strictly highest count (on a tie prefer `info`, '
                 'then `warn`). Finally print `first error at index <i>`, or `first error: none` if there were no '
                 'errors. Close the document and return.')),
    dict(id="hard_account_ledger", title="Stateful account ledger simulation",
         mechanisms=["hard", "stateful_sim", "branch_on_state", "clamp", "type_method", "toon_file"],
         ae="hard_c.ae", data="ops_hard_c.json",
         prompt=('A JSON file named `ops_hard_c.json` is provided in the working directory, shaped '
                 '`{"cap": Int, "operations": [{"kind": Text, "amount": Int}, ...]}` where `kind` is `deposit`, '
                 '`withdraw`, or `interest`. Write an Aether program that ' + GUARD + ' simulate an account '
                 'starting at balance 0, processing operations in order: a `deposit` adds `amount` then clamps '
                 'the balance down to `cap` if it would exceed it; a `withdraw` succeeds only if the balance is '
                 'at least `amount` (subtracting it), otherwise it is rejected and the balance is unchanged; '
                 '`interest` adds `amount` percent of the current balance using integer division. Print one line '
                 'per operation: deposits as `op <i>: deposit <amount> -> <balance>`, successful withdrawals as '
                 '`op <i>: withdraw <amount> -> <balance>`, rejected withdrawals as `op <i>: withdraw <amount> '
                 'REJECTED (balance <balance>)`, and interest as `op <i>: interest <amount>% -> <balance>`. Count '
                 'an operation as applied when it changes state and as rejected otherwise. After all operations '
                 'print `final balance = <balance>`, `applied = <n>`, and `rejected = <n>`, each on its own line. '
                 'Close the document and return.')),
    dict(id="hard_collatz_recursion", title="Recursive Collatz stopping times",
         mechanisms=["hard", "recursion", "helper_call", "argmax", "toon_file"],
         ae="hard_d.ae", data="numbers_hard_d.json",
         prompt=('A JSON file named `numbers_hard_d.json` is provided in the working directory, shaped '
                 '`{"numbers": [{"n": Int}, ...]}`. Write an Aether program that ' + GUARD + ' for each number '
                 'compute its Collatz stopping time: the number of steps to reach 1, where an even value `v` maps '
                 'to `v / 2` and an odd value maps to `3 * v + 1` (a value of 1 or less has 0 steps). Implement '
                 'this with a recursive function. Print one line per number as `number <i>: <n> -> <steps> '
                 'steps`. After the list print `max steps = <maxSteps> (number <n>)`, naming the number that '
                 'produced the maximum (the first to reach it on a tie). Close the document and return.')),
    dict(id="hard_payroll_nested", title="Nested department payroll rollup",
         mechanisms=["hard", "nested_traversal", "argmax", "toon_file"],
         ae="hard_e.ae", data="org_hard_e.json",
         prompt=('A JSON file named `org_hard_e.json` is provided in the working directory, shaped '
                 '`{"departments": [{"name": Text, "members": [{"name": Text, "salary": Int}, ...]}, ...]}`. '
                 'Write an Aether program that ' + GUARD + ' for each department compute its member count and '
                 'the sum of member salaries. Print one line per department as `dept <i>: <name> / <count> '
                 'members / payroll <sum>`. After all departments print `total members = <n>`, `total payroll = '
                 '<n>`, and `top department = <name> (<payroll>)`, naming the department with the highest payroll '
                 '(the first on a tie). Close the document and return.')),
    dict(id="hard_sensor_streak", title="Adjacent reading deltas and longest run",
         mechanisms=["hard", "adjacent_pairs", "streak", "toon_file"],
         ae="hard_f.ae", data="readings_hard_f.json",
         prompt=('A JSON file named `readings_hard_f.json` is provided in the working directory, shaped '
                 '`{"readings": [{"value": Int}, ...]}`. Write an Aether program that ' + GUARD + ' walk the '
                 'readings in order and, for each reading after the first, print `step <i>: <prev> -> <curr> '
                 '(<delta>)` where `<delta>` is `<curr> - <prev>` (which may be negative) and `<i>` is the '
                 'zero-based index of the current reading. Track the largest single-step increase and the length '
                 'of the longest run of consecutive non-decreasing readings (a single reading is a run of length '
                 '1). After the steps print `maxJump = <n>` and `longest non-decreasing run = <n>`, each on its '
                 'own line. Close the document and return.')),
    dict(id="hard_turnstile_fsm", title="Turnstile state machine",
         mechanisms=["hard", "fsm", "branch_on_state", "toon_file"],
         ae="hard_g.ae", data="events_hard_g.json",
         prompt=('A JSON file named `events_hard_g.json` is provided in the working directory, shaped '
                 '`{"events": [{"e": Text}, ...]}` where `e` is `coin` or `push`. Write an Aether program that '
                 + GUARD + ' drive a turnstile that starts `locked`. For each event: when `locked`, a `coin` '
                 'unlocks it (print `event <i>: coin -> unlocked`) and a `push` is denied (print `event <i>: '
                 'push -> DENIED`); when `unlocked`, a `push` passes through and relocks it (print `event <i>: '
                 'push -> PASS`) and a `coin` is thanked without changing state (print `event <i>: coin -> '
                 'thanks`). Count the passes and the denials. After all events print `passes = <n>`, `denied = '
                 '<n>`, and `final state = <state>`, each on its own line. Close the document and return.')),
    dict(id="hard_word_lengths", title="Word length buckets and longest word",
         mechanisms=["hard", "bucket", "argmax_element", "string_len", "toon_file"],
         ae="hard_h.ae", data="words_hard_h.json",
         prompt=('A JSON file named `words_hard_h.json` is provided in the working directory, shaped '
                 '`{"words": [{"w": Text}, ...]}`. Write an Aether program that ' + GUARD + ' classify each word '
                 'by its character length: `short` when length < 4, `long` when length > 7, and `medium` '
                 'otherwise. Print one line per word as `word <i>: <w> (<length>) <kind>`. Sum the total '
                 'characters across all words and track the single longest word. After the list print `short = '
                 '<n>`, `medium = <n>`, `long = <n>`, `total chars = <n>`, and `longest = <word> (<length>)` '
                 '(the first word to reach the maximum length on a tie). Close the document and return.')),
]

out = []
for t in TASKS:
    r = subprocess.run([AE, t["ae"]], cwd=D, capture_output=True, text=True, timeout=30)
    ref = open(os.path.join(D, t["ae"])).read()
    data = open(os.path.join(D, t["data"])).read()
    out.append({
        "id": t["id"], "title": t["title"], "mechanisms": t["mechanisms"],
        "prompt": t["prompt"], "expected_stdout": r.stdout,
        "files": {t["data"]: data}, "reference_solution": ref,
        "achievability": "model-verified",
    })
    print("== %s == rc=%d bytes=%d stderr=%r" % (t["id"], r.returncode, len(r.stdout), r.stderr[:120]))
    print(r.stdout, end="")
    print("-" * 60)

json.dump({"tasks": out}, open(DEST, "w"), indent=2)
print("WROTE %s with %d tasks" % (DEST, len(out)))
