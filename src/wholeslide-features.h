#ifndef WHOLESLIDE_FEATURES_H
#define WHOLESLIDE_FEATURES_H


// for exporting from shared libraries or DLLs
#if defined WIN32
#  ifdef BUILDING_DLL
#    define wholeslide_public __declspec(dllexport)
#  else
#    define wholeslide_public __declspec(dllimport)
#  endif
#elif __GNUC__ > 3
# define wholeslide_public __attribute((visibility("default")))
#else
# define wholeslide_public
#endif



#endif
