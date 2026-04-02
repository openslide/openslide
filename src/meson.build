# generate openslide-tables.c
make_tables = executable(
  'make-tables',
  'make-tables.c',
  dependencies : [libm_native_dep],
  native : true,
)
openslide_tables_c = custom_target(
  'make openslide-tables',
  output : 'openslide-tables.c',
  command : [make_tables, '@OUTPUT@'],
)

# Windows resources
openslide_dll_manifest = configure_file(
  input : 'openslide-dll.manifest.in',
  output : 'openslide-dll.manifest',
  configuration : versions,
)
openslide_dll_rc = configure_file(
  input : 'openslide-dll.rc.in',
  output : 'openslide-dll.rc',
  configuration : versions,
)
if host_machine.system() == 'windows'
  openslide_dll_o = import('windows').compile_resources(
    openslide_dll_rc,
    # https://github.com/llvm/llvm-project/issues/63426
    args : [cc.get_argument_syntax() == 'msvc' ? '/c' : '-c', '65001'],
    depend_files : [openslide_dll_manifest],
  )
else
  openslide_dll_o = []
endif

# Public headers
openslide_headers = files('openslide-features.h', 'openslide.h')
include_subdir = 'openslide'
install_headers(
  openslide_headers,
  subdir : include_subdir,
)

# Library
openslide_sources = files(
  'openslide-cache.c',
  'openslide-decode-bmp.c',
  'openslide-decode-dicom.c',
  'openslide-decode-jp2k.c',
  'openslide-decode-jpeg.c',
  'openslide-decode-png.c',
  'openslide-decode-sqlite.c',
  'openslide-decode-tiff.c',
  'openslide-decode-tifflike.c',
  'openslide-decode-xml.c',
  'openslide-error.c',
  'openslide-file.c',
  'openslide-grid.c',
  'openslide-hash.c',
  'openslide-jdatasrc.c',
  'openslide-util.c',
  'openslide-vendor-aperio.c',
  'openslide-vendor-dicom.c',
  'openslide-vendor-generic-tiff.c',
  'openslide-vendor-hamamatsu.c',
  'openslide-vendor-huron.c',
  'openslide-vendor-leica.c',
  'openslide-vendor-mirax.c',
  'openslide-vendor-philips-tiff.c',
  'openslide-vendor-sakura.c',
  'openslide-vendor-synthetic.c',
  'openslide-vendor-trestle.c',
  'openslide-vendor-ventana.c',
  'openslide-vendor-zeiss.c',
  'openslide.c',
) + [
  openslide_dll_o,
  openslide_tables_c,
]
libopenslide = library(
  'openslide',
  openslide_sources,
  version : soversion,
  c_args : ['-D_OPENSLIDE_BUILDING_DLL', '-DG_LOG_DOMAIN="OpenSlide"'],
  # Meson omits 'lib' by default on Windows except on MinGW.  Maintain
  # compatibility with the MinGW build, since it was here first.
  name_prefix : host_machine.system() == 'windows' ? 'lib' : [],
  gnu_symbol_visibility : visibility,
  include_directories : config_h_include,
  dependencies : [
    glib_dep,
    gio_dep,
    gobject_dep,
    cairo_dep,
    dicom_dep,
    sqlite_dep,
    xml_dep,
    tiff_dep,
    openjpeg_dep,
    jpeg_dep,
    png_dep,
    zlib_dep,
    zstd_dep,
    libm_dep,
  ],
  install : true,
)
openslide_dep = declare_dependency(
  include_directories : '.',
  link_with : libopenslide,
)
import('pkgconfig').generate(
  libopenslide,
  description : 'A library for reading whole slide images.',
  subdirs : include_subdir,
  url : 'https://openslide.org',
)
meson.override_dependency('openslide', openslide_dep)
