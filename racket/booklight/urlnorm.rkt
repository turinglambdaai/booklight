#lang racket/base

(require racket/string)

(provide normalize-base-url
         truncate-chars
         chat-completions-suffix)

(define chat-completions-suffix "/chat/completions")

;; Byte-for-byte port of the v1 (Tauri) llm.rs `normalize_base_url` (including its
;; unit tests): accepts bare hosts, versioned bases (/v1, /v3, /v4, ...) and
;; even full chat-completions URLs. A URL already ending in a version segment
;; is kept as-is; /v1 is only appended when no version segment exists at all.
;;
;; Provider parity cases (see racket/tests/urlnorm-test.rkt):
;;  - GLM  .../api/paas/v4        and .../api/coding/paas/v4 stay untouched
;;  - Doubao .../api/v3           stays untouched (no /v1 appended)
;;  - ".../v1/chat/completions"   -> ".../v1"  (suffix stripped)
;;  - "https://api.example.com"   -> "https://api.example.com/v1"
(define (normalize-base-url input)
  (unless (string? input)
    (raise-argument-error 'normalize-base-url "string?" input))
  (let loop ([base (strip-trailing-slashes (string-trim input))])
    (cond
      [(string-suffix? base chat-completions-suffix)
       (loop
        (strip-trailing-slashes
         (substring base
                    0
                    (- (string-length base)
                       (string-length chat-completions-suffix)))))]
      [(version-segment? (last-path-segment base)) base]
      [else (string-append base "/v1")])))

(define (strip-trailing-slashes s)
  (string-trim s "/" #:left? #f #:repeat? #t))

;; Rust `base.rsplit('/').next()` — the text after the last '/', or the whole
;; string when it contains no '/'.
(define (last-path-segment s)
  (cond
    [(regexp-match #px"/([^/]*)$" s) => cadr]
    [else s]))

;; Rust: segment strips an optional leading 'v'/'V'; the rest must be non-empty
;; and all ASCII digits.
(define (version-segment? segment)
  (and (>= (string-length segment) 2)
       (let ([first (string-ref segment 0)])
         (or (char=? first #\v) (char=? first #\V)))
       (for/and ([ch (in-string (substring segment 1))])
         (and (char>=? ch #\0) (char<=? ch #\9)))))

;; Port of the v1 (Tauri) llm.rs `truncate_chars`: cut by Unicode scalar
;; values (Rust `chars`) and append an ellipsis when anything was dropped.
(define (truncate-chars s max-chars)
  (unless (and (string? s) (exact-nonnegative-integer? max-chars))
    (raise-arguments-error 'truncate-chars
                           "expected string and nonnegative integer"
                           "received" s max-chars))
  (if (<= (string-length s) max-chars)
      s
      (string-append (substring s 0 max-chars) "…")))
