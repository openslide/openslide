# openslide/misc

## But
Scripts utilitaires et outils de developpement pour OpenSlide.

## Pourquoi
Outils d'analyse, debugging et benchmarking des formats de lames. Pas necessaires au build, mais utiles pour le developpement.

## Comment
Scripts Python/C pour decoder, analyser et visualiser les structures internes des formats WSI.

## Structure
```
misc/
  decode-mirax.py              # Decodage structure MRXS
  parse-mirax.py               # Parsing hierarchie MRXS
  read-benchmark.c             # Benchmark de lecture
  read-region.c                # Lecture d'une region
  test.c                       # Tests manuels
  imhex/                       # Patterns ImHex pour analyse binaire
  czi-quickhash.py             # Hash Zeiss CZI
  dicom-quickhash.py           # Hash DICOM
  synthetic.png                # Image de test synthetique
```
