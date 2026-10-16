#lang racket/base

;; The bookshelf: a single documented library file cataloging the user's
;; books in place. Books are NEVER copied — the shelf references the
;; user's own files (see shared/spec/DATA-FORMAT.md); annotations and
;; reading progress live beside each book in sidecars, so library.json
;; holds only catalog state: entries, collections, cover-cache pointers.
;;
;; <config-dir>/library.json, format 1:
;;   {"format": 1,
;;    "books": [{"path": "...",            ; absolute path = identity
;;               "title": "...", "author": "",
;;               "kind": "pdf" | "epub",
;;               "pages": 42,              ; page (pdf) / chapter (epub) count
;;               "added_at": unix-s,
;;               "cover": "<fnv1a64-hex>.png" | "",
;;               "collections": ["..."],
;;               "status": "ok" | "missing"}],
;;    "collections": ["..."]}
;;
;; - The host supplies title/author/pages/cover at import (it owns the
;;   renderers); the engine stores and manages them.
;; - `cover` names a file in the covers cache directory. The cache is
;;   regenerable and not part of the documented format.
;; - `status` turns "missing" when the path stops resolving (the user
;;   moved/renamed/deleted the file outside the app); a rescan that finds
;;   the path again, or a relink, clears it.
;; - Opening a book updates recents (settings.json) and the sidecar, not
;;   this file — the shelf is catalog state, not reading state.

(require json
         racket/file
         racket/list
         racket/string
         "i18n.rkt"
         "jsonw.rkt"
         "fnv.rkt"
         "settings.rkt")

(provide shelf-format
         shelf
         shelf?
         shelf-books
         shelf-collections
         book-entry
         book-entry?
         book-entry-path
         book-entry-title
         book-entry-author
         book-entry-kind
         book-entry-pages
         book-entry-added-at
         book-entry-cover
         book-entry-collections
         book-entry-status
         library-file-path
         covers-dir-path
         cover-file-name
         default-shelf
         load-shelf
         save-shelf!
         update-shelf!
         shelf->storage
         storage->shelf
         upsert-book
         remove-book
         relink-book
         scan-shelf
         collection-create
         collection-delete
         strip-collection
         book-collection-add
         book-collection-remove)

;; ---- model ----

(define shelf-format 1)

(struct shelf (books collections) #:transparent)
(struct book-entry (path title author kind pages added-at cover collections status)
  #:transparent)

(define (default-shelf)
  (shelf '() '()))

;; ---- entry helpers ----

(define (book-entry-kind? value)
  (or (string=? value "pdf") (string=? value "epub")))

;; Path is the identity: dedupe on it. The existing entry's collections
;; and added-at survive an upsert (re-importing a book never reshuffles
;; the shelf); title/author/pages/cover take the fresh values.
(define (upsert-book books entry)
  (unless (book-entry? entry)
    (raise-argument-error 'upsert-book "book-entry?" entry))
  (define existing (findf (lambda (b) (string=? (book-entry-path b)
                                                (book-entry-path entry)))
                          books))
  (define merged
    (if existing
        (struct-copy book-entry entry
                     [added-at (book-entry-added-at existing)]
                     [collections (book-entry-collections existing)])
        entry))
  (cons merged
        (filter (lambda (b) (not (string=? (book-entry-path b)
                                           (book-entry-path entry))))
                books)))

(define (remove-book books path)
  (filter (lambda (b) (not (string=? (book-entry-path b) path))) books))

(define (relink-book books from to)
  (for/list ([b (in-list books)])
    (if (string=? (book-entry-path b) from)
        (struct-copy book-entry b [path to] [status "ok"])
        b)))

;; Stat every entry's path and update the missing flag. `stat` is a
;; path -> boolean predicate injected by the caller (file-exists? in
;; production), keeping this pure and testable.
(define (scan-shelf books stat)
  (for/list ([b (in-list books)])
    (define ok (stat (book-entry-path b)))
    (cond
      [ok (if (string=? (book-entry-status b) "ok")
              b
              (struct-copy book-entry b [status "ok"]))]
      [(string=? (book-entry-status b) "missing") b]
      [else (struct-copy book-entry b [status "missing"])])))

(define (collection-create collections name)
  (unless (string? name)
    (raise-argument-error 'collection-create "string?" name))
  (if (findf (lambda (c) (string=? c name)) collections)
      collections
      (append collections (list name))))

(define (collection-delete collections name)
  (filter (lambda (c) (not (string=? c name))) collections))

;; Deleting a collection also strips the label from every book that
;; carries it (the collection is a view over books, not a folder).
(define (strip-collection books name)
  (for/list ([b (in-list books)])
    (struct-copy book-entry b
                 [collections
                  (filter (lambda (c) (not (string=? c name)))
                          (book-entry-collections b))])))

(define (book-collection-add books path name)
  (map-collection books path
                  (lambda (b)
                    (if (findf (lambda (c) (string=? c name))
                               (book-entry-collections b))
                        b
                        (struct-copy book-entry b
                                     [collections
                                      (append (book-entry-collections b)
                                              (list name))])))))

(define (book-collection-remove books path name)
  (map-collection books path
                  (lambda (b)
                    (struct-copy book-entry b
                                 [collections
                                  (filter (lambda (c)
                                            (not (string=? c name)))
                                          (book-entry-collections b))]))))

(define (map-collection books path f)
  (for/list ([b (in-list books)])
    (if (string=? (book-entry-path b) path) (f b) b)))

;; ---- paths ----

(define (library-file-path)
  (build-path (current-config-dir) "library.json"))

(define (covers-dir-path)
  (build-path (current-config-dir) "covers"))

;; The cover cache file for a book path: "<fnv1a64-hex>.png". Deterministic
;; so a re-import overwrites in place and the format only carries the name.
(define (cover-file-name path)
  (string-append (fnv1a64-hex path) ".png"))

;; ---- storage <-> model ----

(define (string-or default value)
  (if (string? value) value default))

(define (int-or default value)
  (if (exact-integer? value) value default))

(define (status-or value)
  (if (string=? value "missing") "missing" "ok"))

(define (kind-or value)
  (if (book-entry-kind? value) value "pdf"))

(define (jsexpr->book-entry item)
  (and (hash? item)
       (string? (hash-ref item 'path #f))
       (book-entry
        (hash-ref item 'path)
        (string-or "" (hash-ref item 'title #f))
        (string-or "" (hash-ref item 'author #f))
        (kind-or (string-or "pdf" (hash-ref item 'kind #f)))
        (max 1 (int-or 1 (hash-ref item 'pages #f)))
        (int-or 0 (hash-ref item 'added_at #f))
        (string-or "" (hash-ref item 'cover #f))
        (let ((cols (hash-ref item 'collections '())))
          (if (list? cols)
              (filter string? cols)
              '()))
        (status-or (string-or "ok" (hash-ref item 'status #f))))))

(define (book-entry->storage b)
  (jobj
   (list
    (cons "path" (book-entry-path b))
    (cons "title" (book-entry-title b))
    (cons "author" (book-entry-author b))
    (cons "kind" (book-entry-kind b))
    (cons "pages" (book-entry-pages b))
    (cons "added_at" (book-entry-added-at b))
    (cons "cover" (book-entry-cover b))
    (cons "collections" (book-entry-collections b))
    (cons "status" (book-entry-status b)))))

(define (shelf->storage s)
  (jobj
   (list
    (cons "format" shelf-format)
    (cons "books" (map book-entry->storage (shelf-books s)))
    (cons "collections" (shelf-collections s)))))

(define (storage->shelf value)
  (unless (hash? value)
    (raise (exn:fail (tf "backend.error.parse-library" "not a JSON object")
                     (current-continuation-marks))))
  (define books-raw (hash-ref value 'books '()))
  (define collections-raw (hash-ref value 'collections '()))
  (shelf
   (if (list? books-raw)
       (filter book-entry? (map jsexpr->book-entry books-raw))
       '())
   (if (list? collections-raw)
       (filter string? collections-raw)
       '())))

;; ---- file IO (same idioms as settings.rkt) ----

(define shelf-lock (make-semaphore 1))

(define (with-shelf-lock thunk)
  (call-with-semaphore shelf-lock thunk))

;; Missing file -> empty shelf.
(define (load-shelf)
  (define path (library-file-path))
  (if (not (file-exists? path))
      (default-shelf)
      (with-handlers
          ([exn:fail?
            (lambda (e)
              (raise (exn:fail (tf "backend.error.read-library" (exn-message e))
                               (current-continuation-marks))))])
        (define value (call-with-input-file path read-json))
        (storage->shelf value))))

(define (save-shelf! s)
  (unless (shelf? s)
    (raise-argument-error 'save-shelf! "shelf?" s))
  (with-handlers
      ([exn:fail?
        (lambda (e)
          (raise (exn:fail (tf "backend.error.write-library" (exn-message e))
                           (current-continuation-marks))))])
    (make-directory* (current-config-dir))
    (define data (json-value->bytes (shelf->storage s)))
    (define tmp (path-replace-extension (library-file-path) ".json.tmp"))
    (call-with-output-file tmp
      (lambda (out) (write-bytes data out))
      #:exists 'replace)
    (rename-file-or-directory tmp (library-file-path) #t))
  (void))

;; Read-modify-write under the shelf lock; every mutation goes through
;; here so import/remove/scan never interleave.
(define (update-shelf! updater)
  (unless (procedure? updater)
    (raise-argument-error 'update-shelf! "procedure?" updater))
  (with-shelf-lock
   (lambda ()
     (define updated (updater (load-shelf)))
     (unless (shelf? updated)
       (raise-argument-error 'update-shelf! "procedure returning shelf" updater))
     (save-shelf! updated)
     updated)))
