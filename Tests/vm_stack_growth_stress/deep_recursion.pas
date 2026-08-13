program DeepRecursion;
{ VM 2.0 Phase 3 (plan Docs/pscal_vm2_plan.md §5.9): proves the growable
  operand/call stack actually fixes deep recursion. Pre-Phase-3, the
  fixed VM_CALL_STACK_MAX=4096 / VM_STACK_MAX=8192 arrays made this crash
  with a clean "Call stack overflow"/"Stack overflow" at a few thousand
  frames; post-Phase-3, the default ceilings (VM_CALL_STACK_MAX=131072,
  VM_STACK_MAX=1048576, both env-overridable via PSCAL_VM_MAX_CALL_FRAMES /
  PSCAL_VM_MAX_STACK_VALUES) let this succeed cleanly. }

function countdown(n: integer): integer;
begin
  if n <= 0 then
    countdown := 0
  else
    countdown := 1 + countdown(n - 1);
end;

begin
  writeln(countdown(100000));
end.
