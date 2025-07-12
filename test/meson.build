# Required dependencies
foreach dep : ['cjpeg', 'djpeg', 'xdelta3']
  set_variable(
    dep,
    find_program(
      dep,
      required : get_option('test'),
    ),
  )
  if not get_variable(dep).found()
    subdir_done()
  endif
endforeach

# Optional dependencies
valgrind_dep = dependency(
  'valgrind',
  required : false,
)
if valgrind_dep.found()
  conf.set('HAVE_VALGRIND', 1)
endif
foreach dep : ['clang', 'gcov']
  set_variable(
    dep,
    find_program(
      dep,
      required : false,
    ),
  )
endforeach

# libtest
subdir('lib')

# Common binary deps
test_deps = [
  declare_dependency(
    include_directories : config_h_include,
  ),
  openslide_dep,
  openslide_common_dep,
  libtest_dep,
  glib_dep,
]

# Test binaries
executable(
  'extended',
  'extended.c',
  dependencies : test_deps,
)
executable(
  'mosaic',
  'mosaic.c',
  dependencies : [test_deps, cairo_dep],
)
executable(
  'parallel',
  'parallel.c',
  dependencies : test_deps,
)
executable(
  'profile',
  'profile.c',
  dependencies : test_deps,
)
executable(
  'query',
  'query.c',
  dependencies : test_deps,
)
executable(
  'try_open',
  'try_open.c',
  dependencies : test_deps,
  c_args : ['-Wno-deprecated-declarations'],
)

# Driver
configure_file(
  input : 'driver.in',
  output : 'driver',
  configuration : {
    'SRCDIR' : meson.current_source_dir(),
    'BUILDDIR' : meson.current_build_dir(),
    # Path prefix for glib Valgrind suppressions
    'GLIB2_DATADIR' : gio_dep.get_variable(
      default_value : '',
      pkgconfig : 'datadir',
    ),
    'CJPEG' : cjpeg.full_path(),
    'DJPEG' : djpeg.full_path(),
    'XDELTA3' : xdelta3.full_path(),
    'CLANG' : clang.found() ? clang.full_path() : '',
    'GCOV' : gcov.found() ? gcov.full_path() : '',
    'FEATURES' : ' '.join(feature_flags),
    'VERSION' : meson.project_version(),
  },
)
