#lang racket/base

;; SSE line semantics and StreamBuffer, ported from the v1 (Tauri) llm.rs
;; byte-stream loop and sidebar.ts StreamBuffer class.

(require json
         racket/string)

(provide sse-line-action
         payload->delta
         stream-buffer
         stream-buffer?
         make-stream-buffer
         stream-buffer-push!
         stream-buffer-content
         stream-buffer-reasoning
         stream-buffer-state
         stream-buffer-text
         stream-buffer-empty?)

;; ---- line classification (parity with the llm.rs SSE loop) ----
;;
;; Every line is trimmed. Lines without a `data:` prefix (event:/id:/
;; comments/blank) are skipped. `data: [DONE]` terminates the stream. An empty
;; data payload is skipped. Returns '(done), '(skip), or (list 'data payload).
(define (sse-line-action line)
  (unless (string? line)
    (raise-argument-error 'sse-line-action "string?" line))
  (define trimmed (string-trim line))
  (if (string-prefix? trimmed "data:")
      (let ([payload (string-trim (substring trimmed (string-length "data:")))])
        (cond
          [(string=? payload "[DONE]") '(done)]
          [(string=? payload "") '(skip)]
          [else (list 'data payload)]))
      '(skip)))

;; choices[0].delta: `content` wins over `reasoning_content` (Rust checks
;; content first). Non-string content falls through to the reasoning field,
;; exactly like serde's `as_str()`. Returns '(delta text is-reasoning) or
;; '(skip) for non-JSON payloads and deltas without a usable text field.
;; Empty strings are still reported; the caller drops them (parity:
;; `if !text.is_empty()`).
(define (payload->delta payload)
  (unless (string? payload)
    (raise-argument-error 'payload->delta "string?" payload))
  (with-handlers ([exn:fail? (lambda (_) '(skip))])
    (define value (read-json (open-input-string payload)))
    (define choices
      (and (hash? value) (hash-ref value 'choices #f)))
    (define choice
      (and (pair? choices) (hash? (car choices)) (car choices)))
    (define delta
      (and choice (hash-ref choice 'delta #f)))
    (cond
      [(not (hash? delta)) '(skip)]
      [(string? (hash-ref delta 'content #f))
       (list 'delta (hash-ref delta 'content) #f)]
      [(string? (hash-ref delta 'reasoning_content #f))
       (list 'delta (hash-ref delta 'reasoning_content) #t)]
      [else '(skip)])))

;; ---- StreamBuffer (port of the v1 sidebar.ts StreamBuffer class) ----
;;
;; Accumulates reasoning and answer separately. While streaming, reasoning is
;; only a compact "thinking" indicator — raw chain-of-thought is noise for a
;; reader. With #:final? #t the state falls back to the dimmed reasoning,
;; covering providers that put the whole answer into the reasoning field.
(struct stream-buffer (reasoning content got-content?) #:transparent
  #:mutable)

(define (make-stream-buffer)
  (stream-buffer "" "" #f))

;; (stream-buffer-push! buffer text is-reasoning)
;; Reasoning arriving after the answer started is dropped (parity with the
;; `if (!this.gotContent)` guard in sidebar.ts).
(define (stream-buffer-push! buffer text is-reasoning)
  (unless (stream-buffer? buffer)
    (raise-argument-error 'stream-buffer-push! "stream-buffer?" buffer))
  (unless (string? text)
    (raise-argument-error 'stream-buffer-push! "string?" text))
  (unless (boolean? is-reasoning)
    (raise-argument-error 'stream-buffer-push! "boolean?" is-reasoning))
  (if is-reasoning
      (unless (stream-buffer-got-content? buffer)
        (set-stream-buffer-reasoning!
         buffer
         (string-append (stream-buffer-reasoning buffer) text)))
      (begin
        (set-stream-buffer-got-content?! buffer #t)
        (set-stream-buffer-content!
         buffer
         (string-append (stream-buffer-content buffer) text))))
  (void))

;; 'content  — answer text exists (answer replaces the thinking indicator)
;; 'thinking — reasoning seen so far, stream still running
;; 'reasoning — stream ended with reasoning but no answer (degraded display)
;; 'empty    — nothing at all
(define (stream-buffer-state buffer #:final? [final? #f])
  (unless (stream-buffer? buffer)
    (raise-argument-error 'stream-buffer-state "stream-buffer?" buffer))
  (cond
    [(> (string-length (stream-buffer-content buffer)) 0) 'content]
    [(> (string-length (stream-buffer-reasoning buffer)) 0)
     (if final? 'reasoning 'thinking)]
    [else 'empty]))

(define (stream-buffer-text buffer)
  (stream-buffer-content buffer))

(define (stream-buffer-empty? buffer)
  (and (zero? (string-length (stream-buffer-content buffer)))
       (zero? (string-length (stream-buffer-reasoning buffer)))))
