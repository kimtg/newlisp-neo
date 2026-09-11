# Benchmark comparison harness for Python 3.14 vs newLISP Neo
import time

def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

def loop_test(n):
    s = 0
    i = 0
    while i < n:
        s += i
        i += 1
    return s

if __name__ == '__main__':
    print(f'Warmup (fib 20): {fib(20)}')
    print('Benchmarking (fib 30)...')
    t0 = time.perf_counter()
    res_fib = fib(30)
    t1 = time.perf_counter()
    print(f'Result: {res_fib}')
    print(f'Time: {(t1 - t0) * 1000:.3f} ms\n')

    print(f'Warmup: {loop_test(1000)}')
    print('Benchmarking (loop-test 1000000)...')
    t0 = time.perf_counter()
    res_loop = loop_test(1_000_000)
    t1 = time.perf_counter()
    print(f'Result: {res_loop}')
    print(f'Time: {(t1 - t0) * 1000:.3f} ms')
