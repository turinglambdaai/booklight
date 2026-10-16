#lang racket/base

;; FNV-1a 64-bit, byte-for-byte identical to the v1 (Tauri) annotations store.
;; The annotation storage key must stay identical across the old (Rust) and
;; new (Racket) implementations, so the offset basis, prime, and UTF-8 byte
;; iteration order are fixed by cross-version file-name compatibility.

(provide fnv1a64
         fnv1a64-hex)

(define mask64 (sub1 (arithmetic-shift 1 64)))
(define offset-basis 14695981039346656037) ; 0xcbf29ce484222325
(define prime64 1099511628211)             ; 0x00000100000001b3

;; (fnv1a64 string) -> exact nonnegative integer < 2^64
(define (fnv1a64 str)
  (unless (string? str)
    (raise-argument-error 'fnv1a64 "string?" str))
  (for/fold ([hash offset-basis])
            ([byte (in-bytes (string->bytes/utf-8 str))])
    (bitwise-and mask64 (* (bitwise-xor hash byte) prime64))))

;; (fnv1a64-hex string) -> 16 lowercase hex characters, zero-padded.
;; Same rendering as Rust `format!("{:016x}", fnv1a64(s))`.
(define (fnv1a64-hex str)
  (define digits (number->string (fnv1a64 str) 16))
  (string-append (make-string (- 16 (string-length digits)) #\0) digits))
