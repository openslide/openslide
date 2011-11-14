#ifndef OPENSLIDE_CONFIG_H
#define OPENSLIDE_CONFIG_H

/* defined if "stdbool.h" is available */
#cmakedefine HAVE_STDBOOL_H

/* defined to the fopen() flag string that sets FD_CLOEXEC, or an empty
   string if not supported. */
#cmakedefine FOPEN_CLOEXEC_FLAG @FOPEN_CLOEXEC_FLAG@

/* defined if fcntl() exists */
#cmakedefine HAVE_FCNTL

#endif
