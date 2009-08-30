/*
 *  GL Widgets, freefloat
 *  Copyright (C) 2008 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include "glw.h"
#include "glw_freefloat.h"

#define glw_parent_v glw_parent_misc[0]


/**
 *
 */
static int
is_visible(glw_freefloat_t *ff, glw_t *c)
{
  int i;
  for(i = 0; i < GLW_FREEFLOAT_MAX_VISIBLE; i++)
    if(ff->visible[i] == c)
      return 1;
  return 0;
}

/**
 *
 */
static void
glw_freefloat_render(glw_freefloat_t *ff, glw_rctx_t *rc)
{
  glw_t *c;
  int i;
  float a;
  glw_rctx_t rc0;

  for(i = 0; i < ff->num_visible; i++) {
    if((c = ff->visible[i]) == NULL)
      continue;

    rc0 = *rc;
    glw_PushMatrix(rc, &rc0);

    a = (1 - fabs(-1 + (GLW_MAX(0, -0.1 + c->glw_parent_v * 2.1))));

    rc0.rc_alpha = rc->rc_alpha * a;

    glw_Translatef(rc, 
		   c->glw_parent_pos.x,
		   c->glw_parent_pos.y,
		   -5 + c->glw_parent_v * 5);

    glw_signal0(c, GLW_SIGNAL_RENDER, &rc0);
    glw_PopMatrix();    
  }
}


/**
 *
 */
static void
setup_floater(glw_freefloat_t *ff, glw_t *c)
{
  ff->xpos++;
  c->glw_parent_v = 0;

  c->glw_parent_pos.x = -1.0 + (ff->xpos % ff->num_visible) * 2 /
    ((float)ff->num_visible - 1);

  c->glw_parent_pos.y = drand48() * 2.0 - 1.0;
}


/**
 *
 */
static void
glw_freefloat_layout(glw_freefloat_t *ff, glw_rctx_t *rc)
{
  glw_t *w = &ff->w;
  glw_t *c;
  int i, candpos = -1;

  float vmin = 1;

  for(i = 0; i < ff->num_visible; i++) {
    if(ff->visible[i] == NULL) {
      candpos = i;
    } else {
      vmin = GLW_MIN(ff->visible[i]->glw_parent_v, vmin);
    }
  }

  if(vmin > 1.0 / (float)ff->num_visible && candpos != -1) {
    /* Insert new entry */

    if(ff->pick != NULL)
      ff->pick = TAILQ_NEXT(ff->pick, glw_parent_link);
    
    if(ff->pick == NULL)
      ff->pick = TAILQ_FIRST(&w->glw_childs);

    if(ff->pick != NULL && !is_visible(ff, ff->pick)) {
      ff->visible[candpos] = ff->pick;
      setup_floater(ff, ff->pick);
    }
  }

  for(i = 0; i < ff->num_visible; i++) {
    if((c = ff->visible[i]) == NULL)
      continue;

    c->glw_parent_v += 0.001;

    if(c->glw_parent_v >= 1) {
      ff->visible[i] = NULL;
    } else {
      glw_layout0(c, rc);
    }
  }

  c = ff->pick;

  // Layout next few items to pick, to preload textures, etc

  for(i = 0; i < 3 && c != NULL; i++, c = TAILQ_NEXT(c, glw_parent_link)) {
    if(!is_visible(ff, c))
      glw_layout0(c, rc);
  }
}


/**
 *
 */
static int
glw_freefloat_callback(glw_t *w, void *opaque, glw_signal_t signal,
		       void *extra)
{
  glw_freefloat_t *ff = (glw_freefloat_t *)w;
  glw_t *c = extra;

  switch(signal) {
  case GLW_SIGNAL_RENDER:
    glw_freefloat_render(ff, extra);
    return 0;

  case GLW_SIGNAL_LAYOUT:
    glw_freefloat_layout(ff, extra);
    return 0;

  case GLW_SIGNAL_CHILD_DESTROYED:
    assert(!is_visible(ff, c));
    if(c == ff->pick)
      ff->pick = TAILQ_NEXT(ff->pick, glw_parent_link);
    break;

  case GLW_SIGNAL_DETACH_CHILD:
    if(is_visible(ff, c)) {
      // This one is visible, keep it for a while

      if(c == ff->pick)
	ff->pick = TAILQ_NEXT(ff->pick, glw_parent_link);

      glw_remove_from_parent(c, w);
      return 1;
    }
    // Destroy at once
    break;

  default:
    break;
  }
  return 0;
}


/**
 *
 */
void
glw_freefloat_ctor(glw_t *w, int init, va_list ap)
{
  glw_freefloat_t *ff = (glw_freefloat_t *)w;
  glw_attribute_t attrib;

  if(init) {
    ff->num_visible = GLW_FREEFLOAT_MAX_VISIBLE;
    glw_signal_handler_int(w, glw_freefloat_callback);
  }

  do {
    attrib = va_arg(ap, int);
    switch(attrib) {


    default:
      GLW_ATTRIB_CHEW(attrib, ap);
      break;
    }
  } while(attrib);
}