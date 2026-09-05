# newLISP Neo

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Regression Tests](https://img.shields.io/badge/qa--dot-100%25%20Passing-brightgreen.svg)](qa-dot)
[![Speed vs Python](https://img.shields.io/badge/Speed%20vs%20Python%203.12-1.84x%20Faster-orange.svg)](#performance-benchmarks-newlisp-neo-vs-python-312)

**newLISP Neo** is a modernized, high-performance distribution of [newLISP](http://www.newlisp.org) — an elegant, lightweight, LISP-like scripting language originally created by **Lutz Mueller** for general programming, artificial intelligence, data manipulation, and statistical computing.

This enhanced release overhauls the newLISP engine with a **Direct-Threaded Bytecode Virtual Machine** and a high-throughput **Generational Garbage Collector**, achieving order-of-magnitude speedups in recursion and iterative loops while preserving **100% backward compatibility** with the official newLISP test suite and existing codebase.

---

## Key Enhancements

- **Direct-Threaded Bytecode Virtual Machine (`nl-vm.c`, `nl-vm.h`)**:
  - **Computed-Goto Dispatch**: Employs GCC/Clang `&&label` jump tables to eliminate branch mispredictions and loop branching overhead inherent in traditional `switch/case` interpreters.
  - **Specialized Super-Instructions**: Immediate opcode specializations (`OP_LOAD_LOCAL_0..3`, `OP_STORE_LOCAL_0..3`, `OP_CONST_0..2`, `OP_ADD_1`, `OP_SUB_1`, `OP_SUB_2`) bypass operand fetches and optimize frequent variable access and loop arithmetic.
  - **Non-Recursive Call Frame Execution**: Employs a flat frame stack (`vm_frames`) and operand stack (`vm_stack`), completely removing C call-stack recursion overhead during function evaluation and self-recursion (`OP_CALL_SELF`).
  - **Transparent JIT/AST Fallback**: Functions and lambdas containing dynamic binding, metaprogramming, or constructs outside pure bytecode semantics fall back automatically and transparently to newLISP's classic tree-walking evaluator.

- **High-Throughput Generational Garbage Collector (`newlisp.c`, `newlisp.h`)**:
  - **64 MB Gen 0 Nursery**: Ultra-fast bump-pointer allocation (`cell = gen0_ptr++`) eliminates pool searching and per-cell free overhead for short-lived intermediate objects.
  - **Cheney-Style Evacuation**: Live objects surviving nursery collections are promoted (`gcEvacuate`) to the tenured Gen 1 heap.
  - **Comprehensive Root Scanning**: Traverses symbol trees, context tables, runtime stacks (`envStack`, `resultStack`, `lambdaStack`), and active VM execution frames.

- **Memory Safety & Rock-Solid Compatibility**:
  - **Magic-Tagged Bytecode Handles**: Bytecode objects are tagged with `BYTECODE_MAGIC` (`0xBEEC0DE0`) in `cell->aux`, preserving newLISP's native last-element pointer optimization on standard lists and eliminating memory corruption hazards.
  - **100% Test Suite Pass**: All 396 built-in primitives, contexts as objects, and scoping tests in the `qa-dot` suite pass with **0 errors**.

---

## Performance Benchmarks: newLISP Neo vs. Python 3.12

All benchmarks were evaluated under identical conditions on a Windows x86_64 host.

### 1. Recursive Fibonacci: `(fib 30)`

Lisp code ([`bench_fib.lsp`](bench_fib.lsp)):
```lisp
(define (fib n)
  (if (< n 2)
      n
      (+ (fib (- n 1)) (fib (- n 2)))))
```

Python 3.12 reference code:
```python
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)
```

| Runtime Engine | Execution Time | Speedup vs Original newLISP | Comparison vs Python 3.12 |
|---|---|---|---|
| **Original newLISP 10.7.6** (Tree-Walker) | 1,326.0 ms | 1.00x | 7.10x slower |
| **CPython 3.12.3** (Standard Python VM) | 186.9 ms | 7.10x faster | 1.00x (baseline) |
| **newLISP Neo** (Direct-Threaded VM + GenGC) | **101.5 ms** | **13.1x faster** | **1.84x FASTER than Python** |

---

### 2. 1-Million Iteration While Loop: `(loop-test 1000000)`

Lisp code ([`bench_loop.lsp`](bench_loop.lsp)):
```lisp
(define (loop-test n)
  (let (s 0 i 0)
    (while (< i n)
      (set 's (+ s i))
      (set 'i (+ i 1)))
    s))
```

Python 3.12 reference code:
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
| **Original newLISP 10.7.6** (Tree-Walker) | 375.4 ms | 1.00x | 4.76x slower |
| **CPython 3.12.3** (Standard Python VM) | 78.8 ms | 4.76x faster | 1.00x (baseline) |
| **newLISP Neo** (Direct-Threaded VM + GenGC) | **60.9 ms** | **6.16x faster** | **1.29x FASTER than Python** |

---

## Building and Installation

### Prerequisites
- GCC / MinGW-w64 or Clang (supporting C99/GNU extensions for computed gotos)
- GNU Make

### Build on Windows (MinGW-w64)
```powershell
# In PowerShell / Command Prompt with MinGW-w64 in PATH:
mingw32-make -f makefile_mingw64_utf8
```

### Build on Linux, macOS, and BSD
```bash
# Automatic platform detection:
make

# Or configure first:
./configure
make

# Or build with a specific makefile:
make -f makefile_linuxLP64_utf8
make -f makefile_darwinLP64_utf8
make -f makefile_bsdLP64_utf8
```

### Installation
```bash
# System-wide installation (requires root privileges):
sudo make install

# User home directory install (~/bin, ~/share):
make install_home
```

---

## Verification & Benchmarks

### Running the QA Regression Suite
Verify complete language integrity across all primitive functions, scoping, and context features:
```bash
./newlisp qa-dot
```
Expected summary output:
```text
Testing built-in functions ...
...
Testing contexts as objects and scoping rules ...
total time: ...
>>>>> ALL FUNCTIONS FINISHED SUCCESSFUL: ./newlisp
```

Additional test suites can be executed via:
```bash
make check
# or
make testall
```

### Running Performance Benchmarks
```bash
./newlisp bench_fib.lsp
./newlisp bench_loop.lsp
```

---

## Repository Structure

```text
.
├── newlisp.c / newlisp.h     # Core interpreter runtime, GenGC, and memory manager
├── nl-vm.c / nl-vm.h         # Direct-threaded bytecode compiler and virtual machine
├── nl-*.c                    # Built-in subsystems (math, string, socket, filesys, etc.)
├── pcre.c / pcre.h           # Bundled PCRE regular expression library
├── makefile_*                # Cross-platform build definitions for Linux, macOS, BSD, Win32/64
├── bench_fib.lsp             # Recursive Fibonacci benchmark harness
├── bench_loop.lsp            # Arithmetic loop benchmark harness
├── qa-dot / qa-comma         # Complete language regression test suites
├── modules/                  # Standard library modules (crypto, sqlite3, stat, etc.)
├── examples/                 # Sample applications and scripts
├── doc/
│   ├── ARCHITECTURE.md       # VM bytecode instruction set, memory layout & GC architecture
│   ├── CHANGES.txt           # Version history and detailed changelog
│   ├── newlisp_manual.html   # Full reference manual and language specification
│   ├── MemoryManagement.html # Original ORO memory management documentation
│   └── INSTALL.txt           # Detailed platform installation instructions
└── README-old                # Legacy upstream README by Lutz Mueller
```

---

## Documentation Links

- [**Architecture & VM Internals**](doc/ARCHITECTURE.md) — Comprehensive technical breakdown of bytecode opcodes, computed goto dispatch, frame layout, and generational GC evacuation.
- [**Changelog**](doc/CHANGES.txt) — Chronological record of features, fixes, and optimizations.
- [**newLISP Reference Manual**](doc/newlisp_manual.html) — Complete language manual, functions reference, and syntax guide.
- [**Original Upstream README**](README-old) — Lutz Mueller's original documentation, history, and notes.

---

## License & Credits

- **newLISP** was originally designed and implemented by **Lutz Mueller** ([Nuevatec](http://www.newlisp.org)).
- **newLISP Neo** is released under the [GNU General Public License Version 3 (GPLv3)](LICENSE). See [`LICENSE`](LICENSE) or [`doc/COPYING.txt`](doc/COPYING.txt) for the complete license text.
- Documentation files are distributed under the GNU Free Documentation License (GFDL).
