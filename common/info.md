# openslide/common

## But
Utilitaires C partages entre les outils OpenSlide (tests, CLI, slidetool).

## Pourquoi
Factorise le code commun (parsing CLI, gestion erreurs, E/S fichiers) entre les differents executables.

## Comment
- `openslide-common.h` : header principal
- `openslide-common-cmdline.c` : parsing d'arguments CLI
- `openslide-common-fail.c` : gestion des erreurs fatales
- `openslide-common-fd.c` / `openslide-common-file.c` : E/S fichiers

## Structure
```
common/
  openslide-common.h          # Header commun
  openslide-common-cmdline.c  # Parsing CLI
  openslide-common-fail.c     # Gestion erreurs
  openslide-common-fd.c       # File descriptors
  openslide-common-file.c     # Operations fichiers
  meson.build                 # Build config
```
