/*
 *  Functions for probing file contents
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

#include "config.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#if ENABLE_LIBGME
#include <gme/gme.h>
#endif

#include <libavutil/avstring.h>

#include "showtime.h"
#include "fileaccess.h"
#include "fa_probe.h"
#include "navigator.h"
#include "api/lastfm.h"
#include "media.h"
#include "misc/string.h"
#include "misc/isolang.h"
#include "misc/jpeg.h"


#define METADATA_HASH_SIZE 101
#define METADATA_CACHE_SIZE 1000

LIST_HEAD(metadata_list, metadata);
TAILQ_HEAD(metadata_queue, metadata);

static struct metadata_queue metadata_entries;
static int metadata_nentries;
static struct metadata_list metadata_hash[METADATA_HASH_SIZE];
static hts_mutex_t metadata_mutex;

TAILQ_HEAD(metadata_stream_queue, metadata_stream);

typedef struct metadata_stream {
  TAILQ_ENTRY(metadata_stream) ms_link;

  int ms_streamindex;

  rstr_t *ms_info;
  rstr_t *ms_language;

  AVCodec *ms_codec;
  enum CodecType ms_type;

} metadata_stream_t;

/**
 *
 */
typedef struct metadata {
  char *md_url;
  time_t md_mtime;

  char *md_redirect;

  LIST_ENTRY(metadata) md_hash_link;
  TAILQ_ENTRY(metadata) md_queue_link;

  int md_type;
  float md_duration;
  int md_tracks;
  time_t md_time;

  rstr_t *md_title;
  rstr_t *md_album;
  rstr_t *md_artist;
  rstr_t *md_format;

  struct metadata_stream_queue md_streams;

} metadata_t;


/**
 *
 */
static void
metadata_clean(metadata_t *md)
{
  metadata_stream_t *ms;
  rstr_release(md->md_title);
  rstr_release(md->md_album);
  rstr_release(md->md_artist);
  rstr_release(md->md_format);

  free(md->md_url);
  free(md->md_redirect);

  while((ms = TAILQ_FIRST(&md->md_streams)) != NULL) {
    TAILQ_REMOVE(&md->md_streams, ms, ms_link);
    rstr_release(ms->ms_info);
    rstr_release(ms->ms_language);
    free(ms);
  }
}


/**
 *
 */
static void
metadata_add_stream(metadata_t *md, AVCodec *codec, enum CodecType type,
		    int streamindex, const char *info, const char *language)
{
  metadata_stream_t *ms = malloc(sizeof(metadata_stream_t));
  ms->ms_info = rstr_alloc(info);
  ms->ms_language = rstr_alloc(language);

  ms->ms_codec = codec;
  ms->ms_type = type;
  ms->ms_streamindex = streamindex;
  TAILQ_INSERT_TAIL(&md->md_streams, ms, ms_link);
}


/**
 *
 */
static void
metadata_destroy(metadata_t *md)
{
  metadata_clean(md);
  TAILQ_REMOVE(&metadata_entries, md, md_queue_link);
  LIST_REMOVE(md, md_hash_link);
  free(md);
}

static const char *
codecname(AVCodec *codec)
{
  switch(codec->id) {
  case CODEC_ID_AC3:
    return "AC3";
  case CODEC_ID_EAC3:
    return "EAC3";
  case CODEC_ID_DTS:
    return "DTS";
  default:
    return codec->name;
  }
}




/**
 *
 */
static void
metadata_stream_make_prop(metadata_stream_t *ms, prop_t *parent)
{
  prop_t *p, *r = prop_create_check(parent, NULL);
  if(r == NULL)
    return;


  if((p = prop_create_check(r, "id")) != NULL) {
    prop_set_int(p, ms->ms_streamindex);
    prop_ref_dec(p);
  }

  if(ms->ms_codec != NULL && (p = prop_create_check(r, "format")) != NULL) {
    prop_set_string(p, codecname(ms->ms_codec));
    prop_ref_dec(p);
  }

  if((p = prop_create_check(r, "longformat")) != NULL) {
    prop_set_rstring(p, ms->ms_info);
    prop_ref_dec(p);
  }

  if(ms->ms_language && (p = prop_create_check(r, "language")) != NULL) {
    prop_set_rstring(p, ms->ms_language);
    prop_ref_dec(p);
  }

  if((p = prop_create_check(r, "title")) != NULL) {
    if(ms->ms_language)
      prop_set_rstring(p, ms->ms_language);
    else
      prop_set_stringf(p, "Stream %d", ms->ms_streamindex);
    prop_ref_dec(p);
  }

  prop_ref_dec(r);
}


static const uint8_t pngsig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
static const uint8_t isosig[8] = {0x1, 0x43, 0x44, 0x30, 0x30, 0x31, 0x1, 0x0};
static const uint8_t gifsig[6] = {'G', 'I', 'F', '8', '9', 'a'};


/**
 *
 */
static rstr_t *
ffmpeg_metadata_get(AVMetadata *m, const char *key)
{
  AVMetadataTag *tag;
  int len;
  rstr_t *ret;
  const char *str;
  char *d;

  if((tag = av_metadata_get(m, key, NULL, AV_METADATA_IGNORE_SUFFIX)) == NULL)
    return NULL;

  str = tag->value;
  len = strlen(str);
  ret = rstr_allocl(str, len);
  d = rstr_data(ret);

  while(len > 0) {
    len--;
    if(d[len] <= ' ' || d[len] == '-')
      d[len] = 0;
    else
      break;
  }
  if(*d == 0 || !strncasecmp(d, "http://", 7)) {
    rstr_release(ret);
    return NULL;
  }
  return ret;
}

#if 0
/**
 * Obtain details from playlist
 */
static void
fa_probe_playlist(metadata_t *md, const char *url, uint8_t *pb, size_t pbsize)
{
  const char *t;
  char tmp1[300];
  int i;

  t = strrchr(url, '/');
  t = t ? t + 1 : url;

  i = 0;
  while(*t && *t != '.')
    tmp1[i++] = *t++;
  tmp1[i] = 0;
  
  md->md_title = rstr_alloc(tmp1);
  
  t = strstr((char *)pb, "NumberOfEntries=");
  
  if(t != NULL)
    md->md_tracks = atoi(t + 16);
}
#endif

/**
 * Probe SPC files
 */
static void
fa_probe_spc(metadata_t *md, uint8_t *pb)
{
  char buf[33];
  buf[32] = 0;

  if(memcmp("v0.30", pb + 0x1c, 4))
    return;

  if(pb[0x23] != 0x1a)
    return;

  memcpy(buf, pb + 0x2e, 32);
  md->md_title = rstr_alloc(buf);

  memcpy(buf, pb + 0x4e, 32);
  md->md_album = rstr_alloc(buf);

  memcpy(buf, pb + 0xa9, 3);
  buf[3] = 0;

  md->md_duration = atoi(buf);
}


/**
 *
 */
static void 
fa_probe_psid(metadata_t *md, uint8_t *pb)
{
  md->md_title  = rstr_alloc(utf8_from_ISO_8859_1((char *)pb + 0x16, 32));
  md->md_artist = rstr_alloc(utf8_from_ISO_8859_1((char *)pb + 0x36, 32));
}


/**
 * 
 */
static void
metdata_set_redirect(metadata_t *md, const char *fmt, ...)
{
  char buf[URL_MAX];
  va_list ap;
  va_start(ap, fmt);

  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  md->md_redirect = strdup(buf);
}


/**
 *
 */
static int
jpeginfo_reader(void *handle, void *buf, off_t offset, size_t size)
{
  if(fa_seek(handle, offset, SEEK_SET) != offset)
    return -1;
  return fa_read(handle, buf, size);
}


static void
fa_probe_exif(metadata_t *md, const char *url, uint8_t *pb, fa_handle_t *fh)
{
    jpeginfo_t ji;

   if(jpeg_info(&ji, jpeginfo_reader, fh, 
		JPEG_INFO_DIMENSIONS |
		JPEG_INFO_ORIENTATION,
		pb, 256, NULL, 0))
     return;
   
   md->md_time = ji.ji_time;
}


/**
 * Probe file by checking its header
 *
 * pb is guaranteed to point to at least 256 bytes of valid data
 */
static int
fa_probe_header(metadata_t *md, const char *url, uint8_t *pb, fa_handle_t *fh)
{
  uint16_t flags;

  if(!memcmp(pb, "SNES-SPC700 Sound File Data", 27)) {
    fa_probe_spc(md, pb); 
    md->md_type = CONTENT_AUDIO;
    return 1;
  }

  if(!memcmp(pb, "PSID", 4) || !memcmp(pb, "RSID", 4)) {
    fa_probe_psid(md, pb); 
    md->md_type = CONTENT_ALBUM;
    metdata_set_redirect(md, "sidfile://%s/", url);
    return 1;
  }

  if(pb[0] == 'R'  && pb[1] == 'a'  && pb[2] == 'r' && pb[3] == '!' &&
     pb[4] == 0x1a && pb[5] == 0x07 && pb[6] == 0x0 && pb[9] == 0x73) {

    flags = pb[10] | pb[11] << 8;
    if((flags & 0x101) == 1) {
      /* Don't include slave volumes */
      md->md_type = CONTENT_UNKNOWN;
      return 1;
    }

    metdata_set_redirect(md, "rar://%s", url);
    md->md_type = CONTENT_ARCHIVE;
    return 1;
  }

  if(pb[0] == 0x50 && pb[1] == 0x4b && pb[2] == 0x03 && pb[3] == 0x04) {
    metdata_set_redirect(md, "zip://%s", url);
    md->md_type = CONTENT_ARCHIVE;
    return 1;
  }

#if 0
  if(!strncasecmp((char *)pb, "[playlist]", 10)) {
    /* Playlist */
    fa_probe_playlist(md, url, pb, sizeof(pb));
    md->md_type = CONTENT_PLAYLIST;
    return 1;
  }
#endif

  if((pb[6] == 'J' && pb[7] == 'F' && pb[8] == 'I' && pb[9] == 'F') ||
     (pb[6] == 'E' && pb[7] == 'x' && pb[8] == 'i' && pb[9] == 'f')) {
    /* JPEG image */
    md->md_type = CONTENT_IMAGE;
    fa_probe_exif(md, url, pb, fh); // Try to get more info
    return 1;
  }

  if(!memcmp(pb, "<showtimeplaylist", strlen("<showtimeplaylist"))) {
    /* Ugly playlist thing (see fa_video.c) */
    md->md_type = CONTENT_VIDEO;
    return 1;
  }

  if(!memcmp(pb, pngsig, 8)) {
    /* PNG */
    md->md_type = CONTENT_IMAGE;
    return 1;
  }

  if(!memcmp(pb, gifsig, sizeof(gifsig))) {
    /* GIF */
    md->md_type = CONTENT_IMAGE;
    return 1;
  }

  if(pb[0] == '%' && pb[1] == 'P' && pb[2] == 'D' && pb[3] == 'F') {
    md->md_type = CONTENT_UNKNOWN;
    return 1;
  }

  return 0;
}

/**
 * Check if file is an iso image
 * pb is guaranteed to point at 128 byts
 * of data starting 0x8000 of start of file
 */
static int
fa_probe_iso0(metadata_t *md, char *pb)
{
  char *p;

  if(memcmp(pb, isosig, 8))
    return -1;

  p = &pb[40];
  while(*p > 32 && p != &pb[72])
    p++;

  *p = 0;

  if(md != NULL) {
    md->md_title = rstr_alloc(pb + 40);
    md->md_type = CONTENT_DVD;
  }
  return 0;
}



/**
 * Check if file is an iso image
 * pb is guaranteed to point at 64k of data
 */
int
fa_probe_iso(metadata_t *md, fa_handle_t *fh)
{
  char pb[128];

  if(fa_seek(fh, 0x8000, SEEK_SET) != 0x8000)
    return -1;

  if(fa_read(fh, pb, sizeof(pb)) != sizeof(pb))
    return -1;
  return fa_probe_iso0(md, pb);
}


#if ENABLE_LIBGME
/**
 *
 */
static int
gme_probe(metadata_t *md, AVProbeData *pd, fa_handle_t *fh, struct fa_stat *fs)
{
  const char *type = gme_identify_header(pd->buf);
  struct fa_stat fs0;
  char *buf;
  gme_err_t err;
  Music_Emu *emu;
  gme_info_t *info;
  int tracks;

  if(*type == 0)
    return 0;

  if(fs == NULL) {
    if(fa_stat(pd->filename, &fs0, NULL, 0))
      return 0;
    fs = &fs0;
  }

  buf = malloc(fs->fs_size);
  memcpy(buf, pd->buf, pd->buf_size);
  int r = fa_read(fh, buf + pd->buf_size, fs->fs_size - pd->buf_size);
  if(r != fs->fs_size - pd->buf_size) {
    free(buf);
    return 0;
  }

  err = gme_open_data(buf, fs->fs_size, &emu, gme_info_only);
  free(buf);
  if(err != NULL)
    return 0;

  err = gme_track_info(emu, &info, 0);
  if(err != NULL) {
    gme_delete(emu);
    return 0;
  }

  tracks = gme_track_count(emu);

#if 0
  printf("tracks   : %d\n", tracks);
  printf("system   : %s\n", info->system);
  printf("game     : %s\n", info->game);
  printf("song     : %s\n", info->song);
  printf("author   : %s\n", info->author);
  printf("copyright: %s\n", info->copyright);
  printf("comment  : %s\n", info->comment);
  printf("dumper   : %s\n", info->dumper);
#endif

  if(tracks == 1) {

    md->md_title  = info->song[0]   ? rstr_alloc(info->song)   : NULL;
    md->md_album  = info->game[0]   ? rstr_alloc(info->game)   : NULL;
    md->md_artist = info->author[0] ? rstr_alloc(info->author) : NULL;

    md->md_duration = info->play_length / 1000.0;
    md->md_type = CONTENT_AUDIO;

  } else {

    md->md_title  = info->game[0] ? rstr_alloc(info->game)   : NULL;
    md->md_artist = info->author[0] ? rstr_alloc(info->author) : NULL;

    md->md_type = CONTENT_ALBUM;
    metdata_set_redirect(md, "gmefile://%s/", pd->filename);
  }

  gme_free_info(info);
  gme_delete(emu);
  return 1;
}
#endif


/**
 * 
 */
static void
fa_lavf_load_meta(metadata_t *md, AVFormatContext *fctx, const char *url)
{
  int i;
  char tmp1[1024];
  int has_video = 0;
  int has_audio = 0;

  av_metadata_conv(fctx, NULL, fctx->iformat->metadata_conv);

  /* Format meta info */
  
  if(fctx->title[0] == 0) {
    fa_url_get_last_component(tmp1, sizeof(tmp1), url);

    // Strip .xxx ending in filenames
    i = strlen(tmp1);
    if(i > 4 && tmp1[i - 4] == '.')
      tmp1[i - 4] = 0;

    http_deescape(tmp1);
    md->md_title = rstr_alloc(tmp1);
  } else {
    md->md_title = ffmpeg_metadata_get(fctx->metadata, "title");
  }

  md->md_artist = ffmpeg_metadata_get(fctx->metadata, "artist") ?:
    ffmpeg_metadata_get(fctx->metadata, "author");

  md->md_album = ffmpeg_metadata_get(fctx->metadata, "album");

  md->md_format = rstr_alloc(fctx->iformat->long_name);

  if(fctx->duration != AV_NOPTS_VALUE)
    md->md_duration = (float)fctx->duration / 1000000;

  /* Check each stream */

  for(i = 0; i < fctx->nb_streams; i++) {
    AVStream *stream = fctx->streams[i];
    AVCodecContext *avctx = stream->codec;
    AVCodec *codec = avcodec_find_decoder(avctx->codec_id);

    switch(avctx->codec_type) {
    case CODEC_TYPE_VIDEO:
      has_video = !!codec;
      break;
    case CODEC_TYPE_AUDIO:
      has_audio = !!codec;
      break;
    case CODEC_TYPE_SUBTITLE:
      break;

    default:
      continue;
    }

    if(codec == NULL) {

      switch(avctx->codec_id) {
      case CODEC_ID_TEXT:
	snprintf(tmp1, sizeof(tmp1), "Text");
	break;
      default:
	snprintf(tmp1, sizeof(tmp1),
		 "Unsupported codec (0x%x)", avctx->codec_id);
	break;
      }
    } else {
      metadata_from_ffmpeg(tmp1, sizeof(tmp1), codec, avctx);
    }

    metadata_add_stream(md, codec, avctx->codec_type, i, tmp1, 
			isolang_iso2lang(stream->language));
  }
  
  md->md_type = CONTENT_FILE;
  if(has_video)
    md->md_type = CONTENT_VIDEO;
  else if(has_audio)
    md->md_type = CONTENT_AUDIO;
}
  

#define PROBE1_SIZE 4096
#define PROBE2_SIZE 65536

/**
 *
 */
static int
fa_probe_fill_cache(metadata_t *md, const char *url, char *errbuf, 
		    size_t errsize, struct fa_stat *fs)
{
  const char *url0 = url;
  AVInputFormat *f;
  AVFormatContext *fctx;
  fa_handle_t *fh;
  int score = 0;
  AVProbeData pd;
  ByteIOContext *s;

  if((fh = fa_open(url, errbuf, errsize)) == NULL)
    return -1;

  pd.filename = url;
  pd.buf = malloc(PROBE1_SIZE + AVPROBE_PADDING_SIZE);

  pd.buf_size = fa_read(fh, pd.buf, PROBE1_SIZE);
  
  if(pd.buf_size < 256) {
    snprintf(errbuf, errsize, "Short file");
    free(pd.buf);
    return -1;
  }

  if(pd.buf[0] == 'I' &&
     pd.buf[1] == 'D' &&
     pd.buf[2] == '3' && 
     (pd.buf[3] & 0xf8) == 0 &&
     (pd.buf[5] & 0x0f) == 0) {
    f = av_find_input_format("mp3");
    if(f != NULL) {
      free(pd.buf);
      goto found;
    }
  }

#if ENABLE_LIBGME
  if(gme_probe(md, &pd, fh, fs)) {
    fa_close(fh);
    free(pd.buf);
    return 0;
  }
#endif

  if(fa_probe_header(md, url0, pd.buf, fh)) {
    fa_close(fh);
    free(pd.buf);
    return 0;
  }

  // First try using lavc

  f = av_probe_input_format2(&pd, 1, &score);
  
  // If score is low and it is possible, read more
  if(pd.buf_size == PROBE1_SIZE && score < AVPROBE_SCORE_MAX / 2) {
    
    pd.buf = realloc(pd.buf, PROBE2_SIZE + AVPROBE_PADDING_SIZE);
    pd.buf_size += fa_read(fh, pd.buf + PROBE1_SIZE, PROBE2_SIZE - PROBE1_SIZE);
    if(pd.buf_size == PROBE2_SIZE) {
      if(!fa_probe_iso0(md, (char *)pd.buf + 0x8000)) {
	free(pd.buf);
	fa_close(fh);
	return 0;
      }
    }

    f = av_probe_input_format2(&pd, 1, &score);
  }

  free(pd.buf);
	
  if(f == NULL) {
    snprintf(errbuf, errsize, "Unable to probe file (FFmpeg)");
    fa_close(fh);
    return -1;
  }
 found:
  if(fa_lavf_reopen(&s, fh)) {
    snprintf(errbuf, errsize, "Unable to reopen file (FFmpeg)");
    return -1;
  }

  if(av_open_input_stream(&fctx, s, url0, f, NULL) != 0) {
    snprintf(errbuf, errsize, "Unable to open stream (FFmpeg)");
    url_fclose(s);
    return -1;
  }

  if(av_find_stream_info(fctx) < 0) {
    av_close_input_file(fctx);
    snprintf(errbuf, errsize, "Unable to handle file contents");
    return -1;
  }

  fa_lavf_load_meta(md, fctx, url);
  av_close_input_file(fctx);  
  return 0;
}


/**
 *
 */
static int
fa_probe_set_from_cache(const metadata_t *md, prop_t *proproot, 
			char *newurl, size_t newurlsize)
{
  metadata_stream_t *ms;
  prop_t *p;

  if(md->md_redirect != NULL && newurl != NULL)
    av_strlcpy(newurl, md->md_redirect, newurlsize);

  if(md->md_title && (p = prop_create_check(proproot, "title")) != NULL) {
    prop_set_rstring(p, md->md_title);
    prop_ref_dec(p);
  }

  if(md->md_artist) {
    if((p = prop_create_check(proproot, "artist")) != NULL) {
      prop_set_rstring(p, md->md_artist);
      prop_ref_dec(p);
    }

    if((p = prop_create_check(proproot, "artist_images")) != NULL) {
      lastfm_artistpics_init(p, md->md_artist);
      prop_ref_dec(p);
    }
  }

  if(md->md_album) {
    if((p = prop_create_check(proproot, "album")) != NULL) {
      prop_set_rstring(p,  md->md_album);
      prop_ref_dec(p);
    }
    
    if(md->md_artist != NULL &&
       (p = prop_create_check(proproot, "album_art")) != NULL) {
      lastfm_albumart_init(p, md->md_artist, md->md_album);
      prop_ref_dec(p);
    }
  }

  TAILQ_FOREACH(ms, &md->md_streams, ms_link) {

    prop_t *p;

    switch(ms->ms_type) {
    case CODEC_TYPE_AUDIO:
      p = prop_create_check(proproot, "audiostreams");
      break;
    case CODEC_TYPE_VIDEO:
      p = prop_create_check(proproot, "videostreams");
      break;
    case CODEC_TYPE_SUBTITLE:
      p = prop_create_check(proproot, "subtitlestreams");
      break;
    default:
      continue;
    }
    if(p != NULL) {
      metadata_stream_make_prop(ms, p);
      prop_ref_dec(p);
    }
  }

  if(md->md_format && (p = prop_create_check(proproot, "format")) != NULL) {
    prop_set_rstring(p,  md->md_format);
    prop_ref_dec(p);
  }

  if(md->md_duration && (p = prop_create_check(proproot, "duration")) != NULL) {
    prop_set_float(p, md->md_duration);
    prop_ref_dec(p);
  }

  if(md->md_tracks && (p = prop_create_check(proproot, "tracks")) != NULL) {
    prop_set_int(p,  md->md_tracks);
    prop_ref_dec(p);
  }

  if(md->md_time && (p = prop_create_check(proproot, "timestamp")) != NULL) {
    prop_set_int(p,  md->md_time);
    prop_ref_dec(p);
  }

  return md->md_type;
}




/**
 * Probe a file for its type
 */
unsigned int
fa_probe(prop_t *proproot, const char *url, char *newurl, size_t newurlsize,
	 char *errbuf, size_t errsize, struct fa_stat *fs)
{
  struct fa_stat fs0;
  unsigned int hash, r;
  metadata_t *md;

  if(fs == NULL) {
    if(fa_stat(url, &fs0, errbuf, errsize))
      return CONTENT_UNKNOWN;
    fs = &fs0;
  }

  hash = mystrhash(url) % METADATA_HASH_SIZE;

  hts_mutex_lock(&metadata_mutex);
  
  LIST_FOREACH(md, &metadata_hash[hash], md_hash_link)
    if(md->md_mtime == fs->fs_mtime && !strcmp(md->md_url, url))
      break;

  if(md != NULL) {
    TAILQ_REMOVE(&metadata_entries, md, md_queue_link);
    TAILQ_INSERT_TAIL(&metadata_entries, md, md_queue_link);

  } else {
    if(metadata_nentries == METADATA_CACHE_SIZE) {
      md = TAILQ_FIRST(&metadata_entries);
      metadata_destroy(md);
    }

    md = calloc(1, sizeof(metadata_t));
    TAILQ_INIT(&md->md_streams);
    LIST_INSERT_HEAD(&metadata_hash[hash], md, md_hash_link);
    TAILQ_INSERT_TAIL(&metadata_entries, md, md_queue_link);
    md->md_mtime = fs->fs_mtime;
    md->md_url = strdup(url);

    if(fa_probe_fill_cache(md, url, errbuf, errsize, fs)) {
      metadata_destroy(md);
      hts_mutex_unlock(&metadata_mutex);
      return CONTENT_UNKNOWN;
    }
  }

  r = fa_probe_set_from_cache(md, proproot, newurl, newurlsize);

  hts_mutex_unlock(&metadata_mutex);
  return r;
}


/**
 *
 */
void
fa_probe_load_metaprop(prop_t *p, AVFormatContext *fctx, const char *url)
{
  metadata_t md = {0};
  TAILQ_INIT(&md.md_streams);

  fa_lavf_load_meta(&md, fctx, url);
  fa_probe_set_from_cache(&md, p, NULL, 0);
  metadata_clean(&md);
}

/**
 * Probe a directory
 */
unsigned int
fa_probe_dir(prop_t *proproot, const char *url)
{
  char path[URL_MAX];
  struct fa_stat fs;
  int type;

  type = CONTENT_DIR;

  fa_pathjoin(path, sizeof(path), url, "VIDEO_TS");
  if(fa_stat(path, &fs, NULL, 0) == 0 && fs.fs_type == CONTENT_DIR) {
    type = CONTENT_DVD;
  } else {
    fa_pathjoin(path, sizeof(path), url, "video_ts");
    if(fa_stat(path, &fs, NULL, 0) == 0 && fs.fs_type == CONTENT_DIR)
      type = CONTENT_DVD;
  }

  return type;
}


/**
 *
 */
void
fa_probe_init(void)
{
  TAILQ_INIT(&metadata_entries);
  hts_mutex_init(&metadata_mutex);
}
