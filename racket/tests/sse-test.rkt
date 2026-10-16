#lang racket/base

;; SSE semantics: line classification and delta extraction (parity with the
;; llm.rs byte loop) plus StreamBuffer display rules (port of the sidebar.ts
;; StreamBuffer class), driven through fixture streams.

(require rackunit
         racket/port
         "../booklight/sse.rkt"
         "../booklight/llm.rkt")

;; ---- line classification ----

(check-equal? (sse-line-action "data: hello") (list 'data "hello"))
(check-equal? (sse-line-action "data:hello") (list 'data "hello"))
(check-equal? (sse-line-action "data: [DONE]") '(done))
(check-equal? (sse-line-action "data:[DONE]") '(done))
(check-equal? (sse-line-action "data:") '(skip))
(check-equal? (sse-line-action "data:   ") '(skip))
(check-equal? (sse-line-action "") '(skip))
(check-equal? (sse-line-action ": comment") '(skip))
(check-equal? (sse-line-action "event: delta") '(skip))
(check-equal? (sse-line-action "id: 42") '(skip))
(check-equal? (sse-line-action "  data: trimmed  ") (list 'data "trimmed"))

;; ---- delta extraction ----

(check-equal? (payload->delta "{\"choices\":[{\"delta\":{\"content\":\"你\"}}]}")
              (list 'delta "你" #f))
(check-equal? (payload->delta "{\"choices\":[{\"delta\":{\"reasoning_content\":\"想\"}}]}")
              (list 'delta "想" #t))
;; Rust checks content first; null content falls through to reasoning.
(check-equal? (payload->delta "{\"choices\":[{\"delta\":{\"content\":null,\"reasoning_content\":\"想\"}}]}")
              (list 'delta "想" #t))
(check-equal? (payload->delta "{\"choices\":[{\"delta\":{}}]}") '(skip))
(check-equal? (payload->delta "{\"choices\":[{\"delta\":{\"content\":123}}]}") '(skip))
(check-equal? (payload->delta "{\"choices\":[]}") '(skip))
(check-equal? (payload->delta "{}") '(skip))
(check-equal? (payload->delta "not json") '(skip))
(check-equal? (payload->delta "") '(skip))

;; ---- StreamBuffer ----

(define sb (make-stream-buffer))
(check-equal? (stream-buffer-state sb) 'empty)
(check-true (stream-buffer-empty? sb))

;; Reasoning-only stream shows the thinking state while running.
(stream-buffer-push! sb "推理" #t)
(check-equal? (stream-buffer-state sb) 'thinking)
(check-equal? (stream-buffer-state sb #:final? #t) 'reasoning)
(check-equal? (stream-buffer-text sb) "")

;; The answer replaces thinking once it arrives.
(stream-buffer-push! sb "答案" #f)
(check-equal? (stream-buffer-state sb) 'content)
(check-equal? (stream-buffer-state sb #:final? #t) 'content)
(check-equal? (stream-buffer-text sb) "答案")
(check-false (stream-buffer-empty? sb))

;; Reasoning after the answer started is dropped (sidebar.ts gotContent).
(stream-buffer-push! sb "迟到的推理" #t)
(check-equal? (stream-buffer-text sb) "答案")
(check-equal? (stream-buffer-reasoning sb) "推理")

;; Accumulation across chunks.
(define sb2 (make-stream-buffer))
(stream-buffer-push! sb2 "你" #f)
(stream-buffer-push! sb2 "好" #f)
(check-equal? (stream-buffer-text sb2) "你好")

;; ---- fixture streams through the parser ----

(define (collect-stream text)
  (define chunks '())
  (define outcome
    (parse-sse-stream!
     (open-input-string text)
     (lambda (value reasoning)
       (set! chunks (cons (cons value reasoning) chunks)))))
  (values outcome (reverse chunks)))

;; Normal deltas + comments + non-data lines + [DONE]; a trailing data line
;; after [DONE] must never be processed.
(define-values (done-outcome done-chunks)
  (collect-stream
   (string-append
    ": keepalive\n"
    "event: message\n"
    "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"思考\"}}]}\n"
    "\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"你\"}}]}\n"
    "\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"好\"}}]}\n"
    "\n"
    "data: [DONE]\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"ignored\"}}]}\n")))
(check-equal? done-outcome 'done)
(check-equal? done-chunks
              (list (cons "思考" #t) (cons "你" #f) (cons "好" #f)))

;; Reasoning-only stream: parser flags every chunk as reasoning.
(define-values (reasoning-outcome reasoning-chunks)
  (collect-stream
   (string-append
    "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"a\"}}]}\n"
    "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"b\"}}]}\n"
    "data: [DONE]\n")))
(check-equal? reasoning-outcome 'done)
(check-equal? reasoning-chunks (list (cons "a" #t) (cons "b" #t)))

;; Malformed JSON lines are skipped (Rust: `if let Ok(value)`).
(define-values (malformed-outcome malformed-chunks)
  (collect-stream
   (string-append
    "data: {broken\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n"
    "data: [DONE]\n")))
(check-equal? malformed-outcome 'done)
(check-equal? malformed-chunks (list (cons "ok" #f)))

;; Empty-answer deltas are dropped (llm.rs `if !text.is_empty()`).
(define-values (empty-outcome empty-chunks)
  (collect-stream
   (string-append
    "data: {\"choices\":[{\"delta\":{\"content\":\"\"}}]}\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n"
    "data: [DONE]\n")))
(check-equal? empty-outcome 'done)
(check-equal? empty-chunks (list (cons "x" #f)))

;; EOF without [DONE] returns 'end.
(define-values (eof-outcome eof-chunks)
  (collect-stream
   "data: {\"choices\":[{\"delta\":{\"content\":\"z\"}}]}\n"))
(check-equal? eof-outcome 'end)
(check-equal? eof-chunks (list (cons "z" #f)))

;; A single SSE line crossing the 4 MiB cap is a protocol error. The writer
;; blocks on a full pipe once the reader aborts, so it is killed explicitly
;; instead of being waited on. make-pipe returns the read end first.
(define-values (huge-pipe huge-out) (make-pipe))
(define huge-writer
  (thread
   (lambda ()
     (define buf (make-bytes 65536 97)) ; 'a' * 65536
     (let loop ([remaining (add1 (* 4 1024 1024))])
       (unless (zero? remaining)
         (define n (min remaining 65536))
         (write-bytes buf huge-out 0 n)
         (loop (- remaining n))))
     (close-output-port huge-out))))
(check-exn
 (lambda (e)
   (and (exn:fail? e)
        (regexp-match? #rx"单行过长" (exn-message e))))
 (lambda ()
   (parse-sse-stream! huge-pipe (lambda (_v _r) (void)))))
(kill-thread huge-writer)
(close-input-port huge-pipe)
