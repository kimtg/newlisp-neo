# newLISP with High-Performance Bytecode Virtual Machine & Generational GC

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](COPYING.txt)
[![Regression Tests](https://img.shields.io/badge/qa--dot-100%25%20Passing-brightgreen.svg)]()
[![Performance vs Python](https://img.shields.io/badge/Speed%20vs%20Python%203.12-1.84x%20Faster-orange.svg)]()

**newLISP** is an elegant, lightweight, LISP-like scripting language designed for general programming, artificial intelligence, and statistical computing.

This enhanced version upgrades the newLISP engine with a **Direct-Threaded Bytecode Virtual Machine** and a high-throughput **Generational Garbage Collector**, delivering massive speedups across recursion and numeric iteration while maintaining **100% backward compatibility** with existing newLISP code.

---

## Performance Benchmarks: newLISP vs. Python 3.12

All benchmarks were evaluated on a Windows x86_64 host under identical hardware conditions.

### 1. Recursive Fibonacci: `(fib 30)`

**Lisp code ([`bench_fib.lsp`](bench_fib.lsp)):**
```lisp
(define (fib n)
  (if (< n 2)
      n
      (+ (fib (- n 1)) (fib (- n 2)))))
```

**Python 3.12 code:**
```python
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)
```

| Runtime Engine | Execution Time | Speedup vs Original newLISP | Comparison vs Python 3.12 |
|---|---|---|---|
| **Original newLISP 10.7.6** (Tree-Walker) | 1,326.0 ms | 1.00x | 7.1x slower |
| **CPython 3.12.3** (Standard Python VM) | 186.9 ms | 7.10x faster | 1.00x (reference) |
| **Optimized newLISP** (GenGC + Direct-Threaded VM) | **101.5 ms** | **13.1x faster** | **1.84x FASTER than Python!** |

---

### 2. 1-Million Iteration While Loop: `(loop-test 1000000)`

**Lisp code ([`bench_loop.lsp`](bench_loop.lsp)):**
```lisp
(define (loop-test n)
  (let (s 0 i 0)
    (while (< i n)
      (set 's (+ s i))
      (set 'i (+ i 1)))
    s))
```

**Python 3.12 code:**
```python
def loop_test(n):
    s = 0
    i = 0
    while i < n:
        s += i
        i += 1
    return s
```

| Runtime Engine | Execution Time | Speedup vs Original newLISP | Comparison vs Python 3.12 |
|---|---|---|---|
| **Original newLISP 10.7.6** (Tree-Walker) | 375.4 ms | 1.00x | 4.8x slower |
| **CPython 3.12.3** (Standard Python VM) | 78.8 ms | 4.76x faster | 1.00x (reference) |
| **Optimized newLISP** (GenGC + Direct-Threaded VM) | **60.9 ms** | **6.16x faster** | **1.29x FASTER than Python!** |

---

## Architectural Highlights

### 1. Direct-Threaded Bytecode Virtual Machine (`nl-vm.c`, `nl-vm.h`)
- **Computed Goto Dispatch**: Replaces traditional `switch/case` loop with GCC direct threading (`&&label` jump tables), eliminating branch prediction stalls.
- **Specialized Super-Instructions**:
  - `OP_LOAD_LOCAL_0..3` and `OP_STORE_LOCAL_0..3`: Zero-operand immediate local variable slots.
  - `OP_CONST_0..2`: Fast-path constant pushes.
  - `OP_ADD_1`, `OP_SUB_1`, `OP_SUB_2`: Direct unboxed arithmetic bypassing constant tables.
- **Non-Recursive Execution Engine**: Uses flat `vm_frames` and `vm_stack` arrays, avoiding C call stack frames during function evaluation and self-recursion (`OP_CALL_SELF`).
- **Transparent Fallback**: Lambdas with dynamic binding or unsupported constructs seamlessly fall back to newLISP's classic tree walker with zero developer intervention.

### 2. Generational Garbage Collector (`newlisp.c`, `newlisp.h`)
- **Gen 0 Nursery (64 MB)**: Fast bump allocation (`cell = gen0_ptr++`) eliminates chunk-pool traversal and per-cell deallocation during heavy computation.
- **Cheney-Style Evacuation**: Live cells are promoted to Gen 1 tenured storage during nursery resets.
- **Full Root Scanning**: Scans symbol trees, context tables, environment stacks (`envStack`), result stacks (`resultStack`), lambda stack frames, and active VM execution stacks.

### 3. Memory Safety & Backward Compatibility
- **`BYTECODE_MAGIC` (`0xBEEC0DE0`)**: Guaranteed identification of bytecode handles inside `cell->aux`, protecting newLISP's native last-element pointer optimization on standard list cells from memory corruption or invalid free calls.
- **Full Regression Verification**: All 396 built-in primitives, contexts as objects, and scoping test suites (`qa-dot`) pass with **0 errors**.

---

## Building and Testing

### Prerequisites
- GCC / MinGW-w64 (or Clang)
- GNU Make

### Build on Windows (MinGW-w64)
```powershell
# In PowerShell:
$env:Path = "C:\Program Files\CodeBlocks\MinGW\bin;" + $env:Path
mingw32-make -f makefile_mingw64_utf8
```

### Build on Linux / macOS
```bash
make
# Or choose specific platform makefile:
make -f makefile_linuxLP64_utf8
make -f makefile_darwinLP64_utf8
```

### Running Tests
Run the comprehensive test suite:
```bash
./newlisp qa-dot
```
Expected output:
```text
Testing built-in functions ...
  ...
Testing contexts as objects and scoping rules ...
>>>>> ALL FUNCTIONS FINISHED SUCCESSFUL: ./newlisp.exe
```

### Running Benchmarks
```bash
./newlisp bench_fib.lsp
./newlisp bench_loop.lsp
```

---

## Documentation
- [`doc/ARCHITECTURE.md`](doc/ARCHITECTURE.md) - Deep dive into VM opcodes, memory layout, GC evacuation, and safety guarantees.
- [`doc/CHANGES.txt`](doc/CHANGES.txt) - Detailed change log.
- [`doc/newlisp_manual.html`](doc/newlisp_manual.html) - Complete user manual and language reference.
- [`doc/MemoryManagement.html`](doc/MemoryManagement.html) - Original ORO memory management documentation.

---

## License
newLISP is released under the [GNU General Public License Version 3 (GPLv3)](COPYING.txt).
Documentation is distributed under the GNU Free Documentation License (GFDL).

