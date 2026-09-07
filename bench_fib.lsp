(define (fib n)
  (if (< n 2)
      n
      (+ (fib (- n 1)) (fib (- n 2)))))

(println "Warmup (fib 20): " (fib 20))
(println "Benchmarking (fib 30)...")
(set 't0 (time (set 'res (fib 30))))
(println "Result: " res)
(println "Time: " t0 " ms")
(exit)
