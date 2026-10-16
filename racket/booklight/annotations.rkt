#lang racket/base

;; Annotations store, byte-for-byte compatible with v1 (Tauri annotations.rs) so
;; v1 files keep working (drop-in migration):
;;   - path key = FNV-1a 64-bit of the absolute PDF path, 16 lowercase hex
;;     chars, file <config-dir>/annotations/<key>.json
;;   - sidecar mode: <pdf-path>.pdfgist.json next to the PDF (cloud-drive sync)
;;   - file shape {"annotations":[...],"bookmarks":[...]}; 0.8.x bare arrays
;;     load as annotations with empty bookmarks
;;   - serde #[serde(default)] defaults are re-applied on load, matching the
;;     old typed-deserialize behavior
;; Rects stay page-space floats in the JSON (byte-compatible); they only
;; cross the RPC as opaque JSON bytes, so no coordinate scaling happens here.

(require json
         racket/file
         racket/path
         racket/port
         racket/string
         "fnv.rkt"
         "i18n.rkt"
         "jsonw.rkt"
         (only-in "settings.rkt" current-config-dir))

(provide annotation-storage-path
         sidecar-storage-path
         legacy-sidecar-path
         annotations-dir-path
         empty-document
         legacy-array?
         load-document-bytes
         store-document-bytes!
         normalize-document-value
         normalize-annotation)

;; ---- paths ----

(define (annotations-dir-path)
  (build-path (current-config-dir) "annotations"))

;; Sidecar files carry the product name; the pdfgist name is the legacy
;; shape kept readable so pre-booklight sidecars migrate transparently.
(define (sidecar-storage-path pdf-path)
  (unless (string? pdf-path)
    (raise-argument-error 'sidecar-storage-path "string?" pdf-path))
  (string-append pdf-path ".booklight.json"))

(define (legacy-sidecar-path pdf-path)
  (and (string? pdf-path)
       (string-append pdf-path ".pdfgist.json")))

(define (annotation-storage-path pdf-path)
  (unless (string? pdf-path)
    (raise-argument-error 'annotation-storage-path "string?" pdf-path))
  (build-path (annotations-dir-path)
              (string-append (fnv1a64-hex pdf-path) ".json")))

;; ---- model defaults (serde #[serde(default)] on the v1 structs) ----
;;
;; Field order mirrors the v1 Rust structs, so re-serialized files keep the
;; same key order as serde_json output; unknown keys are dropped exactly like
;; the old typed deserialization.

(define annotation-field-order '(id page rects excerpt color kind note created))
(define rect-field-order '(x y width height))
(define bookmark-field-order '(page label created))

(define annotation-defaults
  (hasheq 'id ""
          'page 1
          'rects '()
          'excerpt ""
          'color "yellow"
          'kind "highlight"
          'note ""
          'created 0))

(define rect-defaults
  (hasheq 'x 0.0 'y 0.0 'width 0.0 'height 0.0))

(define bookmark-defaults
  (hasheq 'page 1
          'label ""
          'created 0))

;; Projects a parsed JSON object onto `order`, applying defaults for missing
;; fields (serde #[serde(default)] semantics).
(define (ordered-object value order defaults)
  (define source (if (hash? value) value (hasheq)))
  (jobj
   (for/list ([key (in-list order)])
     (cons (symbol->string key)
           (hash-ref source key (hash-ref defaults key))))))

(define (normalize-rect value)
  (ordered-object value rect-field-order rect-defaults))

(define (normalize-annotation value)
  (define source (if (hash? value) value (hasheq)))
  (define rects (hash-ref source 'rects (hash-ref annotation-defaults 'rects)))
  (define projected
    (ordered-object value annotation-field-order annotation-defaults))
  (jobj
   (for/list ([field (in-list (jobj-fields projected))])
     (if (string=? (car field) "rects")
         (cons "rects"
               (if (list? rects)
                   (map normalize-rect rects)
                   '()))
         field))))

(define (normalize-bookmark value)
  (ordered-object value bookmark-field-order bookmark-defaults))

;; Returns the normalized document as an ordered storage value:
;; {"annotations": [...], "bookmarks": [...]}
(define (normalize-document-value value)
  (cond
    ;; 0.8.x wrote a bare annotation array; 0.9+ writes {annotations, bookmarks}.
    [(list? value)
     (jobj (list (cons "annotations" (map normalize-annotation value))
                 (cons "bookmarks" '())))]
    [(hash? value)
     (define annotations (hash-ref value 'annotations '()))
     (define bookmarks (hash-ref value 'bookmarks '()))
     (unless (and (list? annotations) (list? bookmarks))
       (raise (exn:fail (tr "backend.error.bad-annotations")
                        (current-continuation-marks))))
     (jobj (list (cons "annotations" (map normalize-annotation annotations))
                 (cons "bookmarks" (map normalize-bookmark bookmarks))))]
    [else
     (raise (exn:fail (tr "backend.error.bad-annotations")
                      (current-continuation-marks)))]))

;; Kept exported for tests/hosts that need to detect the legacy shape.
(define (legacy-array? value)
  (list? value))

(define (empty-document)
  (jobj (list (cons "annotations" '()) (cons "bookmarks" '()))))

;; ---- file IO ----

(define (read-file-string file who-key)
  (with-handlers
      ([exn:fail?
        (lambda (e)
          (raise (exn:fail (tf who-key (exn-message e))
                           (current-continuation-marks))))])
    ;; Lossy decode keeps parity with Rust's from_utf8_lossy.
    (bytes->string/utf-8 (call-with-input-file file port->bytes) #\uFFFD)))

;; Returns normalized pretty JSON bytes; missing files yield an empty
;; document, exactly like load_document in annotations.rs.
(define (load-document-bytes pdf-path sidecar?)
  (define file
    (if sidecar?
        (or (and (file-exists? (sidecar-storage-path pdf-path))
                 (sidecar-storage-path pdf-path))
            (let ((legacy (legacy-sidecar-path pdf-path)))
              (and legacy (file-exists? legacy) legacy))
            (sidecar-storage-path pdf-path))
        (annotation-storage-path pdf-path)))
  (if (not (file-exists? file))
      (json-value->bytes (empty-document))
      (let ()
        (define text (read-file-string file "backend.error.read-annotations"))
        (define value
          (with-handlers
              ([exn:fail?
                (lambda (e)
                  (raise (exn:fail (tf "backend.error.parse-annotations" (exn-message e))
                                   (current-continuation-marks))))])
            (read-json (open-input-string text))))
        (json-value->bytes (normalize-document-value value)))))

;; Validates the shape, applies defaults, and stores normalized pretty JSON
;; (what the host gets back from load is exactly what is on disk).
(define (store-document-bytes! pdf-path sidecar? data)
  (unless (bytes? data)
    (raise-argument-error 'store-document-bytes! "bytes?" data))
  (define value
    (with-handlers
        ([exn:fail?
          (lambda (e)
            (raise (exn:fail (tf "backend.error.parse-annotations" (exn-message e))
                             (current-continuation-marks))))])
      (read-json (open-input-bytes data))))
  ;; A bare array was never a valid save payload (only load tolerated it).
  (unless (and (hash? value)
               (hash-has-key? value 'annotations)
               (list? (hash-ref value 'annotations)))
    (raise (exn:fail (tr "backend.error.bad-annotations")
                     (current-continuation-marks))))
  (define normalized (normalize-document-value value))
  (define file
    (if sidecar?
        (sidecar-storage-path pdf-path)
        (annotation-storage-path pdf-path)))
  (with-handlers
      ([exn:fail?
        (lambda (e)
          (raise (exn:fail (tf "backend.error.save-annotations" (exn-message e))
                           (current-continuation-marks))))])
    (unless sidecar?
      (make-directory* (annotations-dir-path)))
    (call-with-output-file file
      (lambda (out) (write-bytes (json-value->bytes normalized) out))
      #:exists 'replace))
  (void))
