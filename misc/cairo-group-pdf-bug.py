import cairo
import math

def fill_background(cr):
    cr.set_source_rgb(0, 0, 1)
    cr.paint()

def paint_from_image(cr, src):
    cr.set_operator(cairo.OPERATOR_SATURATE)

    cr.set_source_surface(src, 0, 0)
    cr.paint()

    cr.set_source_surface(src, 100, 0)
    cr.paint()

    cr.set_source_surface(src, 200, 0)
    cr.paint()

    cr.set_source_surface(src, 300, 0)
    cr.paint()

def clip(cr):
    cr.rectangle(0, 0, 400, 400)
    cr.clip()

def clear(cr):
    cr.set_operator(cairo.OPERATOR_CLEAR)
    cr.paint()


# init image
src = cairo.ImageSurface(cairo.FORMAT_ARGB32, 100, 100)
cr = cairo.Context(src)
cr.set_source_rgb(1, 1, 1)
cr.paint()

# init pdf
pdf = cairo.PDFSurface("out.pdf", 400, 400)
cr = cairo.Context(pdf)
cr.rotate(math.pi / 4)

# page 1, push paint pop paint
fill_background(cr)

cr.push_group()
paint_from_image(cr, src)
cr.pop_group_to_source()
cr.paint()

cr.show_page()

# page 2, push clear paint pop paint
fill_background(cr)

cr.push_group()
clear(cr)
paint_from_image(cr, src)
cr.pop_group_to_source()
cr.paint()

cr.show_page()

# page 3, push clip paint pop paint
fill_background(cr)

cr.push_group()
clip(cr)
paint_from_image(cr, src)
cr.pop_group_to_source()
cr.paint()

cr.show_page()

# page 4, push clip clear paint pop paint
fill_background(cr)

cr.push_group()
clip(cr)
clear(cr)
paint_from_image(cr, src)
cr.pop_group_to_source()
cr.paint()

cr.show_page()


# done
pdf.finish()
