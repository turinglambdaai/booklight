#lang racket/base

;; Bookshelf store: format round-trip in the library.json shape, upsert
;; identity semantics (collections/added-at survive re-import), missing
;; status scanning and clearing, relink, collection create/delete/strip,
;; and the tolerant-decode path (missing/corrupt fields fall back).

(require json
         rackunit
         racket/file
         racket/port
         racket/runtime-path
         racket/string
         "../booklight/jsonw.rkt"
         "../booklight/library.rkt"
         "../booklight/settings.rkt")

(define temp-root (make-temporary-file "pdfgist-library-~a" 'directory))

(dynamic-wind
 void
 (lambda ()
   (parameterize ([current-config-dir temp-root])
     (define (entry path
                    #:title [title "T"]
                    #:author [author ""]
                    #:kind [kind "pdf"]
                    #:pages [pages 10]
                    #:added-at [added-at 100]
                    #:cover [cover ""]
                    #:collections [collections '()]
                    #:status [status "ok"])
       (book-entry path title author kind pages added-at cover collections status))

     ;; ---- defaults on a missing file ----
     (check-false (file-exists? (library-file-path)))
     (define empty (load-shelf))
     (check-equal? (shelf-books empty) '())
     (check-equal? (shelf-collections empty) '())

     ;; ---- upsert + format round-trip ----
     (define b1 (entry "/books/alpha.pdf" #:title "Alpha" #:author "A" #:pages 42))
     (define b2 (entry "/books/beta.epub" #:title "Beta" #:kind "epub" #:pages 7
                       #:cover "abc.png"))
     (save-shelf! (shelf (upsert-book (upsert-book '() b1) b2) '("论文")))
     (check-true (file-exists? (library-file-path)))
     (define reloaded (load-shelf))
     (check-equal? (length (shelf-books reloaded)) 2)
     (check-equal? (book-entry-path (car (shelf-books reloaded)))
                   "/books/beta.epub")
     (check-equal? (book-entry-kind (car (shelf-books reloaded))) "epub")
     (check-equal? (book-entry-cover (car (shelf-books reloaded))) "abc.png")
     (check-equal? (shelf-collections reloaded) '("论文"))

     ;; ordered pretty format: format key first, books, collections
     (define text (call-with-input-file (library-file-path) port->string))
     (check-true (string-contains? text "\"format\": 1"))

     ;; ---- upsert identity: re-import keeps collections + added-at ----
     (define tagged (entry "/books/alpha.pdf" #:title "Alpha 2" #:pages 43
                           #:added-at 999
                           #:collections '("论文")))
     (define after-upsert (upsert-book (shelf-books reloaded) tagged))
     (define kept (findf (lambda (b) (string=? (book-entry-path b)
                                               "/books/alpha.pdf"))
                         after-upsert))
     ;; fresh values win for catalog metadata…
     (check-equal? (book-entry-title kept) "Alpha 2")
     (check-equal? (book-entry-pages kept) 43)
     ;; …but shelf identity is stable
     (check-equal? (book-entry-added-at kept) 100)
     ;; a fresh entry (no collections in the new value) keeps existing ones
     (save-shelf! (shelf (list tagged) '("论文")))
     (define re-imported
       (upsert-book (shelf-books (load-shelf))
                    (entry "/books/alpha.pdf" #:title "Alpha 3")))
     (check-equal? (book-entry-collections
                    (car re-imported))
                   '("论文"))

     ;; ---- remove ----
     (check-equal? (length (remove-book re-imported "/books/alpha.pdf")) 0)

     ;; ---- missing status: scan flags and clears ----
     (define lib (shelf (list (entry "/gone.pdf") (entry "/books/alpha.pdf"))
                        '()))
     (define (fake-stat path) (string=? path "/books/alpha.pdf"))
     (define scanned (scan-shelf (shelf-books lib) fake-stat))
     (check-equal? (book-entry-status (car scanned)) "missing")
     (check-equal? (book-entry-status (cadr scanned)) "ok")
     ;; a second scan keeps "missing" sticky
     (check-equal? (book-entry-status
                    (car (scan-shelf scanned fake-stat)))
                   "missing")
     ;; the file coming back clears it
     (define recovered (scan-shelf scanned (lambda (_) #t)))
     (check-equal? (book-entry-status (car recovered)) "ok")

     ;; ---- relink ----
     (define relinked (relink-book (shelf-books lib) "/gone.pdf" "/new.pdf"))
     (check-equal? (book-entry-path (car relinked)) "/new.pdf")
     (check-equal? (book-entry-status (car relinked)) "ok")

     ;; ---- collections ----
     (define cols '("论文"))
     (check-equal? (collection-create cols "闲书") '("论文" "闲书"))
     (check-equal? (collection-create cols "论文") cols) ; idempotent
     (check-equal? (collection-delete cols "论文") '())

     (define shelf-books* (list (entry "/books/alpha.pdf")))
     (define with-col (book-collection-add shelf-books* "/books/alpha.pdf" "闲书"))
     (check-equal? (book-entry-collections (car with-col)) '("闲书"))
     ;; adding twice does not duplicate
     (check-equal? (book-entry-collections
                    (car (book-collection-add with-col "/books/alpha.pdf" "闲书")))
                   '("闲书"))
     (check-equal? (book-entry-collections
                    (car (book-collection-remove with-col "/books/alpha.pdf" "闲书")))
                   '())
     ;; stripping a collection clears it from books
     (check-equal? (book-entry-collections
                    (car (strip-collection with-col "闲书")))
                   '())

     ;; ---- tolerant decode: junk fields fall back per field ----
     (define junk
       (hasheq 'format 1
               'books (list (hasheq 'path "/x.pdf"
                                    'pages "not-a-number"
                                    'kind "comic"
                                    'collections "not-a-list"
                                    'status "bogus")
                            "not-an-object")
               'collections (list "论文" 42)))
     (define decoded (storage->shelf junk))
     (define dx (car (shelf-books decoded)))
     (check-equal? (book-entry-path dx) "/x.pdf")
     (check-equal? (book-entry-pages dx) 1)         ; bad int -> default 1
     (check-equal? (book-entry-kind dx) "pdf")      ; unknown kind -> pdf
     (check-equal? (book-entry-collections dx) '()) ; not-a-list -> empty
     (check-equal? (book-entry-status dx) "ok")     ; unknown status -> ok
     (check-equal? (length (shelf-books decoded)) 1) ; junk row dropped
     (check-equal? (shelf-collections decoded) '("论文")) ; junk entries dropped

     ;; ---- update-shelf! read-modify-write persists ----
     (update-shelf!
      (lambda (current)
        (shelf (upsert-book (shelf-books current)
                            (entry "/books/alpha.pdf" #:title "Alpha"))
               '("论文"))))
     (check-equal? (book-entry-title
                    (car (shelf-books (load-shelf))))
                   "Alpha")))
 (lambda ()
   (delete-directory/files temp-root)))
