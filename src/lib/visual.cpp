#include <cmath>
#include <cstring>
#include "version.h"

#ifdef USE_FFMPEG
extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/version.h>
#include <libavutil/imgutils.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
#include <libswscale/version.h>
#include <libavformat/version.h>
#include <libavformat/avformat.h>
#else
#ifdef USE_OIIO
#include <OpenImageIO/filter.h>
#include <OpenImageIO/version.h>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#endif
#endif
#include "internal.h"

#ifdef USE_FFMPEG
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVCodec;
struct AVCodecParameters;
struct AVPacket;
#endif

typedef struct ncvisual {
  int packet_outstanding;
  int dstwidth, dstheight;
  int stream_index;        // match against this following av_read_frame()
  int sub_stream_index;    // subtitle stream index, can be < 0 if no subtitles
  float timescale;         // scale frame duration by this value
  ncplane* ncp;
  char* filename;
  // if we're creating the plane based off the first frame's dimensions, these
  // describe where the plane ought be placed, and how it ought be sized. this
  // path sets ncobj. ncvisual_destroy() ought in that case kill the ncplane.
  int placex, placey;
  // ffmpeg sometimes pads lines. this many true bytes per row in data.
  int rowstride;
  ncscale_e style;         // none, scale, or stretch
  uint64_t framenum;
  struct notcurses* ncobj; // set iff this ncvisual "owns" its ncplane
#ifdef USE_FFMPEG
  struct AVFormatContext* fmtctx;
  struct AVCodecContext* codecctx;       // video codec context
  struct AVCodecContext* subtcodecctx;   // subtitle codec context
  struct AVFrame* frame;
  struct AVFrame* oframe;
  struct AVCodec* codec;
  struct AVCodecParameters* cparams;
  struct AVCodec* subtcodec;
  struct AVPacket* packet;
  struct SwsContext* swsctx;
  AVSubtitle subtitle;
#else
#ifdef USE_OIIO
  std::unique_ptr<OIIO::ImageInput> image;  // must be close()d
  std::unique_ptr<OIIO::ImageBuf> raw;
  std::unique_ptr<uint32_t[]> frame;
  OIIO::ImageBuf scaled;
#endif
#endif
  uint32_t* data;          // (scaled) RGBA image data, rowstride bytes per row
  bool owndata;            // we own data iff owndata == true
  int encode_scale;        // 2 iff notcurses_canutf8(), 1 otherwise
} ncvisual;

// returns 2 if utf-8 half-blocks are in play, 1 otherwise
static int
encoding_vert_scale(const ncvisual* nc){
  return nc->encode_scale;
}

static void
set_encoding_vert_scale(const notcurses* nc, ncvisual* ncv){
  ncv->encode_scale = 1 + nc->utf8;
}

static void
ncvisual_set_data(ncvisual* ncv, uint32_t* data, bool owned){
  if(ncv->owndata){
    free(ncv->data);
  }
  ncv->data = data;
  ncv->owndata = owned;
}

auto ncvisual_create(float timescale) -> ncvisual* {
  auto ret = new ncvisual{};
  if(ret == nullptr){
    return nullptr;
  }
  ret->timescale = timescale;
  return ret;
}

auto ncvisual_from_plane(const ncplane* n, int begy, int begx, int leny, int lenx)
                         -> ncvisual* {
  uint32_t* rgba = ncplane_rgba(n, begx, begy, leny, lenx);
  if(rgba == nullptr){
    return nullptr;
  }
  int dimy, dimx;
  ncplane_dim_yx(n, &dimy, &dimx);
  // FIXME needs to make use of begy, begx, leny, lenx!
  auto* ncv = ncvisual_from_rgba(n->nc, rgba, n->leny, n->lenx * 4, n->lenx);
  if(ncv == nullptr){
    free(rgba);
    return nullptr;
  }
  ncplane_destroy(ncv->ncp);
  ncv->ncp = ncplane_dup(n, nullptr);
  ncv->ncobj = n->nc;
  return ncv;
}

auto bgra_to_rgba(const void* data, int rows, int rowstride, int cols) -> void* {
  if(rowstride % 4){ // must be a multiple of 4 bytes
    return nullptr;
  }
//fprintf(stderr, "ROWS: %d\n", rows);
  auto ret = static_cast<uint32_t*>(malloc(rowstride * rows));
  if(ret){
    for(int y = 0 ; y < rows ; ++y){
      for(int x = 0 ; x < cols ; ++x){
        const uint32_t* src = (const uint32_t*)data + (rowstride / 4) * y + x;
        uint32_t* dst = ret + (rowstride / 4) * y + x;
//fprintf(stderr, "SRC: %x DST: %x\n", *src, *dst);
        *dst = ((*src & 0xff00u) << 16u) | (*src & 0xff00ffu) | ((*src & 0xff000000) >> 16u);
//fprintf(stderr, "y: %d x: %d SRC: %x DST: %x\n", y, x, *src, *dst);
      }
    }
  }
  return ret;
}

auto ncvisual_setplane(ncvisual* ncv, ncplane* n) -> int {
  int ret = 0;
  if(n != ncv->ncp){
    if(ncv->ncp){
      ret |= ncplane_destroy(ncv->ncp);
    }
    ncv->ncp = n;
  }
  return ret;
}

// if we're rotating around our center, we can't require any radius greater
// than our longer length. rotation can thus be held entirely within a square
// plane having length of our longest length. after one rotation, this decays
// to the same square throughout any rotations.
static auto
rotate_new_geom(ncvisual* ncv, double rads, double *stheta, double *ctheta) -> int {
  *stheta = sin(rads);
  *ctheta = cos(rads);
  const int scaledy = ncv->ncp->leny * encoding_vert_scale(ncv);
  const int diam = scaledy < ncv->ncp->lenx ? ncv->ncp->lenx : scaledy;
//fprintf(stderr, "rotating %d -> %d / %d\n", ncv->ncp->leny, scaledy, ncv->ncp->lenx);
  if(ncv->ncp->lenx != scaledy){
    if(ncplane_resize_simple(ncv->ncp, diam / encoding_vert_scale(ncv), diam) < 0){
      return -1;
    }
  }
  return diam;
}

auto ncvisual_rotate(ncvisual* ncv, double rads) -> int {
  if(ncv->data == nullptr){
    return -1;
  }
  double stheta, ctheta; // sine, cosine
  auto diam = rotate_new_geom(ncv, rads, &stheta, &ctheta);
  if(diam <= 0){
    return -1;
  }
  ncplane* n = ncvisual_plane(ncv);
  ncplane* newp = rotate_plane(n); // FIXME how to resize properly?
  if(newp == nullptr){
    return -1;
  }
  // pixel diameter
  int pdiam = ncv->dstheight > ncv->dstwidth ? ncv->dstheight : ncv->dstwidth;
//fprintf(stderr, "stride: %d height: %d width: %d\n", ncv->rowstride, ncv->dstheight, ncv->dstwidth);
  assert(ncv->rowstride / 4 >= ncv->dstwidth);
  auto data = static_cast<uint32_t*>(malloc(pdiam * pdiam * 4));
  if(data == nullptr){
    ncplane_destroy(newp);
    return -1;
  }
  // targy <- x, targx <- ncv->dstheight - y - 1
  int centx = ncv->dstwidth / 2; // pixel center
  int centy = ncv->dstheight / 2;
//fprintf(stderr, "DIAM: %d CENTER: %d/%d LEN: %d/%d\n", diam, centy, centx, ncv->ncp->leny, ncv->ncp->lenx);
  for(int y = 0 ; y < ncv->dstheight ; ++y){
      for(int x = 0 ; x < ncv->dstwidth ; ++x){
      const int convy = y - centy; // converted coordinates
      const int convx = x - centx;
      const int targy = convx * stheta + convy * ctheta;
      const int targx = convx * ctheta + convy * stheta;
      const int deconvy = targy + pdiam / 2;
      const int deconvx = targx + pdiam / 2;
//fprintf(stderr, "%d/%d -> %d/%d -> %d/%d -> %d/%d\n", y, x, convy, convx, targy, targx, deconvy, deconvx);
assert(deconvy >= 0);
assert(deconvx >= 0);
assert(deconvy < pdiam);
assert(deconvx < pdiam);
      data[deconvy * pdiam + deconvx] = ncv->data[y * (ncv->rowstride / 4) + x];
//fprintf(stderr, "CW: %d/%d (%08x) -> %d/%d (stride: %d)\n", y, x, ncv->data[y * (ncv->rowstride / 4) + x], targy, targx, ncv->rowstride);
//      data[targy * ncv->dstheight + targx] = ncv->data[y * (ncv->rowstride / 4) + x];
//fprintf(stderr, "wrote %08x to %d (%d)\n", data[targy * ncv->dstheight + targx], targy * ncv->dstheight + targx, (targy * ncv->dstheight + targx) * 4);
    }
  }
  int ret = ncplane_destroy(n);
  ncvisual_set_data(ncv, data, true);
  ncv->dstwidth = pdiam;
  ncv->dstheight = pdiam;
  ncv->rowstride = ncv->dstwidth * 4;
  ncv->ncp = newp;
  return ret;
}

auto ncvisual_from_rgba(notcurses* nc, const void* rgba, int rows,
                        int rowstride, int cols) -> ncvisual* {
  if(rowstride % 4){
    return nullptr;
  }
  ncvisual* ncv = ncvisual_create(1);
  set_encoding_vert_scale(nc, ncv);
//fprintf(stderr, "ROWS: %d STRIDE: %d (%d) COLS: %d\n", rows, rowstride, rowstride / 4, cols);
  ncv->rowstride = rowstride;
  ncv->ncobj = nc;
  ncv->dstwidth = cols;
  ncv->dstheight = rows;
  int disprows;
  if(nc->utf8){
    disprows = ncv->dstheight / 2 + ncv->dstheight % 2;
  }else{
    disprows = ncv->dstheight;
  }
//fprintf(stderr, "MADE INITIAL ONE %d/%d\n", disprows, ncv->dstwidth);
  ncv->ncp = ncplane_new(nc, disprows, ncv->dstwidth, 0, 0, nullptr);
  if(ncv->ncp == nullptr){
    ncvisual_destroy(ncv);
    return nullptr;
  }
  auto data = static_cast<uint32_t*>(memdup(rgba, rowstride * ncv->dstheight));
  if(data == nullptr){
    ncvisual_destroy(ncv);
    return nullptr;
  }
  ncvisual_set_data(ncv, data, true);
  return ncv;
}

auto ncvisual_from_bgra(notcurses* nc, const void* bgra, int rows,
                        int rowstride, int cols) -> ncvisual* {
  if(rowstride % 4){
    return nullptr;
  }
  ncvisual* ncv = ncvisual_create(1);
  ncv->rowstride = rowstride;
  ncv->ncobj = nc;
  ncv->dstwidth = cols;
  ncv->dstheight = rows;
  int disprows;
  if(nc->utf8){
    disprows = ncv->dstheight / 2 + ncv->dstheight % 2;
  }else{
    disprows = ncv->dstheight;
  }
  ncv->ncp = ncplane_new(nc, disprows, ncv->dstwidth, 0, 0, nullptr);
  if(ncv->ncp == nullptr){
    ncvisual_destroy(ncv);
    return nullptr;
  }
  auto data = static_cast<uint32_t*>(memdup(bgra, rowstride * ncv->dstheight));
  if(data == nullptr){
    ncvisual_destroy(ncv);
    return nullptr;
  }
  ncvisual_set_data(ncv, data, true);
  return ncv;
}

auto ncvisual_render(const ncvisual* ncv, int begy, int begx, int leny, int lenx) -> int {
//fprintf(stderr, "render %dx%d+%dx%d\n", begy, begx, leny, lenx);
  if(begy < 0 || begx < 0 || lenx < -1 || leny < -1){
    return -1;
  }
  if(ncv->data == nullptr){
    return -1;
  }
//fprintf(stderr, "render %d/%d to %dx%d+%dx%d\n", ncv->dstheight, ncv->dstwidth, begy, begx, leny, lenx);
  if(begx >= ncv->dstwidth || begy >= ncv->dstheight){
    return -1;
  }
  if(lenx == -1){ // -1 means "to the end"; use all space available
    lenx = ncv->dstwidth - begx;
  }
  if(leny == -1){
    leny = ncv->dstheight - begy;
  }
  if(lenx < 0 || leny < 0){ // no need to draw zero-size object, exit
    return 0;
  }
  if(begx + lenx > ncv->dstwidth || begy + leny > ncv->dstheight){
    return -1;
  }
  int dimy, dimx;
  ncplane_dim_yx(ncv->ncp, &dimy, &dimx);
  ncplane_cursor_move_yx(ncv->ncp, 0, 0);
  // y and x are actual plane coordinates. each row corresponds to two rows of
  // the input (scaled) frame (columns are 1:1). we track the row of the
  // visual via visy.
//fprintf(stderr, "render: %dx%d:%d+%d of %d/%d -> %dx%d\n", begy, begx, leny, lenx, ncv->dstheight, ncv->dstwidth, dimy, dimx);
  int ret = ncblit_rgba(ncv->ncp, ncv->placey, ncv->placex, ncv->rowstride,
                        ncv->data, begy, begx, leny, lenx);
  //av_frame_unref(ncv->oframe);
  return ret;
}

// free common ncv material, after any engine-specific resource deallocation
static void
ncvisual_destroy_common(ncvisual* ncv){
  if(ncv->owndata){
    free(ncv->data);
  }
  free(ncv->filename);
  if(ncv->ncobj && ncv->ncp){
    ncplane_destroy(ncv->ncp);
  }
  delete ncv;
}

ncplane* ncvisual_plane(ncvisual* ncv){
  return ncv->ncp;
}

#ifdef USE_FFMPEG
void ncvisual_destroy(ncvisual* ncv){
  if(ncv){
    avcodec_close(ncv->codecctx);
    avcodec_free_context(&ncv->codecctx);
    av_frame_free(&ncv->frame);
    av_freep(&ncv->oframe);
    //avcodec_parameters_free(&ncv->cparams);
    sws_freeContext(ncv->swsctx);
    av_packet_free(&ncv->packet);
    avformat_close_input(&ncv->fmtctx);
    avsubtitle_free(&ncv->subtitle);
    ncvisual_destroy_common(ncv);
  }
}

bool notcurses_canopen_images(const notcurses* nc __attribute__ ((unused))){
  return true;
}

bool notcurses_canopen_videos(const notcurses* nc __attribute__ ((unused))){
  return true;
}

/*static void
print_frame_summary(const AVCodecContext* cctx, const AVFrame* f){
  char pfmt[128];
  av_get_pix_fmt_string(pfmt, sizeof(pfmt), static_cast<enum AVPixelFormat>(f->format));
  fprintf(stderr, "Frame %05d (%d? %d?) %dx%d pfmt %d (%s)\n",
          cctx->frame_number,
          f->coded_picture_number,
          f->display_picture_number,
          f->width, f->height,
          f->format, pfmt);
  fprintf(stderr, " Data (%d):", AV_NUM_DATA_POINTERS);
  int i;
  for(i = 0 ; i < AV_NUM_DATA_POINTERS ; ++i){
    fprintf(stderr, " %p", f->data[i]);
  }
  fprintf(stderr, "\n Linesizes:");
  for(i = 0 ; i < AV_NUM_DATA_POINTERS ; ++i){
    fprintf(stderr, " %d", f->linesize[i]);
  }
  if(f->sample_aspect_ratio.num == 0 && f->sample_aspect_ratio.den == 1){
    fprintf(stderr, "\n Aspect ratio unknown");
  }else{
    fprintf(stderr, "\n Aspect ratio %d:%d", f->sample_aspect_ratio.num, f->sample_aspect_ratio.den);
  }
  if(f->interlaced_frame){
    fprintf(stderr, " [ILaced]");
  }
  if(f->palette_has_changed){
    fprintf(stderr, " [NewPal]");
  }
  fprintf(stderr, " PTS %ld Flags: 0x%04x\n", f->pts, f->flags);
  fprintf(stderr, " %lums@%lums (%skeyframe) qual: %d\n",
          f->pkt_duration, // FIXME in 'time_base' units
          f->best_effort_timestamp,
          f->key_frame ? "" : "non-",
          f->quality);
}*/

static char*
deass(const char* ass){
  // SSA/ASS formats:
  // Dialogue: Marked=0,0:02:40.65,0:02:41.79,Wolf main,Cher,0000,0000,0000,,Et les enregistrements de ses ondes delta ?
  // FIXME more
  if(strncmp(ass, "Dialogue:", strlen("Dialogue:"))){
    return nullptr;
  }
  const char* delim = strchr(ass, ',');
  int commas = 0; // we want 8
  while(delim && commas < 8){
    delim = strchr(delim + 1, ',');
    ++commas;
  }
  if(!delim){
    return nullptr;
  }
  // handle ASS syntax...\i0, \b0, etc.
  char* dup = strdup(delim + 1);
  char* c = dup;
  while(*c){
    if(*c == '\\'){
      *c = ' ';
      ++c;
      if(*c){
        *c = ' ';;
      }
    }
    ++c;
  }
  return dup;
}

auto ncvisual_subtitle(const ncvisual* ncv) -> char* {
  for(unsigned i = 0 ; i < ncv->subtitle.num_rects ; ++i){
    const AVSubtitleRect* rect = ncv->subtitle.rects[i];
    if(rect->type == SUBTITLE_ASS){
      return deass(rect->ass);
    }else if(rect->type == SUBTITLE_TEXT) {;
      return strdup(rect->text);
    }
  }
  return nullptr;
}

static nc_err_e
averr2ncerr(int averr){
  if(averr == AVERROR_EOF){
    return NCERR_EOF;
  }
  // FIXME need to map averror codes to ncerrors
//fprintf(stderr, "AVERR: %d/%x %d/%x\n", averr, averr, -averr, -averr);
  return NCERR_DECODE;
}

nc_err_e ncvisual_decode(ncvisual* nc){
  if(nc->fmtctx == nullptr){ // not a file-backed ncvisual
    return NCERR_DECODE;
  }
  bool have_frame = false;
  bool unref = false;
  // FIXME what if this was set up with e.g. ncvisual_from_rgba()?
  av_freep(&nc->oframe->data[0]);
  do{
    do{
      if(nc->packet_outstanding){
        break;
      }
      if(unref){
        av_packet_unref(nc->packet);
      }
      int averr;
      if((averr = av_read_frame(nc->fmtctx, nc->packet)) < 0){
        /*if(averr != AVERROR_EOF){
          fprintf(stderr, "Error reading frame info (%s)\n", av_err2str(*averr));
        }*/
        return averr2ncerr(averr);
      }
      unref = true;
      if(nc->packet->stream_index == nc->sub_stream_index){
        int result = 0, ret;
        ret = avcodec_decode_subtitle2(nc->subtcodecctx, &nc->subtitle, &result, nc->packet);
        if(ret >= 0 && result){
          // FIXME?
        }
      }
    }while(nc->packet->stream_index != nc->stream_index);
    ++nc->packet_outstanding;
    if(avcodec_send_packet(nc->codecctx, nc->packet) < 0){
      //fprintf(stderr, "Error processing AVPacket (%s)\n", av_err2str(*ncerr));
      return ncvisual_decode(nc);
    }
    --nc->packet_outstanding;
    av_packet_unref(nc->packet);
    int averr = avcodec_receive_frame(nc->codecctx, nc->frame);
    if(averr >= 0){
      have_frame = true;
    }else if(averr == AVERROR(EAGAIN) || averr == AVERROR_EOF){
      have_frame = false;
    }else if(averr < 0){
      //fprintf(stderr, "Error decoding AVPacket (%s)\n", av_err2str(averr));
      return averr2ncerr(averr);
    }
  }while(!have_frame);
//print_frame_summary(nc->codecctx, nc->frame);
#define IMGALLOCALIGN 32
  int rows, cols;
  if(nc->ncp == nullptr){ // create plane
    if(nc->style == NCSCALE_NONE){
      rows = nc->frame->height / encoding_vert_scale(nc);
      cols = nc->frame->width;
    }else{ // FIXME differentiate between scale/stretch
      notcurses_term_dim_yx(nc->ncobj, &rows, &cols);
      if(nc->placey >= rows || nc->placex >= cols){
        return NCERR_DECODE;
      }
      rows -= nc->placey;
      cols -= nc->placex;
    }
    nc->dstwidth = cols;
    nc->dstheight = rows * encoding_vert_scale(nc);
    nc->ncp = ncplane_new(nc->ncobj, rows, cols, nc->placey, nc->placex, nullptr);
    nc->placey = 0;
    nc->placex = 0;
    if(nc->ncp == nullptr){
      return NCERR_NOMEM;
    }
  }else{ // check for resize
    ncplane_dim_yx(nc->ncp, &rows, &cols);
    if(rows != nc->dstheight / encoding_vert_scale(nc) || cols != nc->dstwidth){
      sws_freeContext(nc->swsctx);
      nc->swsctx = nullptr;
      nc->dstheight = rows * encoding_vert_scale(nc);
      nc->dstwidth = cols;
    }
  }
  const int targformat = AV_PIX_FMT_RGBA;
  nc->swsctx = sws_getCachedContext(nc->swsctx,
                                    nc->frame->width,
                                    nc->frame->height,
                                    static_cast<AVPixelFormat>(nc->frame->format),
                                    nc->dstwidth,
                                    nc->dstheight,
                                    static_cast<AVPixelFormat>(targformat),
                                    SWS_LANCZOS,
                                    nullptr, nullptr, nullptr);
  if(nc->swsctx == nullptr){
    //fprintf(stderr, "Error retrieving swsctx\n");
    return NCERR_DECODE;
  }
  memcpy(nc->oframe, nc->frame, sizeof(*nc->oframe));
  nc->oframe->format = targformat;
  nc->oframe->width = nc->dstwidth;
  nc->oframe->height = nc->dstheight;
  int size = av_image_alloc(nc->oframe->data, nc->oframe->linesize,
                            nc->oframe->width, nc->oframe->height,
                            static_cast<AVPixelFormat>(nc->oframe->format),
                            IMGALLOCALIGN);
  if(size < 0){
    //fprintf(stderr, "Error allocating visual data (%s)\n", av_err2str(size));
    return NCERR_NOMEM;
  }
  int height = sws_scale(nc->swsctx, (const uint8_t* const*)nc->frame->data,
                         nc->frame->linesize, 0,
                         nc->frame->height, nc->oframe->data, nc->oframe->linesize);
  if(height < 0){
    //fprintf(stderr, "Error applying scaling (%s)\n", av_err2str(height));
    return NCERR_NOMEM;
  }
//print_frame_summary(nc->codecctx, nc->oframe);
#undef IMGALLOCALIGN
  av_frame_unref(nc->frame);
  const AVFrame* f = nc->oframe;
  int bpp = av_get_bits_per_pixel(av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format)));
  if(bpp != 32){
	  return NCERR_DECODE;
  }
  nc->rowstride = f->linesize[0];
  ncvisual_set_data(nc, reinterpret_cast<uint32_t*>(f->data[0]), false);
  return NCERR_SUCCESS;
}

static ncvisual*
ncvisual_open(const char* filename, nc_err_e* ncerr){
  *ncerr = NCERR_SUCCESS;
  ncvisual* ncv = ncvisual_create(1);
  if(ncv == nullptr){
    // fprintf(stderr, "Couldn't create %s (%s)\n", filename, strerror(errno));
    *ncerr = NCERR_NOMEM;
    return nullptr;
  }
  memset(ncv, 0, sizeof(*ncv));
  int averr = avformat_open_input(&ncv->fmtctx, filename, nullptr, nullptr);
  if(averr < 0){
//fprintf(stderr, "Couldn't open %s (%d)\n", filename, averr);
    *ncerr = averr2ncerr(averr);
    ncvisual_destroy(ncv);
    return nullptr;
  }
  averr = avformat_find_stream_info(ncv->fmtctx, nullptr);
  if(averr < 0){
//fprintf(stderr, "Error extracting stream info from %s (%d)\n", filename, averr);
    *ncerr = averr2ncerr(averr);
    ncvisual_destroy(ncv);
    return nullptr;
  }
//av_dump_format(ncv->fmtctx, 0, filename, false);
  if((averr = av_find_best_stream(ncv->fmtctx, AVMEDIA_TYPE_SUBTITLE, -1, -1, &ncv->subtcodec, 0)) >= 0){
    ncv->sub_stream_index = averr;
    if((ncv->subtcodecctx = avcodec_alloc_context3(ncv->subtcodec)) == nullptr){
      //fprintf(stderr, "Couldn't allocate decoder for %s\n", filename);
      *ncerr = NCERR_NOMEM;
      ncvisual_destroy(ncv);
      return nullptr;
    }
    // FIXME do we need avcodec_parameters_to_context() here?
    if((averr = avcodec_open2(ncv->subtcodecctx, ncv->subtcodec, nullptr)) < 0){
      //fprintf(stderr, "Couldn't open codec for %s (%s)\n", filename, av_err2str(*averr));
      *ncerr = averr2ncerr(averr);
      ncvisual_destroy(ncv);
      return nullptr;
    }
  }else{
    ncv->sub_stream_index = -1;
  }
  if((ncv->packet = av_packet_alloc()) == nullptr){
    // fprintf(stderr, "Couldn't allocate packet for %s\n", filename);
    *ncerr = NCERR_NOMEM;
    ncvisual_destroy(ncv);
    return nullptr;
  }
  if((averr = av_find_best_stream(ncv->fmtctx, AVMEDIA_TYPE_VIDEO, -1, -1, &ncv->codec, 0)) < 0){
    // fprintf(stderr, "Couldn't find visuals in %s (%s)\n", filename, av_err2str(*averr));
    *ncerr = averr2ncerr(averr);
    ncvisual_destroy(ncv);
    return nullptr;
  }
  ncv->stream_index = averr;
  if(ncv->codec == nullptr){
    //fprintf(stderr, "Couldn't find decoder for %s\n", filename);
    ncvisual_destroy(ncv);
    return nullptr;
  }
  AVStream* st = ncv->fmtctx->streams[ncv->stream_index];
  if((ncv->codecctx = avcodec_alloc_context3(ncv->codec)) == nullptr){
    //fprintf(stderr, "Couldn't allocate decoder for %s\n", filename);
    *ncerr = NCERR_NOMEM;
    goto err;
  }
  if(avcodec_parameters_to_context(ncv->codecctx, st->codecpar) < 0){
    goto err;
  }
  if((averr = avcodec_open2(ncv->codecctx, ncv->codec, nullptr)) < 0){
    //fprintf(stderr, "Couldn't open codec for %s (%s)\n", filename, av_err2str(*averr));
    *ncerr = averr2ncerr(averr);
    goto err;
  }
  /*if((ncv->cparams = avcodec_parameters_alloc()) == nullptr){
    //fprintf(stderr, "Couldn't allocate codec params for %s\n", filename);
    *averr = NCERR_NOMEM;
    goto err;
  }
  if((*averr = avcodec_parameters_from_context(ncv->cparams, ncv->codecctx)) < 0){
    //fprintf(stderr, "Couldn't get codec params for %s (%s)\n", filename, av_err2str(*averr));
    goto err;
  }*/
  if((ncv->frame = av_frame_alloc()) == nullptr){
    // fprintf(stderr, "Couldn't allocate frame for %s\n", filename);
    *ncerr = NCERR_NOMEM;
    goto err;
  }
  if((ncv->oframe = av_frame_alloc()) == nullptr){
    // fprintf(stderr, "Couldn't allocate output frame for %s\n", filename);
    *ncerr = NCERR_NOMEM;
    goto err;
  }
  return ncv;

err:
  ncvisual_destroy(ncv);
  return nullptr;
}

ncvisual* ncplane_visual_open(ncplane* nc, const char* filename, nc_err_e* ncerr){
  ncvisual* ncv = ncvisual_open(filename, ncerr);
  if(ncv == nullptr){
    return nullptr;
  }
  set_encoding_vert_scale(nc->nc, ncv);
  ncplane_dim_yx(nc, &ncv->dstheight, &ncv->dstwidth);
  ncv->dstheight *= encoding_vert_scale(ncv);
  ncv->ncp = nc;
  ncv->style = NCSCALE_STRETCH;
  return ncv;
}

ncvisual* ncvisual_from_file(notcurses* nc, const char* filename,
                             nc_err_e* ncerr, int y, int x, ncscale_e style){
  ncvisual* ncv = ncvisual_open(filename, ncerr);
  if(ncv == nullptr){
    return nullptr;
  }
  set_encoding_vert_scale(nc, ncv);
  ncv->placey = y;
  ncv->placex = x;
  ncv->style = style;
  ncv->ncobj = nc;
  ncv->ncp = nullptr;
  return ncv;
}

// iterative over the decoded frames, calling streamer() with curry for each.
// frames carry a presentation time relative to the beginning, so we get an
// initial timestamp, and check each frame against the elapsed time to sync
// up playback.
int ncvisual_stream(notcurses* nc, ncvisual* ncv, nc_err_e* ncerr,
                    float timescale, streamcb streamer, void* curry){
  *ncerr = NCERR_SUCCESS;
  int frame = 1;
  ncv->timescale = timescale;
  struct timespec begin; // time we started
  clock_gettime(CLOCK_MONOTONIC, &begin);
  uint64_t nsbegin = timespec_to_ns(&begin);
  bool usets = false;
  // each frame has a pkt_duration in milliseconds. keep the aggregate, in case
  // we don't have PTS available.
  uint64_t sum_duration = 0;
  while((*ncerr = ncvisual_decode(ncv)) == NCERR_SUCCESS){
    // codecctx seems to be off by a factor of 2 regularly. instead, go with
    // the time_base from the avformatctx.
    double tbase = av_q2d(ncv->fmtctx->streams[ncv->stream_index]->time_base);
    int64_t ts = ncv->oframe->best_effort_timestamp;
    if(frame == 1 && ts){
      usets = true;
    }
    if(ncvisual_render(ncv, 0, 0, -1, -1) < 0){
      return -1;
    }
    if(streamer){
      int r = streamer(nc, ncv, curry);
      if(r){
        return r;
      }
    }
    ++frame;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t nsnow = timespec_to_ns(&now);
    struct timespec interval;
    uint64_t duration = ncv->oframe->pkt_duration * tbase * NANOSECS_IN_SEC;
//fprintf(stderr, "use: %u dur: %ju ts: %ju cctx: %f fctx: %f\n", usets, duration, ts, av_q2d(ncv->codecctx->time_base), av_q2d(ncv->fmtctx->streams[ncv->stream_index]->time_base));
    sum_duration += (duration * ncv->timescale);
    double schedns = nsbegin;
    if(usets){
      if(tbase == 0){
        tbase = duration;
      }
      schedns += ts * (tbase * ncv->timescale) * NANOSECS_IN_SEC;
    }else{
      schedns += sum_duration;
    }
    if(nsnow < schedns){
      ns_to_timespec(schedns - nsnow, &interval);
      nanosleep(&interval, nullptr);
    }
  }
  if(*ncerr == NCERR_EOF){
    return 0;
  }
  return -1;
}

int ncvisual_init(int loglevel){
  av_log_set_level(loglevel);
  // FIXME could also use av_log_set_callback() and capture the message...
  return 0;
}
} // extern "C"
#else // built without ffmpeg
#ifndef USE_OIIO // built without ffmpeg or oiio
bool notcurses_canopen_images(const notcurses* nc __attribute__ ((unused))){
  return false;
}

bool notcurses_canopen_videos(const notcurses* nc __attribute__ ((unused))){
  return false;
}

nc_err_e ncvisual_decode(ncvisual* nc){
  (void)nc;
  return NCERR_UNIMPLEMENTED;
}

int ncvisual_stream(notcurses* nc, ncvisual* ncv, nc_err_e* ncerr,
                    float timespec, streamcb streamer, void* curry){
  (void)nc;
  (void)ncv;
  (void)ncerr;
  (void)timespec;
  (void)streamer;
  (void)curry;
  return -1;
}

ncvisual* ncplane_visual_open(ncplane* nc, const char* filename, nc_err_e* ncerr){
  (void)nc;
  (void)filename;
  (void)ncerr;
  return nullptr;
}

ncvisual* ncvisual_from_file(notcurses* nc, const char* filename,
                             nc_err_e* ncerr, int y, int x, ncscale_e style){
  (void)nc;
  (void)filename;
  (void)ncerr;
  (void)y;
  (void)x;
  (void)style;
  return nullptr;
}

char* ncvisual_subtitle(const ncvisual* ncv){
  (void)ncv;
  return nullptr;
}

int ncvisual_init(int loglevel){
  (void)loglevel;
  return 0; // allow success here
}

void ncvisual_destroy(ncvisual* ncv){
  if(ncv){
    ncvisual_destroy_common(ncv);
  }
}
#else
#ifdef USE_OIIO
bool notcurses_canopen_images(const notcurses* nc __attribute__ ((unused))){
  return true;
}

bool notcurses_canopen_videos(const notcurses* nc __attribute__ ((unused))){
  return false; // too slow for reliable use at the moment
}

static ncvisual*
ncvisual_open(const char* filename, nc_err_e* err){
  *ncerr = NCERR_SUCCESS;
  ncvisual* ncv = ncvisual_create(1);
  if(ncv == nullptr){
    *err = NCERR_NOMEM;
    return nullptr;
  }
  if((ncv->filename = strdup(filename)) == nullptr){
    *err = NCERR_NOMEM;
    delete ncv;
    return nullptr;
  }
  ncv->image = OIIO::ImageInput::open(filename);
  if(!ncv->image){
    // fprintf(stderr, "Couldn't create %s (%s)\n", filename, strerror(errno));
    *err = NCERR_DECODE;
    ncvisual_destroy(ncv);
    return nullptr;
  }
/*const auto &spec = ncv->image->spec_dimensions();
std::cout << "Opened " << filename << ": " << spec.height << "x" <<
spec.width << "@" << spec.nchannels << " (" << spec.format << ")" << std::endl;*/
  return ncv;
}

ncvisual* ncplane_visual_open(ncplane* nc, const char* filename, nc_err_e* ncerr){
  ncvisual* ncv = ncvisual_open(filename, ncerr);
  if(ncv == nullptr){
    *ncerr = NCERR_NOMEM;
    return nullptr;
  }
  set_encoding_vert_scale(nc->nc, ncv);
  ncplane_dim_yx(nc, &ncv->dstheight, &ncv->dstwidth);
  ncv->dstheight *= encoding_vert_scale(ncv);
  ncv->ncp = nc;
  ncv->style = NCSCALE_STRETCH;
  ncv->ncobj = nullptr;
  return ncv;
}

ncvisual* ncvisual_from_file(notcurses* nc, const char* filename, nc_err_e* ncerr,
                             int y, int x, ncscale_e style){
  ncvisual* ncv = ncvisual_open(filename, ncerr);
  if(ncv == nullptr){
    return nullptr;
  }
  set_encoding_vert_scale(nc->nc, ncv);
  ncv->placey = y;
  ncv->placex = x;
  ncv->style = style;
  ncv->ncobj = nc;
  ncv->ncp = nullptr;
  ncv->ncobj = nc;
  return ncv;
}

nc_err_e ncvisual_decode(ncvisual* nc){
//fprintf(stderr, "current subimage: %d frame: %p\n", nc->image->current_subimage(), nc->frame.get());
  const auto &spec = nc->image->spec_dimensions(nc->framenum);
  if(nc->frame){
//fprintf(stderr, "seeking subimage: %d\n", nc->image->current_subimage() + 1);
    OIIO::ImageSpec newspec;
    if(!nc->image->seek_subimage(nc->image->current_subimage() + 1, 0, newspec)){
       return NCERR_EOF;
    }
    // FIXME check newspec vis-a-vis image->spec()?
  }
//fprintf(stderr, "SUBIMAGE: %d\n", nc->image->current_subimage());
  auto pixels = spec.width * spec.height;// * spec.nchannels;
  if(spec.nchannels < 3 || spec.nchannels > 4){
    return NCERR_DECODE; // FIXME get some to test with
  }
  nc->frame = std::make_unique<uint32_t[]>(pixels);
  if(spec.nchannels == 3){ // FIXME replace with channel shuffle
    std::fill(nc->frame.get(), nc->frame.get() + pixels, 0xfffffffful);
  }
//fprintf(stderr, "READING: %d %ju\n", nc->image->current_subimage(), nc->framenum);
  if(!nc->image->read_image(nc->framenum++, 0, 0, spec.nchannels, OIIO::TypeDesc(OIIO::TypeDesc::UINT8, 4), nc->frame.get(), 4)){
    return NCERR_DECODE;
  }
//fprintf(stderr, "READ: %d %ju\n", nc->image->current_subimage(), nc->framenum);
/*for(int i = 0 ; i < pixels ; ++i){
  //fprintf(stderr, "%06d %02x %02x %02x %02x\n", i,
  fprintf(stderr, "%06d %d %d %d %d\n", i,
      (nc->frame[i]) & 0xff,
      (nc->frame[i] >> 8) & 0xff,
      (nc->frame[i] >> 16) & 0xff,
      nc->frame[i] >> 24
      );
}*/
  OIIO::ImageSpec rgbaspec = spec;
  rgbaspec.nchannels = 4;
  nc->raw = std::make_unique<OIIO::ImageBuf>(rgbaspec, nc->frame.get());
//fprintf(stderr, "SUBS: %d\n", nc->raw->nsubimages());
  int rows, cols;
  if(nc->ncp == nullptr){ // create plane
    if(nc->style == NCSCALE_NONE){
      rows = spec.height / encoding_vert_scale(nc);
      cols = spec.width;
    }else{ // FIXME differentiate between scale/stretch
      notcurses_term_dim_yx(nc->ncobj, &rows, &cols);
      if(nc->placey >= rows || nc->placex >= cols){
        return NCERR_DECODE;
      }
      rows -= nc->placey;
      cols -= nc->placex;
    }
    nc->dstwidth = cols;
    nc->dstheight = rows * encoding_vert_scale(nc);
    nc->ncp = ncplane_new(nc->ncobj, rows, cols, nc->placey, nc->placex, nullptr);
    nc->placey = 0;
    nc->placex = 0;
    if(nc->ncp == nullptr){
      return NCERR_NOMEM;
    }
  }else{ // check for resize
    ncplane_dim_yx(nc->ncp, &rows, &cols);
    if(rows != nc->dstheight / encoding_vert_scale(nc) || cols != nc->dstwidth){
      nc->dstheight = rows * encoding_vert_scale(nc);
      nc->dstwidth = cols;
    }
  }
  ncvisual_set_data(nc, static_cast<uint32_t*>(nc->raw->localpixels()), false);
  if(nc->dstwidth != spec.width || nc->dstheight != spec.height){ // scale it
    OIIO::ROI roi(0, nc->dstwidth, 0, nc->dstheight, 0, 1, 0, 4);
    if(!OIIO::ImageBufAlgo::resize(nc->scaled, *nc->raw, "", 0, roi)){
      return NCERR_DECODE; // FIXME need we do anything further?
    }
    nc->rowstride = nc->dstwidth * 4;
    ncvisual_set_data(nc, static_cast<uint32_t*>(nc->scaled.localpixels()), false);
  }
  nc->rowstride = nc->dstwidth * 4;
  return NCERR_SUCCESS;
}

int ncvisual_stream(notcurses* nc, ncvisual* ncv, nc_err_e* ncerr,
                    float timescale, streamcb streamer, void* curry){
  *ncerr = NCERR_SUCCESS;
  int frame = 1;
  ncv->timescale = timescale;
  struct timespec begin; // time we started
  clock_gettime(CLOCK_MONOTONIC, &begin);
  //uint64_t nsbegin = timespec_to_ns(&begin);
  //bool usets = false;
  // each frame has a pkt_duration in milliseconds. keep the aggregate, in case
  // we don't have PTS available.
  //uint64_t sum_duration = 0;
  while((*ncerr = ncvisual_decode(ncv)) == NCERR_SUCCESS){
    /* codecctx seems to be off by a factor of 2 regularly. instead, go with
    // the time_base from the avformatctx.
    double tbase = av_q2d(ncv->fmtctx->streams[ncv->stream_index]->time_base);
    int64_t ts = ncv->oframe->best_effort_timestamp;
    if(frame == 1 && ts){
      usets = true;
    }*/
    if(ncvisual_render(ncv, 0, 0, -1, -1) < 0){
      return -1;
    }
    if(streamer){
      int r = streamer(nc, ncv, curry);
      if(r){
        return r;
      }
    }
    ++frame;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    /*uint64_t nsnow = timespec_to_ns(&now);
    struct timespec interval;
    uint64_t duration = ncv->oframe->pkt_duration * tbase * NANOSECS_IN_SEC;
    sum_duration += (duration * ncv->timescale);
//fprintf(stderr, "use: %u dur: %ju ts: %ju cctx: %f fctx: %f\n", usets, duration, ts, av_q2d(ncv->codecctx->time_base), av_q2d(ncv->fmtctx->streams[ncv->stream_index]->time_base));
    double schedns = nsbegin;
    if(usets){
      if(tbase == 0){
        tbase = duration;
      }
      schedns += ts * (tbase * ncv->timescale) * NANOSECS_IN_SEC;
    }else{
      schedns += sum_duration;
    }
    if(nsnow < schedns){
      ns_to_timespec(schedns - nsnow, &interval);
      nanosleep(&interval, nullptr);
    }*/
  }
  if(*ncerr == NCERR_EOF){
    return 0;
  }
  return -1;
}

char* ncvisual_subtitle(const ncvisual* ncv){ // no support in OIIO
  (void)ncv;
  return nullptr;
}

int ncvisual_init(int loglevel){
  // FIXME set OIIO global attribute "debug" based on loglevel
  (void)loglevel;
  // FIXME check OIIO_VERSION_STRING components against linked openimageio_version()
  return 0; // allow success here
}

void ncvisual_destroy(ncvisual* ncv){
  if(ncv){
    if(ncv->image){
      ncv->image->close();
    }
    ncvisual_destroy_common(ncv);
  }
}

extern "C" {
// FIXME would be nice to have OIIO::attributes("libraries") in here
const char* oiio_version(void){
  return OIIO_VERSION_STRING;
}
}
#endif
#endif
#endif
