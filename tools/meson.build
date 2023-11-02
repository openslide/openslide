if meson.version().version_compare('>=0.61') and host_machine.system() != 'windows'
  # build slidetool and symlink the other tools to it
  tools_binaries = ['slidetool']
  symlink_targets = [
    'openslide-quickhash1sum',
    'openslide-show-properties',
    'openslide-write-png',
  ]
  foreach target : symlink_targets
    install_symlink(
      target,
      install_dir : get_option('bindir'),
      install_tag : 'bin',
      pointing_to : 'slidetool',
    )
  endforeach
else
  tools_binaries = [
    'openslide-quickhash1sum',
    'openslide-show-properties',
    'openslide-write-png',
    'slidetool',
  ]
endif
foreach target : tools_binaries
  exe = executable(
    target,
    [
      'slidetool.c',
      'slidetool-icc.c',
      'slidetool-image.c',
      'slidetool-prop.c',
      'slidetool-slide.c',
      'slidetool-util.c',
    ],
    include_directories : config_h_include,
    dependencies : [
      openslide_dep,
      openslide_common_dep,
      glib_dep,
      png_dep,
    ],
    install : true,
    install_tag : 'bin',
  )
  set_variable(target.underscorify(), exe)
endforeach

mans = [
  'openslide-quickhash1sum',
  'openslide-show-properties',
  'openslide-write-png',
  'slidetool',
]
foreach m : mans
  install_man(
    configure_file(
      input : m + '.1.in',
      output : m + '.1',
      configuration : versions,
    )
  )
endforeach
