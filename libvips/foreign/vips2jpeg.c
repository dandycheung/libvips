/* wrap jpeg library for write
 *
 * 28/11/03 JC
 *	- better no-overshoot on tile loop
 * 12/11/04
 *	- better demand size choice for eval
 * 30/6/05 JC
 *	- update im_error()/im_warn()
 *	- now loads and saves exif data
 * 30/7/05
 *	- now loads ICC profiles
 *	- now saves ICC profiles from the VIPS header
 * 24/8/05
 *	- jpeg load sets vips xres/yres from exif, if possible
 *	- jpeg save sets exif xres/yres from vips, if possible
 * 29/8/05
 *	- cut from old vips_jpeg.c
 * 20/4/06
 *	- auto convert to sRGB/mono for save
 * 13/10/06
 *	- add </libexif/ prefix if required
 * 19/1/07
 *	- oop, libexif confusion
 * 2/11/07
 *	- use im_wbuffer() API for BG writes
 * 15/2/08
 *	- write CMYK if Bands == 4 and Type == CMYK
 * 12/5/09
 *	- fix signed/unsigned warning
 * 13/8/09
 *	- allow "none" for profile, meaning don't embed one
 * 4/2/10
 *	- gtkdoc
 * 17/7/10
 *	- use g_assert()
 *	- allow space for the header in init_destination(), helps writing very
 *	  small JPEGs (thanks Tim Elliott)
 * 18/7/10
 *	- collect im_vips2bufjpeg() output in a list of blocks ... we no
 *	  longer overallocate or underallocate
 * 8/7/11
 *	- oop CMYK write was not inverting, thanks Ole
 * 12/10/2011
 *	- write XMP data
 * 18/10/2011
 *	- update Orientation as well
 * 3/11/11
 *	- rebuild exif tags from coded metadata values
 * 24/11/11
 *	- turn into a set of write fns ready to be called from a class
 * 7/8/12
 *	- use VIPS_META_RESOLUTION_UNIT to select resolution unit
 * 16/11/12
 *	- read ifds from exif fields
 *	- optionally parse rationals as a/b
 *	- update exif image dimensions
 * 21/11/12
 *	- attach IPTC data (app13), thanks Gary
 * 2/10/13 Lovell Fuller
 *	- add optimize_coding parameter
 *	- add progressive mode
 * 12/11/13
 *	- add "strip" option to remove all metadata
 * 13/11/13
 *	- add a "no_subsample" option to disable chroma subsample
 * 9/9/14
 *	- support "none" as a resolution unit
 * 8/7/15
 *	- omit oversized jpeg markers
 * 15/7/15
 *	- exif tags use @name, not @title
 *	- set arbitrary exif tags from metadata
 * 25/11/15
 *	- don't write JFIF headers if we are stripping, thanks Benjamin
 * 13/4/16
 *	- remove deleted exif fields more carefully
 * 9/5/16 felixbuenemann
 *	- add quant_table
 * 26/5/16
 *	- switch to new orientation tag
 * 9/7/16
 *	- turn off chroma subsample for Q >= 90
 * 7/11/16
 *	- move exif handling out to exif.c
 * 27/2/17
 *	- use dbuf for memory output
 * 19/12/17 Lovell
 *	- fix a leak with an error during buffer output
 * 19/4/19
 *	- fix another leak with error during buffer output
 * 19/7/19
 *	- ignore large XMP
 * 14/10/19
 *	- revise for target IO
 * 18/2/20 Elad-Laufer
 *	- add subsample_mode, deprecate no_subsample
 * 13/9/20
 *	- only write JFIF resolution if we don't have EXIF
 * 7/10/21 Manthey
 *	- add restart_interval
 * 21/10/21 usualuse
 *	- raise single-chunk limit on APP to 65533
 */

/*

	This file is part of VIPS.

	VIPS is free software; you can redistribute it and/or modify
	it under the terms of the GNU Lesser General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
	02110-1301  USA

 */

/*

	These files are distributed with VIPS - http://www.vips.ecs.soton.ac.uk

 */

/*
#define VIPS_DEBUG
#define DEBUG
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <glib/gi18n-lib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <vips/vips.h>
#include <vips/debug.h>
#include <vips/internal.h>

#include "pforeign.h"

#ifdef HAVE_JPEG

#include "jpeg.h"

#define ICC_MARKER (JPEG_APP0 + 2) /* JPEG marker code for ICC */
#define ICC_OVERHEAD_LEN 14		   /* size of non-profile data in APP2 */
#define MAX_BYTES_IN_MARKER 65533  /* maximum data len of a JPEG marker */
#define MAX_DATA_BYTES_IN_MARKER (MAX_BYTES_IN_MARKER - ICC_OVERHEAD_LEN)

const char *vips__jpeg_message_table[] = {
	"premature end of JPEG image",
	"unable to write to target",
	NULL
};

/* New output message method - send to VIPS.
 */
void
vips__new_output_message(j_common_ptr cinfo)
{
	char buffer[JMSG_LENGTH_MAX];

	(*cinfo->err->format_message)(cinfo, buffer);
	vips_error("VipsJpeg", _("%s"), buffer);

#ifdef DEBUG
	printf("vips__new_output_message: \"%s\"\n", buffer);
#endif /*DEBUG*/
}

/* New error_exit handler.
 */
void
vips__new_error_exit(j_common_ptr cinfo)
{
	ErrorManager *eman = (ErrorManager *) cinfo->err;

#ifdef DEBUG
	printf("vips__new_error_exit:\n");
#endif /*DEBUG*/

	/* Close the fp if necessary.
	 */
	if (eman->fp) {
		(void) fclose(eman->fp);
		eman->fp = NULL;
	}

	/* Send the error message to VIPS. This method is overridden above.
	 */
	(*cinfo->err->output_message)(cinfo);

	/* Jump back.
	 */
	longjmp(eman->jmp, 1);
}

/* What we track during a JPEG write.
 */
typedef struct {
	struct jpeg_compress_struct cinfo;
	ErrorManager eman;
	JSAMPROW *row_pointer;
	gboolean invert;
} Write;

static void
write_destroy(Write *write)
{
	jpeg_destroy_compress(&write->cinfo);
	VIPS_FREE(write->row_pointer);

	g_free(write);
}

static Write *
write_new(void)
{
	Write *write;

	if (!(write = g_new0(Write, 1)))
		return NULL;

	write->row_pointer = NULL;
	write->cinfo.err = jpeg_std_error(&write->eman.pub);
	write->cinfo.err->addon_message_table = vips__jpeg_message_table;
	write->cinfo.err->first_addon_message = 1000;
	write->cinfo.err->last_addon_message = 1001;
	write->cinfo.dest = NULL;
	write->eman.pub.error_exit = vips__new_error_exit;
	write->eman.pub.output_message = vips__new_output_message;
	write->eman.fp = NULL;
	write->invert = FALSE;

	return write;
}

static int
write_blob(Write *write, VipsImage *image, const char *field, int app)
{
	unsigned char *data;
	size_t data_length;

	if (!vips_image_get_typeof(image, field))
		return 0;

	if (vips_image_get_blob(image, field,
			(void *) &data, &data_length))
		return -1;

	/* Single jpeg markers can only hold 64kb, large objects must
	 * be split into multiple markers.
	 *
	 * Unfortunately, how this splitting is done depends on the
	 * data type. For example, ICC and XMP have completely
	 * different ways of doing this.
	 *
	 * For now, just ignore oversize objects and warn.
	 */
	if (data_length > MAX_BYTES_IN_MARKER)
		g_warning("field \"%s\" is too large "
				  "for a single JPEG marker, ignoring",
			field);
	else {
#ifdef DEBUG
		printf("write_blob: attaching %zd bytes of %s\n",
			data_length, field);
#endif /*DEBUG*/

		jpeg_write_marker(&write->cinfo, app,
			data, data_length);
	}

	return 0;
}

#define XML_URL "http://ns.adobe.com/xap/1.0/"

static int
write_xmp(Write *write, VipsImage *in)
{
	unsigned char *data;
	size_t data_length;
	char *p;

	if (!vips_image_get_typeof(in, VIPS_META_XMP_NAME))
		return 0;

	if (vips_image_get_blob(in, VIPS_META_XMP_NAME,
			(void *) &data, &data_length))
		return -1;

	/* To write >64kb XMP it you need to parse the whole XMP object,
	 * pull out the most important fields, code just them into the main
	 * XMP block, then write any remaining XMP objects into a set of
	 * extended XMP markers.
	 *
	 * http://wwwimages.adobe.com/content/dam/Adobe/en/devnet/xmp/pdfs/ \
	 *	XMPSpecificationPart3.pdf
	 *
	 * jpeg_write_marker() with some libjpeg versions will throw a fatal
	 * error with large chunks.
	 */
	if (data_length > 60000) {
		g_warning("large XMP not saved");
		return 0;
	}

	/* We need to add the magic XML URL to the start, then a null
	 * character, then the data.
	 */
	p = g_malloc(data_length + strlen(XML_URL) + 1);
	strcpy(p, XML_URL);
	memcpy(p + strlen(XML_URL) + 1, data, data_length);

	jpeg_write_marker(&write->cinfo, JPEG_APP0 + 1,
		(unsigned char *) p, data_length + strlen(XML_URL) + 1);

	g_free(p);

	return 0;
}

static int
write_exif(Write *write, VipsImage *image)
{
	if (write_blob(write, image, VIPS_META_EXIF_NAME, JPEG_APP0 + 1))
		return -1;

	return 0;
}

/* ICC writer from lcms, slight tweaks.
 */

/*
 * This routine writes the given ICC profile data into a JPEG file.
 * It *must* be called AFTER calling jpeg_start_compress() and BEFORE
 * the first call to jpeg_write_scanlines().
 * (This ordering ensures that the APP2 marker(s) will appear after the
 * SOI and JFIF or Adobe markers, but before all else.)
 */

static void
write_profile_data(j_compress_ptr cinfo,
	const JOCTET *icc_data_ptr,
	unsigned int icc_data_len)
{
	unsigned int num_markers; /* total number of markers we'll write */
	int cur_marker = 1;		  /* per spec, counting starts at 1 */
	unsigned int length;	  /* number of bytes to write in this marker */

	/* rounding up will fail for length == 0 */
	g_assert(icc_data_len > 0);

	/* Calculate the number of markers we'll need, rounding up of course */
	num_markers = (icc_data_len + MAX_DATA_BYTES_IN_MARKER - 1) /
		MAX_DATA_BYTES_IN_MARKER;

	while (icc_data_len > 0) {
		/* length of profile to put in this marker */
		length = icc_data_len;
		if (length > MAX_DATA_BYTES_IN_MARKER)
			length = MAX_DATA_BYTES_IN_MARKER;
		icc_data_len -= length;

		/* Write the JPEG marker header (APP2 code and marker length) */
		jpeg_write_m_header(cinfo, ICC_MARKER,
			(unsigned int) (length + ICC_OVERHEAD_LEN));

		/* Write the marker identifying string "ICC_PROFILE" (null-terminated).
		 * We code it in this less-than-transparent way so that the code works
		 * even if the local character set is not ASCII.
		 */
		jpeg_write_m_byte(cinfo, 0x49);
		jpeg_write_m_byte(cinfo, 0x43);
		jpeg_write_m_byte(cinfo, 0x43);
		jpeg_write_m_byte(cinfo, 0x5F);
		jpeg_write_m_byte(cinfo, 0x50);
		jpeg_write_m_byte(cinfo, 0x52);
		jpeg_write_m_byte(cinfo, 0x4F);
		jpeg_write_m_byte(cinfo, 0x46);
		jpeg_write_m_byte(cinfo, 0x49);
		jpeg_write_m_byte(cinfo, 0x4C);
		jpeg_write_m_byte(cinfo, 0x45);
		jpeg_write_m_byte(cinfo, 0x0);

		/* Add the sequencing info */
		jpeg_write_m_byte(cinfo, cur_marker);
		jpeg_write_m_byte(cinfo, (int) num_markers);

		/* Add the profile data */
		while (length--) {
			jpeg_write_m_byte(cinfo, *icc_data_ptr);
			icc_data_ptr++;
		}
		cur_marker++;
	}
}

#ifndef HAVE_EXIF
/* Set the JFIF resolution from the vips xres/yres tags.
 */
static void
vips_jfif_resolution_from_image(struct jpeg_compress_struct *cinfo,
	VipsImage *image)
{
	int xres, yres;
	const char *p;
	int unit;

	/* Default to inches, more progs support it.
	 */
	unit = 1;
	if (vips_image_get_typeof(image, VIPS_META_RESOLUTION_UNIT) &&
		!vips_image_get_string(image,
			VIPS_META_RESOLUTION_UNIT, &p)) {
		if (vips_isprefix("cm", p))
			unit = 2;
		else if (vips_isprefix("none", p))
			unit = 0;
	}

	switch (unit) {
	case 0:
		xres = rint(image->Xres);
		yres = rint(image->Yres);
		break;

	case 1:
		xres = rint(image->Xres * 25.4);
		yres = rint(image->Yres * 25.4);
		break;

	case 2:
		xres = rint(image->Xres * 10.0);
		yres = rint(image->Yres * 10.0);
		break;

	default:
		g_assert_not_reached();
		break;
	}

	VIPS_DEBUG_MSG("vips_jfif_resolution_from_image: "
				   "setting xres = %d, yres = %d, unit = %d\n",
		xres, yres, unit);

	cinfo->density_unit = unit;
	cinfo->X_density = xres;
	cinfo->Y_density = yres;
}
#endif /*HAVE_EXIF*/

/* Write an ICC Profile from a file into the JPEG stream.
 */
static int
write_profile_file(Write *write, const char *profile)
{
	VipsBlob *blob;

	if (vips_profile_load(profile, &blob, NULL))
		return -1;

	if (blob) {
		size_t length;
		const void *data = vips_blob_get(blob, &length);

		write_profile_data(&write->cinfo, (JOCTET *) data, length);

#ifdef DEBUG
		printf("write_profile_file: attached profile \"%s\"\n",
			profile);
#endif /*DEBUG*/

		vips_area_unref((VipsArea *) blob);
	}

	return 0;
}

static int
write_profile_meta(Write *write, VipsImage *in)
{
	const void *data;
	size_t length;

	if (vips_image_get_blob(in,
			VIPS_META_ICC_NAME, &data, &length))
		return -1;

	write_profile_data(&write->cinfo, data, length);

#ifdef DEBUG
	printf("write_profile_meta: attached %zd byte profile from header\n",
		length);
#endif /*DEBUG*/

	return 0;
}

static int
write_jpeg_block(VipsRegion *region, VipsRect *area, void *a)
{
	Write *write = (Write *) a;

	for (int y = 0; y < area->height; y++)
		write->row_pointer[y] = (JSAMPROW)
			VIPS_REGION_ADDR(region, area->left, area->top + y);

	/* Catch any longjmp()s from jpeg_write_scanlines() here.
	 */
	if (setjmp(write->eman.jmp))
		return -1;

	if (write->invert) {
		int n_elements = region->im->Bands * area->width;

		for (int y = 0; y < area->height; y++) {
			unsigned char *line = write->row_pointer[y];

			for (int x = 0; x < n_elements; x++)
				line[x] = 255 - line[x];
		}
	}

	jpeg_write_scanlines(&write->cinfo, write->row_pointer, area->height);

	return 0;
}

/* Set up cinfo. Pass width and height separately so we can be
 * used for region write.
 */
static void
set_cinfo(struct jpeg_compress_struct *cinfo,
	VipsImage *in, int width, int height,
	int qfac,
	gboolean optimize_coding, gboolean progressive,
	gboolean trellis_quant, gboolean overshoot_deringing,
	gboolean optimize_scans, int quant_table,
	VipsForeignSubsample subsample_mode, int restart_interval)
{
	J_COLOR_SPACE space;

	/* Set compression parameters.
	 */
	cinfo->image_width = width;
	cinfo->image_height = height;
	cinfo->input_components = in->Bands;
	if (in->Bands == 4 &&
		in->Type == VIPS_INTERPRETATION_CMYK) {
		space = JCS_CMYK;
	}
	else if (in->Bands == 3)
		space = JCS_RGB;
	else if (in->Bands == 1)
		space = JCS_GRAYSCALE;
	else
		/* Use luminance compression for all channels.
		 */
		space = JCS_UNKNOWN;
	cinfo->in_color_space = space;

#ifdef HAVE_JPEG_EXT_PARAMS
	/* Reset compression profile to libjpeg defaults
	 */
	if (jpeg_c_int_param_supported(cinfo, JINT_COMPRESS_PROFILE))
		jpeg_c_set_int_param(cinfo,
			JINT_COMPRESS_PROFILE, JCP_FASTEST);
#endif

	/* Reset to default.
	 */
	jpeg_set_defaults(cinfo);

	/* Compute optimal Huffman coding tables.
	 */
	cinfo->optimize_coding = optimize_coding;

	/* Use a restart interval.
	 */
	if (restart_interval > 0)
		cinfo->restart_interval = restart_interval;

#ifdef HAVE_JPEG_EXT_PARAMS
	/* Apply trellis quantisation to each 8x8 block. Implies
	 * "optimize_coding".
	 */
	if (trellis_quant) {
		if (jpeg_c_bool_param_supported(cinfo,
				JBOOLEAN_TRELLIS_QUANT)) {
			jpeg_c_set_bool_param(cinfo,
				JBOOLEAN_TRELLIS_QUANT, TRUE);
			cinfo->optimize_coding = TRUE;
		}
		else
			g_warning("trellis_quant unsupported");
	}

	/* Apply overshooting to samples with extreme values e.g. 0 & 255
	 * for 8-bit.
	 */
	if (overshoot_deringing) {
		if (jpeg_c_bool_param_supported(cinfo,
				JBOOLEAN_OVERSHOOT_DERINGING))
			jpeg_c_set_bool_param(cinfo,
				JBOOLEAN_OVERSHOOT_DERINGING, TRUE);
		else
			g_warning("overshoot_deringing unsupported");
	}

	/* Split the spectrum of DCT coefficients into separate scans.
	 * Requires progressive output. Must be set before
	 * jpeg_simple_progression.
	 */
	if (optimize_scans) {
		if (progressive) {
			if (jpeg_c_bool_param_supported(cinfo,
					JBOOLEAN_OPTIMIZE_SCANS))
				jpeg_c_set_bool_param(cinfo,
					JBOOLEAN_OPTIMIZE_SCANS, TRUE);
			else
				g_warning("ignoring optimize_scans");
		}
		else
			g_warning("ignoring optimize_scans for baseline");
	}

	/* Use predefined quantization table.
	 */
	if (quant_table > 0) {
		if (jpeg_c_int_param_supported(cinfo,
				JINT_BASE_QUANT_TBL_IDX))
			jpeg_c_set_int_param(cinfo,
				JINT_BASE_QUANT_TBL_IDX, quant_table);
		else
			g_warning("setting quant_table unsupported");
	}
#else
	/* Using jpeglib.h without extension parameters, warn of ignored
	 * options.
	 */
	if (trellis_quant)
		g_warning("ignoring trellis_quant");
	if (overshoot_deringing)
		g_warning("ignoring overshoot_deringing");
	if (optimize_scans)
		g_warning("ignoring optimize_scans");
	if (quant_table > 0)
		g_warning("ignoring quant_table");
#endif

	/* Set compression quality. Must be called after setting params above.
	 */
	jpeg_set_quality(cinfo, qfac, TRUE);

	/* Enable progressive write.
	 */
	if (progressive)
		jpeg_simple_progression(cinfo);

	/* We must set chroma subsampling explicitly since some libjpegs do not
	 * enable this by default.
	 */
	if (in->Bands == 3 &&
		(subsample_mode == VIPS_FOREIGN_SUBSAMPLE_ON ||
			(subsample_mode == VIPS_FOREIGN_SUBSAMPLE_AUTO &&
				qfac < 90)))
		cinfo->comp_info[0].h_samp_factor = cinfo->comp_info[0].v_samp_factor = 2;
	else
		cinfo->comp_info[0].h_samp_factor = cinfo->comp_info[0].v_samp_factor = 1;

	/* Rest should have sampling factors 1,1.
	 */
	for (int i = 1; i < in->Bands; i++)
		cinfo->comp_info[i].h_samp_factor = cinfo->comp_info[i].v_samp_factor = 1;

	/* Only write the JFIF headers if we have no EXIF.
	 * Some readers get confused if you set both.
	 */
	cinfo->write_JFIF_header = FALSE;
#ifndef HAVE_EXIF
	vips_jfif_resolution_from_image(cinfo, in);
	cinfo->write_JFIF_header = TRUE;
#endif /*HAVE_EXIF*/
}

static int
write_metadata(Write *write, VipsImage *in,
	const char *profile)
{
	if (write_exif(write, in) ||
		write_xmp(write, in) ||
		write_blob(write, in,
			VIPS_META_IPTC_NAME, JPEG_APP0 + 13))
		return -1;

	/* A profile supplied as an argument overrides an embedded
	 * profile.
	 */
	if (profile) {
		if (write_profile_file(write, profile))
			return -1;
	}
	else if (vips_image_get_typeof(in, VIPS_META_ICC_NAME)) {
		if (write_profile_meta(write, in))
			return -1;
	}

	return 0;
}

/* Write a VIPS image to a JPEG compress struct.
 */
static int
write_vips(Write *write, VipsImage *in, int Q, const char *profile,
	gboolean optimize_coding, gboolean progressive,
	gboolean trellis_quant, gboolean overshoot_deringing,
	gboolean optimize_scans, int quant_table,
	VipsForeignSubsample subsample_mode, int restart_interval)
{
	/* Should have been converted for save.
	 */
	g_assert(in->BandFmt == VIPS_FORMAT_UCHAR);
	g_assert(in->Coding == VIPS_CODING_NONE);
	g_assert(in->Bands == 1 ||
		in->Bands == 3 ||
		in->Bands == 4);

	/* Check input image.
	 */
	if (vips_image_pio_input(in))
		return -1;

	set_cinfo(&write->cinfo, in, in->Xsize, in->Ysize,
		Q, optimize_coding, progressive,
		trellis_quant, overshoot_deringing, optimize_scans,
		quant_table, subsample_mode, restart_interval);

	if (in->Bands == 4 &&
		in->Type == VIPS_INTERPRETATION_CMYK)
		/* IJG always sets an Adobe marker, so we should invert CMYK.
		 */
		write->invert = TRUE;

	/* Build VIPS output stuff now we know the image we'll be writing.
	 */
	if (!(write->row_pointer = VIPS_ARRAY(NULL, in->Ysize, JSAMPROW)))
		return -1;

	/* Write app0 and build compress tables.
	 */
	jpeg_start_compress(&write->cinfo, TRUE);

	/* All the other APP chunks come next.
	 */
	if (write_metadata(write, in, profile))
		return -1;

	/* Write data. Note that the write function grabs the longjmp()!
	 */
	if (vips_sink_disc(in, write_jpeg_block, write))
		return -1;

	/* We have to reinstate the setjmp() before we jpeg_finish_compress().
	 */
	if (setjmp(write->eman.jmp))
		return -1;

	/* This should only be called on a successful write.
	 */
	jpeg_finish_compress(&write->cinfo);

	return 0;
}

#define TARGET_BUFFER_SIZE (4096)

typedef struct {
	/* Public jpeg fields.
	 */
	struct jpeg_destination_mgr pub;

	/* Private stuff during write.
	 */

	/* Build the output area here.
	 */
	VipsTarget *target;

	/* Our output buffer.
	 */
	unsigned char buf[TARGET_BUFFER_SIZE];
} Dest;

/* Buffer full method. This is only called when the output area is exactly
 * full.
 */
static jboolean
empty_output_buffer(j_compress_ptr cinfo)
{
	Dest *dest = (Dest *) cinfo->dest;

	if (vips_target_write(dest->target,
			dest->buf, TARGET_BUFFER_SIZE))
		ERREXIT(cinfo, JERR_VIPS_TARGET_WRITE);

	dest->pub.next_output_byte = dest->buf;
	dest->pub.free_in_buffer = TARGET_BUFFER_SIZE;

	return TRUE;
}

/* Init dest method.
 */
static void
init_destination(j_compress_ptr cinfo)
{
	Dest *dest = (Dest *) cinfo->dest;

	dest->pub.next_output_byte = dest->buf;
	dest->pub.free_in_buffer = TARGET_BUFFER_SIZE;
}

/* Flush any remaining bytes to the output.
 */
static void
term_destination(j_compress_ptr cinfo)
{
	Dest *dest = (Dest *) cinfo->dest;

	if (vips_target_write(dest->target,
			dest->buf, TARGET_BUFFER_SIZE - dest->pub.free_in_buffer))
		ERREXIT(cinfo, JERR_VIPS_TARGET_WRITE);
}

/* Set dest to one of our objects.
 */
void
vips__jpeg_target_dest(j_compress_ptr cinfo, VipsTarget *target)
{
	Dest *dest;

	if (!cinfo->dest) /* first time for this JPEG object? */
		cinfo->dest =
			(struct jpeg_destination_mgr *) (*cinfo->mem->alloc_small)(
				(j_common_ptr) cinfo,
				JPOOL_PERMANENT,
				sizeof(Dest));

	dest = (Dest *) cinfo->dest;
	dest->pub.init_destination = init_destination;
	dest->pub.empty_output_buffer = empty_output_buffer;
	dest->pub.term_destination = term_destination;
	dest->target = target;
}

int
vips__jpeg_write_target(VipsImage *in, VipsTarget *target,
	int Q, const char *profile,
	gboolean optimize_coding, gboolean progressive,
	gboolean trellis_quant,
	gboolean overshoot_deringing, gboolean optimize_scans,
	int quant_table, VipsForeignSubsample subsample_mode,
	int restart_interval)
{
	Write *write;

	if (!(write = write_new()))
		return -1;

	/* Make jpeg compression object.
	 */
	if (setjmp(write->eman.jmp)) {
		/* Here for longjmp() during write_vips().
		 */
		write_destroy(write);

		return -1;
	}
	jpeg_create_compress(&write->cinfo);

	/* Attach output.
	 */
	vips__jpeg_target_dest(&write->cinfo, target);

	/* Convert! Write errors come back here as an error return.
	 */
	if (write_vips(write, in,
			Q, profile, optimize_coding, progressive,
			trellis_quant, overshoot_deringing, optimize_scans,
			quant_table, subsample_mode, restart_interval)) {
		write_destroy(write);
		return -1;
	}
	write_destroy(write);

	if (vips_target_end(target))
		return -1;

	return 0;
}

/* Some people want to be able to save as xxx.jfif. libjpeg will write as
 * JFIF if it can, but if you use features like CMYK or YCCK, you'll get a
 * regular JPEG. So saving as .jfif won't (by itself) guarantee strict JFIF
 * conformance.
 */
const char *vips__jpeg_suffs[] = { ".jpg", ".jpeg", ".jpe", ".jfif", NULL };

/* Write a region to a JPEG compress struct.
 */
static int
write_vips_region(Write *write, VipsRegion *region, VipsRect *rect,
	int Q, const char *profile,
	gboolean optimize_coding, gboolean progressive,
	VipsForeignKeep keep,
	gboolean trellis_quant, gboolean overshoot_deringing,
	gboolean optimize_scans, int quant_table,
	VipsForeignSubsample subsample_mode, int restart_interval)
{
	// the image we'll be writing
	VipsImage *in = region->im;
	VipsImage *x;

	set_cinfo(&write->cinfo, in, rect->width, rect->height,
		Q, optimize_coding, progressive,
		trellis_quant, overshoot_deringing, optimize_scans,
		quant_table, subsample_mode, restart_interval);

	/* Should have been converted for save.
	 */
	g_assert(in->BandFmt == VIPS_FORMAT_UCHAR);
	g_assert(in->Coding == VIPS_CODING_NONE);
	g_assert(in->Bands == 1 ||
		in->Bands == 3 ||
		in->Bands == 4);

	if (in->Bands == 4 &&
		in->Type == VIPS_INTERPRETATION_CMYK)
		// FIXME ... need to invert on the fly as we send pixels to
		// libjpeg
		write->invert = TRUE;

	/* Build VIPS output stuff now we know the image we'll be writing.
	 */
	if (!(write->row_pointer = VIPS_ARRAY(NULL, rect->height, JSAMPROW)))
		return -1;

	/* Write app0 and build compress tables.
	 */
	jpeg_start_compress(&write->cinfo, TRUE);

	/* Updating metadata, need to copy the image.
	 */
	if (vips_copy(in, &x, NULL))
		return -1;

	/* All the other APP chunks come next.
	 */
	if (vips__foreign_update_metadata(x, keep) ||
		write_metadata(write, x, profile)) {
		g_object_unref(x);
		return -1;
	}

	g_object_unref(x);

	/* Write data. Note that the write function grabs the longjmp()!
	 */
	if (write_jpeg_block(region, rect, write))
		return -1;

	/* We have to reinstate the setjmp() before we jpeg_finish_compress().
	 */
	if (setjmp(write->eman.jmp))
		return -1;

	/* This should only be called on a successful write.
	 */
	jpeg_finish_compress(&write->cinfo);

	return 0;
}

int
vips__jpeg_region_write_target(VipsRegion *region, VipsRect *rect,
	VipsTarget *target,
	int Q, const char *profile,
	gboolean optimize_coding, gboolean progressive,
	VipsForeignKeep keep, gboolean trellis_quant,
	gboolean overshoot_deringing, gboolean optimize_scans,
	int quant_table, VipsForeignSubsample subsample_mode,
	int restart_interval)
{
	Write *write;

	if (!(write = write_new()))
		return -1;

	/* Make jpeg compression object.
	 */
	if (setjmp(write->eman.jmp)) {
		/* Here for longjmp() during write_vips().
		 */
		write_destroy(write);

		return -1;
	}
	jpeg_create_compress(&write->cinfo);

	/* Attach output.
	 */
	vips__jpeg_target_dest(&write->cinfo, target);

	/* Convert! Write errors come back here as an error return.
	 */
	if (write_vips_region(write, region, rect,
			Q, profile, optimize_coding, progressive, keep,
			trellis_quant, overshoot_deringing, optimize_scans,
			quant_table, subsample_mode, restart_interval)) {
		write_destroy(write);
		return -1;
	}
	write_destroy(write);

	if (vips_target_end(target))
		return -1;

	return 0;
}

#else /*!HAVE_JPEG*/

int
vips__jpeg_region_write_target(VipsRegion *region, VipsRect *rect,
	VipsTarget *target,
	int Q, const char *profile,
	gboolean optimize_coding, gboolean progressive,
	VipsForeignKeep keep, gboolean trellis_quant,
	gboolean overshoot_deringing, gboolean optimize_scans,
	int quant_table, VipsForeignSubsample subsample_mode,
	int restart_interval)
{
	vips_error("vips2jpeg",
		"%s", _("libvips built without JPEG support"));
	return -1;
}

#endif /*HAVE_JPEG*/
