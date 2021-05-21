include GDALmake.opt

GDAL_OBJ	=	port/*.o 


LIBGDAL-yes	:=	$(GDAL_LIB)
LIBGDAL-$(HAVE_LD_SHARED)	+=	$(GDAL_SLIB)
# override if we are using libtool
LIBGDAL-$(HAVE_LIBTOOL)	:= $(LIBGDAL)

default:	lib-target

lib-target:	check-lib;

force-lib:
	$(AR) r $(GDAL_LIB) $(GDAL_OBJ)
	$(RANLIB) $(GDAL_LIB)
	$(LD_SHARED) $(GDAL_SLIB_SONAME) $(GDAL_OBJ) $(GDAL_LIBS) $(LDFLAGS) $(LIBS) \
		-o $(GDAL_SLIB)

static-lib: lib-dependencies GDALmake.opt
	$(MAKE) static-lib-stage2

static-lib-stage2: $(GDAL_OBJ)
	rm -f libgdal.a
	$(AR) r $(GDAL_LIB) $(GDAL_OBJ)

$(GDAL_LIB):	$(GDAL_OBJ) GDALmake.opt
	rm -f libgdal.a
	$(AR) r $(GDAL_LIB) $(GDAL_OBJ)
	$(RANLIB) $(GDAL_LIB)

$(GDAL_SLIB):	$(GDAL_OBJ) $(GDAL_LIB)
	$(LD_SHARED) $(GDAL_SLIB_SONAME) $(GDAL_OBJ) $(GDAL_LIBS) $(LDFLAGS) $(LIBS) \
		-o $(GDAL_SLIB)

#  split potentially long lines
SORTED  := $(sort $(wildcard $(GDAL_OBJ:.o=.lo)))
NSORTED := $(words $(SORTED))
#  mid left and right indices
MIDL := $(shell echo $$(( $(NSORTED) / 2 )) )
MIDR := $(shell echo $$(( $(MIDL) + 1 )) )

$(LIBGDAL):	$(GDAL_OBJ:.o=.lo)
	$(LD) $(LDFLAGS) $(LIBS) -o $@ \
	$(wordlist 1,$(MIDL),$(SORTED)) \
	$(wordlist $(MIDR),$(words $(SORTED)),$(SORTED)) \
	    -rpath $(INST_LIB) \
	    -no-undefined \
	    -version-info $(LIBGDAL_CURRENT):$(LIBGDAL_REVISION):$(LIBGDAL_AGE)
ifeq ($(MACOSX_FRAMEWORK),yes)
	install_name_tool -id ${OSX_VERSION_FRAMEWORK_PREFIX}/GDAL .libs/libgdal.dylib
endif

lib-dependencies:	port-target

check-lib:	lib-dependencies
	$(MAKE) $(LIBGDAL-yes)

port-target:
	(cd port; $(MAKE))

clean:	lclean
	(cd port; $(MAKE) clean)

lclean:
	rm -f *.a *.so config.log config.cache html/*.*
	$(RM) *.la

distclean:	dist-clean

dist-clean:	clean
	rm -f GDALmake.opt port/cpl_config.h config.cache config.status
	rm -f libtool
	rm -rf autom4te.cache

config:	configure
	./configure

GDALmake.opt:	GDALmake.opt.in config.status
	./config.status

config.status:  configure
	./config.status --recheck


