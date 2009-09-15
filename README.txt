OpenSlide

Carnegie Mellon University

http://openslide.cs.cmu.edu/


2009-09-15

==========================


What is this?
=============

This library reads whole slide image files (also known as virtual slides).
It provides a consistent and simple API for reading files from multiple
vendors.


What is the license?
====================

This code is licensed under the GNU GPL version 2, not any later version.
See the file gpl-2.0.txt for the text of the license.


Requirements
============

This library requires libjpeg, libtiff, openjpeg, cairo >= 1.2,
and glib >= 2.12.


Features
========

The library can read Trestle, Aperio, MIRAX, and Hamamatsu formats.

An openslide_t object can be used concurrently from multiple threads
without locking. (But you must lock or otherwise use memory barriers
when passing the object between threads.)


Other Documentation
===================

See the Carnegie Mellon SCS Technical Report:

 CMU-CS-08-136
 A Vendor-Neutral Library and Viewer for Whole-Slide Images
 Adam Goode, M. Satyanarayanan

 http://reports-archive.adm.cs.cmu.edu/anon/2008/abstracts/08-136.html
 http://reports-archive.adm.cs.cmu.edu/anon/2008/CMU-CS-08-136.pdf


Changes from Tech Report
========================

The CMU CS TR contains API documentation for a previous, unreleased
version of this library. Since the report was released, the API has
changed to prefer signed integers over unsigned and to use 64-bit
types for positions and dimensions. The ws_get_region_num_bytes
function was removed. Additionally, "wholeslide" was renamed to
"openslide", the "ws_" prefix changed to "openslide_", and "wholeslide_t"
is now "openslide_t".

See openslide.h in this distribution for the definitive API reference.


How to build?
=============

./configure
make
make install


Good luck!
