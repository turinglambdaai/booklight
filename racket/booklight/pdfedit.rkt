;; Page-edit service layer: file/bytes in, bytes out — the shape the RPC
;; surface needs (the host decides where to persist results). All numeric
;; ratios arrive as milli-units (RVT1 has no floats).
#lang racket/base

(require racket/list
         racket/math
         "pdfdoc.rkt")

(provide edit-delete-pages
         edit-rotate-pages
         edit-insert-blank-after
         edit-extract-pages
         edit-append-doc
         edit-bake-text
         (struct-out edit-box))

(struct edit-box (page x-ratio-milli y-ratio-milli text size) #:transparent)

(define (check-bytes data)
  (unless (and (bytes? data) (>= (bytes-length data) 5)
               (equal? (subbytes data 0 5) #"%PDF-"))
    (raise (pdf-error 'edit "不是有效的 PDF 文件"))))

(define (edit-delete-pages data pages)
  (check-bytes data)
  (define doc (parse-pdf data))
  (pdf-delete-pages! doc (map exact->inexact pages))
  (pdf->bytes doc))

(define (edit-rotate-pages data pages delta)
  (check-bytes data)
  (define doc (parse-pdf data))
  (pdf-rotate-pages! doc (map exact->inexact pages) delta)
  (pdf->bytes doc))

(define (edit-insert-blank-after data page)
  (check-bytes data)
  (define doc (parse-pdf data))
  (pdf-insert-blank-after! doc (exact->inexact page))
  (pdf->bytes doc))

(define (edit-extract-pages data pages)
  (check-bytes data)
  (define doc (parse-pdf data))
  (let-values (((_ out) (pdf-extract-pages doc (map exact->inexact pages))))
    out))

(define (edit-append-doc data other-data)
  (check-bytes data)
  (check-bytes other-data)
  (define doc (parse-pdf data))
  (pdf-append-doc! doc (parse-pdf other-data))
  (pdf->bytes doc))

;; milli ratios -> 0..1; size in points arrives as an integer
(define (edit-bake-text data
                        #:watermark-text (wm-text "")
                        #:watermark-size (wm-size 48)
                        #:watermark-opacity-milli (wm-opacity-milli 300)
                        #:boxes (boxes '()))
  (check-bytes data)
  (define doc (parse-pdf data))
  (pdf-bake-text doc
                 #:watermark-text wm-text
                 #:watermark-size wm-size
                 #:watermark-opacity (min 1.0 (max 0.0 (/ wm-opacity-milli 1000.0)))
                 #:textboxes
                 (for/list ((b (in-list boxes)))
                   (textbox (exact->inexact (edit-box-page b))
                            (min 1.0 (max 0.0 (/ (edit-box-x-ratio-milli b) 1000.0)))
                            (min 1.0 (max 0.0 (/ (edit-box-y-ratio-milli b) 1000.0)))
                            (edit-box-text b)
                            (exact->inexact (edit-box-size b)))))
  (pdf->bytes doc))
