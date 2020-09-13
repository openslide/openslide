# OpenSlide release process

- [ ] Check [Buildbot](https://buildbot.openslide.org/waterfall) for build and test failures.  Check all mosaics.
- [ ] Update `CHANGELOG.txt`, version in `configure.ac` and libtool `-version-info` in `Makefile.am`
- [ ] Create and push signed tag
- [ ] `git clean -dxf && autoreconf -i && ./configure && make distcheck`
- [ ] Attach release notes to [GitHub release](https://github.com/openslide/openslide/releases) and upload tarballs
- [ ] Update openslide-winbuild
- [ ] Update website: `_data/releases.yaml`, `_includes/news.markdown`, `api/`
- [ ] [Start a build of the demo site](https://buildbot.openslide.org/builders/retile)
- [ ] Send mail to -announce and -users
- [ ] Update Fedora package
- [ ] Update MacPorts package
