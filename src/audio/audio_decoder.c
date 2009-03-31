/*
 *  Audio decoder and features
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include "showtime.h"
#include "audio_decoder.h"
#include "audio.h"
#include "event.h"

extern audio_fifo_t *thefifo;

static struct audio_decoder_queue audio_decoders;
static hts_mutex_t audio_decoders_mutex;

#define CLIP16(a) ((a) > 32767 ? 32767 : ((a) < -32768 ? -32768 : a))

static void audio_mix1(audio_decoder_t *ad, audio_mode_t *am, 
		       int channels, int rate, int64_t chlayout,
		       enum CodecID codec_id,
		       int16_t *data0, int frames, int64_t pts,
		       media_pipe_t *mp);

static void audio_mix2(audio_decoder_t *ad, audio_mode_t *am, 
		       int channels, int rate,
		       int16_t *data0, int frames, int64_t pts,
		       media_pipe_t *mp);

static void close_resampler(audio_decoder_t *ad);

static int resample(audio_decoder_t *ad, int16_t *dstmix, int dstavail,
		    int *writtenp, int16_t *srcmix, int srcframes,
		    int channels);

static void ad_decode_buf(audio_decoder_t *ad, media_pipe_t *mp,
			  media_buf_t *mb);

static void audio_deliver(audio_decoder_t *ad, audio_mode_t *am, int16_t *src, 
			  int channels, int frames, int rate, int64_t pts,
			  media_pipe_t *mp);

static void *ad_thread(void *aux);

/**
 *
 */
void
audio_decoder_init(void)
{
  TAILQ_INIT(&audio_decoders);
}


/**
 * Create an audio decoder pipeline.
 *
 * Called from media.c
 */
audio_decoder_t *
audio_decoder_create(media_pipe_t *mp)
{
  audio_decoder_t *ad;

  ad = calloc(1, sizeof(audio_decoder_t));
  ad->ad_mp = mp;
  ad->ad_outbuf = av_malloc(AVCODEC_MAX_AUDIO_FRAME_SIZE);

  TAILQ_INIT(&ad->ad_hold_queue);

  hts_mutex_lock(&audio_decoders_mutex);
  TAILQ_INSERT_TAIL(&audio_decoders, ad, ad_link);
  hts_mutex_unlock(&audio_decoders_mutex);

  hts_thread_create_joinable(&ad->ad_tid, ad_thread, ad);
  return ad;
}



/**
 * Destroy an audio decoder pipeline.
 *
 * Called from media.c
 */
void
audio_decoder_destroy(audio_decoder_t *ad)
{
  mp_send_cmd_head(ad->ad_mp, &ad->ad_mp->mp_audio, MB_CTRL_EXIT);

  hts_thread_join(&ad->ad_tid);
  audio_fifo_clear_queue(&ad->ad_hold_queue);
  hts_mutex_lock(&audio_decoders_mutex);
  TAILQ_REMOVE(&audio_decoders, ad, ad_link);
  hts_mutex_unlock(&audio_decoders_mutex);

  close_resampler(ad);

  av_free(ad->ad_outbuf);

  if(ad->ad_buf != NULL)
    ab_free(ad->ad_buf);

  free(ad);
}

/**
 * Acquire audio output
 */
void
audio_decoder_acquire_output(audio_decoder_t *ad)
{
  hts_mutex_lock(&audio_decoders_mutex);
  TAILQ_REMOVE(&audio_decoders, ad, ad_link);
  TAILQ_INSERT_HEAD(&audio_decoders, ad, ad_link);
  hts_mutex_unlock(&audio_decoders_mutex);
}

/**
 * Return 1 if the audio output is currently silenced (not primary)
 */
int
audio_decoder_is_silenced(audio_decoder_t *ad)
{
  return TAILQ_FIRST(&audio_decoders) != ad;
}

/**
 *
 */
static int
rateflag_from_rate(int rate)
{
  switch(rate) {
  case 96000: return AM_SR_96000;
  case 48000: return AM_SR_48000;
  case 44100: return AM_SR_44100;
  case 32000: return AM_SR_32000;
  case 24000: return AM_SR_24000;
  default:    return 0;
  }
}


/**
 *
 */
static const uint8_t swizzle_ac3[AUDIO_CHAN_MAX] = {
  0, /* Left */
  2, /* Right */
  3, /* Back Left */
  4, /* Back Right */
  1, /* Center */
  5, /* LFE */
};

/**
 *
 */
static const uint8_t swizzle_aac[AUDIO_CHAN_MAX] = {
  1, /* Left */
  2, /* Right */
  4, /* Back Left */
  5, /* Back Right */
  0, /* Center */
  3, /* LFE */
};

/**
 *
 */
static void *
ad_thread(void *aux)
{
  audio_decoder_t *ad = aux;
  media_pipe_t *mp = ad->ad_mp;
  media_queue_t *mq = &mp->mp_audio;
  media_buf_t *mb;
  int hold = 0;
  int run = 1;

  hts_mutex_lock(&mp->mp_mutex);

  while(run) {

    if((mb = TAILQ_FIRST(&mq->mq_q)) == NULL) {
      hts_cond_wait(&mq->mq_avail, &mp->mp_mutex);
      continue;
    }

    if(mb->mb_data_type == MB_AUDIO && hold) {
      hts_cond_wait(&mq->mq_avail, &mp->mp_mutex);
      continue;
    }

    TAILQ_REMOVE(&mq->mq_q, mb, mb_link);
    mq->mq_len--;
    hts_cond_signal(&mp->mp_backpressure);
    hts_mutex_unlock(&mp->mp_mutex);

    switch(mb->mb_data_type) {
    case MB_CTRL_EXIT:
      run = 0;
      break;

    case MB_CTRL_PAUSE:
      /* Copy back any pending audio in the output fifo */
      audio_fifo_purge(thefifo, ad, &ad->ad_hold_queue);
      hold = 1;
      break;

    case MB_CTRL_PLAY:
      hold = 0;
      break;

    case MB_FLUSH:
      ad->ad_do_flush = 1;
      /* Flush any pending audio in the output fifo */
      audio_fifo_purge(thefifo, ad, NULL);
      break;

    case MB_AUDIO:
      ad_decode_buf(ad, mp, mb);
      break;

    default:
      abort();
    }
    media_buf_free(mb);
    hts_mutex_lock(&mp->mp_mutex);
  }
  hts_mutex_unlock(&mp->mp_mutex);
  audio_fifo_purge(thefifo, ad, NULL);
  return NULL;
}

/**
 *
 */
static void
audio_deliver_passthru(media_buf_t *mb, audio_decoder_t *ad, int format,
		       media_pipe_t *mp)
{
  audio_fifo_t *af = thefifo;
  audio_buf_t *ab;

  ab = af_alloc(mb->mb_size, mp);
  ab->ab_channels = 2;
  ab->ab_format   = format;
  ab->ab_rate     = AM_SR_48000;
  ab->ab_frames   = mb->mb_size;
  ab->ab_pts      = mb->mb_pts;

  memcpy(ab->ab_data, mb->mb_data, mb->mb_size);
  
  ab->ab_ref = ad; /* A reference to our decoder. This is used
		      to revert out packets in the play queue during
		      a pause event */
  
  af_enq(af, ab);
}

static const size_t sample_fmt_to_size[] = {
  [SAMPLE_FMT_U8]  = sizeof(uint8_t),
  [SAMPLE_FMT_S16] = sizeof(int16_t),
  [SAMPLE_FMT_S32] = sizeof(int32_t),
  [SAMPLE_FMT_FLT] = sizeof(float),
  [SAMPLE_FMT_DBL] = sizeof(double),
};

/**
 *
 */
static void
ad_decode_buf(audio_decoder_t *ad, media_pipe_t *mp, media_buf_t *mb)
{
  audio_mode_t *am = audio_mode_current;
  uint8_t *buf;
  int size, r, data_size, channels, rate, frames, delay, i;
  codecwrap_t *cw = mb->mb_cw;
  AVCodecContext *ctx;
  enum CodecID codec_id;
  int64_t pts, chlayout;
  
  if(cw == NULL) {
    /* Raw PCM */
    audio_mix1(ad, am, mb->mb_channels, mb->mb_rate, 0,
	       CODEC_ID_NONE, mb->mb_data, 
	       mb->mb_size / sizeof(int16_t) / mb->mb_channels,
	       mb->mb_pts, mp);
    return;
  }

  ctx = cw->codec_ctx;


  if(TAILQ_FIRST(&audio_decoders) == ad) {
    switch(ctx->codec_id) {
    case CODEC_ID_AC3:
      if(am->am_formats & AM_FORMAT_AC3) {
	audio_deliver_passthru(mb, ad, AM_FORMAT_AC3, mp);
	return;
      }
      break;

    case CODEC_ID_DTS:
      if(am->am_formats & AM_FORMAT_DTS) {
	audio_deliver_passthru(mb, ad, AM_FORMAT_DTS, mp);
	return;
      }
      break;

    default:
      break;
    }
  }
  buf = mb->mb_data;
  size = mb->mb_size;
  pts = mb->mb_pts;
  
  while(size > 0) {

    if(ad->ad_do_flush) {
      avcodec_flush_buffers(cw->codec_ctx);
      ad->ad_do_flush = 0;
    }

    if(audio_mode_stereo_only(am))
      ctx->request_channels = 2; /* We can only output stereo.
				    Ask codecs to do downmixing for us. */
    else
      ctx->request_channels = 0;

    data_size = AVCODEC_MAX_AUDIO_FRAME_SIZE;
    r = avcodec_decode_audio2(ctx, ad->ad_outbuf, &data_size, buf, size);
    if(r == -1)
      break;

    channels = ctx->channels;
    rate     = ctx->sample_rate;
    codec_id = ctx->codec_id;
    chlayout = ctx->channel_layout;

    if(mb->mb_time != AV_NOPTS_VALUE)
      mp_set_current_time(mp, mb->mb_time);

    /* Convert to signed 16bit */

    frames = data_size / sample_fmt_to_size[ctx->sample_fmt];

    switch(ctx->sample_fmt) {
    case SAMPLE_FMT_NONE:
    case SAMPLE_FMT_NB:
      return;

    case SAMPLE_FMT_U8:
      for(i = frames - 1; i >= 0; i--)
	ad->ad_outbuf[i] = (((uint8_t *)ad->ad_outbuf)[i] - 0x80) << 8;
	  break;
    case SAMPLE_FMT_S16:
      break;
    case SAMPLE_FMT_S32:
      for(i = 0; i < frames; i++)
	ad->ad_outbuf[i] = ((int32_t *)ad->ad_outbuf)[i] >> 16;
      break;
    case SAMPLE_FMT_FLT:
      for(i = 0; i < frames; i++)
	ad->ad_outbuf[i] = rintf(((float *)ad->ad_outbuf)[i]) * (1 << 15);
      break;
    case SAMPLE_FMT_DBL:
      for(i = 0; i < frames; i++)
	ad->ad_outbuf[i] = rint(((float *)ad->ad_outbuf)[i]) * (1 << 15);
      break;
    }

    frames /= channels;

    if(TAILQ_FIRST(&audio_decoders) == ad) {

      /* We are the primary audio decoder == we may play, forward
	 to the mixer stages */

      /* But first, if we have any pending packets (due to a previous pause),
	 release them */
      
      audio_fifo_reinsert(thefifo, &ad->ad_hold_queue);

      if(data_size > 0)
	audio_mix1(ad, am, channels, rate, chlayout, codec_id, ad->ad_outbuf, 
		   frames, pts, mp);

      /**
       * Force the global status to point to us
       */

      media_set_currentmedia(mp);

    } else {

      /* We are just suppoed to be silent, emulate some kind of 
	 delay, this is not accurate, so we also turn off the
	 audio clock valid indicator */

      mp->mp_audio_clock_valid = 0;

      delay = (int64_t)frames * 1000000LL / rate;
      usleep(delay); /* XXX: Must be better */
	
      /* Flush any packets in the pause pending queue */
      
      audio_fifo_clear_queue(&ad->ad_hold_queue);
    }
    pts = AV_NOPTS_VALUE;
    buf += r;
    size -= r;
  }
}



/**
 * Audio mixing stage 1
 *
 * All stages that reduces the number of channels is performed here.
 * This reduces the CPU load required during the (optional) resampling.
 */
static void
audio_mix1(audio_decoder_t *ad, audio_mode_t *am, 
	   int channels, int rate, int64_t chlayout, enum CodecID codec_id,
	   int16_t *data0, int frames, int64_t pts,
	   media_pipe_t *mp)
{
  volume_control_t *vc = &global_volume; // Needed for soft-gain
  int16_t tmp[AUDIO_CHAN_MAX];
  int x, y, z, i, c;
  int16_t *data, *src, *dst;
  int rf = rateflag_from_rate(rate);

  /**
   * Channel swizzling
   */

  if(chlayout != 0 && channels > 2) {

    uint8_t s[AUDIO_CHAN_MAX], d[AUDIO_CHAN_MAX];
    int ochan = 0;

    x = 0;
    i = 0;

    if(chlayout & CH_FRONT_LEFT)            s[x] = i++; d[x++] = 0;
    if(chlayout & CH_FRONT_RIGHT)           s[x] = i++; d[x++] = 1;
    if(chlayout & CH_FRONT_CENTER)          s[x] = i++; d[x++] = 4;
    if(chlayout & CH_LOW_FREQUENCY)         s[x] = i++; d[x++] = 5;
    if(chlayout & CH_BACK_LEFT)             s[x] = i++; d[x++] = 6;
    if(chlayout & CH_BACK_RIGHT)            s[x] = i++; d[x++] = 7;
    if(chlayout & CH_FRONT_LEFT_OF_CENTER)         i++;
    if(chlayout & CH_FRONT_RIGHT_OF_CENTER)        i++;
    if(chlayout & CH_BACK_CENTER)                  i++;
    if(chlayout & CH_SIDE_LEFT)             s[x] = i++; d[x++] = 2;
    if(chlayout & CH_SIDE_RIGHT)            s[x] = i++; d[x++] = 3;
 
    ochan = 0;
    for(i = 0; i < x; i++) if(d[i] > ochan) ochan = d[i];
    ochan++;

    memset(tmp, 0, sizeof(tmp));

    if(ochan > channels) {

      src = data0 + frames * channels;
      dst = data0 + frames * ochan;

      for(i = 0; i < frames; i++) {

	src -= channels;
	dst -= ochan;

	for(c = 0; c < x; c++)
	  tmp[d[c]] = src[s[c]];
      
	for(c = 0; c < ochan; c++)
	  dst[c] = tmp[c];
      }

    } else {

      src = data0;
      dst = data0;

      for(i = 0; i < frames; i++) {

	for(c = 0; c < x; c++)
	  tmp[d[c]] = src[s[c]];
      
	for(c = 0; c < ochan; c++)
	  dst[c] = tmp[c];

	src += channels;
	dst += ochan;
      }
    }

    channels = ochan;

  } else {

    const uint8_t *swizzle = NULL; 

    /* Fixed swizzle based on codec */
    switch(codec_id) {

    case CODEC_ID_AAC:
      if(channels > 2)
	swizzle = swizzle_aac;
      break;

    case CODEC_ID_AC3:
      if(channels > 2)
	swizzle = swizzle_ac3;
      break;
    default:
      break;
    }

    if(swizzle != NULL) {
      data = data0;
      for(i = 0; i < frames; i++) {
	for(c = 0; c < channels; c++)
	  tmp[c] = data[swizzle[c]];

	for(c = 0; c < channels; c++)
	  *data++ = tmp[c];
      }
    }
  }


  /**
   * 5.1 to stereo downmixing, coeffs are stolen from AAC spec
   */
  if(channels == 6 && audio_mode_stereo_only(am)) {

    src = data0;
    dst = data0;

    for(i = 0; i < frames; i++) {

      x = (src[0] * 26869) >> 16;
      y = (src[1] * 26869) >> 16;

      z = (src[4] * 19196) >> 16;
      x += z;
      y += z;

      z = (src[5] * 13571) >> 16;
      x += z;
      y += z;

      z = (src[2] * 13571) >> 16;
      x -= z;
      y += z;

      z = (src[3] * 19196) >> 16;
      x -= z;
      y += z;

      src += 6;

      *dst++ = CLIP16(x);
      *dst++ = CLIP16(y);
    }
    channels = 2;
  }

  /**
   * Phantom LFE, mix it into front speakers
   */
  if(am->am_phantom_lfe && channels > 5) {
    data = data0;
    for(i = 0; i < frames; i++) {
      x = data[0];
      y = data[1];

      z = (data[5] * 46334) >> 16;
      x += z;
      y += z;

      data[0] = CLIP16(x);
      data[1] = CLIP16(y);
      data[5] = 0;
      data += channels;
    }
  }


  /**
   * Phantom center, mix it into front speakers
   */
  if(am->am_phantom_center && channels > 4) {
    data = data0;
    for(i = 0; i < frames; i++) {
      x = data[0];
      y = data[1];

      z = (data[4] * 46334) >> 16;
      x += z;
      y += z;

      data[0] = CLIP16(x);
      data[1] = CLIP16(y);
      data[4] = 0;
      data += channels;
    }
  }


  /**
   * Apply softgain, (if needed)
   */

  if(vc->vc_soft_gain_needed) {
    data = data0;
    for(i = 0; i < frames; i++) {
      for(c = 0; c < channels; c++)
	data[c] = CLIP16((data[c] * vc->vc_soft_gain[c]) >> 16);
      data += channels;
    }
  }

  /**
   * Resampling
   */
  if(!(rf & am->am_sample_rates)) {

    int dstrate = 48000;
    int consumed;
    int written;
    int resbufsize = 4096;

    if(ad->ad_resampler_srcrate  != rate    || 
       ad->ad_resampler_dstrate  != dstrate ||
       ad->ad_resampler_channels != channels) {

      /* Must reconfigure, close */
      close_resampler(ad);

      ad->ad_resampler_srcrate  = rate;
      ad->ad_resampler_dstrate  = dstrate;
      ad->ad_resampler_channels = channels;
    }

    if(ad->ad_resampler == NULL) {
      ad->ad_resbuf = malloc(resbufsize * sizeof(int16_t) * channels);
      ad->ad_resampler = av_resample_init(dstrate, rate, 16, 10, 0, 1.0);
    }

    src = data0;
    rate= dstrate;

    /* If we have something in spill buffer, adjust PTS */
    /* XXX: need this ?, it's very small */
    if(pts != AV_NOPTS_VALUE)
      pts -= 1000000LL * ad->ad_resampler_spill_size / rate;

    while(frames > 0) {
      consumed = 
	resample(ad, ad->ad_resbuf, resbufsize,
		 &written, src, frames, channels);
      src += consumed * channels;
      frames -= consumed;

      audio_mix2(ad, am, channels, rate, ad->ad_resbuf, written, pts, mp);
      pts = AV_NOPTS_VALUE;
    }
  } else {
    close_resampler(ad);
    audio_mix2(ad, am, channels, rate, data0, frames, pts, mp);
  }
}


 /**
  * Audio mixing stage 2
  *
  * All stages that increases the number of channels is performed here now
  * after resampling is done
  */
static void
audio_mix2(audio_decoder_t *ad, audio_mode_t *am, 
	   int channels, int rate, int16_t *data0, int frames, int64_t pts,
	   media_pipe_t *mp)
{
  int x, y, i, c;
  int16_t *data, *src, *dst;

  /**
   * Mono expansion (ethier to center speaker or to L + R)
   * We also mix to LFE if possible
   */
  if(channels == 1) {
    src = data0 + frames;

    if(am->am_formats & AM_FORMAT_PCM_5DOT1 && !am->am_phantom_center &&
       !am->am_force_downmix) {
      
      /* Mix mono to center and LFE */

      dst = data0 + frames * 6;
      for(i = 0; i < frames; i++) {
	src--;
	dst -= 6;

	x = *src;

	dst[0] = 0;
	dst[1] = 0;
	dst[2] = 0;
	dst[3] = 0;
	dst[4] = x;
	dst[5] = x;
      }
      channels = 6;
    } else if(am->am_formats & AM_FORMAT_PCM_5DOT1 && !am->am_force_downmix) {

      /* Mix mono to L + R and LFE */

      dst = data0 + frames * 6;
      for(i = 0; i < frames; i++) {
	src--;
	dst -= 6;

	x = *src;

	dst[5] = x;
	x = (x * 46334) >> 16;
	dst[0] = x;
	dst[1] = x;
	dst[2] = 0;
	dst[3] = 0;
	dst[4] = 0;
      }
      channels = 6;
    } else {
      /* Mix mono to L + R  */

      dst = data0 + frames * 2;
      for(i = 0; i < frames; i++) {
	src--;
	dst -= 2;

	x = *src;

	x = (x * 46334) >> 16;

	dst[0] = x;
	dst[1] = x;
      }
      channels = 2;
    }
  } else /* 'Small front' already dealt with */ { 

    /**
     * Small front speakers (need to mix front audio to LFE)
     */
    if(am->am_formats & AM_FORMAT_PCM_5DOT1 && am->am_small_front) {
      if(channels >= 6) {
	data = data0;
	for(i = 0; i < frames; i++) {
	  x = data[5] + (data[0] + data[1]) / 2;
	  data[5] = CLIP16(x);
	}
	data += channels;
      } else {
	src = data0 + frames * channels;
	dst = data0 + frames * 6;

	for(i = 0; i < frames; i++) {
	  src -= channels;
	  dst -= 6;

	  x = (src[0] + src[1]) / 2;
	
	  for(c = 0; c < channels; c++)
	    dst[c] = src[c];

	  for(; c < 5; c++)
	    dst[c] = 0;

	  dst[5] = x;
	}
	channels = 6;
      }
    }
  }

  /**
   * Swap Center + LFE with Surround
   */
  if(am->am_swap_surround && channels > 5) {
    data = data0;
    for(i = 0; i < frames; i++) {
      x = data[4];
      y = data[5];
      data[4] = data[2];
      data[5] = data[3];
      data[2] = x;
      data[3] = y;
      
      data += channels;
    }
  }

  audio_deliver(ad, am, data0, channels, frames, rate, pts, mp);
}


/**
 * Enqueue audio into fifo.
 * We slice the audio into fixed size blocks, if 'am_preferred_size' is
 * set by the audio output module, we use that size, otherwise 1024 frames.
 */
static void
audio_deliver(audio_decoder_t *ad, audio_mode_t *am, int16_t *src, 
	      int channels, int frames, int rate, int64_t pts,
	      media_pipe_t *mp)
{
  audio_buf_t *ab = ad->ad_buf;
  audio_fifo_t *af = thefifo;
  int outsize = am->am_preferred_size ?: 1024;
  int c, r;

  int format;
  int rf = rateflag_from_rate(rate);

  switch(channels) {
  case 2: format = AM_FORMAT_PCM_STEREO; break;
  case 6: format = AM_FORMAT_PCM_5DOT1;  break;
  case 8: format = AM_FORMAT_PCM_7DOT1;  break;
  default:
    return;
  }

  while(frames > 0) {

    if(ab != NULL && ab->ab_channels != channels) {
      /* Channels have changed, flush buffer */
      ab_free(ab);
      ab = NULL;
    }

    if(ab == NULL) {
      ab = af_alloc(sizeof(int16_t) * channels * outsize, mp);
      ab->ab_channels = channels;
      ab->ab_alloced = outsize;
      ab->ab_format = format;
      ab->ab_rate = rf;
      ab->ab_frames = 0;
      ab->ab_pts = AV_NOPTS_VALUE;
    }

    if(ab->ab_pts == AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE) {
      pts -= 1000000LL * ab->ab_frames / rate;
      ab->ab_pts = pts; 
      pts = AV_NOPTS_VALUE;
    }


    r = ab->ab_alloced - ab->ab_frames;
    c = r < frames ? r : frames;

    memcpy(ab->ab_data + sizeof(int16_t) * channels * ab->ab_frames,
	   src,          sizeof(int16_t) * channels * c);

    src           += c * channels;
    ab->ab_frames += c;
    frames        -= c;

    if(ab->ab_frames == ab->ab_alloced) {
      ab->ab_ref = ad; /* A reference to our decoder. This is used
			  to revert out packets in the play queue during
			  a pause event */
      af_enq(af, ab);
      ab = NULL;
    }
  }
  ad->ad_buf = ab;
}







/**
 *
 */
static void
close_resampler(audio_decoder_t *ad)
{
  int c;

  if(ad->ad_resampler == NULL) 
    return;

  free(ad->ad_resbuf);
  ad->ad_resbuf = NULL;

  av_resample_close(ad->ad_resampler);
  
  for(c = 0; c < AUDIO_CHAN_MAX; c++) {
    free(ad->ad_resampler_spill[c]);
    ad->ad_resampler_spill[c] = NULL;
  }

  ad->ad_resampler_spill_size = 0;
  ad->ad_resampler_channels = 0;
  ad->ad_resampler = NULL;
}


/**
 *
 */
static int
resample(audio_decoder_t *ad, int16_t *dstmix, int dstavail,
	 int *writtenp, int16_t *srcmix, int srcframes, int channels)
{
  int c, i, j;
  int16_t *src;
  int16_t *dst;
  int written = 0;
  int consumed;
  int srcsize;
  int spill = ad->ad_resampler_spill_size;
  
   if(spill > srcframes)
     srcframes = 0;

   dst = malloc(dstavail * sizeof(uint16_t));

   for(c = 0; c < channels; c++) {

     if(ad->ad_resampler_spill[c] != NULL) {

       srcsize = spill + srcframes;

       src = malloc(srcsize * sizeof(uint16_t));

       j = 0;

       for(i = 0; i < spill; i++)
	 src[j++] = ad->ad_resampler_spill[c][i];

       for(i = 0; i < srcframes; i++)
	 src[j++] = srcmix[i * channels + c];

       free(ad->ad_resampler_spill[c]);
       ad->ad_resampler_spill[c] = NULL;

     } else {

       srcsize = srcframes;

       src = malloc(srcsize * sizeof(uint16_t));

       for(i = 0; i < srcframes; i++)
	 src[i] = srcmix[i * channels + c];

     }

     written = av_resample(ad->ad_resampler, dst, src, &consumed, 
			   srcsize, dstavail, c == channels - 1);

     if(consumed != srcsize) {
       ad->ad_resampler_spill_size = srcsize - consumed;

       ad->ad_resampler_spill[c] = 
	 malloc(ad->ad_resampler_spill_size * sizeof(uint16_t));

       memcpy(ad->ad_resampler_spill[c], src + consumed, 
	      ad->ad_resampler_spill_size * sizeof(uint16_t));
     }

     for(i = 0; i < written; i++)
       dstmix[i * channels + c] = dst[i];

     free(src);
   }

   *writtenp = written;

   free(dst);

   return srcframes;
 }


