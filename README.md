# newLISP Neo

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Regression Tests](https://img.shields.io/badge/qa--dot-100%25%20Passing-brightgreen.svg)](qa-dot)
[![Tail Call Optimization](https://img.shields.io/badge/TCO-O(1)%20Stack-blueviolet.svg)](#3-tail-call-optimization-tco-and-mutual-recursion)
[![Speed vs Python](https://img.shields.io/badge/Speed%20vs%20Python%203.14-2.02x%20Faster-orange.svg)](#performance-benchmarks-newlisp-neo-vs-python-314)
[![Security: Hardened](https://img.shields.io/badge/Security-Hardened-success.svg)](#key-enhancements)

**newLISP Neo** is a modernized, high-performance distribution of [newLISP](http://www.newlisp.org) — an elegant, lightweight, LISP-like scripting language originally created by **Lutz Mueller** for general programming, artificial intelligence, data manipulation, and statistical computing.

This enhanced release overhauls the newLISP engine with a **Direct-Threaded Bytecode Virtual Machine**, full **Tail Call Optimization (TCO)**, and a high-throughput **Generational Garbage Collector**, achieving order-of-magnitude speedups in recursion and iterative loops while preserving **100% backward compatibility** with the official newLISP test suite and existing codebase.

---

## Key Enhancements

- **Direct-Threaded Bytecode Virtual Machine (`nl-vm.c`, `nl-vm.h`)**:
  - **Computed-Goto Dispatch**: Employs GCC/Clang `&&label` jump tables to eliminate branch mispredictions and loop branching overhead inherent in traditional `switch/case` interpreters.
  - **Specialized Super-Instructions**: Immediate opcode specializations (`OP_LOAD_LOCAL_0..3`, `OP_STORE_LOCAL_0..3`, `OP_CONST_0..2`, `OP_ADD_1`, `OP_SUB_1`, `OP_SUB_2`) bypass operand fetches and optimize frequent variable access and loop arithmetic.
  - **Non-Recursive Call Frame Execution**: Employs a flat frame stack (`vm_frames`) and operand stack (`vm_stack`), completely removing C call-stack recursion overhead during function evaluation.
  - **Transparent JIT/AST Fallback**: Functions and lambdas containing dynamic binding, metaprogramming, or constructs outside pure bytecode semantics fall back automatically and transparently to newLISP's classic tree-walking evaluator.

- **Full Tail Call Optimization (TCO) (`nl-vm.c`, `nl-vm.h`)**:
  - **Guaranteed $O(1)$ Stack Space**: Eliminates call stack overflow hazards for arbitrarily deep and infinite recursions by reusing execution frames in-place.
  - **Self-Tail Recursion (`OP_TAIL_CALL_SELF`)**: Directly reuses the caller's frame via zero-overhead stack argument copying and local variable clearing. Over **100,000,000 recursive steps** execute in **~1.02 seconds** with zero stack growth.
  - **Mutual & General Tail Calls (`OP_TAIL_CALL`)**: Reuses the active frame for calls to other compiled functions, enabling clean, idiomatic state machines and mutual recursion (`my-even?` / `my-odd?` 10,000,000 steps in **~167 ms**).
  - **Comprehensive Tail Position Analysis**: Automatically propagates tail positions through control-flow constructs: `if`, `when`, `cond`, `begin`, `let`, `local`, `and`, and `or`.

- **High-Throughput Generational Garbage Collector (`newlisp.c`, `newlisp.h`)**:
  - **64 MB Gen 0 Nursery**: Ultra-fast bump-pointer allocation (`cell = gen0_ptr++`) eliminates pool searching and per-cell free overhead for short-lived intermediate objects.
  - **Cheney-Style Evacuation**: Live objects surviving nursery collections are promoted (`gcEvacuate`) to the tenured Gen 1 heap.
  - **Comprehensive Root Scanning**: Traverses symbol trees, context tables, runtime stacks (`envStack`, `resultStack`, `lambdaStack`), and active VM execution frames.

- **Memory Safety, Security Hardening & Robustness**:
  - **Magic-Tagged Bytecode Handles**: Bytecode objects are tagged with `BYTECODE_MAGIC` (`0xBEEC0DE0`) in `cell->aux`, preserving newLISP's native last-element pointer optimization on standard lists and eliminating memory corruption hazards.
  - **HTTP Path Traversal Defense (`nl-web.c`)**: Canonicalizes requested file paths with `realpath` and asserts containment within `startupDir`, rejecting `..` traversal sequences, redundant slashes, and drive-letter escapes with HTTP 403 Forbidden.
  - **Memory-Safe JSON & URL Parsing (`nl-xml-json.c`, `nl-web.c`)**: Dynamic buffer allocation replaces static stack buffers for numeric JSON literals of arbitrary length; `url_decode` strictly validates hexadecimal format before consumption to eliminate out-of-bounds reads.
  - **Bundled PCRE 5.0 Security Hardening (`pcre.c`)**: Hardened regex compilation against integer wrapping in quantifier repeat counters (CVE-2005-2491), malformed POSIX character class length discrepancies (CVE-2006-7225), and pattern allocation overflow.
  - **Atomic Temporary File Creation (`nl-web.c`)**: Replaced predictable pseudo-random filenames in CGI handling with atomic OS temporary file creation (`GetTempFileNameA` on Windows, `mkstemp` on POSIX), eliminating symlink race conditions.
  - **GC Heap Stability & Variadic Safety (`newlisp.c`, `nl-filesys.c`, `nl-math.c`, `nl-list.c`)**: Evacuates global runtime registers (`throwResult`, `countCell`) during Gen 0 sweeps; allocates long-lived multi-cell structures in list/math generators (`sequence`, `fft`, `normal`, `count`) directly in Gen 1 to eliminate unrooted transient cell hazards; uses C99 `va_copy` in `my_vasprintf` for variadic safety on 64-bit platforms.
  - **100% Test Suite Pass**: All 396 built-in primitives, contexts as objects, and scoping tests in the `qa-dot` suite pass with **0 errors**.

- **Modernized Interactive REPL (`newlisp.c`)**:
  - **Automatic Multi-Line Input**: Automatically detects incomplete expressions (unclosed parentheses `(...)`, double-quoted strings `"..."`, `{...}` braced strings with nesting, and `[text]...[/text]` tags) and seamlessly collects continuation lines until brackets are balanced, then evaluates immediately.
  - **Clean Line & Interrupt Handling**: Hitting `[Enter]` on an empty line returns a fresh prompt instead of entering legacy batch mode. `Ctrl+C` cleanly resets partial input buffers, and multi-line code can be piped directly from scripts/stdin without syntax errors.

---

## Performance Benchmarks: newLISP Neo vs. Python 3.14

All benchmarks were evaluated under identical conditions on a Windows x86_64 host.

### 1. Recursive Fibonacci: `(fib 30)`

Lisp code ([`bench_fib.lsp`](bench_fib.lsp)):
```lisp
(define (fib n)
  (if (< n 2)
      n
      (+ (fib (- n 1)) (fib (- n 2)))))
```

Python reference code ([`bench.py`](bench.py)):
```python
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)
```

| Runtime Engine | Execution Time | Speedup vs Original newLISP | Comparison vs Python 3.14 |
|---|---|---|---|
| **Original newLISP 10.7.6** (Tree-Walker) | 592.5 ms | 1.00x | 6.89x slower |
| **CPython 3.12.3** (Standard Python VM) | 186.9 ms | 3.17x faster | 2.17x slower |
| **CPython 3.14.4** (Optimized Python VM) | 86.0 ms | 6.89x faster | 1.00x (baseline) |
| **newLISP Neo** (Direct-Threaded VM + TCO + GenGC) | **42.6 ms** | **13.9x faster** | **2.02x FASTER than Python 3.14** |

---

### 2. 1-Million Iteration Loop: `(loop-test 1000000)`

Lisp code ([`bench_loop.lsp`](bench_loop.lsp)):
```lisp
;; 1. Iterative While Loop
(define (loop-test n)
  (let (s 0 i 0)
    (while (< i n)
      (set 's (+ s i))
      (set 'i (+ i 1)))
    s))

;; 2. Tail-Recursive Loop (TCO)
(define (loop-tco-acc n s i)
  (if (< i n)
      (loop-tco-acc n (+ s i) (+ i 1))
      s))
(define (loop-tco n)
  (loop-tco-acc n 0 0))
```

Python reference code ([`bench.py`](bench.py)):
```python
def loop_test(n):
    s = 0
    i = 0
    while i < n:
        s += i
        i += 1
    return s
```

| Runtime Engine | Execution Time | Speedup vs Original newLISP | Comparison vs Python 3.14 |
|---|---|---|---|
| **Original newLISP 10.7.6** (Tree-Walker, `while`) | 190.2 ms | 1.00x | 5.11x slower |
| **Original newLISP 10.7.6** (Tail Recursion) | Stack Overflow | — | — |
| **CPython 3.12.3** (Standard Python VM, `while`) | 78.8 ms | 2.41x faster | 2.12x slower |
| **CPython 3.14.4** (Optimized Python VM, `while`) | 37.2 ms | 5.11x faster | 1.00x (baseline) |
| **CPython 3.14.4** (Tail Recursion) | RecursionError | — | — |
| **newLISP Neo** (Direct-Threaded VM, `while` loop) | **31.3 ms** | **6.08x faster** | **1.19x FASTER than Python 3.14** |
| **newLISP Neo** (TCO Engine, Tail-Recursive loop) | **17.3 ms** | **11.0x faster** | **2.15x FASTER than Python 3.14** |

---

### 3. Tail Call Optimization (TCO) and Mutual Recursion

Lisp code ([`bench_tco.lsp`](bench_tco.lsp)):
```lisp
;; 100,000,000 step self-tail recursion
(define (count-down n)
  (if (<= n 0) "done" (count-down (- n 1))))

;; 10,000,000 step mutual tail recursion
(define (my-even? n)
  (if (= n 0) true (my-odd? (- n 1))))

(define (my-odd? n)
  (if (= n 0) nil (my-even? (- n 1))))
```

Python reference:
> *Python does not support tail call optimization. Deep recursions trigger `RecursionError: maximum recursion depth exceeded` (default limit 1,000).*

| Benchmark Task | Original newLISP 10.7.6 | Python 3.14 | newLISP Neo (TCO Engine) |
|---|---|---|---|
| **Self-Tail Recursion (100M steps)** | Stack Overflow (`ERR: out of call stack`) | `RecursionError` | **1,023 ms** ($O(1)$ stack, constant memory) |
| **Tail Accumulator (10M steps)** | Stack Overflow (`ERR: out of call stack`) | `RecursionError` | **151 ms** ($O(1)$ stack, constant memory) |
| **Mutual Tail Recursion (10M steps)** | Stack Overflow (`ERR: out of call stack`) | `RecursionError` | **167 ms** ($O(1)$ stack, constant memory) |
| **Tail Call in `cond` (1M steps)** | Stack Overflow (`ERR: out of call stack`) | `RecursionError` | **21 ms** ($O(1)$ stack, constant memory) |
| **Tail Call in `let` (1M steps)** | Stack Overflow (`ERR: out of call stack`) | `RecursionError` | **12 ms** ($O(1)$ stack, constant memory) |

---

## Building and Installation

### Prerequisites
- GCC / MinGW-w64 or Clang (supporting C99/GNU extensions for computed gotos)
- GNU Make

### Build on Windows (MinGW-w64)
```powershell
# Batch file (with MinGW-w64 in PATH):
.\make.bat

# Or build with a specific makefile:
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
./newlisp bench_tco.lsp
python bench.py
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
├── bench_tco.lsp             # Tail Call Optimization (TCO) benchmark harness
├── bench.py                  # Python 3.14 benchmark comparison harness
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

- Copyright (C) 2020 Lutz Mueller
- Copyright (C) 2026 KIM Taegyoon
- **newLISP** was originally designed and implemented by **Lutz Mueller** ([Nuevatec](http://www.newlisp.org)).
- **newLISP Neo** is released under the [GNU General Public License Version 3 (GPLv3)](LICENSE). See [`LICENSE`](LICENSE) or [`doc/COPYING.txt`](doc/COPYING.txt) for the complete license text.
- Documentation files are distributed under the GNU Free Documentation License (GFDL).
