#!/usr/bin/env python
#
# mosaic-coords - Small web viewer for finding an appropriate mosaic tile
#
# Copyright (c) 2010-2014 Carnegie Mellon University
#
# This library is free software; you can redistribute it and/or modify it
# under the terms of version 2.1 of the GNU Lesser General Public License
# as published by the Free Software Foundation.
#
# This library is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
# License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this library; if not, write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#

from flask import Flask, abort, make_response, render_template_string
from io import BytesIO
import json
from optparse import OptionParser
import os

# Set up environment before importing openslide
os.putenv('OPENSLIDE_DEBUG', 'tiles')

from openslide import OpenSlide
from openslide.deepzoom import DeepZoomGenerator

DEEPZOOM_SLIDE = None
DEBUG = False
INDEX_TEMPLATE = '''
<!doctype html>
<title>{{ slide }}</title>

<style type="text/css">
body {
    margin: 20px;
    padding: 0;
}
div#info {
    margin-bottom: 20px;
    font-family: monospace;
}
div#view {
    width: 256px;
    height: 256px;
    background-color: black;
    border: 1px solid black;
}
</style>

<div id="info">
    slide = {{ slide }}<br>
    level = <span id="level">0</span><br>
    x = <span id="x">0</span><br>
    y = <span id="y">0</span>
</div>

<div id="view"></div>

<script type="text/javascript"
        src="https://code.jquery.com/jquery-1.11.1.min.js"></script>
<script type="text/javascript"
        src="https://openslide.org/demo/openseadragon.min.js"></script>
<script type="text/javascript">
$(document).ready(function() {
    var width = {{ width }};
    var height = {{ height }};
    var downsamples = {{ downsamples|safe }};
    var viewer = new OpenSeadragon({
        id: "view",
        tileSources: "{{ url_for('dzi') }}",
        prefixUrl: "https://openslide.org/demo/images/",
        showNavigationControl: false,
        animationTime: 0.5,
        blendTime: 0.1,
        constrainDuringPan: true,
        maxZoomPixelRatio: 1,
        minPixelRatio: 1,
        minZoomLevel: 1,
        visibilityRatio: 1,
        zoomPerScroll: 2,
        timeout: 120000,
    });
    viewer.addHandler("open", function() {
        viewer.source.minLevel = 8;
    });
    viewer.addHandler("animation", function() {
        var bounds = viewer.viewport.getBounds(true);
        $('#x').text(Math.round(width * bounds.x));
        $('#y').text(Math.round(width * bounds.y));  // y is scaled by width

        var zoom = viewer.viewport.getZoom(true);
        var downsample = 1 / viewer.viewport.viewportToImageZoom(zoom);
        for (var i = downsamples.length - 1; i >= 0; i--) {
            if (downsample >= downsamples[i]) {
                $('#level').text(i);
                break;
            }
        }
    });
});
</script>
'''

app = Flask(__name__)
app.config.from_object(__name__)


@app.before_first_request
def load_slide():
    slidefile = app.config['DEEPZOOM_SLIDE']
    if slidefile is None:
        raise ValueError('No slide file specified')
    app.slide = OpenSlide(slidefile)
    app.dz = DeepZoomGenerator(app.slide)


@app.route('/')
def index():
    return render_template_string(
        app.config['INDEX_TEMPLATE'],
        slide=os.path.basename(app.config['DEEPZOOM_SLIDE']),
        width=app.slide.dimensions[0],
        height=app.slide.dimensions[1],
        downsamples=json.dumps(app.slide.level_downsamples)
    )


@app.route('/slide.dzi')
def dzi():
    resp = make_response(app.dz.get_dzi('jpeg'))
    resp.mimetype = 'application/xml'
    return resp


@app.route('/slide_files/<int:level>/<int:col>_<int:row>.jpeg')
def tile(level, col, row):
    try:
        tile = app.dz.get_tile(level, (col, row))
    except ValueError:
        # Invalid level or coordinates
        abort(404)
    buf = BytesIO()
    tile.save(buf, 'jpeg', quality=90)
    resp = make_response(buf.getvalue())
    resp.mimetype = 'image/jpeg'
    return resp


if __name__ == '__main__':
    parser = OptionParser(usage='Usage: %prog [options] <slide>')
    parser.add_option('-l', '--listen', metavar='ADDRESS', dest='host',
                default='127.0.0.1',
                help='address to listen on [127.0.0.1]')
    parser.add_option('-p', '--port', metavar='PORT', dest='port',
                type='int', default=5000,
                help='port to listen on [5000]')

    (opts, args) = parser.parse_args()
    app.config['DEEPZOOM_SLIDE'] = args[0]

    app.run(host=opts.host, port=opts.port, threaded=True)
