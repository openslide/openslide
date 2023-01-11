test_deps = [
  declare_dependency(
    include_directories : config_h_include,
  ),
  test_cflags,
  openslide_dep,
  openslide_common_dep,
  glib_dep,
]

# Test binaries
executable(
  'extended', 'extended.c',
  dependencies : test_deps,
)
executable(
  'mosaic', 'mosaic.c',
  dependencies : [test_deps, cairo_dep],
)
executable(
  'parallel', 'parallel.c',
  dependencies : test_deps,
)
executable(
  'profile', 'profile.c',
  dependencies : [test_deps, valgrind_dep],
)
executable(
  'query', 'query.c',
  dependencies : test_deps,
)
test_synth = executable(
  'synth', 'synth.c',
  dependencies : test_deps,
)
executable(
  'try_open', 'try_open.c',
  dependencies : [test_deps, valgrind_dep],
  c_args : ['-Wno-deprecated-declarations'],
)

# Tests
test('synth', test_synth)

# Driver
configure_file(
  input : 'driver.in',
  output : 'driver',
  configuration : {
    'SRCDIR': meson.current_source_dir(),
    'BUILDDIR': meson.current_build_dir(),
    # Path prefix for glib Valgrind suppressions
    'GLIB2_DATADIR': gio_dep.get_variable(
      default_value : '',
      pkgconfig : 'datadir'
    ),
    'FEATURES': ' '.join(feature_flags),
  },
)
