# openslide/misc/imhex

## But
Patterns ImHex pour visualiser les structures binaires des formats de lames.

## Pourquoi
Permet d'inspecter visuellement les headers et structures des fichiers WSI avec l'editeur hexadecimal ImHex.

## Comment
Fichiers `.hexpat` qui decrivent la structure binaire de chaque format (Hamamatsu, MRXS, Zeiss CZI).

## Structure
```
imhex/
  hamamatsu-ndpi.hexpat        # Structure NDPI
  hamamatsu-vms-opt.hexpat     # Structure VMS
  hamamatsu-vmu-ngr.hexpat     # Structure VMU
  mirax-index.hexpat           # Index MRXS
  mirax-position.hexpat        # Positions tiles MRXS
  zeiss-czi.hexpat             # Structure Zeiss CZI
```
