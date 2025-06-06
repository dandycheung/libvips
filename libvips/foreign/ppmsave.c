/* save to ppm
 *
 * 2/12/11
 * 	- wrap a class around the ppm writer
 * 13/11/19
 * 	- redone with targets
 * 18/6/20
 * 	- add "bitdepth" param, cf. tiffsave
 * 27/6/20
 * 	- add ppmsave_target
 * 20/11/20
 * 	- byteswap on save, if necessary [ewelot]
 * 2/12/20
 * 	- don't add date with @strip [ewelot]
 * 28/10/21
 * 	- add @format, default type by filename
 * 30/8/22
 *  - add ".pnm", save as image format [ewelot]
 * 9/3/23
 *	- pfm saves as linear 0-1 [NiHoel]
 *	- append spaces to the scale field to hit a 4 byte boundary for data
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
#define DEBUG_VERBOSE
#define DEBUG
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <glib/gi18n-lib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vips/vips.h>
#include <vips/internal.h>

#include "pforeign.h"

#ifdef HAVE_PPM

typedef struct _VipsForeignSavePpm VipsForeignSavePpm;

typedef int (*VipsSavePpmFn)(VipsForeignSavePpm *, VipsImage *, VipsPel *);

struct _VipsForeignSavePpm {
	VipsForeignSave parent_object;

	VipsTarget *target;
	VipsForeignPpmFormat format;
	gboolean ascii;
	int bitdepth;

	VipsSavePpmFn fn;

	/* Deprecated.
	 */
	gboolean squash;
};

typedef VipsForeignSaveClass VipsForeignSavePpmClass;

G_DEFINE_ABSTRACT_TYPE(VipsForeignSavePpm, vips_foreign_save_ppm,
	VIPS_TYPE_FOREIGN_SAVE);

static void
vips_foreign_save_ppm_dispose(GObject *gobject)
{
	VipsForeignSavePpm *ppm = (VipsForeignSavePpm *) gobject;

	VIPS_UNREF(ppm->target);

	G_OBJECT_CLASS(vips_foreign_save_ppm_parent_class)->dispose(gobject);
}

static int
vips_foreign_save_ppm_line_ascii(VipsForeignSavePpm *ppm,
	VipsImage *image, VipsPel *p)
{
	const int n_elements = image->Xsize * image->Bands;

	int i;

	for (i = 0; i < n_elements; i++) {
		switch (image->BandFmt) {
		case VIPS_FORMAT_UCHAR:
			vips_target_writef(ppm->target, "%d ", p[i]);
			break;

		case VIPS_FORMAT_USHORT:
			vips_target_writef(ppm->target, "%d ", ((unsigned short *) p)[i]);
			break;

		case VIPS_FORMAT_UINT:
			vips_target_writef(ppm->target, "%d ", ((unsigned int *) p)[i]);
			break;

		default:
			g_assert_not_reached();
		}
	}

	if (vips_target_writes(ppm->target, "\n"))
		return -1;

	return 0;
}

static int
vips_foreign_save_ppm_line_ascii_1bit(VipsForeignSavePpm *ppm,
	VipsImage *image, VipsPel *p)
{
	int x;

	for (x = 0; x < image->Xsize; x++)
		vips_target_writef(ppm->target, "%d ", p[x] > 127 ? 0 : 1);

	if (vips_target_writes(ppm->target, "\n"))
		return -1;

	return 0;
}

static int
vips_foreign_save_ppm_line_binary(VipsForeignSavePpm *ppm,
	VipsImage *image, VipsPel *p)
{
	if (vips_target_write(ppm->target, p, VIPS_IMAGE_SIZEOF_LINE(image)))
		return -1;

	return 0;
}

static int
vips_foreign_save_ppm_line_binary_1bit(VipsForeignSavePpm *ppm,
	VipsImage *image, VipsPel *p)
{
	int x;
	int bits;
	int n_bits;

	bits = 0;
	n_bits = 0;
	for (x = 0; x < image->Xsize; x++) {
		bits = VIPS_LSHIFT_INT(bits, 1);
		n_bits += 1;
		bits |= p[x] > 127 ? 0 : 1;

		if (n_bits == 8) {
			if (VIPS_TARGET_PUTC(ppm->target, bits))
				return -1;

			bits = 0;
			n_bits = 0;
		}
	}

	/* Flush any remaining bits in this line.
	 */
	if (n_bits &&
		VIPS_TARGET_PUTC(ppm->target, bits))
		return -1;

	return 0;
}

static int
vips_foreign_save_ppm_block(VipsRegion *region, VipsRect *area, void *a)
{
	VipsForeignSavePpm *ppm = (VipsForeignSavePpm *) a;
	VipsImage *image = region->im;

	int y;

	for (y = 0; y < area->height; y++) {
		VipsPel *p = VIPS_REGION_ADDR(region, 0, area->top + y);

		if (ppm->fn(ppm, image, p))
			return -1;
	}

	return 0;
}

static int
vips_foreign_save_ppm_build(VipsObject *object)
{
	VipsObjectClass *class = VIPS_OBJECT_GET_CLASS(object);
	VipsForeignSave *save = (VipsForeignSave *) object;
	VipsForeignSavePpm *ppm = (VipsForeignSavePpm *) object;
	VipsImage **t = (VipsImage **) vips_object_local_array(object, 4);

	VipsImage *image;
	char *magic;
	char *date;
	VipsInterpretation target_interpretation;
	int target_bands;

	if (VIPS_OBJECT_CLASS(vips_foreign_save_ppm_parent_class)->build(object))
		return -1;

	image = save->ready;

	/* We don't allow alpha. Just flatten it out.
	 */
	if (vips_image_hasalpha(image)) {
		if (vips_flatten(image, &t[0],
				"background", save->background,
				NULL))
			return -1;
		image = t[0];
	}

	/* ppm types to set the defaults for bitdepth etc.
	 *
	 *   pbm ... 1 band 1 bit
	 *   pgm ... 1 band many bit
	 *   ppm ... 3 band many bit
	 *   pfm ... 1 or 3 bands, 32 bit
	 *   pnm ... pick from input
	 */
	switch (ppm->format) {
	case VIPS_FOREIGN_PPM_FORMAT_PBM:
		if (!vips_object_argument_isset(object, "bitdepth"))
			ppm->bitdepth = 1;
		target_interpretation = VIPS_INTERPRETATION_B_W;
		target_bands = 1;
		break;

	case VIPS_FOREIGN_PPM_FORMAT_PGM:
		if (image->BandFmt == VIPS_FORMAT_USHORT)
			target_interpretation = VIPS_INTERPRETATION_GREY16;
		else
			target_interpretation = VIPS_INTERPRETATION_B_W;
		target_bands = 1;
		break;

	case VIPS_FOREIGN_PPM_FORMAT_PPM:
		if (image->BandFmt == VIPS_FORMAT_USHORT)
			target_interpretation = VIPS_INTERPRETATION_RGB16;
		else
			target_interpretation = VIPS_INTERPRETATION_sRGB;
		target_bands = 3;
		break;

	case VIPS_FOREIGN_PPM_FORMAT_PFM:
		target_interpretation = VIPS_INTERPRETATION_scRGB;
		if (image->Bands > 1)
			target_bands = 3;
		else
			// can have mono pfm, curiously
			target_bands = 1;
		break;

	case VIPS_FOREIGN_PPM_FORMAT_PNM:
	default:
		/* Just use the input interpretation and bands.
		 */
		target_interpretation = image->Type;
		target_bands = image->Bands;
		break;
	}

	if (vips_colourspace(image, &t[1], target_interpretation, NULL))
		return -1;
	image = t[1];

	/* Get bands right.
	 */
	if (image->Bands > target_bands) {
		if (vips_extract_band(image, &t[2], 0,
				"n", target_bands,
				NULL))
			return -1;
		image = t[2];
	}
	if (image->Bands < target_bands) {
		vips_error(class->nickname, "%s", _("too few bands for format"));
		return -1;
	}

	/* Handle the deprecated squash parameter.
	 */
	if (ppm->squash)
		ppm->bitdepth = 1;

	if (vips_check_uintorf("vips2ppm", image) ||
		vips_check_bands_1or3("vips2ppm", image) ||
		vips_check_uncoded("vips2ppm", image) ||
		vips_image_pio_input(image))
		return -1;

	if (ppm->ascii &&
		image->BandFmt == VIPS_FORMAT_FLOAT) {
		g_warning("float images must be binary -- disabling ascii");
		ppm->ascii = FALSE;
	}

	/* One bit images must come from a 8 bit, one band source.
	 */
	if (ppm->bitdepth &&
		(image->Bands != 1 ||
		 image->BandFmt != VIPS_FORMAT_UCHAR)) {
		g_warning("can only save 1 band uchar images as 1 bit -- "
				  "disabling 1 bit save");
		ppm->bitdepth = 0;
	}

	magic = "unset";
	if (image->BandFmt == VIPS_FORMAT_FLOAT &&
		image->Bands == 3)
		magic = "PF";
	else if (image->BandFmt == VIPS_FORMAT_FLOAT &&
		image->Bands == 1)
		magic = "Pf";
	else if (image->Bands == 1 &&
		ppm->ascii &&
		ppm->bitdepth)
		magic = "P1";
	else if (image->Bands == 1 &&
		ppm->ascii)
		magic = "P2";
	else if (image->Bands == 1 &&
		!ppm->ascii &&
		ppm->bitdepth)
		magic = "P4";
	else if (image->Bands == 1 &&
		!ppm->ascii)
		magic = "P5";
	else if (image->Bands == 3 &&
		ppm->ascii)
		magic = "P3";
	else if (image->Bands == 3 &&
		!ppm->ascii)
		magic = "P6";
	else
		g_assert_not_reached();

	vips_target_writef(ppm->target, "%s\n", magic);
	if (save->keep & VIPS_FOREIGN_KEEP_OTHER) {
		date = vips__get_iso8601();
		vips_target_writef(ppm->target, "#vips2ppm - %s\n", date);
		g_free(date);
	}
	vips_target_writef(ppm->target, "%d %d\n", image->Xsize, image->Ysize);

	if (!ppm->bitdepth)
		switch (image->BandFmt) {
		case VIPS_FORMAT_UCHAR:
			vips_target_writef(ppm->target, "%d\n", UCHAR_MAX);
			break;

		case VIPS_FORMAT_USHORT:
			vips_target_writef(ppm->target, "%d\n", USHRT_MAX);
			break;

		case VIPS_FORMAT_UINT:
			vips_target_writef(ppm->target, "%d\n", UINT_MAX);
			break;

		case VIPS_FORMAT_FLOAT: {
			double scale;
			char buf[G_ASCII_DTOSTR_BUF_SIZE];

			scale = 1.0;
			if (vips_image_get_typeof(image, "pfm-scale"))
				vips_image_get_double(image, "pfm-scale", &scale);

			/* Need to be locale independent.
			 */
			g_ascii_dtostr(buf, G_ASCII_DTOSTR_BUF_SIZE, scale);
			vips_target_writes(ppm->target, buf);

			/* Add some secret trailing spaces to the scale field to try to
			 * get the data to land on a 4 byte (the largest pixel ppm can
			 * handle) boundary.
			 *
			 * This means values in the file will be aligned when it's read
			 * back, making vector operations like byteswap a lot quicker.
			 */
			if (!ppm->ascii) {
				gint64 position = vips_target_seek(ppm->target, 0, SEEK_CUR);
				// +1 for the \n after the padding
				int padding = 4 - (position + 1) % 4;

				vips_target_writef(ppm->target, "%*c", padding, ' ');
			}

			vips_target_writes(ppm->target, "\n");
		} break;

		default:
			g_assert_not_reached();
		}

	if (ppm->bitdepth)
		ppm->fn = ppm->ascii
			? vips_foreign_save_ppm_line_ascii_1bit
			: vips_foreign_save_ppm_line_binary_1bit;
	else
		ppm->fn = ppm->ascii
			? vips_foreign_save_ppm_line_ascii
			: vips_foreign_save_ppm_line_binary;

	/* 16 and 32-bit binary write might need byteswapping.
	 */
	if (!ppm->ascii &&
		(image->BandFmt == VIPS_FORMAT_USHORT ||
		 image->BandFmt == VIPS_FORMAT_UINT ||
		 image->BandFmt == VIPS_FORMAT_FLOAT)) {
		if (vips__byteswap_bool(image, &t[3], !vips_amiMSBfirst()))
			return -1;
		image = t[3];
	}

	if (vips_sink_disc(image, vips_foreign_save_ppm_block, ppm))
		return -1;

	if (vips_target_end(ppm->target))
		return -1;

	return 0;
}

/* Save a bit of typing.
 */
#define UC VIPS_FORMAT_UCHAR
#define C VIPS_FORMAT_CHAR
#define US VIPS_FORMAT_USHORT
#define S VIPS_FORMAT_SHORT
#define UI VIPS_FORMAT_UINT
#define I VIPS_FORMAT_INT
#define F VIPS_FORMAT_FLOAT
#define X VIPS_FORMAT_COMPLEX
#define D VIPS_FORMAT_DOUBLE
#define DX VIPS_FORMAT_DPCOMPLEX

static VipsBandFormat bandfmt_ppm[10] = {
	/* Band format:  UC  C   US  S   UI  I   F  X  D  DX */
	/* Promotion: */ UC, UC, US, US, UI, UI, F, F, F, F
};

static void
vips_foreign_save_ppm_class_init(VipsForeignSavePpmClass *class)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(class);
	VipsObjectClass *object_class = (VipsObjectClass *) class;
	VipsForeignSaveClass *save_class = (VipsForeignSaveClass *) class;

	gobject_class->dispose = vips_foreign_save_ppm_dispose;
	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "ppmsave_base";
	object_class->description = _("save to ppm");
	object_class->build = vips_foreign_save_ppm_build;

	save_class->saveable = VIPS_FOREIGN_SAVEABLE_ANY;
	save_class->format_table = bandfmt_ppm;

	VIPS_ARG_ENUM(class, "format", 2,
		_("Format"),
		_("Format to save in"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsForeignSavePpm, format),
		VIPS_TYPE_FOREIGN_PPM_FORMAT,
		VIPS_FOREIGN_PPM_FORMAT_PPM);

	VIPS_ARG_BOOL(class, "ascii", 10,
		_("ASCII"),
		_("Save as ascii"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsForeignSavePpm, ascii),
		FALSE);

	VIPS_ARG_INT(class, "bitdepth", 15,
		_("Bit depth"),
		_("Set to 1 to write as a 1 bit image"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsForeignSavePpm, bitdepth),
		0, 1, 0);

	VIPS_ARG_BOOL(class, "squash", 11,
		_("Squash"),
		_("Save as one bit"),
		VIPS_ARGUMENT_OPTIONAL_INPUT | VIPS_ARGUMENT_DEPRECATED,
		G_STRUCT_OFFSET(VipsForeignSavePpm, squash),
		FALSE);
}

static void
vips_foreign_save_ppm_init(VipsForeignSavePpm *ppm)
{
	ppm->format = VIPS_FOREIGN_PPM_FORMAT_PPM;
}

typedef struct _VipsForeignSavePpmFile {
	VipsForeignSavePpm parent_object;

	char *filename;
} VipsForeignSavePpmFile;

typedef VipsForeignSavePpmClass VipsForeignSavePpmFileClass;

G_DEFINE_TYPE(VipsForeignSavePpmFile, vips_foreign_save_ppm_file,
	vips_foreign_save_ppm_get_type());

static int
vips_foreign_save_ppm_file_build(VipsObject *object)
{
	VipsForeignSavePpm *ppm = (VipsForeignSavePpm *) object;
	VipsForeignSavePpmFile *file = (VipsForeignSavePpmFile *) object;

	if (file->filename &&
		!(ppm->target = vips_target_new_to_file(file->filename)))
		return -1;

	if (vips_iscasepostfix(file->filename, ".pbm"))
		ppm->format = VIPS_FOREIGN_PPM_FORMAT_PBM;
	else if (vips_iscasepostfix(file->filename, ".pgm"))
		ppm->format = VIPS_FOREIGN_PPM_FORMAT_PGM;
	else if (vips_iscasepostfix(file->filename, ".pfm"))
		ppm->format = VIPS_FOREIGN_PPM_FORMAT_PFM;
	else if (vips_iscasepostfix(file->filename, ".pnm"))
		ppm->format = VIPS_FOREIGN_PPM_FORMAT_PNM;

	return VIPS_OBJECT_CLASS(vips_foreign_save_ppm_file_parent_class)
		->build(object);
}

static void
vips_foreign_save_ppm_file_class_init(VipsForeignSavePpmFileClass *class)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(class);
	VipsObjectClass *object_class = (VipsObjectClass *) class;
	VipsForeignClass *foreign_class = (VipsForeignClass *) class;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "ppmsave";
	object_class->description = _("save image to ppm file");
	object_class->build = vips_foreign_save_ppm_file_build;

	foreign_class->suffs = vips__ppm_suffs;

	VIPS_ARG_STRING(class, "filename", 1,
		_("Filename"),
		_("Filename to save to"),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsForeignSavePpmFile, filename),
		NULL);
}

static void
vips_foreign_save_ppm_file_init(VipsForeignSavePpmFile *file)
{
}

typedef struct _VipsForeignSavePpmTarget {
	VipsForeignSavePpm parent_object;

	VipsTarget *target;
} VipsForeignSavePpmTarget;

typedef VipsForeignSavePpmClass VipsForeignSavePpmTargetClass;

G_DEFINE_TYPE(VipsForeignSavePpmTarget, vips_foreign_save_ppm_target,
	vips_foreign_save_ppm_get_type());

static int
vips_foreign_save_ppm_target_build(VipsObject *object)
{
	VipsForeignSavePpm *ppm = (VipsForeignSavePpm *) object;
	VipsForeignSavePpmTarget *target =
		(VipsForeignSavePpmTarget *) object;

	if (target->target) {
		ppm->target = target->target;
		g_object_ref(ppm->target);
	}

	return VIPS_OBJECT_CLASS(vips_foreign_save_ppm_target_parent_class)
		->build(object);
}

static void
vips_foreign_save_ppm_target_class_init(
	VipsForeignSavePpmTargetClass *class)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(class);
	VipsObjectClass *object_class = (VipsObjectClass *) class;
	VipsForeignClass *foreign_class = (VipsForeignClass *) class;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "ppmsave_target";
	object_class->build = vips_foreign_save_ppm_target_build;

	foreign_class->suffs = vips__save_ppm_suffs;

	VIPS_ARG_OBJECT(class, "target", 1,
		_("Target"),
		_("Target to save to"),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsForeignSavePpmTarget, target),
		VIPS_TYPE_TARGET);
}

static void
vips_foreign_save_ppm_target_init(VipsForeignSavePpmTarget *target)
{
}

typedef VipsForeignSavePpmTarget VipsForeignSavePbmTarget;
typedef VipsForeignSavePpmTargetClass VipsForeignSavePbmTargetClass;

G_DEFINE_TYPE(VipsForeignSavePbmTarget, vips_foreign_save_pbm_target,
	vips_foreign_save_ppm_target_get_type());

static void
vips_foreign_save_pbm_target_class_init(
	VipsForeignSavePbmTargetClass *class)
{
	VipsObjectClass *object_class = (VipsObjectClass *) class;
	VipsForeignClass *foreign_class = (VipsForeignClass *) class;
	VipsOperationClass *operation_class = (VipsOperationClass *) class;

	object_class->nickname = "pbmsave_target";
	object_class->description = _("save image in pbm format");

	foreign_class->suffs = vips__save_pbm_suffs;

	/* Hide from UI.
	 */
	operation_class->flags |= VIPS_OPERATION_DEPRECATED;
}

static void
vips_foreign_save_pbm_target_init(VipsForeignSavePbmTarget *target)
{
	VipsForeignSavePpm *ppm = (VipsForeignSavePpm *) target;

	ppm->format = VIPS_FOREIGN_PPM_FORMAT_PBM;
}

typedef VipsForeignSavePpmTarget VipsForeignSavePgmTarget;
typedef VipsForeignSavePpmTargetClass VipsForeignSavePgmTargetClass;

G_DEFINE_TYPE(VipsForeignSavePgmTarget, vips_foreign_save_pgm_target,
	vips_foreign_save_ppm_target_get_type());

static void
vips_foreign_save_pgm_target_class_init(
	VipsForeignSavePgmTargetClass *class)
{
	VipsObjectClass *object_class = (VipsObjectClass *) class;
	VipsForeignClass *foreign_class = (VipsForeignClass *) class;
	VipsOperationClass *operation_class = (VipsOperationClass *) class;

	object_class->nickname = "pgmsave_target";
	object_class->description = _("save image in pgm format");

	foreign_class->suffs = vips__save_pgm_suffs;

	/* Hide from UI.
	 */
	operation_class->flags |= VIPS_OPERATION_DEPRECATED;
}

static void
vips_foreign_save_pgm_target_init(VipsForeignSavePgmTarget *target)
{
	VipsForeignSavePpm *ppm = (VipsForeignSavePpm *) target;

	ppm->format = VIPS_FOREIGN_PPM_FORMAT_PGM;
}

typedef VipsForeignSavePpmTarget VipsForeignSavePfmTarget;
typedef VipsForeignSavePpmTargetClass VipsForeignSavePfmTargetClass;

G_DEFINE_TYPE(VipsForeignSavePfmTarget, vips_foreign_save_pfm_target,
	vips_foreign_save_ppm_target_get_type());

static void
vips_foreign_save_pfm_target_class_init(
	VipsForeignSavePfmTargetClass *class)
{
	VipsObjectClass *object_class = (VipsObjectClass *) class;
	VipsForeignClass *foreign_class = (VipsForeignClass *) class;
	VipsOperationClass *operation_class = (VipsOperationClass *) class;

	object_class->nickname = "pfmsave_target";
	object_class->description = _("save image in pfm format");

	foreign_class->suffs = vips__save_pfm_suffs;

	/* Hide from UI.
	 */
	operation_class->flags |= VIPS_OPERATION_DEPRECATED;
}

static void
vips_foreign_save_pfm_target_init(VipsForeignSavePfmTarget *target)
{
	VipsForeignSavePpm *ppm = (VipsForeignSavePpm *) target;

	ppm->format = VIPS_FOREIGN_PPM_FORMAT_PFM;
}

typedef VipsForeignSavePpmTarget VipsForeignSavePnmTarget;
typedef VipsForeignSavePpmTargetClass VipsForeignSavePnmTargetClass;

G_DEFINE_TYPE(VipsForeignSavePnmTarget, vips_foreign_save_pnm_target,
	vips_foreign_save_ppm_target_get_type());

static void
vips_foreign_save_pnm_target_class_init(
	VipsForeignSavePfmTargetClass *class)
{
	VipsObjectClass *object_class = (VipsObjectClass *) class;
	VipsForeignClass *foreign_class = (VipsForeignClass *) class;
	VipsOperationClass *operation_class = (VipsOperationClass *) class;

	object_class->nickname = "pnmsave_target";
	object_class->description = _("save image in pnm format");

	foreign_class->suffs = vips__save_pnm_suffs;

	/* Hide from UI.
	 */
	operation_class->flags |= VIPS_OPERATION_DEPRECATED;
}

static void
vips_foreign_save_pnm_target_init(VipsForeignSavePfmTarget *target)
{
	VipsForeignSavePpm *ppm = (VipsForeignSavePpm *) target;

	ppm->format = VIPS_FOREIGN_PPM_FORMAT_PNM;
}

#endif /*HAVE_PPM*/

/**
 * vips_ppmsave: (method)
 * @in: image to save
 * @filename: file to write to
 * @...: `NULL`-terminated list of optional named arguments
 *
 * Write a VIPS image to a file as PPM.
 *
 * It can write 1, 8, 16 or
 * 32 bit unsigned integer images, float images, colour or monochrome,
 * stored as binary or ASCII.
 * Integer images of more than 8 bits can only be stored in ASCII.
 *
 * When writing float (PFM) images the scale factor is set from the
 * "pfm-scale" metadata.
 *
 * Set @ascii to `TRUE` to write as human-readable ASCII. Normally data is
 * written in binary.
 *
 * Set @bitdepth to 1 to write a one-bit image.
 *
 * @format defaults to the sub-type for this filename suffix.
 *
 * ::: tip "Optional arguments"
 *     * @format: [enum@ForeignPpmFormat], format to save in
 *     * @ascii: `gboolean`, save as ASCII rather than binary
 *     * @bitdepth: `gint`, bitdepth to save at
 *
 * ::: seealso
 *     [method@Image.write_to_file].
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_ppmsave(VipsImage *in, const char *filename, ...)
{
	va_list ap;
	int result;

	va_start(ap, filename);
	result = vips_call_split("ppmsave", ap, in, filename);
	va_end(ap);

	return result;
}

/**
 * vips_ppmsave_target: (method)
 * @in: image to save
 * @target: save image to this target
 * @...: `NULL`-terminated list of optional named arguments
 *
 * As [method@Image.ppmsave], but save to a target.
 *
 * ::: tip "Optional arguments"
 *     * @format: [enum@ForeignPpmFormat], format to save in
 *     * @ascii: `gboolean`, save as ASCII rather than binary
 *     * @bitdepth: `gint`, bitdepth to save at
 *
 * ::: seealso
 *     [method@Image.ppmsave].
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_ppmsave_target(VipsImage *in, VipsTarget *target, ...)
{
	va_list ap;
	int result;

	va_start(ap, target);
	result = vips_call_split("ppmsave_target", ap, in, target);
	va_end(ap);

	return result;
}
