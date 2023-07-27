# OpenSlide

OpenSlide is a C library for reading whole slide image files (also known as
virtual slides).  It provides a consistent and simple API for reading files
from multiple vendors.


## Features

OpenSlide can read brightfield whole slide images in [several formats][]:

* [Aperio][] (`.svs`, `.tif`)
* [DICOM][] (`.dcm`)
* [Hamamatsu][] (`.ndpi`, `.vms`, `.vmu`)
* [Leica][] (`.scn`)
* [MIRAX][] (`.mrxs`)
* [Philips][] (`.tiff`)
* [Sakura][] (`.svslide`)
* [Trestle][] (`.tif`)
* [Ventana][] (`.bif`, `.tif`)
* [Generic tiled TIFF][] (`.tif`)

OpenSlide can also provide access to ICC profiles, textual metadata, and
associated images such as a slide label and thumbnail.

[several formats]: https://openslide.org/formats/
[Aperio]: https://openslide.org/formats/aperio/
[DICOM]: https://openslide.org/formats/dicom/
[Hamamatsu]: https://openslide.org/formats/hamamatsu/
[Leica]: https://openslide.org/formats/leica/
[MIRAX]: https://openslide.org/formats/mirax/
[Philips]: https://openslide.org/formats/philips/
[Sakura]: https://openslide.org/formats/sakura/
[Trestle]: https://openslide.org/formats/trestle/
[Ventana]: https://openslide.org/formats/ventana/
[Generic tiled TIFF]: https://openslide.org/formats/generic-tiff/


## Documentation

The [API reference][API] is available on the web, and is also included as
`doc/html/openslide_8h.html` in the source tarball.  [Additional
documentation][docs] is available on the [OpenSlide website][website].

[API]: https://openslide.org/api/openslide_8h.html
[docs]: https://openslide.org/#documentation
[website]: https://openslide.org/


## License

OpenSlide is released under the terms of the [GNU Lesser General Public
License, version 2.1](https://openslide.org/license/).

OpenSlide is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.


## Compiling

To build OpenSlide, you will need:

- Meson
- cairo &ge; 1.2
- GDK-PixBuf
- glib &ge; 2.56
- libdicom &ge; 1.0 (automatically built if missing)
- libjpeg
- libpng
- libtiff &ge; 4.0
- libxml2
- OpenJPEG &ge; 2.1
- SQLite &ge; 3.14
- zlib

Then:

```
meson setup builddir
meson compile -C builddir
meson install -C builddir
```


## Acknowledgements

OpenSlide has been developed by Carnegie Mellon University and other
contributors.

OpenSlide has been supported by the National Institutes of Health and
the Clinical and Translational Science Institute at the University of
Pittsburgh.
