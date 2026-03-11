# openslide/test

## But
Suite de tests pour OpenSlide — 364 cas de test couvrant tous les formats vendeur.

## Pourquoi
Valide que chaque format est correctement decode, y compris les cas limites (fichiers corrompus, structures invalides, variantes non-standard).

## Comment
- Tests C (query, mosaic, parallel, profile, extended) drives par meson
- `cases/` : 364 repertoires de fixtures, chacun avec un `config.yaml` decrivant le test
- Chaque cas teste un format ou une variante specifique (aperio, hamamatsu, ventana, mirax, dicom, generic-tiff...)

## Structure
```
test/
  query.c       # Tests requetes API (open, read, properties)
  mosaic.c      # Tests assemblage de tiles
  parallel.c    # Tests acces concurrent
  profile.c     # Tests profiling
  extended.c    # Tests etendus
  driver.in     # Script driver des tests
  cases/        # 364 fixtures de test (un repertoire par cas)
  meson.build   # Config de build des tests
  *.supp        # Fichiers de suppression (clang sanitizers)
```
