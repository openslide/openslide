
To build, open terminal in /app directory, run:
    ./configure
    make
    make install

To test with DZI web interface:
    Make image files available in container - e.g. copy to subdir in code directory.

    In terminal:
        ldconfig /usr/local/lib
        cd /app_py/examples/deepzoom
        python3 deepzoom_server.py /app/data/061-P1_Ovary.tif
        
    In browser on host:
        To get image stats:
            http://127.0.0.1:5000/slide.dzi
        To get a tile level=9, x=1, y=2:
            http://127.0.0.1:5000/slide_files/9/0_0.jpeg
            http://127.0.0.1:5000/slide_files/8/0_0.jpeg
