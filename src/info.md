# openslide/src

## But
Code source principal d'OpenSlide — decodeurs de formats WSI et API publique.

## Pourquoi
Chaque format de lame numerisee a une structure propriétaire differente. Chaque decodeur traduit le format vendeur vers l'API unifiee OpenSlide.

## Comment
- `openslide.c` : point d'entree API (open, read_region, get_properties...)
- `openslide-vendor-*.c` : un decodeur par vendeur (aperio, hamamatsu, ventana, leica, zeiss, dicom...)
- `openslide-decode-*.c` : decodeurs de compression (JPEG, JP2K, PNG, TIFF, BMP, SQLite)
- `openslide-cache.c` : cache de tiles en memoire

## Structure
```
src/
  openslide.c                  # API publique
  openslide-cache.c            # Cache tiles
  openslide-vendor-aperio.c    # Decodeur Aperio SVS
  openslide-vendor-hamamatsu.c # Decodeur Hamamatsu NDPI/VMS
  openslide-vendor-ventana.c   # Decodeur Ventana BIF (patche)
  openslide-vendor-leica.c     # Decodeur Leica SCN
  openslide-vendor-mirax.c     # Decodeur 3DHISTECH MRXS
  openslide-vendor-dicom.c     # Decodeur DICOM WSI
  openslide-decode-*.c/h       # Decodeurs compression
  meson.build                  # Build config
```
