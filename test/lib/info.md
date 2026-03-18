# openslide/test/lib

## But
Bibliotheque partagee pour les tests OpenSlide.

## Pourquoi
Factorise le code commun aux differents executables de test (gestion file descriptors, utilitaires).

## Comment
- `libtest.h` : header commun pour les tests
- `libtest-fd.c` : gestion des file descriptors (anciennement dans common/)

## Structure
```
lib/
  libtest.h      # Header commun tests
  libtest-fd.c   # File descriptor utilities
  meson.build    # Build config
```
