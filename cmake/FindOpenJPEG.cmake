#
#  Copyright (c) 2006-2010 Mathieu Malaterre <mathieu.malaterre@gmail.com>
#
#  Redistribution and use is allowed according to the terms of the New
#  BSD license.
#  For details see the accompanying COPYING-CMAKE-SCRIPTS file.
#
# Apparently libopenjpeg-dev finally made it into debian.
# Installation is:
# 
# /usr/lib/libopenjpeg.a
# /usr/include/openjpeg.h
# /usr/lib/libopenjpeg.so


FIND_PATH(OPENJPEG_INCLUDE_DIR openjpeg.h #openjpeg-1.0/openjpeg.h
/usr/local/include
/usr/local/include/openjpeg-1.0
/usr/include
/usr/include/openjpeg-1.0
)

FIND_LIBRARY(OPENJPEG_LIBRARY
  NAMES openjpeg
  PATHS /usr/lib /usr/local/lib
  )

IF (OPENJPEG_LIBRARY AND OPENJPEG_INCLUDE_DIR)
    SET(OPENJPEG_LIBRARIES ${OPENJPEG_LIBRARY})
    SET(OPENJPEG_INCLUDE_DIRS ${OPENJPEG_INCLUDE_DIR})
    SET(OPENJPEG_FOUND "YES")
ELSE (OPENJPEG_LIBRARY AND OPENJPEG_INCLUDE_DIR)
  SET(OPENJPEG_FOUND "NO")
ENDIF (OPENJPEG_LIBRARY AND OPENJPEG_INCLUDE_DIR)


IF (OPENJPEG_FOUND)
   IF (NOT OPENJPEG_FIND_QUIETLY)
      MESSAGE(STATUS "Found OPENJPEG: ${OPENJPEG_LIBRARIES}")
   ENDIF (NOT OPENJPEG_FIND_QUIETLY)
ELSE (OPENJPEG_FOUND)
   IF (OPENJPEG_FIND_REQUIRED)
      MESSAGE(FATAL_ERROR "Could not find OPENJPEG library")
   ENDIF (OPENJPEG_FIND_REQUIRED)
ENDIF (OPENJPEG_FOUND)

MARK_AS_ADVANCED(
  OPENJPEG_LIBRARY
  OPENJPEG_INCLUDE_DIR
  )
