import cairo
import math

def fill_background(cr):
    cr.set_source_rgb(0, 0, 1)
    cr.paint()

def fill_rectangles(cr):
    cr.set_operator(cairo.OPERATOR_SATURATE)
    cr.set_source_rgb(1, 1, 1)

    cr.rectangle(0, 0, 100, 100)
    cr.fill()

    cr.rectangle(100, 0, 100, 100)
    cr.fill()

    cr.rectangle(200, 0, 100, 100)
    cr.fill()

    cr.rectangle(300, 0, 100, 100)
    cr.fill()

def clip(cr):
    cr.rectangle(0, 0, 400, 400)
    cr.clip()

def clear(cr):
    cr.set_operator(cairo.OPERATOR_CLEAR)
    cr.paint()


# init pdf
pdf = cairo.PDFSurface("out.pdf", 400, 400)
cr = cairo.Context(pdf)
cr.rotate(math.pi / 4)

# page 1, push paint pop paint
fill_background(cr)

cr.push_group()
fill_rectangles(cr)
cr.pop_group_to_source()
cr.paint()

cr.show_page()

# page 2, push clear paint pop paint
fill_background(cr)

cr.push_group()
clear(cr)
fill_rectangles(cr)
cr.pop_group_to_source()
cr.paint()

cr.show_page()

# page 3, push clip paint pop paint
fill_background(cr)

cr.push_group()
clip(cr)
fill_rectangles(cr)
cr.pop_group_to_source()
cr.paint()

cr.show_page()

# page 4, push clip clear paint pop paint
fill_background(cr)

cr.push_group()
clip(cr)
clear(cr)
fill_rectangles(cr)
cr.pop_group_to_source()
cr.paint()

cr.show_page()


# done
pdf.finish()
