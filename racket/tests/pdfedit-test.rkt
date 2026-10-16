;; Tests for the page-edit service layer (byte-in/byte-out shapes).
#lang racket/base

(require rackunit
         racket/bytes
         racket/string
         "../booklight/pdfdoc.rkt"
         "../booklight/pdfedit.rkt")

;; Racket 9.3 lacks bytes-contains?
(define (bytes-contains? haystack needle)
  (and (regexp-match? (byte-regexp needle) haystack) #t))

;; minimal in-memory doc via the writer (source of valid PDF bytes)
(define (mk-bytes texts)
  (define objects (make-hash))
  (define page-refs
    (for/list ((i (in-range 1 (add1 (length texts)))))
      (ref (+ 10 (* i 10)) 0)))
  (hash-set! objects (cons 1 0) (hasheq '/Type '/Catalog '/Pages (ref 2 0)))
  (hash-set! objects (cons 2 0)
             (hasheq '/Type '/Pages
                     '/Kids (list->vector page-refs)
                     '/Count (length texts)))
  (for ((t (in-list texts)) (pr (in-list page-refs)) (i (in-naturals 1)))
    (hash-set! objects (cons (ref-num pr) 0)
               (hasheq '/Type '/Page '/Parent (ref 2 0)
                       '/MediaBox (vector 0 0 612 792)
                       '/Resources (hasheq)
                       '/Contents (ref (+ 11 (* i 10)) 0)))
    (hash-set! objects (cons (+ 11 (* i 10)) 0)
               (pstream (hasheq)
                        (string->bytes/latin-1 (format "BT (~a) Tj ET" t)))))
  (pdf->bytes (pdfdoc objects (ref 1 0))))

(define base (mk-bytes (list "one" "two" "three")))

(test-case "delete: bytes out reparse with fewer pages"
  (define out (edit-delete-pages base (list 2)))
  (define d (parse-pdf out))
  (check-equal? (length (pdf-page-refs d)) 2))

(test-case "rotate + insert + extract + append roundtrip"
  (define r (parse-pdf (edit-rotate-pages base (list 1) 90)))
  (check-equal? (hash-ref (pdf-resolve r (car (pdf-page-refs r))) '/Rotate) 90)
  (define i (parse-pdf (edit-insert-blank-after base 1)))
  (check-equal? (length (pdf-page-refs i)) 4)
  (define e (parse-pdf (edit-extract-pages base (list 3 1))))
  (check-equal? (length (pdf-page-refs e)) 2)
  (define a (parse-pdf (edit-append-doc base base)))
  (check-equal? (length (pdf-page-refs a)) 6))

(test-case "bake: milli ratios arrive, watermark + box in content"
  (define out
    (edit-bake-text base
                    #:watermark-text "DRAFT"
                    #:watermark-size 48
                    #:watermark-opacity-milli 300
                    #:boxes (list (edit-box 2 250 500 "note" 14))))
  (define d (parse-pdf out))
  (define pd2 (pdf-resolve d (list-ref (pdf-page-refs d) 1)))
  (define cs (pdf-resolve d (hash-ref pd2 '/Contents)))
  (check-true (bytes-contains? (pstream-payload cs) #"DRAFT"))
  (check-true (bytes-contains? (pstream-payload cs) #"note")))

(test-case "invalid input rejected with a domain error"
  (check-exn pdf-error? (λ () (edit-delete-pages #"not a pdf" (list 1)))))
