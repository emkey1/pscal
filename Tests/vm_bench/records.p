program BenchRecords;
{ Record/array copy-heavy workload: whole-array-of-records value
  assignment and pass-by-value into functions, split into a read-mostly
  phase and a copy-then-mutate phase.  Exercises the VM 2.0 Phase 4j
  copy-on-write path (plan.md §5.10) for genuinely value-typed data.

  Uses a *static* array (`array[1..N] of TRecord`), not `array of TRecord`,
  deliberately: this language's dynamic arrays are reference-counted,
  alias-on-assign types (matching Delphi/FPC dynamic-array semantics) --
  a by-value dynamic-array parameter shares its backing buffer with the
  caller, and mutating an element through it is *visible to the caller*,
  confirmed identically on both the VM 1.x baseline and current VM 2.0, so
  that is long-standing, correct, by-design behavior, not something Phase
  4j changed or a bug. Only `array[1..N] of T` (fixed bounds) has true
  independent-copy-on-assignment value semantics in both VM generations,
  so it's the only array shape where "record/array copy-heavy" actually
  means what it sounds like -- see plan.md §11 for the measured result and
  the full story of how an earlier draft of this file got that distinction
  wrong.

  Phase A never mutates a copy; Phase B copies then mutates every round.
  Reporting one blended number intentionally averages the two regimes
  together rather than cherry-picking the favorable one. }

const
  N = 180;          { records per array }
  ROUNDS_RO = 750;  { read-only-copy rounds }
  ROUNDS_MUT = 300; { copy-then-mutate rounds }

type
  TRecord = record
    id: integer;
    tag: string;
    scores: array[1..8] of integer;
  end;
  TRecords = array[1..N] of TRecord;

var
  t0, t1: double;
  checkResult: integer;

procedure FillBase(var base: TRecords);
var
  i, j: integer;
begin
  for i := 1 to N do
  begin
    base[i].id := i;
    base[i].tag := 'rec' + IntToStr(i mod 37);
    for j := 1 to 8 do
      base[i].scores[j] := (i * 31 + j * 17) mod 10007;
  end;
end;

function SumRecords(r: TRecords): integer;
var
  i, j, acc: integer;
begin
  acc := 0;
  for i := 1 to N do
  begin
    acc := acc + r[i].id;
    for j := 1 to 8 do
      acc := acc + r[i].scores[j];
  end;
  SumRecords := acc mod 1000003;
end;

function MutateCopy(base: TRecords; seed: integer): integer;
var
  i: integer;
  acc: integer;
begin
  { base arrived by value -- a true independent copy for a static array.
    Mutating it here must not affect the caller's array. }
  acc := 0;
  for i := 1 to N do
  begin
    base[i].id := base[i].id + seed;
    base[i].scores[1] := (base[i].scores[1] + seed) mod 10007;
    acc := acc + base[i].id;
  end;
  MutateCopy := acc mod 1000003;
end;

function Kernel: integer;
var
  base, copyA: TRecords;
  r: integer;
  acc: integer;
begin
  FillBase(base);
  acc := 0;

  { Phase A: read-mostly copies. }
  for r := 1 to ROUNDS_RO do
  begin
    copyA := base;
    acc := (acc + SumRecords(copyA)) mod 1000003;
  end;

  { Phase B: copy-then-mutate. }
  for r := 1 to ROUNDS_MUT do
    acc := (acc + MutateCopy(base, r)) mod 1000003;

  { base itself must be unmutated by Phase B's by-value calls. }
  acc := (acc + SumRecords(base)) mod 1000003;

  Kernel := acc;
end;

begin
  t0 := RealTimeClock();
  checkResult := Kernel;
  t1 := RealTimeClock();
  writeln('check=', checkResult);
  writeln('elapsed_s=', (t1 - t0):0:6);
end.
