/*
 *  Video decoder
 *  Copyright (C) 2007 Andreas Öman
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

#ifndef VIDEO_DECODER_H
#define VIDEO_DECODER_H

#include "media.h"
#include "misc/avgtime.h"
#include "misc/kalman.h"

#if ENABLE_DVD
TAILQ_HEAD(dvdspu_queue, dvdspu);
#include <dvdnav/dvdnav.h>
#endif

TAILQ_HEAD(subtitle_queue, subtitle);


struct AVCodecContext;
struct AVFrame;
struct video_decoder;

typedef struct frame_info {
  int width;
  int height;
  int pix_fmt;
  int64_t pts;
  int epoch;
  int duration;

  float dar;

  char interlaced;     // Frame delivered is interlaced 
  char tff;            // For interlaced frame, top-field-first
  char prescaled;      // Output frame is prescaled to requested size
  
  enum AVColorSpace color_space;
  enum AVColorRange color_range;


} frame_info_t;


/**
 *
 */
typedef void (vd_frame_deliver_t)(uint8_t * const data[], const int pitch[],
				  const frame_info_t *info, void *opaque);

/**
 *
 */
typedef struct video_decoder {

  void *vd_opaque;

  vd_frame_deliver_t *vd_frame_deliver;

  hts_thread_t vd_decoder_thread;

  int vd_hold;

  int vd_compensate_thres;

  LIST_ENTRY(glw_video) vd_global_link;


  media_pipe_t *vd_mp;

  int vd_decoder_running;
  int vd_do_flush;
  int vd_skip;

  int64_t vd_nextpts;
  int64_t vd_prevpts;
  int vd_prevpts_cnt;
  int vd_estimated_duration;

  AVFrame *vd_frame;

  /* Clock (audio - video sync, etc) related members */

  int vd_avdiff;
  int vd_avd_delta;

   
  /* stats */

  avgtime_t vd_decode_time;
  avgtime_t vd_upload_time;


  /* Kalman filter for AVdiff compensation */

  kalman_t vd_avfilter;
  float vd_avdiff_x;

  /* Deinterlacing */

  int vd_interlaced; // Used to keep deinterlacing on

  int vd_may_update_avdiff;

  /**
   * DVD / SPU related members
   */
#ifdef CONFIG_DVD
  struct dvdspu_queue vd_spu_queue;

  uint32_t *vd_spu_clut;
  
  hts_mutex_t vd_spu_mutex;

  pci_t vd_pci;

  int vd_spu_curbut;
  int vd_spu_repaint;

#endif
  int vd_spu_in_menu;

  /**
   * Subtitling
   */
  struct subtitle_queue vd_sub_queue;
  hts_mutex_t vd_sub_mutex;

  /**
   *
   */
#define VD_FRAME_SIZE_LEN 16
#define VD_FRAME_SIZE_MASK (VD_FRAME_SIZE_LEN - 1)

  int vd_frame_size[VD_FRAME_SIZE_LEN];
  int vd_frame_size_ptr;

} video_decoder_t;


video_decoder_t *video_decoder_create(media_pipe_t *mp, 
				      vd_frame_deliver_t *frame_delivery,
				      void *opaque);

void video_decoder_stop(video_decoder_t *gv);

void video_decoder_destroy(video_decoder_t *gv);

void video_deliver_frame(video_decoder_t *vd, media_pipe_t *mp, media_buf_t *mb,
			 AVCodecContext *ctx, AVFrame *frame,
			 int64_t tim, int64_t pts, int64_t dts,
			 int duration, int epoch);


/**
 * DVD SPU (SubPicture Units)
 *
 * This include both subtitling and menus on DVDs
 */
#if ENABLE_DVD

typedef struct dvdspu {

  TAILQ_ENTRY(dvdspu) d_link;

  uint8_t *d_data;
  size_t d_size;

  int d_cmdpos;
  int64_t d_pts;

  uint8_t d_palette[4];
  uint8_t d_alpha[4];
  
  int d_x1, d_y1;
  int d_x2, d_y2;

  uint8_t *d_bitmap;

  int d_destroyme;

} dvdspu_t;

void dvdspu_decoder_init(video_decoder_t *vd);

void dvdspu_decoder_deinit(video_decoder_t *vd);

void dvdspu_destroy(video_decoder_t *vd, dvdspu_t *d);

void dvdspu_decoder_dispatch(video_decoder_t *vd, media_buf_t *mb,
			     media_pipe_t *mp);

int dvdspu_decode(dvdspu_t *d, int64_t pts);

#endif


typedef struct subtitle_rect {
  int x,y,w,h;
  char *bitmap;
} subtitle_rect_t;

/**
 * Subtitling
 */
typedef struct subtitle {

  TAILQ_ENTRY(subtitle) s_link;

  int s_active;

  int64_t s_start;
  int64_t s_stop;

  char *s_text;

  int s_num_rects;
  subtitle_rect_t s_rects[0];

} subtitle_t;

void video_subtitle_destroy(video_decoder_t *vd, subtitle_t *s);

void video_subtitles_init(video_decoder_t *vd);

void video_subtitles_deinit(video_decoder_t *vd);

void video_subtitles_decode(video_decoder_t *vd, media_buf_t *mb);


#endif /* VIDEO_DECODER_H */

