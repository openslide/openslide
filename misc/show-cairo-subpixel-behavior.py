import cairo

# this shows that even though we are using integer values
# for set_source_surface, the subpixel translation is
# capturing pixel values from adjacent pixels of the source

src = cairo.ImageSurface(cairo.FORMAT_RGB24, 60, 40)
cr = cairo.Context(src)

#cr.set_source_rgb(1, 0, 0)
#cr.rectangle(0, 0, 20, 40)
#cr.fill()
cr.set_source_rgb(0, 1, 0)
cr.rectangle(20, 0, 20, 40)
cr.fill()
#cr.set_source_rgb(0, 0, 1)
#cr.rectangle(40, 0, 20, 40)
#cr.fill()
src.write_to_png("src.png")


dst = cairo.ImageSurface(cairo.FORMAT_ARGB32, 400, 400)
cr = cairo.Context(dst)
cr.set_source_rgb(1, 1, 1)
cr.paint()

for z in range(0, 400):
    cr.translate(0.01, 1)
    cr.set_source_surface(src, -20, 0)
    cr.rectangle(0, 0, 20, 40)
    cr.fill()
dst.write_to_png("dst.png")
