#ifndef _WHOLESLIDE_H_
#define _WHOLESLIDE_H_

#include <stdio.h>
#include <stdint.h>


typedef struct _wholeslide wholeslide_t;

/* one of many possible create functions */
wholeslide_t *wholeslide_create_from_file(FILE *f);

/* copy uncompressed RGB data into region */
/* TODO: different RGB formats? */
void wholeslide_fill_region(wholeslide_t *ws,
			    void *region,
			    double x, double y, double z,
			    uint32_t w, uint32_t h);

/* query list of preferred zoom factors */
uint32_t wholeslide_get_preferred_zoom_count(wholeslide_t *ws);
double wholeslide_get_one_preferred_zoom(wholeslide_t *ws, uint32_t n);

/* destructor */
void wholeslide_destroy(wholeslide_t *ws);


/* TODO: error handling */
/* TODO: metadata */


#endif
