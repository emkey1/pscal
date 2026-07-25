## 2024-05-18 - [Optimization of INT_DIV and MOD]
**Learning:** When adding fast paths for arithmetic in the VM using smaller data types (like 32-bit math for `TYPE_INT32`), you must be extremely careful to preserve the original semantics of the fallback path. In this codebase, the standard integer types are backed by 64-bit `long long`. Therefore, an operation that overflows a 32-bit integer (like `INT32_MIN / -1`) should *not* trap or throw an error; it needs to be promoted cleanly to its valid 64-bit result to avoid breaking valid language semantics.
**Action:** Always verify that edge cases in numerical fast paths yield the exact same result as the slower generic path, rather than naively enforcing the bounds of the smaller optimized type.
## 2026-07-25 - [Optimization of TYPE_INT32 Arithmetic]
**Learning:** The codebase relies on GCC builtins (e.g., `__builtin_add_overflow`) in `src/vm/vm.c` for performance optimizations. Automated review warnings regarding MSVC portability for these specific intrinsics can be safely ignored, as they represent an established pattern in the VM core.
**Action:** Use GCC built-ins where appropriate for arithmetic bounds checking.
