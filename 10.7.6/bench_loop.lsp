(define (loop-test n)
  (let (s 0 i 0)
    (while (< i n)
      (set 's (+ s i))
      (set 'i (+ i 1)))
    s))

(println "Warmup: " (loop-test 1000))
(set 't0 (time (set 'res (loop-test 1000000))))
(println "Result: " res)
(println "Time: " t0 " ms")
(exit)

