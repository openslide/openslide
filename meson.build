project(
  'openslide', 'c',
  version : '4.0.0',
  license : 'LGPL-2.1-only',
  default_options : [
    'buildtype=debugoptimized',
    'c_std=gnu99',
    'warning_level=2',
  ],
  # limited by Ubuntu 20.04
  meson_version : '>=0.53',
)
# Shared library version.  Follow SemVer rules.
soversion = '1.0.0'

if not meson.is_subproject()
  meson.add_dist_script(
    'scripts/dist.py'
  )
endif

conf = configuration_data()

# Calculate derived versions
version = meson.project_version()
version_suffix = get_option('version_suffix')
if version_suffix != ''
  suffixed_version = '@0@-@1@'.format(version, version_suffix)
  message('Using version string ' + suffixed_version)
else
  suffixed_version = version
endif
parts = (version + '.0.0.0').split('.')
windows_versioninfo = '@0@,@1@,@2@,@3@'.format(
  parts[0], parts[1], parts[2], parts[3]
)
conf.set_quoted('VERSION', version)
conf.set_quoted('SUFFIXED_VERSION', suffixed_version)
# unquoted versions
versions = {
  'VERSION': version,
  'SUFFIXED_VERSION': suffixed_version,
  'WINDOWS_VERSIONINFO': windows_versioninfo,
}

# Compiler flags
cc = meson.get_compiler('c')
cc_native = meson.get_compiler('c', native : true)
add_project_arguments(
  cc.get_supported_arguments(
    '-Wstrict-prototypes',
    '-Wmissing-prototypes',
    '-Wmissing-declarations',
    '-Wnested-externs',
    '-fno-common',
  ),
  '-DG_DISABLE_SINGLE_INCLUDES',
  '-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_56',
  '-DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_MIN_REQUIRED',
  language : 'c'
)
add_project_link_arguments(
  cc.get_supported_link_arguments(
    '-Wl,--no-undefined',
  ),
  language : 'c'
)

# fopen cloexec flag
if host_machine.system() in ['dragonfly', 'freebsd', 'linux', 'netbsd', 'openbsd']
  message('Using "e" flag for close-on-exec')
  conf.set_quoted('FOPEN_CLOEXEC_FLAG', 'e')
elif host_machine.system() == 'windows'
  message('Using "N" flag for close-on-exec (Windows)')
  conf.set_quoted('FOPEN_CLOEXEC_FLAG', 'N')
else
  message('Using no close-on-exec flag (unknown)')
  conf.set_quoted('FOPEN_CLOEXEC_FLAG', '')
  conf.set('NONATOMIC_CLOEXEC', 1)
endif
if get_option('_nonatomic_cloexec')
  # CI sets this on Linux distros with libraries that don't correctly set
  # CLOEXEC.  Disable CLOEXEC leak check.
  conf.set('NONATOMIC_CLOEXEC', 1)
endif

# Math library, host and build
foreach i : [[cc, 'libm_dep'], [cc_native, 'libm_native_dep']]
  if not i[0].has_function('floor', prefix : '#include <math.h>')
    set_variable(i[1], i[0].find_library('m'))
  else
    set_variable(i[1], declare_dependency())
  endif
endforeach

# Dependencies
feature_flags = []
zlib_dep       = dependency('zlib')
jpeg_dep       = dependency('libjpeg')
png_dep        = dependency('libpng', version : '>1.2')
openjpeg_dep   = dependency('libopenjp2', version : '>=2.1.0')
tiff_dep       = dependency('libtiff-4')
glib_dep       = dependency('glib-2.0', version : '>=2.56')
gio_dep        = dependency('gio-2.0')
gobject_dep    = dependency('gobject-2.0')
cairo_dep      = dependency('cairo', version : '>=1.2')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0', version : '>=2.14')
xml_dep        = dependency('libxml-2.0')
sqlite_dep     = dependency('sqlite3', version : '>=3.14')
dicom_dep      = dependency(
  'libdicom',
  # avoid 'check' dependency
  default_options : ['tests=false'],
  fallback : ['libdicom', 'libdicom_dep'],
  version : '>=1.0.0',
)
valgrind_dep   = dependency('valgrind', required : false)

doxygen = find_program(
  'doxygen',
  required : get_option('doc'),
  disabler : true,
)

# Dependency checks
if valgrind_dep.found()
  conf.set('HAVE_VALGRIND', 1)
endif

if glib_dep.type_name() != 'internal'
  # Courtesy check that the compiler supports the cleanup attribute.  If
  # glib is another subpackage, we can't check, so fail at build time if not.
  # glib only offers autoptr macros when the compiler supports them.
  cc.has_header_symbol(
    'glib.h', 'G_DEFINE_AUTOPTR_CLEANUP_FUNC',
    dependencies : glib_dep,
    required : true,
  )
endif

# Test configuration
test_cflags = declare_dependency()
if not get_option('test').disabled() and not get_option('check_windows_abi_compat').disabled()
  # gcc assumes Win32 stack frames are 16-byte-aligned by default, but the
  # Win32 ABI only requires 4-byte alignment.  If gcc emits aligned SSE
  # instructions that access the stack, this can cause GPFs if we're called by
  # a program compiled by a different compiler such as MSVC.  The user is
  # responsible for choosing compiler options appropriate for their desired
  # ABI, but this is a sufficiently insidious error that it's worth checking
  # in the test suite by default.  We do that by compiling the test programs
  # with 4-byte stack alignment on Win32.  We allow the user to disable that
  # behavior, however, in case they don't care about ABI compatibility with
  # other compilers.
  if host_machine.system() == 'windows' and host_machine.cpu_family() == 'x86'
    message('Configuring tests with 4-byte stack alignment')
    test_cflags = declare_dependency(
      compile_args : cc.get_supported_arguments('-mpreferred-stack-boundary=2'),
    )
  endif
endif

# Test suite options
visibility = get_option('_export_internal_symbols') ? '' : 'hidden'
if get_option('_gcov')
  add_project_arguments(
    '-O0',
    '-g',
    '-fprofile-arcs',
    '-ftest-coverage',
    language : 'c'
  )
  # add_project_dependencies() is a bit cleaner but requires Meson >= 0.63
  add_project_link_arguments(
    '-lgcov',
    language : 'c'
  )
endif
if get_option('_sanitize')
  sanitize = [
    '-fno-omit-frame-pointer',
    '-fsanitize-ignorelist=' + meson.current_source_dir() / 'test/clang.supp',
    # address and leak sanitizers
    '-fsanitize=address',
    # UB sanitizer
    '-fsanitize=undefined',
    '-fsanitize=float-divide-by-zero',
    '-fsanitize=unsigned-integer-overflow',
    '-fsanitize=implicit-conversion',
    '-fsanitize=local-bounds',
  ]
  if host_machine.system() != 'darwin'
    # CFI (not supported on macOS)
    sanitize += [
      '-fsanitize=cfi',
      '-fno-sanitize-trap=cfi',
      '-fsanitize-recover=cfi',
      '-flto',
      # Needed in all binaries, not just libopenslide
      '-fvisibility=hidden',
    ]
  endif
  add_project_arguments(
    sanitize,
    language : 'c',
  )
  undefs = ['-Wl,-z', '-Wl,undefs']
  if not cc.has_multi_link_arguments(undefs)
    undefs = []
  endif
  add_project_link_arguments(
    sanitize,
    undefs,
    language : 'c',
  )
endif

# config.h
configure_file(
  output : 'config.h',
  configuration : conf,
)
config_h_include = include_directories('.')

# Subdirs
subdir('src')
subdir('common')
subdir('tools')
if not get_option('test').disabled()
  subdir('test')
endif
if doxygen.found()
  subdir('doc')
endif
