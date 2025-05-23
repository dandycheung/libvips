/* tracked memory
 *
 * 2/11/99 JC
 *	- from im_open.c and callback.c
 *	- malloc tracking stuff added
 * 11/3/01 JC
 * 	- im_strncpy() added
 * 20/4/01 JC
 * 	- im_(v)snprintf() added
 * 6/7/05
 *	- more tracking for DEBUGM
 * 20/10/06
 * 	- return NULL for size <= 0
 * 11/5/06
 * 	- abort() on malloc() failure with DEBUG
 * 20/10/09
 * 	- gtkdoc comment
 * 6/11/09
 *	- im_malloc()/im_free() now call g_try_malloc/g_free() ... removes
 *	  confusion over whether to use im_free() or g_free() for things like
 *	  im_header_string()
 * 21/9/11
 * 	- rename as vips_tracked_malloc() to emphasise difference from
 * 	  g_malloc()/g_free()
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <glib/gi18n-lib.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_IO_H
#include <io.h>
#endif /*HAVE_IO_H*/
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#if defined(HAVE__ALIGNED_MALLOC) || defined(HAVE_MEMALIGN)
#include <malloc.h>
#endif

#include <vips/vips.h>

/* g_assert_not_reached() on memory errors.
#define DEBUG
 */

/* Track malloc/free and open/close.
#define DEBUG_VERBOSE_MEM
#define DEBUG_VERBOSE_FD
 */

#ifdef DEBUG
#warning DEBUG on in libsrc/iofuncs/memory.c
#endif /*DEBUG*/

static int vips_tracked_allocs = 0;
static size_t vips_tracked_mem = 0;
static int vips_tracked_files = 0;
static size_t vips_tracked_mem_highwater = 0;
static GMutex vips_tracked_mutex;

/**
 * VIPS_NEW:
 * @OBJ: allocate memory local to @OBJ, or `NULL` for no auto-free
 * @T: type of thing to allocate
 *
 * Allocate memory for a thing of type @T. The memory is not
 * cleared.
 *
 * This macro cannot fail. See [func@tracked_malloc] if you are
 * allocating large amounts of memory.
 *
 * ::: seealso
 *     [func@malloc].
 *
 * Returns: A pointer of type @T *.
 */

/**
 * VIPS_ARRAY:
 * @OBJ: allocate memory local to @OBJ, or `NULL` for no auto-free
 * @N: number of @T 's to allocate
 * @T: type of thing to allocate
 *
 * Allocate memory for an array of objects of type @T. The memory is not
 * cleared.
 *
 * This macro cannot fail. See [func@tracked_malloc] if you are
 * allocating large amounts of memory.
 *
 * ::: seealso
 *     [func@malloc].
 *
 * Returns: A pointer of type @T *.
 */

static void
vips_malloc_cb(VipsObject *object, char *buf)
{
	g_free(buf);
}

/**
 * vips_malloc:
 * @object: (nullable): allocate memory local to this [class@Object], or `NULL`
 * @size: number of bytes to allocate
 *
 * [func@GLib.malloc] local to @object, that is, the memory will be automatically
 * freed for you when the object is closed. If @object is `NULL`, you need to
 * free the memory explicitly with [func@GLib.free].
 *
 * This function cannot fail. See [func@tracked_malloc] if you are
 * allocating large amounts of memory.
 *
 * ::: seealso
 *     [func@tracked_malloc].
 *
 * Returns: (transfer full): a pointer to the allocated memory.
 */
void *
vips_malloc(VipsObject *object, size_t size)
{
	void *buf;

	buf = g_malloc0(size);

	if (object) {
		g_signal_connect(object, "postclose",
			G_CALLBACK(vips_malloc_cb), buf);
		object->local_memory += size;
	}

	return buf;
}

/**
 * vips_strdup:
 * @object: (nullable): allocate memory local to this [class@Object], or `NULL`
 * @str: string to copy
 *
 * [func@GLib.strdup] a string. When @object is freed, the string will be freed for
 * you.  If @object is `NULL`, you need to
 * free the memory yourself with [func@GLib.free].
 *
 * This function cannot fail.
 *
 * ::: seealso
 *     [func@malloc].
 *
 * Returns: (transfer full): a pointer to the allocated memory
 */
char *
vips_strdup(VipsObject *object, const char *str)
{
	char *str_dup;

	str_dup = g_strdup(str);

	if (object) {
		g_signal_connect(object, "postclose",
			G_CALLBACK(vips_malloc_cb), str_dup);
		object->local_memory += strlen(str);
	}

	return str_dup;
}

/**
 * vips_tracked_free:
 * @s: (transfer full): memory to free
 *
 * Only use it to free memory that was
 * previously allocated with [func@tracked_malloc]
 * with a `NULL` first argument.
 *
 * ::: seealso
 *     [func@tracked_malloc].
 */
void
vips_tracked_free(void *s)
{
	/* Keep the size of the alloc in the previous 16 bytes. Ensures
	 * alignment rules are kept.
	 */
	void *start = (void *) ((char *) s - 16);
	size_t size = *((size_t *) start);

	g_mutex_lock(&vips_tracked_mutex);

#ifdef DEBUG_VERBOSE_MEM
	printf("vips_tracked_free: %p, %zd bytes\n", s, size);
#endif /*DEBUG_VERBOSE_MEM*/

	if (vips_tracked_allocs <= 0)
		g_warning("vips_free: too many frees");
	if (vips_tracked_mem < size)
		g_warning("vips_free: too much free");

	vips_tracked_mem -= size;
	vips_tracked_allocs -= 1;

	g_mutex_unlock(&vips_tracked_mutex);

	g_free(start);

	VIPS_GATE_FREE(size);
}

/**
 * vips_tracked_aligned_free:
 * @s: (transfer full): memory to free
 *
 * Only use it to free memory that was
 * previously allocated with [func@tracked_aligned_alloc]
 * with a `NULL` first argument.
 *
 * ::: seealso
 *     [func@tracked_aligned_alloc].
 */
void
vips_tracked_aligned_free(void *s)
{
	void *start = (size_t *) s - 1;
	size_t size = *((size_t *) start);

	g_mutex_lock(&vips_tracked_mutex);

#ifdef DEBUG_VERBOSE
	printf("vips_tracked_aligned_free: %p, %zd bytes\n", s, size);
#endif /*DEBUG_VERBOSE*/

	if (vips_tracked_allocs <= 0)
		g_warning("vips_free: too many frees");
	if (vips_tracked_mem < size)
		g_warning("vips_free: too much free");

	vips_tracked_mem -= size;
	vips_tracked_allocs -= 1;

	g_mutex_unlock(&vips_tracked_mutex);

#ifdef HAVE__ALIGNED_MALLOC
	_aligned_free(start);
#else /*defined(HAVE_POSIX_MEMALIGN) || defined(HAVE_MEMALIGN)*/
	free(start);
#endif

	VIPS_GATE_FREE(size);
}

/**
 * vips_tracked_malloc:
 * @size: number of bytes to allocate
 *
 * Allocate an area of memory that will be tracked by [func@tracked_get_mem]
 * and friends.
 *
 * If allocation fails, [func@tracked_malloc] returns `NULL` and
 * sets an error message.
 *
 * You must only free the memory returned with [func@tracked_free].
 *
 * ::: seealso
 *     [func@tracked_free], [func@malloc].
 *
 * Returns: (transfer full): a pointer to the allocated memory, or `NULL` on error.
 */
void *
vips_tracked_malloc(size_t size)
{
	void *buf;

	/* Need an extra sizeof(size_t) bytes to track
	 * size of this block. Ask for an extra 16 to make sure we don't break
	 * alignment rules.
	 */
	size += 16;

	if (!(buf = g_try_malloc0(size))) {
#ifdef DEBUG
		g_assert_not_reached();
#endif /*DEBUG*/

		vips_error("vips_tracked",
			_("out of memory -- size == %dMB"),
			(int) (size / (1024.0 * 1024.0)));
		g_warning("out of memory -- size == %dMB",
			(int) (size / (1024.0 * 1024.0)));

		return NULL;
	}

	g_mutex_lock(&vips_tracked_mutex);

	*((size_t *) buf) = size;
	buf = (void *) ((char *) buf + 16);

	vips_tracked_mem += size;
	if (vips_tracked_mem > vips_tracked_mem_highwater)
		vips_tracked_mem_highwater = vips_tracked_mem;
	vips_tracked_allocs += 1;

#ifdef DEBUG_VERBOSE_MEM
	printf("vips_tracked_malloc: %p, %zd bytes\n", buf, size);
#endif /*DEBUG_VERBOSE_MEM*/

	g_mutex_unlock(&vips_tracked_mutex);

	VIPS_GATE_MALLOC(size);

	return buf;
}

/**
 * vips_tracked_aligned_alloc:
 * @size: number of bytes to allocate
 * @align: specifies the alignment
 *
 * Allocate an area of memory aligned on a boundary specified
 * by @align that will be tracked by [func@tracked_get_mem]
 * and friends.
 *
 * If allocation fails, [func@tracked_aligned_alloc] returns `NULL`
 * and sets an error message.
 *
 * You must only free the memory returned with [func@tracked_aligned_free].
 *
 * ::: seealso
 *     [func@tracked_malloc], [func@tracked_aligned_free], [func@malloc].
 *
 * Returns: (transfer full): a pointer to the allocated memory, or `NULL` on error.
 */
void *
vips_tracked_aligned_alloc(size_t size, size_t align)
{
	void *buf;

	g_assert(!(align & (align - 1)));

	/* Need an extra sizeof(size_t) bytes to track
	 * size of this block.
	 */
	size += sizeof(size_t);

#ifdef HAVE__ALIGNED_MALLOC
	if (!(buf = _aligned_malloc(size, align))) {
#elif defined(HAVE_POSIX_MEMALIGN)
	if (posix_memalign(&buf, align, size)) {
#elif defined(HAVE_MEMALIGN)
	if (!(buf = memalign(align, size))) {
#else
#error Missing aligned alloc implementation
#endif
#ifdef DEBUG
		g_assert_not_reached();
#endif /*DEBUG*/

		vips_error("vips_tracked",
			_("out of memory -- size == %dMB"),
			(int) (size / (1024.0 * 1024.0)));
		g_warning("out of memory -- size == %dMB",
			(int) (size / (1024.0 * 1024.0)));

		return NULL;
	}

	memset(buf, 0, size);

	g_mutex_lock(&vips_tracked_mutex);

	*((size_t *) buf) = size;

	vips_tracked_mem += size;
	if (vips_tracked_mem > vips_tracked_mem_highwater)
		vips_tracked_mem_highwater = vips_tracked_mem;
	vips_tracked_allocs += 1;

#ifdef DEBUG_VERBOSE
	printf("vips_tracked_aligned_alloc: %p, %zd bytes\n", buf, size);
#endif /*DEBUG_VERBOSE*/

	g_mutex_unlock(&vips_tracked_mutex);

	VIPS_GATE_MALLOC(size);

	return (void *) ((size_t *) buf + 1);
}

/**
 * vips_tracked_open:
 * @pathname: name of file to open
 * @flags: flags for `open()`
 * @mode: open mode
 *
 * Exactly as [`open()`](man:open(2)), but the number of files currently open via
 * [func@tracked_open] is available via [func@tracked_get_files]. This is used
 * by the vips operation cache to drop cache when the number of files
 * available is low.
 *
 * You must only close the file descriptor with [func@tracked_close].
 *
 * @pathname should be utf8.
 *
 * ::: seealso
 *     [func@tracked_close], [func@tracked_get_files].
 *
 * Returns: a file descriptor, or -1 on error.
 */
int
vips_tracked_open(const char *pathname, int flags, int mode)
{
	int fd;

	if ((fd = vips__open(pathname, flags, mode)) == -1)
		return -1;

	g_mutex_lock(&vips_tracked_mutex);

	vips_tracked_files += 1;
#ifdef DEBUG_VERBOSE_FD
	printf("vips_tracked_open: %s = %d (%d)\n",
		pathname, fd, vips_tracked_files);
#endif /*DEBUG_VERBOSE_FD*/

	g_mutex_unlock(&vips_tracked_mutex);

	return fd;
}

/**
 * vips_tracked_close:
 * @fd: file to `close()`
 *
 * Exactly as [`close()`](man:close(2)), but update the number of files currently open via
 * [func@tracked_get_files]. This is used
 * by the vips operation cache to drop cache when the number of files
 * available is low.
 *
 * You must only close file descriptors opened with [func@tracked_open].
 *
 * ::: seealso
 *     [func@tracked_open], [func@tracked_get_files].
 *
 * Returns: a file descriptor, or -1 on error.
 */
int
vips_tracked_close(int fd)
{
	int result;

	g_mutex_lock(&vips_tracked_mutex);

	/* libvips uses fd -1 to mean invalid descriptor.
	 */
	g_assert(fd != -1);
	g_assert(vips_tracked_files > 0);

	vips_tracked_files -= 1;
#ifdef DEBUG_VERBOSE_FD
	printf("vips_tracked_close: %d (%d)\n", fd, vips_tracked_files);
	printf("   from thread %p\n", g_thread_self());
#endif /*DEBUG_VERBOSE_FD*/

	g_mutex_unlock(&vips_tracked_mutex);

	result = close(fd);

	return result;
}

/**
 * vips_tracked_get_mem:
 *
 * Returns the number of bytes currently allocated via [func@malloc] and
 * friends. vips uses this figure to decide when to start dropping cache, see
 * [class@Operation].
 *
 * Returns: the number of currently allocated bytes
 */
size_t
vips_tracked_get_mem(void)
{
	size_t mem;

	g_mutex_lock(&vips_tracked_mutex);

	mem = vips_tracked_mem;

	g_mutex_unlock(&vips_tracked_mutex);

	return mem;
}

/**
 * vips_tracked_get_mem_highwater:
 *
 * Returns the largest number of bytes simultaneously allocated via
 * [func@tracked_malloc]. Handy for estimating max memory requirements for a
 * program.
 *
 * Returns: the largest number of currently allocated bytes
 */
size_t
vips_tracked_get_mem_highwater(void)
{
	size_t mx;

	g_mutex_lock(&vips_tracked_mutex);

	mx = vips_tracked_mem_highwater;

	g_mutex_unlock(&vips_tracked_mutex);

	return mx;
}

/**
 * vips_tracked_get_allocs:
 *
 * Returns the number of active allocations.
 *
 * Returns: the number of active allocations
 */
int
vips_tracked_get_allocs(void)
{
	int n;

	g_mutex_lock(&vips_tracked_mutex);

	n = vips_tracked_allocs;

	g_mutex_unlock(&vips_tracked_mutex);

	return n;
}

/**
 * vips_tracked_get_files:
 *
 * Returns the number of open files.
 *
 * Returns: the number of open files
 */
int
vips_tracked_get_files(void)
{
	int n;

	g_mutex_lock(&vips_tracked_mutex);

	n = vips_tracked_files;

	g_mutex_unlock(&vips_tracked_mutex);

	return n;
}
