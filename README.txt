OpenSlide

Carnegie Mellon University and others

http://openslide.org/


==========================


What is this?
=============

This library reads whole slide image files (also known as virtual slides).
It provides a consistent and simple API for reading files from multiple
vendors.


What is the license?
====================

This code is licensed under the GNU LGPL version 2.1, not any later version.
See the file lgpl-2.1.txt for the text of the license.


Requirements
============

This library requires libjpeg, libtiff, openjpeg, cairo >= 1.2,
and glib >= 2.12.


Features
========

The library can read Trestle, Aperio, MIRAX, and Hamamatsu formats,
as well as TIFF files that conform to a simple convention. (InterScope
files tend to be readable as this generic TIFF.)

More information about formats is here:
http://openslide.org/Supported%20Virtual%20Slide%20Formats/

An openslide_t object can be used concurrently from multiple threads
without locking. (But you must lock or otherwise use memory barriers
when passing the object between threads.)


Properties
==========

The library exposes certain properties as string key-value pairs for
a given virtual slide. (These are accessed by way of the
"openslide_get_property_names" and "openslide_get_property_value" calls.)

These properties are generally uninterpreted data gathered from the
on-disk files. New properties can be added over time in subsequent releases
of OpenSlide. A list of some properties can be found at:
http://openslide.org/List%20of%20Known%20Properties/

OpenSlide itself creates these properties (for now):

 openslide.vendor
   The name of the vendor backend.

 openslide.comment
   A free-form text comment, the same as returned from openslide_get_comment.

 openslide.quickhash-1
   A non-cryptographic hash of a subset of the slide data. It can be used
   to uniquely identify a particular virtual slide, but cannot be used
   to detect file corruption or modification.

 openslide.background-color
   The background color of the slide, given as an RGB hex triplet. This property
   is not always present.


Other Documentation
===================

The website:
http://openslide.org/

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


Acknowledgements
================

OpenSlide has been supported by the National Institutes of Health and
the Clinical and Translational Science Institute at the University of
Pittsburgh.


How to build?
=============

./configure
make
make install


Good luck!
