Wholeslide

Carnegie Mellon University

http://wholeslide.cs.cmu.edu/


2008-11-03

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

This library requires libjpeg, libtiff, and glib >= 2.10.


Features
========

The library can read Trestle, Aperio, and Hamamatsu formats.


Other Documentation
===================

See the Carnegie Mellon SCS Technical Report:

 CMU-CS-08-136
 A Vendor-Neutral Library and Viewer for Whole-Slide Images
 Adam Goode, M. Satyanarayanan

 http://reports-archive.adm.cs.cmu.edu/anon/2008/abstracts/08-136.html
 http://reports-archive.adm.cs.cmu.edu/anon/2008/CMU-CS-08-136.pdf

Note that this report contains API documentation for a previous, unreleased
version of this library. Since the report was released, the API has changed
to prefer signed integers over unsigned and to use 64-bit types for
positions and dimensions. The ws_get_region_num_bytes function was also
removed.

See wholeslide.h in this distribution for the definitive API reference.


How to build?
=============

./configure
make
make install


Good luck!
