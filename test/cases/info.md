# openslide/test/cases

## But
364 fixtures de test couvrant tous les formats WSI et leurs variantes.

## Pourquoi
Chaque sous-repertoire est un cas de test individuel. Couvre les cas normaux et les cas d'erreur (fichiers corrompus, metadata invalide, compressions non supportees).

## Comment
Chaque fixture contient un `config.yaml` qui decrit les parametres du test (format, resultat attendu, hash, erreur attendue). Le driver de test les parcourt automatiquement.

## Formats couverts
- aperio (SVS, JP2K, multi-variantes)
- hamamatsu (NDPI, VMS, VMU)
- ventana (BIF, inclus variante LEFT patchee)
- mirax (MRXS)
- leica (SCN)
- zeiss (CZI)
- dicom (DICOM WSI)
- generic-tiff
- sakura, philips, synthetic
