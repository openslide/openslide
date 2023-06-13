# Notable Changes in OpenSlide


## Version 3.4.1, 2015-04-20

* New formats: Philips TIFF, Ventana TIFF
* Support OpenJPEG 2.1.0
* Improve performance of JPEG and JP2K decoding
* Add `openslide.region[i].*` properties
* Improve MATLAB compatibility
* Enable function deprecation warnings with MSVC
* Many portability fixes
* aperio: Detect OpenJPEG chroma subsampling breakage during `open`
* aperio: Fill in missing tiles with downsampled data
* aperio: Report MPP for slides scanned in locales with decimal comma
* hamamatsu: Support NDPI files &gt; 4 GB
* hamamatsu: Properly detect NDPI slides produced by NDP.toolkit
* hamamatsu: Support VMS/VMU slides without a `NoLayers` key
* hamamatsu: Report MPP for VMS/VMU
* leica: Support slides with `2010/03/10` XML namespace
* leica: Base64-decode `leica.barcode` property in `2010/10/01` namespace
* sakura: Support slides with multiple focal planes
* sakura: Support slides without tile table
* ventana: Support slides with multiple focal planes
* ventana: Improve positioning of AOIs within level
* ventana: Fix failure to recognize macro image on some slides


## Version 3.4.0, 2014-01-25

* Major internal restructuring
* New formats: Hamamatsu NDPI, Sakura SVSLIDE, Ventana BIF (preliminary)
* Add `openslide_detect_vendor()`
* Deprecate `openslide_can_open()` (not very useful and often misused)
* Document performance considerations for `openslide_open()`
* Add properties giving the bounds of the non-empty region of the slide
* leica: Support multiple main images if their levels are coplanar
* leica: Use slide size as level size
* mirax: Support PNG- and BMP-formatted slides
* mirax: Fix `Expected first 0 value` error opening some slides
* mirax: Fix incorrect tile placement on some slides without overlaps
* mirax: Never synthesize downsampled levels
* Add `OPENSLIDE_DEBUG` environment variable (`OPENSLIDE_DEBUG=?` for help)
* Fix some crashes in error paths
* Add tests for many error paths


## Version 3.3.3, 2013-04-13

* Fix inclusion of `openslide.h` with MSVC
* Properly handle Aperio JP2K slides with zero-length tiles
* Support Hamamatsu slides with blank `MacroImage` key


## Version 3.3.2, 2012-12-01

* Fix seams in MIRAX 2.2 slides (thanks, Agelos Pappas)
* Fix associated image naming in single-level Aperio slides
* Stop decoding MIRAX tiles outside requested region
* Stop decoding unneeded tiles during tile-aligned accesses
* Increase Hamamatsu VMU tile size to reduce rendering overhead
* Document performance considerations for `openslide_can_open()`


## Version 3.3.1, 2012-10-14

* Parallelize concurrent `openslide_read_region` calls on an `openslide_t`
* Eliminate background scanning of tile headers in MIRAX
* Scan many fewer tiles during first accesses to Hamamatsu VMS
* Ignore Leica Z-planes other than 0
* Add experimental tile-size properties
* Document API thread safety


## Version 3.3.0, 2012-09-08

* Support Leica SCN format (requires libtiff &ge; 4) (thanks, Agelos Pappas)
* Allow opening MIRAX 2.2 slides (though there are seams, bug #92)
* Add standardized microns-per-pixel and objective-power properties
* Add `macro` associated image in Trestle
* Rename `layer` to `level` throughout the API (deprecate `layer` functions;
  remove `layer` properties)
* Report parse errors in `openslide_open()` by returning an `openslide_t` in
  error state
* Deprecate `openslide_get_comment()`
* Add `openslide_get_version()`
* Improve command-line tools; add manpages
* Support building with MinGW-w64; drop CMake, MSVC, mingw32
* Add tests for many error paths


## Version 3.2.6, 2012-02-23

* Support downsampled MIRAX files
* Improve performance on MIRAX slides without tile overlaps
* Fix `openslide_read_region` for large dimensions on layer &gt; 0
  (3.2.5 regression)
* Correct subpixel error in MIRAX tile placement
* Fix unlikely use-after-free with Hamamatsu VMU


## Version 3.2.5, 2011-12-16

* Support MIRAX 1.03 files (thanks, Jan Harkes)
* Fix `openslide_read_region` for large dimensions
* Use subpixel precision in all backends
* Don't keep associated images in memory
* Disable quickhash-1 for TIFF files with very large top layer
* Various build fixes (thanks, Jan, Marco Feuerstein, and Mathieu Malaterre)
* Fix some unlikely memory leaks


## Version 3.2.4, 2011-03-07

* Support MIRAX files that do not have non-hierarchical data
  (thanks, Jan Harkes)
* Fix compilation on Windows (thanks Hauke Heibel)
* Work around a bug in `GKeyFile` parser (thanks, Jan)


## Version 3.2.3, 2010-09-09

* Support MIRAX files that use a variant format for tile positions
  (thanks, Hauke Heibel and Marco Feuerstein)
* Update location of website
* Add background color property, for slides that have it
* Update CMake scripts and other Windows fixes (thanks Hauke and Marco)
* Fix some `test.c` bugs
* Fix incorrect MIRAX drawing at certain resolutions (thanks Hauke and Marco)
* Support quickhash-1 on older systems (thanks, Jan Harkes)


## Version 3.2.2, 2010-06-16

* Rework some internals of `openslide_read_region`
* Support negative coordinates and zero-sized dimensions in
  `openslide_read_region`
* Clarify the documentation about `openslide_read_region`
* Fix Windows build bug with new NGR support
* Enable untested BigTIFF support


## Version 3.2.1, 2010-06-03

* Fix crashes on Windows when trying to read Hamamatsu files
* Fix jpeg 7 problems in `read_from_one_jpeg`
* Quiet the error handling system after the first error


## Version 3.2.0, 2010-06-01

* Add experimental CMake support and fixes for building with MSVC
  (thanks to Hauke Heibel!)
* Enable detecting runtime errors
* Add initial Hamamatsu Nanozoomer VMU support (thanks to Steve Lamont)
* Add `openslide-write-png` tool


## Version 3.1.1, 2010-04-27

* Fix a crash when reading an invalid VMS file
* Fix memory leaks when reading an invalid VMS file
* Accept VMS files that have more than one focal plane (non-0 planes ignored)
* Fix bug that could cause problems with libtiff 4
* Relax the required version of `Microsoft.VC80.CRT`


## Version 3.1.0, 2010-04-01

* Enable large file access on Windows (requires `Microsoft.VC80.CRT`)
* Support newer Aperio files (compression 33005)
* Be more robust in reading raw TIFF tiles
* Reject invalid TIFF files earlier
* Fix many memory leaks when probing for TIFF files


## Version 3.0.3, 2010-03-01

* Fix nasty artifacts in some MIRAX files (seen at some zoom levels)


## Version 3.0.2, 2010-02-17

* Restore ability to build with glib 2.12, at the expense of not having
  "quickhash-1" in that configuration


## Version 3.0.1, 2010-02-04

* Fix edge-drawing bug in TIFF backend
* Ship `CHANGELOG.txt`


## Version 3.0.0, 2010-01-28

* Switch from GPLv2 to LGPLv2
* Reduce some unlikely memory leaks
* Support of more MIRAX files
* Improve performance of MIRAX rendering, vastly in some cases
* Reduce appearance of seams in MIRAX
* Add "quickhash-1" hash property
* Add `openslide-quickhash1sum` and `openslide-show-properties` tools
* Rework the API documentation
* Remove never-implemented prefetch functions from `openslide.h` (but retain
  with warnings in the library)
* Start attempting to figure out Trestle tile position files
* Add some defined property names to the header file

---

## Version 2.3.1, 2009-12-14

* Eliminate Aperio regression introduced in Version 2.3.0


## Version 2.3.0, 2009-12-11

* Support for generic tiled TIFF format (for InterScope files)
* Bug fixes
* Reduction of some TIFF error messages
* Fixes for some build problems
* Deprecate prefetch functions (never implemented)


## Version 2.2.1, 2009-10-23

* Fixes for thread safety problems in 2.2.0


## Version 2.2.0, 2009-09-15

* Thread safety (lockless with Hamamatsu and MIRAX files)


## Version 2.1.0, 2009-08-18

* Support for MIRAX `mrxs`


## Version 2.0.0, 2009-07-16

* Support for image metadata and associated images
* Support Aperio variant
* Internally rework a lot in preparation for MIRAX
* Win32 support

---

## Version 1.1.1, 2009-02-25

* Remove never-functional generic JPEG 2000 support
* Switch Aperio to use the released version of OpenJPEG
* Be more robust to errors in general


## Version 1.1.0, 2008-12-20

* Greatly speed up Hamamatsu with a tile cache and background scanning thread


## Version 1.0.0, 2008-12-09

* Renamed to "OpenSlide"
* Multi-file Hamamatsu support
* Switch to 64-bit signed integers in public API where possible

---

## Version 0.5.0, 2008-10-21

* GPLv2 release
* Working Aperio support
* More work on generic JPEG 2000


## Version 0.4.2, 2008-09-05

* Documentation updates
* For Aperio, remove Jasper in lieu of using OpenJPEG
* Preliminary and non-functional generic JPEG 2000 support


## Version 0.4.0, 2008-03-12

* Update simple test program


## Version 0.3.0, 2008-01-31

* Broken and unusably slow Aperio support


## Version 0.2.0, 2008-01-19

* Using glib
* Layers are numbered instead of named
* Actual start of implementation
* Initial Trestle support
* Initial Aperio support (without tile codec)
* Initial slow and incomplete Hamamatsu support
* Initial test program
* Documentation updates


## Version 0.1.0, 2007-12-06

* Unreleased, just documentation and headers (called "Wholeslide")
