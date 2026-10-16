#lang racket/base

;; Release identity duplicated from rivet.rktd. The packaged app cannot read
;; the project file at runtime, so the updater embeds these constants. Keep
;; them in sync with rivet.rktd (AGENTS.md lists this as a release checklist
;; item).

(provide app-version
         app-build
         app-identifier
         app-channel
         app-display-name
         update-key-id
         update-public-key-b64
         default-update-base-url)

(define app-version "0.3.3")
(define app-build 1)
(define app-identifier "site.jrtx.pdfgist")
(define app-channel 'stable)
(define app-display-name "PDFGist")

(define update-key-id "pdfgist-2026-10")

;; SubjectPublicKeyInfo DER, base64. Rotate by shipping a build that trusts
;; the next key before signing releases exclusively with it (rivet
;; docs/release-and-updates.md). The private half lives only in ~/Sync/Keys
;; plus the repo's UPDATE_ED25519_PRIVATE_KEY_B64 secret; it never ships.
(define update-public-key-b64
  "MCowBQYDK2VwAyEAR3gKODKPJd2j9k08KgSUYdYQ63FSDgKxq/b7LLhvlis=")

(define default-update-base-url
  "https://github.com/turinglambdaai/pdfgist/releases/latest/download")
