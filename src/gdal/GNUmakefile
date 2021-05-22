include GDALmake.opt

GDAL_OBJ	=	port/*.o 


LIBGDAL-yes	:=	$(GDAL_LIB)
LIBGDAL-$(HAVE_LD_SHARED)	+=	$(GDAL_SLIB)
# override if we are using libtool
LIBGDAL-$(HAVE_LIBTOOL)	:= $(LIBGDAL)

default:	lib-target gdal.pc

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

install: install-lib
	$(INSTALL_DIR) $(DESTDIR)$(INST_BIN)
	$(INSTALL_DIR) $(DESTDIR)$(INST_INCLUDE)
ifeq ($(MACOSX_FRAMEWORK),yes)
	$(INSTALL_DIR) $(DESTDIR)${OSX_VERSION_FRAMEWORK_PREFIX}/PlugIns
	$(INSTALL_DIR) $(DESTDIR)${OSX_VERSION_FRAMEWORK_PREFIX}/unix/lib
	ln -sfh Versions/Current/unix $(DESTDIR)${OSX_FRAMEWORK_PREFIX}/unix
	ln -sfh ../Programs $(DESTDIR)${OSX_VERSION_FRAMEWORK_PREFIX}/unix/bin
	ln -sfh ../Headers $(DESTDIR)${OSX_VERSION_FRAMEWORK_PREFIX}/unix/include
	ln -sf ../../GDAL $(DESTDIR)${OSX_VERSION_FRAMEWORK_PREFIX}/unix/lib/libgdal.dylib
	ln -sfh $(GDAL_VERSION_MAJOR).$(GDAL_VERSION_MINOR) $(DESTDIR)${OSX_FRAMEWORK_PREFIX}/Versions/Current
	ln -sfh Versions/Current/Headers $(DESTDIR)${OSX_FRAMEWORK_PREFIX}/Headers
	ln -sfh Versions/Current/Programs $(DESTDIR)${OSX_FRAMEWORK_PREFIX}/Programs
	ln -sfh Versions/Current/Resources $(DESTDIR)${OSX_FRAMEWORK_PREFIX}/Resources
	ln -sfh Versions/Current/GDAL $(DESTDIR)${OSX_FRAMEWORK_PREFIX}/GDAL
endif
	(cd port; $(MAKE) install)
	$(LIBTOOL_FINISH) $(DESTDIR)$(INST_LIB)
	$(INSTALL_DIR) $(DESTDIR)$(INST_LIB)/pkgconfig
	$(INSTALL_DATA) gdal.pc $(DESTDIR)$(INST_LIB)/pkgconfig/gdal.pc

ifeq ($(HAVE_LIBTOOL),yes)

install-lib: default
	$(INSTALL_DIR) $(DESTDIR)$(INST_LIB)
	for f in $(LIBGDAL-yes) ; do $(INSTALL_LIB) $$f $(DESTDIR)$(INST_LIB) ; done
ifeq ($(MACOSX_FRAMEWORK),yes)
	if [ -f "$(DESTDIR)$(INST_LIB)/libgdal.$(GDAL_VERSION_MAJOR).$(LIBGDAL_AGE).$(LIBGDAL_REVISION).dylib" ] ; then mv -f $(DESTDIR)$(INST_LIB)/libgdal.$(GDAL_VERSION_MAJOR).$(LIBGDAL_AGE).$(LIBGDAL_REVISION).dylib $(DESTDIR)${OSX_VERSION_FRAMEWORK_PREFIX}/GDAL ; fi
	if [ -f "$(DESTDIR)$(INST_LIB)/libgdal.$(GDAL_VERSION_MAJOR).$(LIBGDAL_AGE).dylib" ] ; then mv -f $(DESTDIR)$(INST_LIB)/libgdal.$(GDAL_VERSION_MAJOR).$(LIBGDAL_AGE).dylib $(DESTDIR)${OSX_VERSION_FRAMEWORK_PREFIX}/GDAL ; fi
	if [ -f "$(DESTDIR)$(INST_LIB)/libgdal.$(GDAL_VERSION_MAJOR).dylib" ] ; then mv -f $(DESTDIR)$(INST_LIB)/libgdal.$(GDAL_VERSION_MAJOR).dylib $(DESTDIR)${OSX_VERSION_FRAMEWORK_PREFIX}/GDAL ; fi
	if [ -f "$(DESTDIR)$(INST_LIB)/libgdal.dylib" ] ; then mv -f $(DESTDIR)$(INST_LIB)/libgdal.dylib $(DESTDIR)${OSX_VERSION_FRAMEWORK_PREFIX}/GDAL ; fi
	rm -f $(DESTDIR)$(INST_LIB)/libgdal.$(GDAL_VERSION_MAJOR).$(LIBGDAL_AGE).$(LIBGDAL_REVISION).dylib
	rm -f $(DESTDIR)$(INST_LIB)/libgdal.$(GDAL_VERSION_MAJOR).$(LIBGDAL_AGE).dylib
	rm -f $(DESTDIR)$(INST_LIB)/libgdal.$(GDAL_VERSION_MAJOR).dylib
	rm -f $(DESTDIR)$(INST_LIB)/libgdal.dylib
	rm -f $(DESTDIR)$(INST_LIB)/libgdal.la
else
	$(INSTALL_DIR) $(DESTDIR)$(INST_LIB)/gdalplugins
endif

else

ifeq ($(HAVE_LD_SHARED),yes)

GDAL_SLIB_B	=	$(notdir $(GDAL_SLIB))

install-lib: default

	$(INSTALL_DIR) $(DESTDIR)$(INST_LIB)
ifeq ($(MACOSX_FRAMEWORK),yes)
	$(INSTALL_LIB) $(GDAL_SLIB) $(DESTDIR)$(INST_LIB)/GDAL
else
	rm -f $(DESTDIR)$(INST_LIB)/$(GDAL_SLIB_B)
	rm -f $(DESTDIR)$(INST_LIB)/$(GDAL_SLIB_B).$(GDAL_VERSION_MAJOR)
	rm -f $(DESTDIR)$(INST_LIB)/$(GDAL_SLIB_B).$(GDAL_VER)
	$(INSTALL_LIB) $(GDAL_SLIB) $(DESTDIR)$(INST_LIB)/$(GDAL_SLIB_B).$(GDAL_VER)
	(cd $(DESTDIR)$(INST_LIB) ; \
	 ln -s $(GDAL_SLIB_B).$(GDAL_VER) $(GDAL_SLIB_B).$(GDAL_VERSION_MAJOR))
	(cd $(DESTDIR)$(INST_LIB) ; \
	 ln -s $(GDAL_SLIB_B).$(GDAL_VERSION_MAJOR) $(GDAL_SLIB_B))
	$(INSTALL_DIR) $(DESTDIR)$(INST_LIB)/gdalplugins
endif

else

install-lib: default
	$(INSTALL_DIR) $(DESTDIR)$(INST_LIB)
	$(INSTALL_LIB) $(GDAL_LIB) $(DESTDIR)$(INST_LIB)

endif # HAVE_LD_SHARED=no

endif # HAVE_LIBTOOL=no

gdal.pc:	gdal.pc.in GDALmake.opt ./GNUmakefile VERSION
	rm -f gdal.pc
	echo 'CONFIG_VERSION='`cat ./VERSION`'' >> gdal.pc
	echo 'CONFIG_INST_PREFIX=$(INST_PREFIX)' >> gdal.pc
	echo 'CONFIG_INST_LIBS=$(CONFIG_LIBS_INS)' >> gdal.pc
	echo 'CONFIG_INST_CFLAGS=-I$(INST_INCLUDE)' >> gdal.pc
	cat gdal.pc.in >> gdal.pc
