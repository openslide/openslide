# openslide/tools

## But
Outils CLI d'OpenSlide pour inspecter et manipuler les lames WSI.

## Pourquoi
Fournit des utilitaires en ligne de commande pour diagnostiquer les fichiers de lames sans passer par l'API C.

## Comment
- `slidetool.c` : outil principal multi-commandes (info, export, test, proprietes, ICC)
- Sous-commandes dans `slidetool-*.c`
- Pages man generees depuis `*.1.in`

## Structure
```
tools/
  slidetool.c           # Point d'entree CLI principal
  slidetool.h           # Header commun
  slidetool-image.c     # Export d'images (PNG)
  slidetool-prop.c      # Affichage proprietes
  slidetool-slide.c     # Info generale de la lame
  slidetool-icc.c       # Profil ICC
  slidetool-test.c      # Tests integres
  slidetool-util.c      # Utilitaires internes
  *.1.in                # Templates pages man
  meson.build           # Build config
```
