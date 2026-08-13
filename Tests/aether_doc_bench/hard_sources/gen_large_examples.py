import json, subprocess, os
from string import Template

AE = "/home/claw/aether-current/build/aether"
WORK = "/tmp/large_gen"
os.makedirs(WORK, exist_ok=True)
SYS = "You generate canonical Aether. When asked for code, output raw Aether source only."

# ---------------------------------------------------------------- shape A: two-pass deviation
A = Template('''@pure
fn absReal(x: Real) -> Real {
    if x < 0.0 { ret 0.0 - x; }
    ret x;
}

type Stats {
    flagged: Int;
    maxDev: Real;

    fn note(dev: Real, hit: Bool) -> Void {
        if hit { self.flagged = self.flagged + 1; }
        if dev > self.maxDev { self.maxDev = dev; }
        ret;
    }
}

fn main() -> Void {
    if !has_toon() { fx { println("yyjson unavailable"); } ret; }

    let doc: ToonDoc = toon_parse_file("$datafile");
    let root: ToonNode = toon_root(doc);
    let items: ToonNode = toon_key(root, "$coll");
    let n: Int = toon_len(items);

    let total: Int = 0;
    loop i in 0..n {
        let e: ToonNode = toon_at(items, i);
        total = total + toon_get_int_or(e, "$vf", 0);
    }
    let mean: Real = 0.0;
    if n > 0 { mean = total * 1.0 / n; }
    let threshold: Real = mean * 0.5;

    let stats: Stats = new Stats();
    loop i in 0..n {
        let e: ToonNode = toon_at(items, i);
        let name: Text = toon_get_text_or(e, "$nf", "?");
        let v: Int = toon_get_int_or(e, "$vf", 0);
        let dev: Real = absReal(v * 1.0 - mean);
        let hit: Bool = false;
        if dev > threshold { hit = true; }
        stats.note(dev, hit);
        fx { println("$row ", i, ": ", name, " / ", v, " / dev ", dev:0:2, " / ", hit); }
    }
    fx {
        println("count = ", n);
        println("mean = ", mean:0:2);
        println("$flag = ", stats.flagged);
        println("maxDev = ", stats.maxDev:0:2);
    }
    toon_close(doc);
    ret;
}
''')

# ---------------------------------------------------------------- shape B: group + argmax + first-match
B = Template('''fn main() -> Void {
    if !has_toon() { fx { println("yyjson unavailable"); } ret; }

    let doc: ToonDoc = toon_parse_file("$datafile");
    let root: ToonNode = toon_root(doc);
    let items: ToonNode = toon_key(root, "$coll");
    let n: Int = toon_len(items);

    let c1: Int = 0;
    let c2: Int = 0;
    let c3: Int = 0;
    let firstSpecial: Int = -1;
    loop i in 0..n {
        let ev: ToonNode = toon_at(items, i);
        let cat: Text = toon_get_text_or(ev, "$cf", "$v1");
        let label: Text = toon_get_text_or(ev, "$lf", "");
        if cat == "$v1" { c1 = c1 + 1; }
        if cat == "$v2" { c2 = c2 + 1; }
        if cat == "$v3" {
            c3 = c3 + 1;
            if firstSpecial < 0 { firstSpecial = i; }
        }
        fx { println("$row ", i, ": ", cat, " / ", label); }
    }
    let dom: Text = "$v1";
    let domCount: Int = c1;
    if c2 > domCount { dom = "$v2"; domCount = c2; }
    if c3 > domCount { dom = "$v3"; domCount = c3; }
    fx {
        println("$v1 = ", c1);
        println("$v2 = ", c2);
        println("$v3 = ", c3);
        println("dominant = ", dom);
    }
    if firstSpecial < 0 { fx { println("first $v3: none"); } }
    if firstSpecial >= 0 { fx { println("first $v3 at index ", firstSpecial); } }
    toon_close(doc);
    ret;
}
''')

# ---------------------------------------------------------------- shape C: stateful sim w/ rejection + clamp
C = Template('''type Acc {
    level: Int;
    applied: Int;
    rejected: Int;

    fn add(amount: Int, cap: Int) -> Void {
        self.level = self.level + amount;
        if self.level > cap { self.level = cap; }
        self.applied = self.applied + 1;
        ret;
    }

    fn remove(amount: Int) -> Bool {
        if amount > self.level {
            self.rejected = self.rejected + 1;
            ret false;
        }
        self.level = self.level - amount;
        self.applied = self.applied + 1;
        ret true;
    }

    fn scale(pct: Int) -> Void {
        let delta: Int = self.level * pct / 100;
        self.level = self.level + delta;
        self.applied = self.applied + 1;
        ret;
    }
}

fn main() -> Void {
    if !has_toon() { fx { println("yyjson unavailable"); } ret; }

    let doc: ToonDoc = toon_parse_file("$datafile");
    let root: ToonNode = toon_root(doc);
    let ops: ToonNode = toon_key(root, "$coll");
    let cap: Int = toon_get_int_or(root, "cap", 1000000);

    let acc: Acc = new Acc();
    loop i in 0..toon_len(ops) {
        let op: ToonNode = toon_at(ops, i);
        let kind: Text = toon_get_text_or(op, "kind", "");
        let amount: Int = toon_get_int_or(op, "amount", 0);
        if kind == "$add" {
            acc.add(amount, cap);
            fx { println("op ", i, ": $add ", amount, " -> ", acc.level); }
        }
        if kind == "$rem" {
            let ok: Bool = acc.remove(amount);
            if ok { fx { println("op ", i, ": $rem ", amount, " -> ", acc.level); } }
            if !ok { fx { println("op ", i, ": $rem ", amount, " REJECTED (level ", acc.level, ")"); } }
        }
        if kind == "$scl" {
            acc.scale(amount);
            fx { println("op ", i, ": $scl ", amount, "% -> ", acc.level); }
        }
    }
    fx {
        println("final $levelword = ", acc.level);
        println("applied = ", acc.applied);
        println("rejected = ", acc.rejected);
    }
    toon_close(doc);
    ret;
}
''')

# ---------------------------------------------------------------- shape D: recursion + argmax  ($recfn provides the helper)
D = Template('''$recfn

fn main() -> Void {
    if !has_toon() { fx { println("yyjson unavailable"); } ret; }

    let doc: ToonDoc = toon_parse_file("$datafile");
    let root: ToonNode = toon_root(doc);
    let nums: ToonNode = toon_key(root, "$coll");
    let n: Int = toon_len(nums);

    let maxVal: Int = -1;
    let maxNum: Int = 0;
    loop i in 0..n {
        let elem: ToonNode = toon_at(nums, i);
        let num: Int = toon_get_int_or(elem, "$vf", 0);
        let r: Int = $call(num);
        if r > maxVal { maxVal = r; maxNum = num; }
        fx { println("$row ", i, ": ", num, " -> ", r, " $unit"); }
    }
    fx { println("$maxlabel = ", maxVal, " (number ", maxNum, ")"); }
    toon_close(doc);
    ret;
}
''')

# ---------------------------------------------------------------- shape E: nested traversal + argmax
E = Template('''fn main() -> Void {
    if !has_toon() { fx { println("yyjson unavailable"); } ret; }

    let doc: ToonDoc = toon_parse_file("$datafile");
    let root: ToonNode = toon_root(doc);
    let groups: ToonNode = toon_key(root, "$outer");

    let totalItems: Int = 0;
    let totalSum: Int = 0;
    let topSum: Int = -1;
    let topName: Text = "none";

    loop g in 0..toon_len(groups) {
        let grp: ToonNode = toon_at(groups, g);
        let name: Text = toon_get_text_or(grp, "name", "?");
        let members: ToonNode = toon_key(grp, "$inner");
        let count: Int = toon_len(members);
        let sum: Int = 0;
        loop m in 0..count {
            let item: ToonNode = toon_at(members, m);
            sum = sum + toon_get_int_or(item, "$vf", 0);
        }
        totalItems = totalItems + count;
        totalSum = totalSum + sum;
        if sum > topSum { topSum = sum; topName = name; }
        fx { println("$row ", g, ": ", name, " / ", count, " $itemword / $sumword ", sum); }
    }
    fx {
        println("total $itemword = ", totalItems);
        println("total $sumword = ", totalSum);
        println("top $row = ", topName, " (", topSum, ")");
    }
    toon_close(doc);
    ret;
}
''')

# ---------------------------------------------------------------- shape F: adjacent-pair + streak
F = Template('''fn main() -> Void {
    if !has_toon() { fx { println("yyjson unavailable"); } ret; }

    let doc: ToonDoc = toon_parse_file("$datafile");
    let root: ToonNode = toon_root(doc);
    let series: ToonNode = toon_key(root, "$coll");
    let n: Int = toon_len(series);

    let prev: Int = 0;
    let havePrev: Bool = false;
    let maxJump: Int = 0;
    let run: Int = 1;
    let bestRun: Int = 1;

    loop i in 0..n {
        let r: ToonNode = toon_at(series, i);
        let v: Int = toon_get_int_or(r, "$vf", 0);
        if havePrev {
            let jump: Int = v - prev;
            if jump > maxJump { maxJump = jump; }
            if v >= prev { run = run + 1; }
            if v < prev { run = 1; }
            if run > bestRun { bestRun = run; }
            fx { println("step ", i, ": ", prev, " -> ", v, " (", jump, ")"); }
        }
        prev = v;
        havePrev = true;
    }
    fx {
        println("$jumpword = ", maxJump);
        println("$runword = ", bestRun);
    }
    toon_close(doc);
    ret;
}
''')

# ---------------------------------------------------------------- shape G: two-state FSM
G = Template('''fn main() -> Void {
    if !has_toon() { fx { println("yyjson unavailable"); } ret; }

    let doc: ToonDoc = toon_parse_file("$datafile");
    let root: ToonNode = toon_root(doc);
    let events: ToonNode = toon_key(root, "$coll");

    let state: Text = "$s1";
    let passes: Int = 0;
    let denied: Int = 0;

    loop i in 0..toon_len(events) {
        let ev: ToonNode = toon_at(events, i);
        let e: Text = toon_get_text_or(ev, "e", "");
        let cur: Text = state;
        if cur == "$s1" {
            if e == "$e1" { state = "$s2"; fx { println("event ", i, ": $e1 -> $s2"); } }
            if e == "$e2" { denied = denied + 1; fx { println("event ", i, ": $e2 -> $deniedword"); } }
        }
        if cur == "$s2" {
            if e == "$e1" { fx { println("event ", i, ": $e1 -> $idleword"); } }
            if e == "$e2" { state = "$s1"; passes = passes + 1; fx { println("event ", i, ": $e2 -> $password"); } }
        }
    }
    fx {
        println("$passlabel = ", passes);
        println("$deniedlabel = ", denied);
        println("final state = ", state);
    }
    toon_close(doc);
    ret;
}
''')

# ---------------------------------------------------------------- shape H: bucket + argmax element
H = Template('''fn main() -> Void {
    if !has_toon() { fx { println("yyjson unavailable"); } ret; }

    let doc: ToonDoc = toon_parse_file("$datafile");
    let root: ToonNode = toon_root(doc);
    let items: ToonNode = toon_key(root, "$coll");

    let smallN: Int = 0;
    let medN: Int = 0;
    let largeN: Int = 0;
    let total: Int = 0;
    let maxVal: Int = -1;
    let topName: Text = "none";

    loop i in 0..toon_len(items) {
        let it: ToonNode = toon_at(items, i);
        let name: Text = toon_get_text_or(it, "$nf", "?");
        let v: Int = toon_get_int_or(it, "$vf", 0);
        total = total + v;
        let kind: Text = "$bmed";
        if v < $t1 { kind = "$bsmall"; }
        if v > $t2 { kind = "$blarge"; }
        if kind == "$bsmall" { smallN = smallN + 1; }
        if kind == "$bmed" { medN = medN + 1; }
        if kind == "$blarge" { largeN = largeN + 1; }
        if v > maxVal { maxVal = v; topName = name; }
        fx { println("$row ", i, ": ", name, " (", v, ") ", kind); }
    }
    fx {
        println("$bsmall = ", smallN);
        println("$bmed = ", medN);
        println("$blarge = ", largeN);
        println("total $unit = ", total);
        println("$toplabel = ", topName, " (", maxVal, ")");
    }
    toon_close(doc);
    ret;
}
''')

TASKS = []

# ----- A scenarios -----
TASKS += [
 dict(tpl=A, id="lg_twopass_latency", tags="toon, two_pass, type, real", row="route", flag="slow",
      sub=dict(coll="requests", vf="ms", nf="route", row="route", flag="slow"),
      behavior="two-pass over requests: mean latency, then flag routes whose deviation exceeds half the mean",
      data={"requests":[{"route":"home","ms":40},{"route":"list","ms":52},{"route":"item","ms":47},{"route":"cart","ms":44},{"route":"search","ms":300},{"route":"ping","ms":6}]}),
 dict(tpl=A, id="lg_twopass_scores", tags="toon, two_pass, type, real",
      sub=dict(coll="scores", vf="points", nf="player", row="player", flag="anomalies"),
      behavior="two-pass over scores: mean points, then flag players deviating more than half the mean",
      data={"scores":[{"player":"al","points":80},{"player":"bo","points":75},{"player":"cy","points":82},{"player":"di","points":78},{"player":"ed","points":300},{"player":"fi","points":10}]}),
 dict(tpl=A, id="lg_twopass_rainfall", tags="toon, two_pass, type, real",
      sub=dict(coll="days", vf="mm", nf="day", row="day", flag="extremes"),
      behavior="two-pass over daily rainfall: mean mm, then flag days deviating beyond half the mean",
      data={"days":[{"day":"mon","mm":12},{"day":"tue","mm":9},{"day":"wed","mm":14},{"day":"thu","mm":11},{"day":"fri","mm":90},{"day":"sat","mm":2}]}),
 dict(tpl=A, id="lg_twopass_packets", tags="toon, two_pass, type, real",
      sub=dict(coll="packets", vf="bytes", nf="id", row="packet", flag="oversized"),
      behavior="two-pass over packet sizes: mean bytes, then flag packets deviating past half the mean",
      data={"packets":[{"id":"p0","bytes":500},{"id":"p1","bytes":520},{"id":"p2","bytes":480},{"id":"p3","bytes":510},{"id":"p4","bytes":2000},{"id":"p5","bytes":40}]}),
]

# ----- B scenarios -----
TASKS += [
 dict(tpl=B, id="lg_group_tickets", tags="toon, group, argmax",
      sub=dict(coll="tickets", cf="priority", lf="title", v1="low", v2="medium", v3="high", row="ticket"),
      behavior="group tickets by priority (low/medium/high), report counts, dominant priority, and first high",
      data={"tickets":[{"priority":"low","title":"typo"},{"priority":"high","title":"outage"},{"priority":"medium","title":"slow"},{"priority":"high","title":"data loss"},{"priority":"low","title":"polish"},{"priority":"high","title":"crash"}]}),
 dict(tpl=B, id="lg_group_status", tags="toon, group, argmax",
      sub=dict(coll="responses", cf="cls", lf="path", v1="ok", v2="redirect", v3="error", row="resp"),
      behavior="group responses by class (ok/redirect/error), report counts, dominant class, and first error",
      data={"responses":[{"cls":"ok","path":"/a"},{"cls":"ok","path":"/b"},{"cls":"redirect","path":"/c"},{"cls":"error","path":"/d"},{"cls":"ok","path":"/e"},{"cls":"error","path":"/f"}]}),
 dict(tpl=B, id="lg_group_reviews", tags="toon, group, argmax",
      sub=dict(coll="reviews", cf="rating", lf="user", v1="poor", v2="ok", v3="great", row="review"),
      behavior="group reviews by rating (poor/ok/great), report counts, dominant rating, and first great",
      data={"reviews":[{"rating":"ok","user":"a"},{"rating":"great","user":"b"},{"rating":"poor","user":"c"},{"rating":"great","user":"d"},{"rating":"ok","user":"e"},{"rating":"great","user":"f"},{"rating":"ok","user":"g"}]}),
 dict(tpl=B, id="lg_group_weather", tags="toon, group, argmax",
      sub=dict(coll="readings", cf="sky", lf="hour", v1="clear", v2="cloudy", v3="storm", row="hour"),
      behavior="group weather readings by sky (clear/cloudy/storm), report counts, dominant sky, and first storm",
      data={"readings":[{"sky":"clear","hour":"08"},{"sky":"cloudy","hour":"09"},{"sky":"cloudy","hour":"10"},{"sky":"storm","hour":"11"},{"sky":"clear","hour":"12"},{"sky":"storm","hour":"13"}]}),
]

# ----- C scenarios -----
TASKS += [
 dict(tpl=C, id="lg_sim_inventory", tags="toon, stateful, type, clamp",
      sub=dict(coll="ops", add="restock", rem="ship", scl="shrink", levelword="stock"),
      behavior="sequential stock sim: restock adds then clamps to cap, ship rejects if short, shrink adjusts by percent",
      data={"cap":1000,"ops":[{"kind":"restock","amount":400},{"kind":"ship","amount":150},{"kind":"ship","amount":900},{"kind":"restock","amount":800},{"kind":"shrink","amount":-10},{"kind":"ship","amount":200}]}),
 dict(tpl=C, id="lg_sim_battery", tags="toon, stateful, type, clamp",
      sub=dict(coll="ops", add="charge", rem="drain", scl="regen", levelword="charge"),
      behavior="sequential battery sim: charge adds then clamps to cap, drain rejects if short, regen adjusts by percent",
      data={"cap":100,"ops":[{"kind":"charge","amount":60},{"kind":"drain","amount":20},{"kind":"drain","amount":90},{"kind":"charge","amount":70},{"kind":"regen","amount":10},{"kind":"drain","amount":30}]}),
 dict(tpl=C, id="lg_sim_tank", tags="toon, stateful, type, clamp",
      sub=dict(coll="ops", add="fill", rem="draw", scl="evap", levelword="volume"),
      behavior="sequential tank sim: fill adds then clamps to cap, draw rejects if short, evap adjusts by percent",
      data={"cap":500,"ops":[{"kind":"fill","amount":200},{"kind":"draw","amount":50},{"kind":"draw","amount":600},{"kind":"fill","amount":400},{"kind":"evap","amount":-5},{"kind":"draw","amount":100}]}),
 dict(tpl=C, id="lg_sim_score", tags="toon, stateful, type, clamp",
      sub=dict(coll="ops", add="gain", rem="spend", scl="bonus", levelword="score"),
      behavior="sequential score sim: gain adds then clamps to cap, spend rejects if short, bonus adjusts by percent",
      data={"cap":300,"ops":[{"kind":"gain","amount":120},{"kind":"spend","amount":40},{"kind":"spend","amount":500},{"kind":"gain","amount":250},{"kind":"bonus","amount":10},{"kind":"spend","amount":60}]}),
]

# ----- D scenarios -----
DIGIT = ('''@pure
fn digitSum(n: Int) -> Int {
    if n < 10 { ret n; }
    let last: Int = n - (n / 10) * 10;
    ret last + digitSum(n / 10);
}''', "digitSum")
HALV = ('''@pure
fn halvings(n: Int) -> Int {
    if n <= 1 { ret 0; }
    ret 1 + halvings(n / 2);
}''', "halvings")
SUMN = ('''@pure
fn sumTo(n: Int) -> Int {
    if n <= 0 { ret 0; }
    ret n + sumTo(n - 1);
}''', "sumTo")
FIB = ('''@pure
fn fib(n: Int) -> Int {
    if n < 2 { ret n; }
    ret fib(n - 1) + fib(n - 2);
}''', "fib")
TASKS += [
 dict(tpl=D, id="lg_rec_digitsum", tags="toon, recursion, argmax",
      sub=dict(recfn=DIGIT[0], call=DIGIT[1], coll="numbers", vf="n", row="number", unit="digitsum", maxlabel="max digitsum"),
      behavior="recursive digit sum of each number; report the maximum and which number produced it",
      data={"numbers":[{"n":42},{"n":999},{"n":17},{"n":808},{"n":123}]}),
 dict(tpl=D, id="lg_rec_halvings", tags="toon, recursion, argmax",
      sub=dict(recfn=HALV[0], call=HALV[1], coll="numbers", vf="n", row="number", unit="halvings", maxlabel="max halvings"),
      behavior="recursive count of halvings to reach 1; report the maximum and which number produced it",
      data={"numbers":[{"n":8},{"n":33},{"n":1024},{"n":17},{"n":100}]}),
 dict(tpl=D, id="lg_rec_sumto", tags="toon, recursion, argmax",
      sub=dict(recfn=SUMN[0], call=SUMN[1], coll="numbers", vf="n", row="number", unit="sum", maxlabel="max sum"),
      behavior="recursive sum from 1 to each number; report the maximum and which number produced it",
      data={"numbers":[{"n":4},{"n":10},{"n":7},{"n":2},{"n":9}]}),
 dict(tpl=D, id="lg_rec_fib", tags="toon, recursion, argmax",
      sub=dict(recfn=FIB[0], call=FIB[1], coll="numbers", vf="n", row="index", unit="fib", maxlabel="max fib"),
      behavior="recursive Fibonacci of each index; report the maximum and which index produced it",
      data={"numbers":[{"n":5},{"n":9},{"n":3},{"n":11},{"n":7}]}),
]

# ----- E scenarios -----
TASKS += [
 dict(tpl=E, id="lg_nested_warehouse", tags="toon, nested, argmax",
      sub=dict(outer="warehouses", inner="items", vf="qty", row="warehouse", itemword="items", sumword="units"),
      behavior="nested traversal: per warehouse count items and sum units, then grand totals and the top warehouse",
      data={"warehouses":[{"name":"north","items":[{"qty":10},{"qty":20}]},{"name":"south","items":[{"qty":5}]},{"name":"east","items":[{"qty":12},{"qty":8},{"qty":15}]}]}),
 dict(tpl=E, id="lg_nested_teams", tags="toon, nested, argmax",
      sub=dict(outer="teams", inner="players", vf="points", row="team", itemword="players", sumword="points"),
      behavior="nested traversal: per team count players and sum points, then grand totals and the top team",
      data={"teams":[{"name":"reds","players":[{"points":7},{"points":9}]},{"name":"blues","players":[{"points":4}]},{"name":"greens","players":[{"points":6},{"points":8},{"points":3}]}]}),
 dict(tpl=E, id="lg_nested_regions", tags="toon, nested, argmax",
      sub=dict(outer="regions", inner="stores", vf="revenue", row="region", itemword="stores", sumword="revenue"),
      behavior="nested traversal: per region count stores and sum revenue, then grand totals and the top region",
      data={"regions":[{"name":"west","stores":[{"revenue":100},{"revenue":120}]},{"name":"central","stores":[{"revenue":90}]},{"name":"coast","stores":[{"revenue":110},{"revenue":80},{"revenue":95}]}]}),
 dict(tpl=E, id="lg_nested_albums", tags="toon, nested, argmax",
      sub=dict(outer="albums", inner="tracks", vf="seconds", row="album", itemword="tracks", sumword="seconds"),
      behavior="nested traversal: per album count tracks and sum seconds, then grand totals and the top album",
      data={"albums":[{"name":"dawn","tracks":[{"seconds":200},{"seconds":180}]},{"name":"dusk","tracks":[{"seconds":240}]},{"name":"noon","tracks":[{"seconds":210},{"seconds":160},{"seconds":190}]}]}),
]

# ----- F scenarios -----
TASKS += [
 dict(tpl=F, id="lg_adj_stocks", tags="toon, adjacent, streak",
      sub=dict(coll="prices", vf="price", jumpword="maxGain", runword="longest rising run"),
      behavior="adjacent-pair deltas over prices; report the largest single-step gain and longest non-decreasing run",
      data={"prices":[{"price":100},{"price":104},{"price":104},{"price":99},{"price":101},{"price":108}]}),
 dict(tpl=F, id="lg_adj_elevation", tags="toon, adjacent, streak",
      sub=dict(coll="points", vf="meters", jumpword="maxClimb", runword="longest ascending run"),
      behavior="adjacent-pair deltas over elevation; report the largest single-step climb and longest non-decreasing run",
      data={"points":[{"meters":300},{"meters":320},{"meters":320},{"meters":290},{"meters":340},{"meters":360}]}),
 dict(tpl=F, id="lg_adj_steps", tags="toon, adjacent, streak",
      sub=dict(coll="days", vf="steps", jumpword="maxIncrease", runword="longest improving run"),
      behavior="adjacent-pair deltas over daily steps; report the largest single-step increase and longest non-decreasing run",
      data={"days":[{"steps":5000},{"steps":6000},{"steps":6000},{"steps":4000},{"steps":7000},{"steps":9000}]}),
 dict(tpl=F, id="lg_adj_temps", tags="toon, adjacent, streak",
      sub=dict(coll="hours", vf="temp", jumpword="maxJump", runword="longest warming run"),
      behavior="adjacent-pair deltas over temperatures; report the largest single-step jump and longest non-decreasing run",
      data={"hours":[{"temp":15},{"temp":17},{"temp":17},{"temp":13},{"temp":16},{"temp":20}]}),
]

# ----- G scenarios -----
TASKS += [
 dict(tpl=G, id="lg_fsm_door", tags="toon, fsm, stateful",
      sub=dict(coll="events", s1="locked", s2="open", e1="key", e2="handle", deniedword="JAMMED", idleword="still open", password="THROUGH", passlabel="opens", deniedlabel="jams"),
      behavior="two-state door FSM (locked/open) over key/handle events; count opens and jams and the final state",
      data={"events":[{"e":"handle"},{"e":"key"},{"e":"handle"},{"e":"handle"},{"e":"key"},{"e":"key"},{"e":"handle"}]}),
 dict(tpl=G, id="lg_fsm_vending", tags="toon, fsm, stateful",
      sub=dict(coll="events", s1="idle", s2="ready", e1="coin", e2="pick", deniedword="NOCREDIT", idleword="extra credit", password="VEND", passlabel="vends", deniedlabel="rejects"),
      behavior="two-state vending FSM (idle/ready) over coin/pick events; count vends and rejects and the final state",
      data={"events":[{"e":"pick"},{"e":"coin"},{"e":"pick"},{"e":"pick"},{"e":"coin"},{"e":"coin"},{"e":"pick"}]}),
 dict(tpl=G, id="lg_fsm_gate", tags="toon, fsm, stateful",
      sub=dict(coll="events", s1="shut", s2="open", e1="badge", e2="walk", deniedword="BLOCKED", idleword="held open", password="ENTER", passlabel="entries", deniedlabel="blocks"),
      behavior="two-state gate FSM (shut/open) over badge/walk events; count entries and blocks and the final state",
      data={"events":[{"e":"walk"},{"e":"badge"},{"e":"walk"},{"e":"walk"},{"e":"badge"},{"e":"badge"},{"e":"walk"}]}),
 dict(tpl=G, id="lg_fsm_faucet", tags="toon, fsm, stateful",
      sub=dict(coll="events", s1="off", s2="on", e1="turn", e2="use", deniedword="DRY", idleword="running", password="FLOW", passlabel="flows", deniedlabel="drys"),
      behavior="two-state faucet FSM (off/on) over turn/use events; count flows and drys and the final state",
      data={"events":[{"e":"use"},{"e":"turn"},{"e":"use"},{"e":"use"},{"e":"turn"},{"e":"turn"},{"e":"use"}]}),
]

# ----- H scenarios -----
TASKS += [
 dict(tpl=H, id="lg_bucket_files", tags="toon, bucket, argmax",
      sub=dict(coll="files", nf="name", vf="kb", t1="100", t2="1000", bsmall="small", bmed="medium", blarge="large", row="file", unit="kb", toplabel="biggest"),
      behavior="bucket files by size (small <100, large >1000, else medium), sum kb, track the biggest file",
      data={"files":[{"name":"a.txt","kb":20},{"name":"b.png","kb":450},{"name":"c.iso","kb":4000},{"name":"d.log","kb":80},{"name":"e.zip","kb":1500},{"name":"f.csv","kb":300}]}),
 dict(tpl=H, id="lg_bucket_cities", tags="toon, bucket, argmax",
      sub=dict(coll="cities", nf="name", vf="pop", t1="50", t2="500", bsmall="town", bmed="city", blarge="metro", row="place", unit="pop", toplabel="largest"),
      behavior="bucket places by population (town <50, metro >500, else city), sum pop, track the largest",
      data={"cities":[{"name":"elm","pop":20},{"name":"oak","pop":120},{"name":"bay","pop":900},{"name":"fen","pop":40},{"name":"port","pop":600},{"name":"vale","pop":300}]}),
 dict(tpl=H, id="lg_bucket_parcels", tags="toon, bucket, argmax",
      sub=dict(coll="parcels", nf="id", vf="grams", t1="200", t2="2000", bsmall="light", bmed="medium", blarge="heavy", row="parcel", unit="grams", toplabel="heaviest"),
      behavior="bucket parcels by weight (light <200, heavy >2000, else medium), sum grams, track the heaviest",
      data={"parcels":[{"id":"p1","grams":50},{"id":"p2","grams":800},{"id":"p3","grams":3000},{"id":"p4","grams":150},{"id":"p5","grams":2500},{"id":"p6","grams":500}]}),
 dict(tpl=H, id="lg_bucket_songs", tags="toon, bucket, argmax",
      sub=dict(coll="songs", nf="title", vf="sec", t1="120", t2="300", bsmall="short", bmed="medium", blarge="long", row="song", unit="sec", toplabel="longest"),
      behavior="bucket songs by duration (short <120, long >300, else medium), sum seconds, track the longest",
      data={"songs":[{"title":"intro","sec":40},{"title":"verse","sec":200},{"title":"epic","sec":600},{"title":"skit","sec":90},{"title":"jam","sec":420},{"title":"outro","sec":150}]}),
]

# ---------------------------------------------------------------- run + verify + emit
records = []; fails = []
for t in TASKS:
    code = t["tpl"].substitute(datafile=t["id"] + ".json", **t["sub"])
    df = t["id"] + ".json"
    data_str = json.dumps(t["data"])
    open(os.path.join(WORK, t["id"] + ".ae"), "w").write(code)
    open(os.path.join(WORK, df), "w").write(data_str)
    r = subprocess.run([AE, t["id"] + ".ae"], cwd=WORK, capture_output=True, text=True, timeout=30)
    if r.returncode != 0 or not r.stdout.strip():
        fails.append((t["id"], r.returncode, (r.stderr or "empty stdout")[:160]))
        continue
    out = r.stdout
    expect = "".join("// expect: " + ln + "\n" for ln in out.splitlines())
    assistant = "// " + t["id"] + "\n// Concepts: " + t["behavior"] + "\n" + expect + "\n" + code
    user = ("Write canonical Aether source only.\n\nProgram id: " + t["id"] +
            ".\n\nConcept tags: " + t["tags"] + ".\n\nBehavior notes: " + t["behavior"] +
            "\n\nExact stdout must be:\n\n" + out.rstrip("\n"))
    records.append({
        "kind": "corpus_instruction_sft", "id": t["id"],
        "messages": [{"role": "system", "content": SYS},
                     {"role": "user", "content": user},
                     {"role": "assistant", "content": assistant}],
        "expected_stdout": out, "files": {df: data_str},
        "verification": {"returncode": 0, "stdout": out, "stderr": "", "exact_stdout_match": True},
    })

with open(WORK + "/large_examples_sft.jsonl", "w") as f:
    for r in records:
        f.write(json.dumps(r) + "\n")
print("VERIFIED OK: %d / %d" % (len(records), len(TASKS)))
for fid, rc, err in fails:
    print("  FAIL %s rc=%s : %s" % (fid, rc, err))
print("wrote", WORK + "/large_examples_sft.jsonl")
