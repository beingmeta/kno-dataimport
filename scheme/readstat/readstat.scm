;;; -*- Mode: Scheme; Character-encoding: utf-8; -*-
;;; Copyright (C) 2005-2021 beingmeta, inc.  All rights reserved.
;;; Copyright (C) 2021-20222 Kenneth Haase.  All rights reserved.

(in-module 'readstat)

;; This is the C wrapper for the readstat library
(use-module 'creadstat)

(use-module '{texttools})

(module-export! '{readstat/load})

(define creadstat (get-module 'creadstat))

(define (readstat/load file (opts #f))
  (cond ((has-suffix file ".dta") (readstat/load/dta file opts))
	((has-suffix file ".sav") (readstat/load/sav file opts))
	((has-suffix file ".por") (readstat/load/por file opts))
	((has-suffix file ".sas7bdat") (readstat/load/sas7bdat file opts))
	((has-suffix file ".sas7bcat") (readstat/load/sas7bcat file opts))
	((has-suffix file ".xport") (readstat/load/xport file opts))
	(else (error |Can't handle file type| file))))

(define readstat-output (get creadstat 'readstat-output))
(define readstat-labels (get creadstat 'readstat-labels))
(define readstat-dataframe (get creadstat 'readstat-dataframe))
(define readstat-source (get creadstat 'readstat-source))
(define readstat-type (get creadstat 'readstat-type))
(define readstat-count (get creadstat 'readstat-count))

(module-export! '{readstat-source readstat-type
		  readstat-labels
		  readstat-dataframe
		  readstat-count 
		  readstat-output})
