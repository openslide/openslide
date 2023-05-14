tools_deps = [
  openslide_dep,
  openslide_common_dep,
  glib_dep,
]

tools = [
  ['openslide-quickhash1sum', tools_deps],
  ['openslide-show-properties', tools_deps],
  ['openslide-write-png', [tools_deps, png_dep]],
  ['openslide-write-associated-images', [tools_deps, png_dep]],
]
foreach t : tools
  executable(
    t[0],
    t[0] + '.c',
    dependencies : t[1],
    install : true,
  )
  install_man(
    configure_file(
      input : t[0] + '.1.in',
      output : t[0] + '.1',
      configuration : versions,
    )
  )
endforeach
