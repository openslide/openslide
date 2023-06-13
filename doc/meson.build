doxyfile = configure_file(
  input : 'Doxyfile.in',
  output : 'Doxyfile',
  configuration : {
    'abs_builddir': meson.current_build_dir(),
    'abs_top_srcdir':  meson.current_source_dir() / '..',
  },
)

custom_target(
  'html',
  depend_files : '../src/openslide.h',
  input : doxyfile,
  output : 'html',
  command : [doxygen, '@INPUT@'],
  build_by_default : false,
)
