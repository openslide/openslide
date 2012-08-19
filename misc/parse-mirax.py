#!/usr/bin/python
#
#  OpenSlide, a library for reading whole slide image files
#
#  Copyright (c) 2007-2012 Carnegie Mellon University
#  Copyright (c) 2011 Google, Inc.
#  All rights reserved.
#
#  OpenSlide is free software: you can redistribute it and/or modify
#  it under the terms of the GNU Lesser General Public License as
#  published by the Free Software Foundation, version 2.1.
#
#  OpenSlide is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
#  GNU Lesser General Public License for more details.
#
#  You should have received a copy of the GNU Lesser General Public
#  License along with OpenSlide. If not, see
#  <http://www.gnu.org/licenses/>.
#

from __future__ import division
from ConfigParser import RawConfigParser, NoOptionError
import io
import os
import struct
import sys

class Reporter(object):
    def __init__(self, level=0):
        self._level = level
        self._indent = ' ' * level * 2

    def __call__(self, key, value):
        print '%s%-30s %s' % (self._indent, key + ':', value)

    def child(self, desc):
        print '%s%s:' % (self._indent, desc)
        return type(self)(self._level + 1)


class SlidedatHierarchy(object):
    SECTION = 'HIERARCHICAL'

    def __init__(self, dat):
        self._by_name = {}
        self._next_offset = 0

        layers = dat.getint(self.SECTION, self.LAYER_COUNT_KEY)
        for layer_id in range(layers):
            layer = HierLayer(self, dat, layer_id)
            self._by_name[layer.name] = layer

    def get_layer_by_name(self, name):
        return self._by_name[name]

    def next_offset(self):
        self._next_offset += 1
        return self._next_offset - 1


class HierTree(SlidedatHierarchy):
    LAYER_COUNT_KEY = 'HIER_COUNT'
    LAYER_NAME_KEY = 'HIER_%d_NAME'
    LAYER_SECTION_KEY = 'HIER_%d_SECTION'
    LEVEL_COUNT_KEY = 'HIER_%d_COUNT'
    LEVEL_NAME_KEY = 'HIER_%d_VAL_%d'
    LEVEL_SECTION_KEY = 'HIER_%d_VAL_%d_SECTION'


class NonHierTree(SlidedatHierarchy):
    LAYER_COUNT_KEY = 'NONHIER_COUNT'
    LAYER_NAME_KEY = 'NONHIER_%d_NAME'
    LAYER_SECTION_KEY = 'NONHIER_%d_SECTION'
    LEVEL_COUNT_KEY = 'NONHIER_%d_COUNT'
    LEVEL_NAME_KEY = 'NONHIER_%d_VAL_%d'
    LEVEL_SECTION_KEY = 'NONHIER_%d_VAL_%d_SECTION'


class HierLayer(object):
    def __init__(self, h, dat, layer_id):
        self.name = dat.get(h.SECTION, h.LAYER_NAME_KEY % layer_id)
        self.section = dat.get(h.SECTION, h.LAYER_SECTION_KEY % layer_id)
        self.levels = dat.getint(h.SECTION, h.LEVEL_COUNT_KEY % layer_id)

        self._by_id = []
        self._by_name = {}
        for level_id in range(self.levels):
            level = HierLevel(h, dat, layer_id, level_id)
            self._by_id.append(level)
            self._by_name[level.name] = level

    def get_level_by_name(self, name):
        return self._by_name[name]

    def get_level_by_id(self, id):
        return self._by_id[id]


class HierLevel(object):
    def __init__(self, h, dat, layer_id, level_id):
        self.name = dat.get(h.SECTION,
                h.LEVEL_NAME_KEY % (layer_id, level_id))
        self.section = dat.get(h.SECTION,
                h.LEVEL_SECTION_KEY % (layer_id, level_id))
        self.offset = h.next_offset()


def read_zoom_level(r, dat, section):
    r('Concat factor', dat.getint(section, 'IMAGE_CONCAT_FACTOR'))
    r('Tile width', dat.getint(section, 'DIGITIZER_WIDTH'))
    r('Tile height', dat.getint(section, 'DIGITIZER_HEIGHT'))
    r('Overlap X', dat.getfloat(section, 'OVERLAP_X'))
    r('Overlap Y', dat.getfloat(section, 'OVERLAP_Y'))

    fill = dat.getint(section, 'IMAGE_FILL_COLOR_BGR')
    # Swap to RGB
    fill = struct.unpack('<I', struct.pack('>I', fill))[0] >> 8
    r('Background', '%x' % fill)


def read_len(f, size):
    ret = f.read(size)
    assert(len(ret) == size)
    return ret


def read_int32(f):
    buf = f.read(4)
    assert(len(buf) == 4)
    return struct.unpack('<i', buf)[0]


def assert_int32(f, value):
    v = read_int32(f)
    assert(v == value)


def read_nonhier_record(r, datafiles, f, root_position, record):
    r('Nonhier record', record)
    f.seek(root_position)
    # seek to record
    table_base = read_int32(f)
    f.seek(table_base + record * 4)
    # seek to list head
    list_head = read_int32(f)
    f.seek(list_head)
    # seek to data page
    pagesize = read_int32(f)
    if pagesize == 0x302e3130:
        # Magic constant indicating an empty section
        r('File', 'None')
        return
    else:
        assert(pagesize == 0)
    page = read_int32(f)
    f.seek(page)
    # check pagesize
    assert_int32(f, 1)
    # check rest of prologue
    assert_int32(f, 0)
    assert_int32(f, 0)
    assert_int32(f, 0)
    # read actual data
    position = read_int32(f)
    size = read_int32(f)
    fileno = read_int32(f)
    r('File', os.path.basename(datafiles[fileno]))
    r('Position', position)
    r('Length', size)
    return (fileno, position, size)


def read_hier_record(r, f, root_position, datafiles, record, tiles_x):
    # find start of hier table
    f.seek(root_position)
    # seek to record
    table_base = read_int32(f)
    f.seek(table_base + record * 4)
    # seek to list head
    list_head = read_int32(f)
    f.seek(list_head)
    # get offset of first data page
    assert_int32(f, 0)
    data_page = read_int32(f)
    # read data pages
    while data_page != 0:
        f.seek(data_page)
        entries = read_int32(f)
        data_page = read_int32(f)
        for i in range(entries):
            tile_index = read_int32(f)
            position = read_int32(f)
            size = read_int32(f)
            fileno = read_int32(f)
            r('Tile %5d x %5d' % (tile_index % tiles_x,
                    tile_index // tiles_x), '%s %10d + %10d' % (
                    os.path.basename(datafiles[fileno]), position, size))


def read_tile_position_map(r, image_divisions, tiles_x, f, offset, len):
    assert(len % 9 == 0)
    f.seek(offset)
    images_x = tiles_x // image_divisions
    for i in range(len // 9):
        zz = struct.unpack('B', read_len(f, 1))[0]
        x = read_int32(f)
        y = read_int32(f)
        if x != 0 or y != 0:
            r('Tile %5d x %5d' % ((i % images_x) * image_divisions,
                    (i // images_x) * image_divisions),
                    '%8d x %8d  (%3d)' % (x, y, zz))


def dump_mirax(path, r=None):
    if r is None:
        r = Reporter()

    dirname, ext = os.path.splitext(path)
    if ext != '.mrxs':
        raise Exception('Not a MIRAX file: %s' % path)

    # Start parsing slidedat
    f = io.open(os.path.join(dirname, 'Slidedat.ini'), encoding='utf-8-sig')
    dat = RawConfigParser()
    dat.readfp(f)
    tiles_x = dat.getint('GENERAL', 'IMAGENUMBER_X')
    tiles_y = dat.getint('GENERAL', 'IMAGENUMBER_Y')
    slide_id = dat.get('GENERAL', 'SLIDE_ID')
    try:
        slide_type = dat.get('GENERAL', 'SLIDE_TYPE')
    except NoOptionError:
        slide_type = 'unknown'
    try:
        image_divisions = dat.getint('GENERAL', 'CameraImageDivisionsPerSide')
    except NoOptionError:
        image_divisions = 1
    datafiles = [os.path.join(dirname, dat.get('DATAFILE', 'FILE_%d' % i))
            for i in range(dat.getint('DATAFILE', 'FILE_COUNT'))]
    r('Slide version', dat.get('GENERAL', 'SLIDE_VERSION'))
    r('Slide ID', slide_id)
    r('Slide type', slide_type)
    r('Tiles in X', tiles_x)
    r('Tiles in Y', tiles_y)
    r('Image divisions per side', image_divisions)

    # Parse hierarchical tree
    hier_tree = HierTree(dat)
    slide_zoom_layer = hier_tree.get_layer_by_name('Slide zoom level')

    # Parse nonhierarchical tree
    nonhier_tree = NonHierTree(dat)
    nonhier_offsets = {}
    # Associated images (may be missing)
    for key, level in (('macro', 'ScanDataLayer_SlideThumbnail'),
            ('label', 'ScanDataLayer_SlideBarcode'),
            ('thumbnail', 'ScanDataLayer_SlidePreview')):
        try:
            nonhier_offsets[key] = nonhier_tree.get_layer_by_name(
                    'Scan data layer').get_level_by_name(level).offset
        except KeyError:
            pass
    # Position map (may be missing)
    try:
        position_layer = nonhier_tree.get_layer_by_name(
                'VIMSLIDE_POSITION_BUFFER')
        nonhier_offsets['index'] = position_layer.get_level_by_name(
                'default').offset
        r('Position map ver', dat.get(position_layer.section,
                'VIMSLIDE_POSITION_DATA_FORMAT_VERSION'))
    except KeyError:
        pass

    # Start parsing index.dat
    index = open(os.path.join(dirname, dat.get('HIERARCHICAL', 'INDEXFILE')))
    index_version = read_len(index, 5)
    index_id = read_len(index, len(slide_id))
    hier_root = index.tell()
    nonhier_root = hier_root + 4
    r('Index version', index_version)
    r('Index ID', index_id)

    # Print associated images
    rr = r.child('Associated images')
    for associated in 'macro', 'label', 'thumbnail':
        if associated in nonhier_offsets:
            read_nonhier_record(rr.child(associated), datafiles, index,
                    nonhier_root, nonhier_offsets[associated])

    # Print slidedat zoom levels
    rr = r.child('Zoom levels')
    for i in range(slide_zoom_layer.levels):
        read_zoom_level(rr.child('Level %d' % i), dat,
                slide_zoom_layer.get_level_by_id(i).section)

    # Print tile position map
    if 'index' in nonhier_offsets:
        rr = r.child('Tile positions')
        fileno, position, size = read_nonhier_record(rr, datafiles, index,
                nonhier_root, nonhier_offsets['index'])
        read_tile_position_map(rr, image_divisions, tiles_x,
                open(datafiles[fileno]), position, size)
    else:
        r('Tile positions', 'None')

    # Print tile locations
    rr = r.child('Tiles')
    for level in range(slide_zoom_layer.levels):
        read_hier_record(rr.child('Level %d' % level), index, hier_root,
                datafiles, level, tiles_x)


if __name__ == '__main__':
    dump_mirax(sys.argv[1])
