libtest_sources = files(
  'libtest-fd.c',
)

libtest = static_library(
  'libtest',
  libtest_sources,
  dependencies : [openslide_common_dep, glib_dep, valgrind_dep],
  include_directories : config_h_include,
)

libtest_dep = declare_dependency(
  include_directories : '.',
  link_with : libtest,
)
