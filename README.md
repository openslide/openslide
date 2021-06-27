OpenSlide
=========

Carnegie Mellon University and others
https://openslide.org/



What is this?
-------------

This library reads whole slide image files (also known as virtual slides).
It provides a consistent and simple API for reading files from multiple
vendors.

The version on NKI-AI is forked by the AI for Oncology group at the Netherlands 
Cancer Institute to add some small changes to work with our scanners.
* Allow the reading of a larger class of 3dhistech images.
* Some Aperio scanners save `Mag` as float, adapted this accordingly.


What is the license?
--------------------

This code is licensed under the GNU LGPL version 2.1, not any later version.
See the file lgpl-2.1.txt for the text of the license.


Requirements
------------

This library requires zlib, libpng, libjpeg, libtiff >= 4.0, OpenJPEG >= 2.1,
GDK-PixBuf, libxml2, SQLite >= 3.6.20, cairo >= 1.2, and glib >= 2.56.

If you want to run the test suite, you will need PyYAML, python-requests,
xdelta3, cjpeg and djpeg (from libjpeg), a Git checkout of OpenSlide,
at least one installed font, and > 120 GB of disk space.  Valgrind mode
requires Valgrind, plus debug symbols for library dependencies (particularly
glib2) and Fontconfig.  Profile mode requires Valgrind.  Coverage mode
requires gcov and Doxygen.


Features
--------

The library can read Aperio, Hamamatsu, Leica, MIRAX, Sakura, Trestle,
and Ventana formats, as well as TIFF files that conform to a simple
convention. (InterScope files tend to be readable as this generic TIFF.)

More information about formats is here:
https://openslide.org/formats/

An openslide_t object can be used concurrently from multiple threads
without locking. (But you must lock or otherwise use memory barriers
when passing the object between threads.)


Properties
----------

The library exposes certain properties as string key-value pairs for
a given virtual slide. (These are accessed by way of the
`openslide_get_property_names` and `openslide_get_property_value` calls.)

These properties are generally uninterpreted data gathered from the
on-disk files. New properties can be added over time in subsequent releases
of OpenSlide. A list of some properties can be found at:
https://openslide.org/properties/

OpenSlide itself creates these properties (for now):

 * `openslide.background-color`
   The background color of the slide, given as an RGB hex triplet.
   This property is not always present.
 * `openslide.bounds-height`
   The height of the rectangle bounding the non-empty region of the slide.
   This property is not always present.
 * `openslide.bounds-width`
   The width of the rectangle bounding the non-empty region of the slide.
   This property is not always present.
 * `openslide.bounds-x`
   The X coordinate of the rectangle bounding the non-empty region of the
   slide. This property is not always present.
 * `openslide.bounds-y`
   The Y coordinate of the rectangle bounding the non-empty region of the
   slide. This property is not always present.
 * `openslide.comment`
   A free-form text comment.
 * `openslide.mpp-x`
   Microns per pixel in the X dimension of level 0. May not be present or
   accurate.
 * `openslide.mpp-y`
   Microns per pixel in the Y dimension of level 0. May not be present or
   accurate.
 * `openslide.objective-power`
   Magnification power of the objective. Often inaccurate; sometimes missing.
 * `openslide.quickhash-1`
   A non-cryptographic hash of a subset of the slide data. It can be used
   to uniquely identify a particular virtual slide, but cannot be used
   to detect file corruption or modification.
 * `openslide.vendor`
   The name of the vendor backend.


Other Documentation
-------------------

The definitive API reference is in openslide.h. For an HTML version, see
doc/html/openslide_8h.html in this distribution.

Additional documentation is available from the OpenSlide website:
https://openslide.org/

The design and implementation of the library are described in a published
technical note:

 OpenSlide: A Vendor-Neutral Software Foundation for Digital Pathology
 Adam Goode, Benjamin Gilbert, Jan Harkes, Drazen Jukic, M. Satyanarayanan
 Journal of Pathology Informatics 2013, 4:27

 http://download.openslide.org/docs/JPatholInform_2013_4_1_27_119005.pdf

There is also an older technical report:

 CMU-CS-08-136
 A Vendor-Neutral Library and Viewer for Whole-Slide Images
 Adam Goode, M. Satyanarayanan

 http://reports-archive.adm.cs.cmu.edu/anon/2008/abstracts/08-136.html
 http://reports-archive.adm.cs.cmu.edu/anon/2008/CMU-CS-08-136.pdf


Acknowledgements
----------------

OpenSlide has been supported by the National Institutes of Health and
the Clinical and Translational Science Institute at the University of
Pittsburgh.


How to build?
-------------

If you want to build from the Git repository, you will first need to install a few packages. 

### Ubuntu

On Ubuntu the following should work:
```
sudo apt-get install pkg-config libtool zlib1g-dev libpng-dev\
                     libjpeg-dev libopenjp2-7-dev make\
                     libtiff-dev libglib2.0-dev libcairo-dev\
                     libgdk-pixbuf2.0-dev libxml2-dev libsqlite3-dev
```

### Fedora/CentOS

On Fedora/CentOS the following should work:
```
sudo yum install glib2-devel libtiff-devel gdk-pixbuf2-devel libxml2-devel sqlite-devel
sudo dnf --enablerepo=powertools install openjpeg2-devel
```

The package can subsequently built using
```
autoreconf -i
./configure
make
sudo make install
```
Once built, run `sudo ldconfig` to ensure that the new library is properly found.
If you have installed `openslide-python` it should now use the recently compiled version.


Good luck!
