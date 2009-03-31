/*
 *  GL Widgets, common stuff
 *  Copyright (C) 2007 Andreas �man
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

#ifndef GLW_H
#define GLW_H

#include "config.h"

#include <queue.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include <time.h>
#include <math.h>
#include "prop.h"
#include "event.h"
#include "ui/ui.h"
#include "showtime.h"

TAILQ_HEAD(glw_queue, glw);
LIST_HEAD(glw_head, glw);
LIST_HEAD(glw_event_map_list, glw_event_map);
LIST_HEAD(glw_prop_sub_list, glw_prop_sub);
LIST_HEAD(glw_loadable_texture_list, glw_loadable_texture);
TAILQ_HEAD(glw_loadable_texture_queue, glw_loadable_texture);
LIST_HEAD(glw_video_list, glw_video);

#if CONFIG_GLW_BACKEND_OPENGL
#include "glw_opengl.h"
#elif CONFIG_GLW_BACKEND_GX
#include "glw_gx.h"
#else
#error No backend for glw
#endif

#include <ft2build.h>  
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H

#define GLW_LERP(a, y0, y1) ((y0) + (a) * ((y1) - (y0)))
#define GLW_S(a) (sin(GLW_LERP(a, M_PI * -0.5, M_PI * 0.5)) * 0.5 + 0.5)
#define GLW_LP(a, y0, y1) (((y0) * ((a) - 1.0) + (y1)) / (a))
#define GLW_MIN(a, b) ((a) < (b) ? (a) : (b))
#define GLW_MAX(a, b) ((a) > (b) ? (a) : (b))
#define GLW_DEG2RAD(a) ((a) * M_PI * 2.0f / 360.0f)
#define GLW_RESCALE(x, min, max) (((x) - (min)) / ((max) - (min)))

typedef enum {
  GLW_DUMMY,        /* Emtpy placeholder, wont render anything */
  GLW_MODEL,
  GLW_CONTAINER_X,  /* Horizonal weight based container */
  GLW_CONTAINER_Y,  /* Vertical weight based container */
  GLW_CONTAINER_Z,  /* Depth container */
  GLW_STACK_X,      /* Horizonal aspect based stack */
  GLW_STACK_Y,      /* Vertical aspect based stack */
  GLW_LIST_X,
  GLW_LIST_Y,
  GLW_DECK,
  GLW_EXPANDER,
  GLW_ANIMATOR,
  GLW_IMAGE,
  GLW_LABEL,
  GLW_TEXT,
  GLW_INTEGER,
  GLW_ROTATOR,      /* Rotating device */
  GLW_CURSOR,
  GLW_MIRROR,
  GLW_FX_TEXROT,
  GLW_VIDEO,
  GLW_SLIDER_X,
  GLW_SLIDER_Y,
  GLW_LAYER,
} glw_class_t;


typedef enum {
  GLW_ATTRIB_END = 0,
  GLW_ATTRIB_PARENT,
  GLW_ATTRIB_PARENT_HEAD,
  GLW_ATTRIB_PARENT_BEFORE,
  GLW_ATTRIB_SIGNAL_HANDLER,
  GLW_ATTRIB_WEIGHT,
  GLW_ATTRIB_CAPTION,
  GLW_ATTRIB_VALUE,
  GLW_ATTRIB_SOURCE,
  GLW_ATTRIB_ASPECT,
  GLW_ATTRIB_ALPHA,
  GLW_ATTRIB_ALPHA_SELF,
  GLW_ATTRIB_ANGLE,
  GLW_ATTRIB_ALIGNMENT,
  GLW_ATTRIB_SET_FLAGS,
  GLW_ATTRIB_CLR_FLAGS,
  GLW_ATTRIB_EXTRA,
  GLW_ATTRIB_SLICES,
  GLW_ATTRIB_X_SLICES,
  GLW_ATTRIB_Y_SLICES,
  GLW_ATTRIB_PREVIEW,
  GLW_ATTRIB_CONTENT,
  GLW_ATTRIB_MODE,
  GLW_ATTRIB_TEXTURE_COORDS,
  GLW_ATTRIB_MIRROR,
  GLW_ATTRIB_ID,
  GLW_ATTRIB_DISPLACEMENT,
  GLW_ATTRIB_RGB,
  GLW_ATTRIB_TIME,
  GLW_ATTRIB_INT_STEP,
  GLW_ATTRIB_INT_MIN,
  GLW_ATTRIB_INT_MAX,
  GLW_ATTRIB_PROPROOT,
  GLW_ATTRIB_TRANSITION_EFFECT,
  GLW_ATTRIB_EXPAND,
  GLW_ATTRIB_BIND_TO_PROPERTY,
  GLW_ATTRIB_BIND_TO_ID,
  GLW_ATTRIB_SIZE,
  GLW_ATTRIB_REPEAT_X,
  GLW_ATTRIB_REPEAT_Y,
  GLW_ATTRIB_PIXMAP,
  GLW_ATTRIB_ORIGINATING_PROP,
} glw_attribute_t;

#define GLW_MIRROR_X   0x1
#define GLW_MIRROR_Y   0x2


#define GLW_MODE_XFADE    0
#define GLW_MODE_SLIDE    1

typedef struct glw_vertex {
  float x, y, z;
} glw_vertex_t;

typedef struct glw_rgb {
  float r, g, b;
} glw_rgb_t;

typedef enum {
  GLW_ALIGN_CENTER,
  GLW_ALIGN_LEFT,
  GLW_ALIGN_RIGHT,
  GLW_ALIGN_BOTTOM,
  GLW_ALIGN_TOP,
} glw_alignment_t;


/**
 * XXX Document these
 */
typedef enum {
  GLW_SIGNAL_NONE,
  GLW_SIGNAL_DESTROY,
  GLW_SIGNAL_DTOR,
  GLW_SIGNAL_INACTIVE,
  GLW_SIGNAL_LAYOUT,
  GLW_SIGNAL_RENDER,

  GLW_SIGNAL_CHILD_CREATED,
  GLW_SIGNAL_CHILD_DESTROYED,

  GLW_SIGNAL_DETACH_CHILD,

  GLW_SIGNAL_NEW_FRAME,

  GLW_SIGNAL_EVENT_BUBBLE,

  GLW_SIGNAL_EVENT,

  GLW_SIGNAL_CHANGED,


  /**
   * Sent to parent to switch currently selected child.
   * Parent should NOT send GLW_SIGNAL_SELECTED_UPDATE to the child
   * in this case.
   */
  GLW_SIGNAL_SELECT,

  /**
   * Emitted by parent to child when it has been selected.
   */
  GLW_SIGNAL_SELECTED_UPDATE,

  /**
   * Sent to widget when its focused child is changed.
   * Argument is newly focused child.
   */
  GLW_SIGNAL_FOCUS_CHILD_CHANGED,

  /**
   * Sent to widget when it is focused
   */
  GLW_SIGNAL_FOCUS_SELF,

  /**
   *
   */
  GLW_SIGNAL_POINTER_EVENT,

  /**
   *
   */
  GLW_SIGNAL_SLIDER_METRICS,

  /**
   *
   */
  GLW_SIGNAL_SCROLL,

} glw_signal_t;


typedef struct {
  float knob_size;
  float position;
} glw_slider_metrics_t;

typedef struct {
  float value;
} glw_scroll_t;



/**
 * GLW root context
 */
typedef struct glw_root {
  uii_t gr_uii;

  char *gr_theme;

  hts_thread_t gr_thread;
  hts_mutex_t gr_mutex;
  prop_courier_t *gr_courier;

  struct glw_queue gr_destroyer_queue;
  
  int gr_frameduration;

  struct glw_head gr_active_list;
  struct glw_head gr_active_flush_list;
  struct glw_head gr_active_dummy_list;
  struct glw_head gr_every_frame_list;


  /**
   * Font renderer
   */
  LIST_HEAD(,  glw_text_bitmap) gr_gtbs;
  TAILQ_HEAD(, glw_text_bitmap) gr_gtb_render_queue;
  hts_cond_t gr_gtb_render_cond;
  FT_Face gr_gtb_face;
  float gr_fontsize;
  
  /**
   * Image/Texture loader
   */
  hts_mutex_t gr_tex_mutex;
  hts_cond_t gr_tex_load_cond;

  struct glw_loadable_texture_list gr_tex_active_list;
  struct glw_loadable_texture_list gr_tex_flush_list;
  struct glw_loadable_texture_queue gr_tex_rel_queue;
  struct glw_loadable_texture_queue gr_tex_load_queue[2];
  struct glw_loadable_texture_list gr_tex_list;

  /**
   * Root focus leader
   */
  struct glw *gr_pointer_grab;
  struct glw *gr_current_focus;

  /**
   * Backend specifics
   */ 
  glw_backend_root_t gr_be;

} glw_root_t;



/**
 * Render context
 */
typedef struct glw_rctx {
  float rc_alpha;

  float rc_size_x;
  float rc_size_y;

  float rc_zoom;
  float rc_fullscreen;

  struct glw_cursor_painter *rc_cursor_painter;

  float rc_exp_req;

  /**
   * Backend specifics
   */ 
  glw_backend_rctx_t rc_be;

} glw_rctx_t;


#ifdef CONFIG_GLW_BACKEND_GX
#include "glw_gx_ops.h"
#endif


typedef int (glw_callback_t)(struct glw *w, void *opaque, 
			     glw_signal_t signal, void *value);

/**
 * Signal handler
 */
typedef struct glw_signal_handler {
  LIST_ENTRY(glw_signal_handler) gsh_link;
  glw_callback_t *gsh_func;
  void *gsh_opaque;
  int gsh_pri;
} glw_signal_handler_t;

LIST_HEAD(glw_signal_handler_list, glw_signal_handler);


/**
 * GL widget
 */
typedef struct glw {
  glw_class_t glw_class;
  glw_root_t *glw_root;
  int glw_refcnt;
  prop_t *glw_originating_prop;  /* Source prop we spawned from */

  LIST_ENTRY(glw) glw_active_link;
  LIST_ENTRY(glw) glw_every_frame_link;

  struct glw_signal_handler_list glw_signal_handlers;

  struct glw *glw_parent;
  TAILQ_ENTRY(glw) glw_parent_link;
  struct glw_queue glw_childs;

  struct glw_queue glw_render_list;
  TAILQ_ENTRY(glw) glw_render_link;
		   
  struct glw *glw_selected;
  struct glw *glw_focused;

  /** 
   * All the glw_parent stuff is operated by this widgets
   * parents. That is, a widget may never touch these themselfs
   */		  
  float glw_parent_alpha;
  glw_vertex_t glw_parent_pos;
  glw_vertex_t glw_parent_scale;
  float glw_parent_misc[4];

  int glw_flags;  

#define GLW_FOCUS_DISABLED      0x1     /* Can not receive focus right now */
#define GLW_KEEP_ASPECT         0x2     /* Keep aspect (for bitmaps) */
#define GLW_DESTROYED           0x4     /* was destroyed but someone
					   is holding references */
#define GLW_RENDER_LINKED       0x8     /* glw_render_link is linked */
#define GLW_EVERY_FRAME         0x10    /* Want GLW_SIGNAL_NEW_FRAME
					   at all times */
#define GLW_DRAW_SKEL           0x20    /* Draw extra lines to
					    visualize details */
#define GLW_FOCUS_DRAW_CURSOR   0x40    /* Draw cursor when we have focus */
#define GLW_DEBUG               0x80    /* Debug this object */
#define GLW_PASSWORD            0x100   /* Don't display real contents */
#define GLW_FOCUSABLE           0x200
#define GLW_FOCUS_BLOCKED       0x400


  glw_vertex_t glw_displacement;

  float glw_conf_weight;             /* Relative weight (configured) */
  float glw_norm_weight;             /* Relative weight (normalized) */
  float glw_aspect;                  /* Aspect */
  float glw_alpha;                   /* Alpha set by user */
  float glw_extra;

  float glw_time;                    /* Time constant */

  glw_alignment_t glw_alignment;

  char *glw_id;

  struct glw_event_map_list glw_event_maps;		  

  struct glw_prop_sub_list glw_prop_subscriptions;

  struct token *glw_dynamic_expressions;

  float *glw_matrix;

  float glw_exp_req;

} glw_t;

int glw_init(glw_root_t *gr, float fontsize);

void glw_flush0(glw_root_t *gr);

void *glw_get_opaque(glw_t *w, glw_callback_t *func);

void glw_set_active0(glw_t *w);

void glw_reaper0(glw_root_t *gr);

void glw_cond_wait(glw_root_t *gr, hts_cond_t *c);

void glw_detach0(glw_t *w);

void glw_lock(glw_root_t *gr);

void glw_unlock(glw_root_t *gr);

int glw_dispatch_event(uii_t *uii, event_t *e);


/**
 *
 */
#define glw_is_focusable(w) (!!((w)->glw_flags & GLW_FOCUSABLE))

int glw_is_focused(glw_t *w);

void glw_store_matrix(glw_t *w, glw_rctx_t *rc);

void glw_focus_set(glw_t *w);

void glw_focus_set_if_parent_is_in_focus(glw_t *w);

void glw_focus_unblock_path(glw_t *w);

void glw_focus_crawl(glw_t *w, int forward);

int glw_focus_step(glw_t *w, int forward);


/**
 * Models
 */
glw_t *glw_model_create(glw_root_t *gr, const char *src, 
			glw_t *parent, int flags,
			struct prop *prop);

#define GLW_MODEL_CACHE 0x1

/**
 * Transitions
 */
typedef enum {
  GLW_TRANS_BLEND,
  GLW_TRANS_FLIP_HORIZONTAL,
  GLW_TRANS_FLIP_VERTICAL,
  GLW_TRANS_SLIDE_HORIZONTAL,
  GLW_TRANS_SLIDE_VERTICAL,
  GLW_TRANS_num,
} glw_transition_type_t;


static inline void
glw_flush_render_list(glw_t *w)
{
  glw_t *c;
  TAILQ_FOREACH(c, &w->glw_render_list, glw_render_link)
    c->glw_flags &= ~GLW_RENDER_LINKED;
  TAILQ_INIT(&w->glw_render_list);
}


static inline void
glw_link_render_list(glw_t *w, glw_t *c)
{
  TAILQ_INSERT_TAIL(&w->glw_render_list, c, glw_render_link);
  c->glw_flags |= GLW_RENDER_LINKED;
}


/*
 *
 */

#define GLW_ATTRIB_CHEW(attrib, ap)		\
do {						\
  switch(attrib) {				\
  case GLW_ATTRIB_END:				\
    break;					\
  case GLW_ATTRIB_SIGNAL_HANDLER:               \
    (void)va_arg(ap, void *);			\
    (void)va_arg(ap, void *);			\
    (void)va_arg(ap, int);			\
    break;                                      \
  case GLW_ATTRIB_PARENT_BEFORE:		\
  case GLW_ATTRIB_BIND_TO_PROPERTY:		\
    (void)va_arg(ap, void *);			\
  case GLW_ATTRIB_PARENT:			\
  case GLW_ATTRIB_PARENT_HEAD:			\
  case GLW_ATTRIB_SOURCE:			\
  case GLW_ATTRIB_CAPTION:			\
  case GLW_ATTRIB_PREVIEW:			\
  case GLW_ATTRIB_CONTENT:			\
  case GLW_ATTRIB_ID:         			\
  case GLW_ATTRIB_PROPROOT:         		\
  case GLW_ATTRIB_BIND_TO_ID: 			\
  case GLW_ATTRIB_PIXMAP: 			\
  case GLW_ATTRIB_ORIGINATING_PROP: 		\
    (void)va_arg(ap, void *);			\
    break;					\
  case GLW_ATTRIB_ALIGNMENT:			\
  case GLW_ATTRIB_SET_FLAGS:			\
  case GLW_ATTRIB_CLR_FLAGS:			\
  case GLW_ATTRIB_SLICES:                       \
  case GLW_ATTRIB_X_SLICES:                     \
  case GLW_ATTRIB_Y_SLICES:                     \
  case GLW_ATTRIB_MODE:                         \
  case GLW_ATTRIB_MIRROR:                       \
  case GLW_ATTRIB_TRANSITION_EFFECT:            \
  case GLW_ATTRIB_REPEAT_X:                     \
  case GLW_ATTRIB_REPEAT_Y:                     \
    (void)va_arg(ap, int);			\
    break;					\
  case GLW_ATTRIB_TEXTURE_COORDS:               \
    (void)va_arg(ap, double);			\
  case GLW_ATTRIB_DISPLACEMENT:                 \
  case GLW_ATTRIB_RGB:                          \
    (void)va_arg(ap, double);			\
    (void)va_arg(ap, double);			\
  case GLW_ATTRIB_WEIGHT:			\
  case GLW_ATTRIB_ASPECT:			\
  case GLW_ATTRIB_ALPHA:			\
  case GLW_ATTRIB_ALPHA_SELF:			\
  case GLW_ATTRIB_ANGLE:			\
  case GLW_ATTRIB_EXTRA:			\
  case GLW_ATTRIB_TIME:                         \
  case GLW_ATTRIB_EXPAND:                       \
  case GLW_ATTRIB_VALUE:                        \
  case GLW_ATTRIB_INT_STEP:                     \
  case GLW_ATTRIB_INT_MIN:                      \
  case GLW_ATTRIB_INT_MAX:                      \
  case GLW_ATTRIB_SIZE:                         \
    (void)va_arg(ap, double);			\
    break;					\
  }						\
} while(0)



static inline int
glw_signal0(glw_t *w, glw_signal_t sig, void *extra)
{
  glw_signal_handler_t *gsh, *next;
  for(gsh = LIST_FIRST(&w->glw_signal_handlers); gsh != NULL; gsh = next) {
    next = LIST_NEXT(gsh, gsh_link);
    if(gsh->gsh_func(w, gsh->gsh_opaque, sig, extra))
      return 1;
  }
  return 0;
}


void glw_render0(glw_t *w, glw_rctx_t *rc);

void glw_layout0(glw_t *w, glw_rctx_t *rc);

glw_t *glw_create0(glw_root_t *gr, glw_class_t class, va_list ap);

glw_t *glw_create_i(glw_root_t *gr, 
		    glw_class_t class, ...) __attribute__((__sentinel__(0)));

#define glw_lock_assert() glw_lock_check(__FILE__, __LINE__)

void glw_lock_check(const char *file, const int line);

int glw_attrib_set0(glw_t *w, int init, va_list ap);

void glw_set_i(glw_t *w, ...);

void glw_destroy0(glw_t *w);

void glw_deref0(glw_t *w);

int glw_get_text0(glw_t *w, char *buf, size_t buflen);

int glw_get_int0(glw_t *w, int *result);

glw_t *glw_get_prev_n(glw_t *c, int count);

glw_t *glw_get_next_n(glw_t *c, int count);

glw_t *glw_get_prev_n_all(glw_t *c, int count);

glw_t *glw_get_next_n_all(glw_t *c, int count);

int glw_event(glw_root_t *gr, event_t *e);

int glw_event_to_widget(glw_t *w, event_t *e, int local);

typedef enum {
  GLW_POINTER_CLICK,
  GLW_POINTER_MOTION,
  GLW_POINTER_FOCUS_MOTION,
  GLW_POINTER_RELEASE,
  GLW_POINTER_SCROLL_UP,
  GLW_POINTER_SCROLL_DOWN,
} glw_pointer_event_type_t;

typedef struct glw_pointer_event {
  float x, y;
  glw_pointer_event_type_t type;
} glw_pointer_event_t;

void glw_pointer_event(glw_root_t *gr, glw_t *top, glw_pointer_event_t *gpe);


int glw_navigate(glw_t *w, event_t *e, int local);

glw_t *glw_find_neighbour(glw_t *w, const char *id);

#define GLW_SIGNAL_PRI_INTERNAL 100

void glw_signal_handler_register(glw_t *w, glw_callback_t *func, void *opaque, 
				 int pri);

void glw_signal_handler_unregister(glw_t *w, glw_callback_t *func,
				   void *opaque);

#define glw_signal_handler_int(w, func) \
 glw_signal_handler_register(w, func, NULL, GLW_SIGNAL_PRI_INTERNAL)

int glw_signal0(glw_t *w, glw_signal_t sig, void *extra);

#define glw_render0(w, rc) glw_signal0(w, GLW_SIGNAL_RENDER, rc)
#define glw_layout0(w, rc) glw_signal0(w, GLW_SIGNAL_LAYOUT, rc)

void glw_check_system_features(glw_root_t *gr);

void glw_render_T(glw_t *c, glw_rctx_t *rc, glw_rctx_t *prevrc);

void glw_render_TS(glw_t *c, glw_rctx_t *rc, glw_rctx_t *prevrc);

void glw_rescale(glw_rctx_t *rc, float t_aspect);

extern const glw_vertex_t align_vertices[];

static inline void
glw_align_1(glw_rctx_t *rc, glw_alignment_t a)
{
  if(a != GLW_ALIGN_CENTER)
    glw_Translatef(rc, 
		   align_vertices[a].x, 
		   align_vertices[a].y, 
		   align_vertices[a].z);
}

static inline void
glw_align_2(glw_rctx_t *rc, glw_alignment_t a)
{
  if(a != GLW_ALIGN_CENTER)
    glw_Translatef(rc, 
		   -align_vertices[a].x, 
		   -align_vertices[a].y, 
		   -align_vertices[a].z);
}

/**
 * Render interface abstraction
 */

#define GLW_RENDER_ATTRIBS_NONE       0
#define GLW_RENDER_ATTRIBS_TEX        1
#define GLW_RENDER_ATTRIBS_TEX_COLOR  2


void glw_render_init(glw_renderer_t *gr, int vertices, int attribs);

void glw_render_free(glw_renderer_t *gr);

void glw_render_vtx_pos(glw_renderer_t *gr, int vertex,
			float x, float y, float z);

void glw_render_vtx_st(glw_renderer_t *gr, int vertex,
		       float s, float t);

void glw_render_vts_col(glw_renderer_t *gr, int vertex,
			float r, float g, float b, float a);

void glw_render(glw_renderer_t *gr, glw_rctx_t *rc, int mode, int attribs,
		glw_backend_texture_t *be_tex,
		float r, float g, float b, float a);

/**
 * Global flush interface 
 */
typedef struct glw_gf_ctrl {
  LIST_ENTRY(glw_gf_ctrl) link;
  void (*flush)(void *opaque);
  void *opaque;
} glw_gf_ctrl_t;

void glw_gf_register(glw_gf_ctrl_t *ggc);

void glw_gf_unregister(glw_gf_ctrl_t *ggc);

void glw_gf_do(void);

void glw_font_change_size(glw_root_t *gr, float fontsize);

#endif /* GLW_H */