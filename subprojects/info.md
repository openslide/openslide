# openslide/subprojects

## But
Dependances wrappees pour le build meson d'OpenSlide.

## Pourquoi
Meson subprojects permettent d'integrer des dependances qui ne sont pas disponibles via le systeme de packages.

## Comment
Fichiers `.wrap` qui pointent vers les sources des dependances. Meson les telecharge et compile automatiquement si absentes du systeme.

## Structure
```
subprojects/
  libdicom.wrap   # Bibliotheque DICOM (lecture fichiers DICOM WSI)
  uthash.wrap     # Hash table en C (header-only)
```
