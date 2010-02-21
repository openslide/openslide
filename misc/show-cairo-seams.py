import cairo

src = cairo.ImageSurface(cairo.FORMAT_RGB24, 60, 40)
cr = cairo.Context(src)

cr.set_source_rgb(0, 0, 0)
cr.paint()
src.write_to_png("src.png")


dst = cairo.ImageSurface(cairo.FORMAT_ARGB32, 400, 400)
cr = cairo.Context(dst)
cr.set_source_rgb(1, 1, 1)
cr.paint()

cr.translate(0, 0.5)       # comment out to hide seams

cr.translate(20, 20)
cr.set_source_surface(src, 0, 0)
cr.rectangle(0, 0, 60, 40)
cr.fill()

cr.translate(0, 40)
cr.set_source_surface(src, 0, 0)
cr.rectangle(0, 0, 60, 40)
cr.fill()

dst.write_to_png("dst.png")
