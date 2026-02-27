# openslide/test/env

## But
Environnement virtuel Python pour les tests OpenSlide.

## Pourquoi
Isole les dependances Python des tests (pyfuse3, etc.) du systeme.

## Comment
`mkenv.py` cree un virtualenv avec les dependances de `requirements.txt`. Les sous-dossiers `bin/` et `Scripts/` contiennent les scripts meson pour Linux/Windows.

## Structure
```
env/
  mkenv.py       # Createur de virtualenv
  meson.build    # Integration build
  bin/           # Scripts meson (Linux)
  Scripts/       # Scripts meson (Windows)
```
