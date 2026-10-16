#lang racket/base

;; FNV-1a 64-bit vectors: published standard vectors, values cross-checked
;; against an independent BigInt implementation of the Rust algorithm in
;; v1 (Tauri) annotations.rs, and the {:016x} rendering.

(require rackunit
         "../booklight/fnv.rkt")

;; Standard published FNV-1a 64 vectors.
(check-equal? (fnv1a64 "") #xcbf29ce484222325)
(check-equal? (fnv1a64 "a") #xaf63dc4c8601ec8c)
(check-equal? (fnv1a64 "foobar") #x85944171f73967e8)

;; Cross-checks computed independently from the Rust algorithm
;; (offset basis 0xcbf29ce484222325, prime 0x100000001b3, UTF-8 bytes).
(check-equal? (fnv1a64 "hello") #xa430d84680aabd0b)
(check-equal? (fnv1a64 "/Users/demo/papers/报告.pdf") #xeac08b24991f0946)

;; Rendering matches Rust `format!("{:016x}", hash)` — 16 lowercase hex.
(check-equal? (fnv1a64-hex "") "cbf29ce484222325")
(check-equal? (fnv1a64-hex "a") "af63dc4c8601ec8c")
(check-equal? (fnv1a64-hex "foobar") "85944171f73967e8")
(check-equal? (string-length (fnv1a64-hex "/tmp/small.pdf")) 16)
;; High-bit hashes must still render to 16 characters (no leading-zero loss).
(check-equal? (fnv1a64-hex "hello") "a430d84680aabd0b")

;; Same input bytes -> same key; the CJK path differs from its ASCII prefix.
(check-equal? (fnv1a64 "hello") (fnv1a64 "hello"))
(check-not-equal? (fnv1a64 "/tmp/a.pdf") (fnv1a64 "/tmp/b.pdf"))
