/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2014 Carnegie Mellon University
 *  Copyright (c) 2011 Google, Inc.
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <config.h>

#include "openslide-private.h"
#include "openslide-decode-jpeg.h"

#include <glib.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <jpeglib.h>
#include <jerror.h>

struct openslide_jpeg_error_mgr {
  struct jpeg_error_mgr base;
  jmp_buf *env;
  GError *err;
};

struct associated_image {
  struct _openslide_associated_image base;
  char *filename;
  int64_t offset;
};


static void my_error_exit(j_common_ptr cinfo) {
  struct openslide_jpeg_error_mgr *jerr =
    (struct openslide_jpeg_error_mgr *) cinfo->err;

  (jerr->base.output_message) (cinfo);

  //  g_debug("JUMP");
  longjmp(*(jerr->env), 1);
}

static void my_output_message(j_common_ptr cinfo) {
  struct openslide_jpeg_error_mgr *jerr =
    (struct openslide_jpeg_error_mgr *) cinfo->err;
  char buffer[JMSG_LENGTH_MAX];

  (*cinfo->err->format_message) (cinfo, buffer);

  g_set_error(&jerr->err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
              "%s", buffer);
}

static void my_emit_message(j_common_ptr cinfo, int msg_level) {
  if (msg_level < 0) {
    // Warning message.  Convert to fatal error.
    (*cinfo->err->error_exit) (cinfo);
  }
}

// the caller must assign the jpeg_decompress_struct * before calling setjmp()
// so that nothing will be clobbered by a longjmp()
struct jpeg_decompress_struct *_openslide_jpeg_create_decompress(void) {
  return g_slice_new0(struct jpeg_decompress_struct);
}

// after setjmp(), initialize error handler and start decompressing
void _openslide_jpeg_init_decompress(struct jpeg_decompress_struct *cinfo,
                                     jmp_buf *env) {
  struct openslide_jpeg_error_mgr *jerr =
    g_slice_new0(struct openslide_jpeg_error_mgr);
  jpeg_std_error(&(jerr->base));
  jerr->base.error_exit = my_error_exit;
  jerr->base.output_message = my_output_message;
  jerr->base.emit_message = my_emit_message;
  jerr->env = env;
  cinfo->err = (struct jpeg_error_mgr *) jerr;
  jpeg_create_decompress(cinfo);
}

void _openslide_jpeg_propagate_error(GError **err,
                                     struct jpeg_decompress_struct *cinfo) {
  g_assert(cinfo->err->error_exit == my_error_exit);
  struct openslide_jpeg_error_mgr *jerr =
    (struct openslide_jpeg_error_mgr *) cinfo->err;
  g_propagate_error(err, jerr->err);
  jerr->err = NULL;
}

void _openslide_jpeg_destroy_decompress(struct jpeg_decompress_struct *cinfo) {
  jpeg_destroy_decompress(cinfo);
  if (cinfo->err) {
    g_assert(cinfo->err->error_exit == my_error_exit);
    struct openslide_jpeg_error_mgr *jerr =
      (struct openslide_jpeg_error_mgr *) cinfo->err;
    g_assert(jerr->err == NULL);
    g_slice_free(struct openslide_jpeg_error_mgr, jerr);
  }
  g_slice_free(struct jpeg_decompress_struct, cinfo);
}

static bool jpeg_get_dimensions(FILE *f,  // or:
                                const void *buf, uint32_t buflen,
                                int32_t *w, int32_t *h,
                                GError **err) {
  volatile bool result = false;
  jmp_buf env;

  struct jpeg_decompress_struct *cinfo = _openslide_jpeg_create_decompress();

  if (setjmp(env) == 0) {
    _openslide_jpeg_init_decompress(cinfo, &env);

    if (f) {
      _openslide_jpeg_stdio_src(cinfo, f);
    } else {
      _openslide_jpeg_mem_src(cinfo, (void *) buf, buflen);
    }

    int header_result = jpeg_read_header(cinfo, TRUE);
    if ((header_result != JPEG_HEADER_OK
	 && header_result != JPEG_HEADER_TABLES_ONLY)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't read JPEG header");
      goto DONE;
    }

    jpeg_calc_output_dimensions(cinfo);

    *w = cinfo->output_width;
    *h = cinfo->output_height;
    result = true;
  } else {
    // setjmp returned again
    _openslide_jpeg_propagate_error(err, cinfo);
  }

DONE:
  // free buffers
  _openslide_jpeg_destroy_decompress(cinfo);

  return result;
}

bool _openslide_jpeg_read_dimensions(const char *filename,
                                     int64_t offset,
                                     int32_t *w, int32_t *h,
                                     GError **err) {
  FILE *f = _openslide_fopen(filename, "rb", err);
  if (f == NULL) {
    return false;
  }
  if (offset && fseeko(f, offset, SEEK_SET) == -1) {
    _openslide_io_error(err, "Cannot seek to offset");
    fclose(f);
    return false;
  }

  bool success = jpeg_get_dimensions(f, NULL, 0, w, h, err);

  fclose(f);
  return success;
}

bool _openslide_jpeg_decode_buffer_dimensions(const void *buf, uint32_t len,
                                              int32_t *w, int32_t *h,
                                              GError **err) {
  return jpeg_get_dimensions(NULL, buf, len, w, h, err);
}

static bool jpeg_decode(FILE *f,  // or:
                        const void *buf, uint32_t buflen,
                        void * const _dest, bool grayscale,
                        int32_t w, int32_t h,
                        GError **err) {
  volatile bool result = false;
  jmp_buf env;
  volatile gsize row_size = 0;  // preserve across longjmp

  struct jpeg_decompress_struct *cinfo = _openslide_jpeg_create_decompress();
  JSAMPARRAY buffer = g_slice_alloc0(sizeof(JSAMPROW) * MAX_SAMP_FACTOR);

  if (setjmp(env) == 0) {
    _openslide_jpeg_init_decompress(cinfo, &env);

    // set up I/O
    if (f) {
      _openslide_jpeg_stdio_src(cinfo, f);
    } else {
      _openslide_jpeg_mem_src(cinfo, (void *) buf, buflen);
    }

    // read header
    int header_result = jpeg_read_header(cinfo, TRUE);
    if ((header_result != JPEG_HEADER_OK
	 && header_result != JPEG_HEADER_TABLES_ONLY)) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Couldn't read JPEG header");
      goto DONE;
    }

    cinfo->out_color_space = grayscale ? JCS_GRAYSCALE : JCS_RGB;

    jpeg_start_decompress(cinfo);

    // ensure buffer dimensions are correct
    int32_t width = cinfo->output_width;
    int32_t height = cinfo->output_height;
    if (w != width || h != height) {
      g_set_error(err, OPENSLIDE_ERROR, OPENSLIDE_ERROR_FAILED,
                  "Dimensional mismatch reading JPEG, "
                  "expected %dx%d, got %dx%d",
                  w, h, width, height);
      goto DONE;
    }

    // allocate scanline buffers
    row_size = sizeof(JSAMPLE) * cinfo->output_width *
               cinfo->output_components;
    for (int i = 0; i < cinfo->rec_outbuf_height; i++) {
      buffer[i] = g_slice_alloc(row_size);
    }

    // decompress
    uint32_t *dest32 = _dest;
    uint8_t *dest8 = _dest;
    while (cinfo->output_scanline < cinfo->output_height) {
      JDIMENSION rows_read = jpeg_read_scanlines(cinfo,
						 buffer,
						 cinfo->rec_outbuf_height);
      int cur_buffer = 0;
      while (rows_read > 0) {
        // copy a row
        int32_t i;
        if (cinfo->output_components == 1) {
          // grayscale
          for (i = 0; i < (int32_t) cinfo->output_width; i++) {
            dest8[i] = buffer[cur_buffer][i];
          }
          dest8 += cinfo->output_width;
        } else {
          // RGB
          for (i = 0; i < (int32_t) cinfo->output_width; i++) {
            dest32[i] = 0xFF000000 |                // A
              buffer[cur_buffer][i * 3 + 0] << 16 | // R
              buffer[cur_buffer][i * 3 + 1] << 8 |  // G
              buffer[cur_buffer][i * 3 + 2];        // B
          }
          dest32 += cinfo->output_width;
        }

	// advance 1 row
	rows_read--;
	cur_buffer++;
      }
    }
    result = true;
  } else {
    // setjmp has returned again
    _openslide_jpeg_propagate_error(err, cinfo);
  }

DONE:
  // free buffers
  for (int i = 0; i < cinfo->rec_outbuf_height; i++) {
    g_slice_free1(row_size, buffer[i]);
  }
  g_slice_free1(sizeof(JSAMPROW) * MAX_SAMP_FACTOR, buffer);

  _openslide_jpeg_destroy_decompress(cinfo);

  return result;
}

bool _openslide_jpeg_read(const char *filename,
                          int64_t offset,
                          uint32_t *dest,
                          int32_t w, int32_t h,
                          GError **err) {
  //g_debug("read JPEG: %s %"PRId64, filename, offset);

  FILE *f = _openslide_fopen(filename, "rb", err);
  if (f == NULL) {
    return false;
  }
  if (offset && fseeko(f, offset, SEEK_SET) == -1) {
    _openslide_io_error(err, "Cannot seek to offset");
    fclose(f);
    return false;
  }

  bool success = jpeg_decode(f, NULL, 0, dest, false, w, h, err);

  fclose(f);
  return success;
}

bool _openslide_jpeg_decode_buffer(const void *buf, uint32_t len,
                                   uint32_t *dest,
                                   int32_t w, int32_t h,
                                   GError **err) {
  //g_debug("decode JPEG buffer: %x %u", buf, len);

  return jpeg_decode(NULL, buf, len, dest, false, w, h, err);
}

bool _openslide_jpeg_decode_buffer_gray(const void *buf, uint32_t len,
                                        uint8_t *dest,
                                        int32_t w, int32_t h,
                                        GError **err) {
  //g_debug("decode grayscale JPEG buffer: %x %u", buf, len);

  return jpeg_decode(NULL, buf, len, dest, true, w, h, err);
}

static bool get_associated_image_data(struct _openslide_associated_image *_img,
                                      uint32_t *dest,
                                      GError **err) {
  struct associated_image *img = (struct associated_image *) _img;

  //g_debug("read JPEG associated image: %s %"PRId64, img->filename, img->offset);

  return _openslide_jpeg_read(img->filename, img->offset, dest,
                              img->base.w, img->base.h, err);
}

static void destroy_associated_image(struct _openslide_associated_image *_img) {
  struct associated_image *img = (struct associated_image *) _img;

  g_free(img->filename);
  g_slice_free(struct associated_image, img);
}

static const struct _openslide_associated_image_ops jpeg_associated_ops = {
  .get_argb_data = get_associated_image_data,
  .destroy = destroy_associated_image,
};

bool _openslide_jpeg_add_associated_image(openslide_t *osr,
					  const char *name,
					  const char *filename,
					  int64_t offset,
					  GError **err) {
  int32_t w, h;
  if (!_openslide_jpeg_read_dimensions(filename, offset, &w, &h, err)) {
    g_prefix_error(err, "Can't read %s associated image: ", name);
    return false;
  }

  struct associated_image *img = g_slice_new0(struct associated_image);
  img->base.ops = &jpeg_associated_ops;
  img->base.w = w;
  img->base.h = h;
  img->filename = g_strdup(filename);
  img->offset = offset;

  g_hash_table_insert(osr->associated_images, g_strdup(name), img);

  return true;
}
