# openslide/doc

## But
Configuration Doxygen pour generer la documentation de l'API OpenSlide.

## Pourquoi
Documentation automatique de l'API C a partir des commentaires dans le code source.

## Comment
`Doxyfile.in` est un template traite par meson au build pour generer la doc HTML.

## Structure
```
doc/
  Doxyfile.in   # Template Doxygen (configure par meson)
  meson.build   # Cible de build documentation
```
