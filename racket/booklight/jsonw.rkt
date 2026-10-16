#lang racket/base

;; Minimal ordered pretty-JSON writer. serde_json::to_string_pretty (used by
;; the old Rust backend for settings.json and annotation files) keeps struct
;; field order and formats with two-space indentation; racket/json hashes
;; have no defined key order. The v1 files therefore round-trip through
;; ordered values here:
;;   - (jobj fields)          JSON object, fields is (list (cons "key" value) ...)
;;   - list                   JSON array
;;   - string / bool / number scalar

(require json
         racket/port)

(provide jobj
         jobj?
         jobj-fields
         write-json-value
         json-value->bytes)

(struct jobj (fields) #:transparent)

(define (write-json-value v out [indent 0])
  (define pad (make-string indent #\space))
  (define pad-child (make-string (+ indent 2) #\space))
  (cond
    [(jobj? v)
     (define fields (jobj-fields v))
     (cond
       [(null? fields) (display "{}" out)]
       [else
        (display "{\n" out)
        (let loop ([items fields])
          (display pad-child out)
          (display (jsexpr->string (car (car items))) out)
          (display ": " out)
          (write-json-value (cdr (car items)) out (+ indent 2))
          (cond
            [(null? (cdr items))
             (display "\n" out)
             (display (string-append pad "}") out)]
            [else
             (display ",\n" out)
             (loop (cdr items))]))])]
    [(list? v)
     (cond
       [(null? v) (display "[]" out)]
       [else
        (display "[\n" out)
        (let loop ([items v])
          (display pad-child out)
          (write-json-value (car items) out (+ indent 2))
          (cond
            [(null? (cdr items))
             (display "\n" out)
             (display (string-append pad "]") out)]
            [else
             (display ",\n" out)
             (loop (cdr items))]))])]
    [(string? v) (display (jsexpr->string v) out)]
    [(boolean? v) (display (if v "true" "false") out)]
    [(exact-integer? v) (display (number->string v) out)]
    ;; Floats print like Rust f64 for the common cases (0.0 -> "0.0").
    [(real? v) (display (format "~a" (exact->inexact v)) out)]
    [else (raise-argument-error 'write-json-value "json-value" v)]))

;; Matches the v1 files: serde_json pretty output with no trailing newline.
(define (json-value->bytes v)
  (call-with-output-bytes
   (lambda (out)
     (write-json-value v out))))
