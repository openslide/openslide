# openslide (submodule)

## But
Fork d'OpenSlide — bibliotheque C pour lire les lames de microscopie virtuelle (WSI).

## Pourquoi
Fork `Yanstart/openslide` branche `varuna-patches` pour supporter des variantes vendeur non gerees par upstream (ex: Ventana BIF direction LEFT).

## Comment
- Bibliotheque C utilisant meson comme systeme de build
- Supporte 15+ formats vendeur (Aperio, Hamamatsu, Ventana, Leica, Zeiss, DICOM...)
- Decodeurs specialises par format dans `src/`
- Suite de tests exhaustive (364 cas dans `test/cases/`)

## Structure
```
openslide/
  src/           # Code source C (decodeurs par format, cache, API)
  common/        # Utilitaires partages (CLI, gestion erreurs, fichiers)
  doc/           # Documentation Doxygen
  test/          # Suite de tests (364 cas de test)
  tools/         # Outils CLI (slidetool, quickhash, proprietes)
  misc/          # Scripts utilitaires et helpers (ImHex patterns, benchmarks)
  scripts/       # Scripts de build/distribution
  subprojects/   # Dependances wrappees (libdicom, uthash)
  .github/       # CI GitHub Actions
  meson.build    # Build system principal
```
