# OpenSlide release process

- [ ] Check CI for build and test failures.  Check all mosaics.
- [ ] Update `CHANGELOG.txt`, version in `configure.ac` and libtool `-version-info` in `Makefile.am`
- [ ] Create and push signed tag
- [ ] `git clean -dxf && autoreconf -i && ./configure && make distcheck`
- [ ] Attach release notes to [GitHub release](https://github.com/openslide/openslide/releases/new) and upload tarballs
- [ ] [Update openslide-winbuild](https://github.com/openslide/openslide-winbuild/issues/new?labels=release&template=release.md)
- [ ] Update website: `_data/releases.yaml`, `_includes/news.md`, `api/`
- [ ] Start a CI build of the demo site
- [ ] Send mail to -announce and -users
- [ ] Update Fedora package
- [ ] Update MacPorts package
