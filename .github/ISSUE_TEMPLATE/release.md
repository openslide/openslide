# OpenSlide release process

- [ ] Update `CHANGELOG.md` and `version` and `soversion` in `meson.build`
- [ ] Create and push signed tag
- [ ] Ensure Meson version is at least 0.60
- [ ] `git clean -dxf && meson setup builddir && meson dist -C builddir`
- [ ] Attach release notes to [GitHub release](https://github.com/openslide/openslide/releases/new) and upload tarball
- [ ] [Update openslide-bin](https://github.com/openslide/openslide-bin/issues/new?labels=release&template=release.md)
- [ ] Update website: `_data/releases.yaml`, `_includes/news.md`, `api/`
- [ ] Start a [CI build](https://github.com/openslide/openslide.github.io/actions/workflows/retile.yml) of the demo site
- [ ] Update Ubuntu PPA
- [ ] Send mail to -announce and -users
- [ ] Update Fedora and EPEL packages
- [ ] Update MacPorts package
