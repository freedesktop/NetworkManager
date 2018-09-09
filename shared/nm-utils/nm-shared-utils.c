/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * (C) Copyright 2016 Red Hat, Inc.
 */

#include "nm-default.h"

#include "nm-shared-utils.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nm-str-buf.h"

/*****************************************************************************/

const void *const _NM_PTRARRAY_EMPTY[1] = { NULL };

/*****************************************************************************/

const NMIPAddr nm_ip_addr_zero = { 0 };

/*****************************************************************************/

gsize
nm_utils_get_next_realloc_size (gboolean true_realloc, gsize requested)
{
	gsize n;

	/* https://doc.qt.io/qt-5/containers.html#growth-strategies */

	if (requested <= 40) {
		/* small allocations. Increase in small steps of 8 bytes.
		 *
		 * We get thus sizes of 8, 16, 32, 40. */
		if (requested <= 8)
			return 8;
		if (requested <= 16)
			return 16;
		if (requested <= 32)
			return 32;
		return 40;
	}

	if (requested <= 0x2000 - 24
	    || (   !true_realloc
	        && requested <= ((G_MAXSIZE / 2) + 1) - 24)) {
		/* mid sized allocations. Double the size, minus 24 bytes head-room.
		 *
		 * With !true_realloc, we also do this scheme, because we don't actually
		 * realloc(). Hence, we always want to grow the buffer exponentially.
		 *
		 * We get thus sizes of 104, 232, 488, 1000, 2024, 4072, 8168... */
		n = 128;
		while (n - 24 < requested)
			n <<= 1;
		return n - 24;
	}

	if (requested > G_MAXSIZE - 0x0FFF)
		return G_MAXSIZE;

	/* For large allocations (with !do_bzero_mem) we allocate memory in chunks of
	 * 4K (- 24 bytes head room), assuming that the memory gets mmapped and thus
	 * realloc() is efficient by just reordering pages. */
	return ((requested + 0x0FFF) & ~((gsize) 0x0FFF)) - 24;
}

/*****************************************************************************/

void
nm_utils_strbuf_append_c (char **buf, gsize *len, char c)
{
	switch (*len) {
	case 0:
		return;
	case 1:
		(*buf)[0] = '\0';
		*len = 0;
		(*buf)++;
		return;
	default:
		(*buf)[0] = c;
		(*buf)[1] = '\0';
		(*len)--;
		(*buf)++;
		return;
	}
}

void
nm_utils_strbuf_append_str (char **buf, gsize *len, const char *str)
{
	gsize src_len;

	switch (*len) {
	case 0:
		return;
	case 1:
		if (!str || !*str) {
			(*buf)[0] = '\0';
			return;
		}
		(*buf)[0] = '\0';
		*len = 0;
		(*buf)++;
		return;
	default:
		if (!str || !*str) {
			(*buf)[0] = '\0';
			return;
		}
		src_len = g_strlcpy (*buf, str, *len);
		if (src_len >= *len) {
			*buf = &(*buf)[*len];
			*len = 0;
		} else {
			*buf = &(*buf)[src_len];
			*len -= src_len;
		}
		return;
	}
}

void
nm_utils_strbuf_append (char **buf, gsize *len, const char *format, ...)
{
	char *p = *buf;
	va_list args;
	int retval;

	if (*len == 0)
		return;

	va_start (args, format);
	retval = g_vsnprintf (p, *len, format, args);
	va_end (args);

	if ((gsize) retval >= *len) {
		*buf = &p[*len];
		*len = 0;
	} else {
		*buf = &p[retval];
		*len -= retval;
	}
}

/**
 * nm_utils_strbuf_seek_end:
 * @buf: the input/output buffer
 * @len: the input/output lenght of the buffer.
 *
 * Commonly, one uses nm_utils_strbuf_append*(), to incrementally
 * append strings to the buffer. However, sometimes we need to use
 * existing API to write to the buffer.
 * After doing so, we want to adjust the buffer counter.
 * Essentially,
 *
 *   g_snprintf (buf, len, ...);
 *   nm_utils_strbuf_seek_end (&buf, &len);
 *
 * is almost the same as
 *
 *   nm_utils_strbuf_append (&buf, &len, ...);
 *
 * The only difference is the behavior when the string got truncated:
 * nm_utils_strbuf_append() will recognize that and set the remaining
 * length to zero.
 *
 * In general, the behavior is:
 *
 *  - if *len is zero, do nothing
 *  - if the buffer contains a NUL byte within the first *len characters,
 *    the buffer is pointed to the NUL byte and len is adjusted. In this
 *    case, the remaining *len is always >= 1.
 *    In particular, that is also the case if the NUL byte is at the very last
 *    position ((*buf)[*len -1]). That happens, when the previous operation
 *    either fit the string exactly into the buffer or the string was truncated
 *    by g_snprintf(). The difference cannot be determined.
 *  - if the buffer contains no NUL bytes within the first *len characters,
 *    write NUL at the last position, set *len to zero, and point *buf past
 *    the NUL byte. This would happen with
 *
 *       strncpy (buf, long_str, len);
 *       nm_utils_strbuf_seek_end (&buf, &len).
 *
 *    where strncpy() does truncate the string and not NUL terminate it.
 *    nm_utils_strbuf_seek_end() would then NUL terminate it.
 */
void
nm_utils_strbuf_seek_end (char **buf, gsize *len)
{
	gsize l;
	char *end;

	nm_assert (len);
	nm_assert (buf && *buf);

	if (*len <= 1) {
		if (   *len == 1
		    && (*buf)[0])
			goto truncate;
		return;
	}

	end = memchr (*buf, 0, *len);
	if (end) {
		l = end - *buf;
		nm_assert (l < *len);

		*buf = end;
		*len -= l;
		return;
	}

truncate:
	/* hm, no NUL character within len bytes.
	 * Just NUL terminate the array and consume them
	 * all. */
	*buf += *len;
	(*buf)[-1] = '\0';
	*len = 0;
	return;
}

/*****************************************************************************/

/**
 * nm_utils_gbytes_equals:
 * @bytes: (allow-none): a #GBytes array to compare. Note that
 *   %NULL is treated like an #GBytes array of length zero.
 * @mem_data: the data pointer with @mem_len bytes
 * @mem_len: the length of the data pointer
 *
 * Returns: %TRUE if @bytes contains the same data as @mem_data. As a
 *   special case, a %NULL @bytes is treated like an empty array.
 */
gboolean
nm_utils_gbytes_equal_mem (GBytes *bytes,
                           gconstpointer mem_data,
                           gsize mem_len)
{
	gconstpointer p;
	gsize l;

	if (!bytes) {
		/* as a special case, let %NULL GBytes compare idential
		 * to an empty array. */
		return (mem_len == 0);
	}

	p = g_bytes_get_data (bytes, &l);
	return    l == mem_len
	       && (   mem_len == 0 /* allow @mem_data to be %NULL */
	           || memcmp (p, mem_data, mem_len) == 0);
}

GVariant *
nm_utils_gbytes_to_variant_ay (GBytes *bytes)
{
	const guint8 *p;
	gsize l;

	if (!bytes) {
		/* for convenience, accept NULL to return an empty variant */
		return g_variant_new_array (G_VARIANT_TYPE_BYTE, NULL, 0);
	}

	p = g_bytes_get_data (bytes, &l);
	return g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE, p, l, 1);
}

/*****************************************************************************/

/**
 * nm_strquote:
 * @buf: the output buffer of where to write the quoted @str argument.
 * @buf_len: the size of @buf.
 * @str: (allow-none): the string to quote.
 *
 * Writes @str to @buf with quoting. The resulting buffer
 * is always NUL terminated, unless @buf_len is zero.
 * If @str is %NULL, it writes "(null)".
 *
 * If @str needs to be truncated, the closing quote is '^' instead
 * of '"'.
 *
 * This is similar to nm_strquote_a(), which however uses alloca()
 * to allocate a new buffer. Also, here @buf_len is the size of @buf,
 * while nm_strquote_a() has the number of characters to print. The latter
 * doesn't include the quoting.
 *
 * Returns: the input buffer with the quoted string.
 */
const char *
nm_strquote (char *buf, gsize buf_len, const char *str)
{
	const char *const buf0 = buf;

	if (!str) {
		nm_utils_strbuf_append_str (&buf, &buf_len, "(null)");
		goto out;
	}

	if (G_UNLIKELY (buf_len <= 2)) {
		switch (buf_len) {
		case 2:
			*(buf++) = '^';
			/* fall-through */
		case 1:
			*(buf++) = '\0';
			break;
		}
		goto out;
	}

	*(buf++) = '"';
	buf_len--;

	nm_utils_strbuf_append_str (&buf, &buf_len, str);

	/* if the string was too long we indicate truncation with a
	 * '^' instead of a closing quote. */
	if (G_UNLIKELY (buf_len <= 1)) {
		switch (buf_len) {
		case 1:
			buf[-1] = '^';
			break;
		case 0:
			buf[-2] = '^';
			break;
		default:
			nm_assert_not_reached ();
			break;
		}
	} else {
		nm_assert (buf_len >= 2);
		*(buf++) = '"';
		*(buf++) = '\0';
	}

out:
	return buf0;
}

/*****************************************************************************/

char _nm_utils_to_string_buffer[];

void
nm_utils_to_string_buffer_init (char **buf, gsize *len)
{
	if (!*buf) {
		*buf = _nm_utils_to_string_buffer;
		*len = sizeof (_nm_utils_to_string_buffer);
	}
}

gboolean
nm_utils_to_string_buffer_init_null (gconstpointer obj, char **buf, gsize *len)
{
	nm_utils_to_string_buffer_init (buf, len);
	if (!obj) {
		g_strlcpy (*buf, "(null)", *len);
		return FALSE;
	}
	return TRUE;
}

/*****************************************************************************/

const char *
nm_utils_flags2str (const NMUtilsFlags2StrDesc *descs,
                    gsize n_descs,
                    unsigned flags,
                    char *buf,
                    gsize len)
{
	gsize i;
	char *p;

#if NM_MORE_ASSERTS > 10
	nm_assert (descs);
	nm_assert (n_descs > 0);
	for (i = 0; i < n_descs; i++) {
		gsize j;

		nm_assert (descs[i].name && descs[i].name[0]);
		for (j = 0; j < i; j++)
			nm_assert (descs[j].flag != descs[i].flag);
	}
#endif

	nm_utils_to_string_buffer_init (&buf, &len);

	if (!len)
		return buf;

	buf[0] = '\0';
	p = buf;
	if (!flags) {
		for (i = 0; i < n_descs; i++) {
			if (!descs[i].flag) {
				nm_utils_strbuf_append_str (&p, &len, descs[i].name);
				break;
			}
		}
		return buf;
	}

	for (i = 0; flags && i < n_descs; i++) {
		if (   descs[i].flag
		    && NM_FLAGS_ALL (flags, descs[i].flag)) {
			flags &= ~descs[i].flag;

			if (buf[0] != '\0')
				nm_utils_strbuf_append_c (&p, &len, ',');
			nm_utils_strbuf_append_str (&p, &len, descs[i].name);
		}
	}
	if (flags) {
		if (buf[0] != '\0')
			nm_utils_strbuf_append_c (&p, &len, ',');
		nm_utils_strbuf_append (&p, &len, "0x%x", flags);
	}
	return buf;
};

/*****************************************************************************/

/**
 * _nm_utils_ip4_prefix_to_netmask:
 * @prefix: a CIDR prefix
 *
 * Returns: the netmask represented by the prefix, in network byte order
 **/
guint32
_nm_utils_ip4_prefix_to_netmask (guint32 prefix)
{
	return prefix < 32 ? ~htonl(0xFFFFFFFF >> prefix) : 0xFFFFFFFF;
}

/**
 * _nm_utils_ip4_get_default_prefix:
 * @ip: an IPv4 address (in network byte order)
 *
 * When the Internet was originally set up, various ranges of IP addresses were
 * segmented into three network classes: A, B, and C.  This function will return
 * a prefix that is associated with the IP address specified defining where it
 * falls in the predefined classes.
 *
 * Returns: the default class prefix for the given IP
 **/
/* The function is originally from ipcalc.c of Red Hat's initscripts. */
guint32
_nm_utils_ip4_get_default_prefix (guint32 ip)
{
	if (((ntohl (ip) & 0xFF000000) >> 24) <= 127)
		return 8;  /* Class A - 255.0.0.0 */
	else if (((ntohl (ip) & 0xFF000000) >> 24) <= 191)
		return 16;  /* Class B - 255.255.0.0 */

	return 24;  /* Class C - 255.255.255.0 */
}

gboolean
nm_utils_ip_is_site_local (int addr_family,
                           const void *address)
{
	in_addr_t addr4;

	switch (addr_family) {
	case AF_INET:
		/* RFC1918 private addresses
		 * 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16 */
		addr4 = ntohl (*((const in_addr_t *) address));
		return    (addr4 & 0xff000000) == 0x0a000000
		       || (addr4 & 0xfff00000) == 0xac100000
		       || (addr4 & 0xffff0000) == 0xc0a80000;
	case AF_INET6:
		return IN6_IS_ADDR_SITELOCAL (address);
	default:
		g_return_val_if_reached (FALSE);
	}
}

/*****************************************************************************/

gboolean
nm_utils_parse_inaddr_bin (int addr_family,
                           const char *text,
                           gpointer out_addr)
{
	NMIPAddr addrbin;

	g_return_val_if_fail (text, FALSE);

	if (addr_family == AF_UNSPEC)
		addr_family = strchr (text, ':') ? AF_INET6 : AF_INET;
	else
		g_return_val_if_fail (NM_IN_SET (addr_family, AF_INET, AF_INET6), FALSE);

	/* use a temporary variable @addrbin, to guarantee that @out_addr
	 * is only modified on success. */
	if (inet_pton (addr_family, text, &addrbin) != 1)
		return FALSE;

	if (out_addr) {
		switch (addr_family) {
		case AF_INET:
			*((in_addr_t *) out_addr) = addrbin.addr4;
			break;
		case AF_INET6:
			*((struct in6_addr *) out_addr) = addrbin.addr6;
			break;
		default:
			nm_assert_not_reached ();
		}
	}
	return TRUE;
}

gboolean
nm_utils_parse_inaddr (int addr_family,
                       const char *text,
                       char **out_addr)
{
	NMIPAddr addrbin;
	char addrstr_buf[MAX (INET_ADDRSTRLEN, INET6_ADDRSTRLEN)];

	nm_assert (!out_addr || !*out_addr);

	if (!nm_utils_parse_inaddr_bin (addr_family, text, &addrbin))
		return FALSE;
	NM_SET_OUT (out_addr, g_strdup (inet_ntop (addr_family, &addrbin, addrstr_buf, sizeof (addrstr_buf))));
	return TRUE;
}

gboolean
nm_utils_parse_inaddr_prefix_bin (int addr_family,
                                  const char *text,
                                  gpointer out_addr,
                                  int *out_prefix)
{
	gs_free char *addrstr_free = NULL;
	int prefix = -1;
	const char *slash;
	const char *addrstr;
	NMIPAddr addrbin;
	int addr_len;

	g_return_val_if_fail (text, FALSE);

	if (addr_family == AF_UNSPEC)
		addr_family = strchr (text, ':') ? AF_INET6 : AF_INET;

	if (addr_family == AF_INET)
		addr_len = sizeof (in_addr_t);
	else if (addr_family == AF_INET6)
		addr_len = sizeof (struct in6_addr);
	else
		g_return_val_if_reached (FALSE);

	slash = strchr (text, '/');
	if (slash)
		addrstr = addrstr_free = g_strndup (text, slash - text);
	else
		addrstr = text;

	if (inet_pton (addr_family, addrstr, &addrbin) != 1)
		return FALSE;

	if (slash) {
		prefix = _nm_utils_ascii_str_to_int64 (slash + 1, 10,
		                                       0,
		                                       addr_family == AF_INET ? 32 : 128,
		                                       -1);
		if (prefix == -1)
			return FALSE;
	}

	if (out_addr)
		memcpy (out_addr, &addrbin, addr_len);
	NM_SET_OUT (out_prefix, prefix);
	return TRUE;
}

gboolean
nm_utils_parse_inaddr_prefix (int addr_family,
                              const char *text,
                              char **out_addr,
                              int *out_prefix)
{
	NMIPAddr addrbin;
	char addrstr_buf[MAX (INET_ADDRSTRLEN, INET6_ADDRSTRLEN)];

	if (!nm_utils_parse_inaddr_prefix_bin (addr_family, text, &addrbin, out_prefix))
		return FALSE;
	NM_SET_OUT (out_addr, g_strdup (inet_ntop (addr_family, &addrbin, addrstr_buf, sizeof (addrstr_buf))));
	return TRUE;
}

/*****************************************************************************/

/* _nm_utils_ascii_str_to_int64:
 *
 * A wrapper for g_ascii_strtoll, that checks whether the whole string
 * can be successfully converted to a number and is within a given
 * range. On any error, @fallback will be returned and %errno will be set
 * to a non-zero value. On success, %errno will be set to zero, check %errno
 * for errors. Any trailing or leading (ascii) white space is ignored and the
 * functions is locale independent.
 *
 * The function is guaranteed to return a value between @min and @max
 * (inclusive) or @fallback. Also, the parsing is rather strict, it does
 * not allow for any unrecognized characters, except leading and trailing
 * white space.
 **/
gint64
_nm_utils_ascii_str_to_int64 (const char *str, guint base, gint64 min, gint64 max, gint64 fallback)
{
	gint64 v;
	const char *s = NULL;

	if (str) {
		while (g_ascii_isspace (str[0]))
			str++;
	}
	if (!str || !str[0]) {
		errno = EINVAL;
		return fallback;
	}

	errno = 0;
	v = g_ascii_strtoll (str, (char **) &s, base);

	if (errno != 0)
		return fallback;
	if (s[0] != '\0') {
		while (g_ascii_isspace (s[0]))
			s++;
		if (s[0] != '\0') {
			errno = EINVAL;
			return fallback;
		}
	}
	if (v > max || v < min) {
		errno = ERANGE;
		return fallback;
	}

	return v;
}

/*****************************************************************************/

/* like nm_strcmp_p(), suitable for g_ptr_array_sort_with_data().
 * g_ptr_array_sort() just casts nm_strcmp_p() to a function of different
 * signature. I guess, in glib there are knowledgeable people that ensure
 * that this additional argument doesn't cause problems due to different ABI
 * for every architecture that glib supports.
 * For NetworkManager, we'd rather avoid such stunts.
 **/
int
nm_strcmp_p_with_data (gconstpointer a, gconstpointer b, gpointer user_data)
{
	const char *s1 = *((const char **) a);
	const char *s2 = *((const char **) b);

	return strcmp (s1, s2);
}

int
nm_cmp_uint32_p_with_data (gconstpointer p_a, gconstpointer p_b, gpointer user_data)
{
	const guint32 a = *((const guint32 *) p_a);
	const guint32 b = *((const guint32 *) p_b);

	if (a < b)
		return -1;
	if (a > b)
		return 1;
	return 0;
}

int
nm_cmp_int2ptr_p_with_data (gconstpointer p_a, gconstpointer p_b, gpointer user_data)
{
	/* p_a and p_b are two pointers to a pointer, where the pointer is
	 * interpreted as a integer using GPOINTER_TO_INT().
	 *
	 * That is the case of a hash-table that uses GINT_TO_POINTER() to
	 * convert integers as pointers, and the resulting keys-as-array
	 * array. */
	const int a = GPOINTER_TO_INT (*((gconstpointer *) p_a));
	const int b = GPOINTER_TO_INT (*((gconstpointer *) p_b));

	if (a < b)
		return -1;
	if (a > b)
		return 1;
	return 0;
}

/*****************************************************************************/

const char *
nm_utils_dbus_path_get_last_component (const char *dbus_path)
{
	if (dbus_path) {
		dbus_path = strrchr (dbus_path, '/');
		if (dbus_path)
			return dbus_path + 1;
	}
	return NULL;
}

static gint64
_dbus_path_component_as_num (const char *p)
{
	gint64 n;

	/* no odd stuff. No leading zeros, only a non-negative, decimal integer.
	 *
	 * Otherwise, there would be multiple ways to encode the same number "10"
	 * and "010". That is just confusing. A number has no leading zeros,
	 * if it has, it's not a number (as far as we are concerned here). */
	if (p[0] == '0') {
		if (p[1] != '\0')
			return -1;
		else
			return 0;
	}
	if (!(p[0] >= '1' && p[0] <= '9'))
		return -1;
	if (!NM_STRCHAR_ALL (&p[1], ch, (ch >= '0' && ch <= '9')))
		return -1;
	n = _nm_utils_ascii_str_to_int64 (p, 10, 0, G_MAXINT64, -1);
	nm_assert (n == -1 || nm_streq0 (p, nm_sprintf_bufa (100, "%"G_GINT64_FORMAT, n)));
	return n;
}

int
nm_utils_dbus_path_cmp (const char *dbus_path_a, const char *dbus_path_b)
{
	const char *l_a, *l_b;
	gsize plen;
	gint64 n_a, n_b;

	/* compare function for two D-Bus paths. It behaves like
	 * strcmp(), except, if both paths have the same prefix,
	 * and both end in a (positive) number, then the paths
	 * will be sorted by number. */

	NM_CMP_SELF (dbus_path_a, dbus_path_b);

	/* if one or both paths have no slash (and no last component)
	 * compare the full paths directly. */
	if (   !(l_a = nm_utils_dbus_path_get_last_component (dbus_path_a))
	    || !(l_b = nm_utils_dbus_path_get_last_component (dbus_path_b)))
		goto comp_full;

	/* check if both paths have the same prefix (up to the last-component). */
	plen = l_a - dbus_path_a;
	if (plen != (l_b - dbus_path_b))
		goto comp_full;
	NM_CMP_RETURN (strncmp (dbus_path_a, dbus_path_b, plen));

	n_a = _dbus_path_component_as_num (l_a);
	n_b = _dbus_path_component_as_num (l_b);
	if (n_a == -1 && n_b == -1)
		goto comp_l;

	/* both components must be convertiable to a number. If they are not,
	 * (and only one of them is), then we must always strictly sort numeric parts
	 * after non-numeric components. If we wouldn't, we wouldn't have
	 * a total order.
	 *
	 * An example of a not total ordering would be:
	 *   "8"   < "010"  (numeric)
	 *   "0x"  < "8"    (lexical)
	 *   "0x"  > "010"  (lexical)
	 * We avoid this, by forcing that a non-numeric entry "0x" always sorts
	 * before numeric entries.
	 *
	 * Additionally, _dbus_path_component_as_num() would also reject "010" as
	 * not a valid number.
	 */
	if (n_a == -1)
		return -1;
	if (n_b == -1)
		return 1;

	NM_CMP_DIRECT (n_a, n_b);
	nm_assert (nm_streq (dbus_path_a, dbus_path_b));
	return 0;

comp_full:
	NM_CMP_DIRECT_STRCMP0 (dbus_path_a, dbus_path_b);
	return 0;
comp_l:
	NM_CMP_DIRECT_STRCMP0 (l_a, l_b);
	nm_assert (nm_streq (dbus_path_a, dbus_path_b));
	return 0;
}

/*****************************************************************************/

/**
 * nm_utils_strsplit_set:
 * @str: the string to split.
 * @delimiters: the set of delimiters. If %NULL, defaults to " \t\n",
 *   like bash's $IFS.
 * @allow_escaping: whether delimiters can be escaped by a backslash
 *
 * This is a replacement for g_strsplit_set() which avoids copying
 * each word once (the entire strv array), but instead copies it once
 * and all words point into that internal copy.
 *
 * Another difference from g_strsplit_set() is that this never returns
 * empty words. Multiple delimiters are combined and treated as one.
 *
 * If @allow_escaping is %TRUE, delimiters prefixed by a backslash are
 * not treated as a separator. Such delimiters and their escape
 * character are copied to the current word without unescaping them.
 *
 * Returns: %NULL if @str is %NULL or contains only delimiters.
 *   Otherwise, a %NULL terminated strv array containing non-empty
 *   words, split at the delimiter characters (delimiter characters
 *   are removed).
 *   The strings to which the result strv array points to are allocated
 *   after the returned result itself. Don't free the strings themself,
 *   but free everything with g_free().
 */
const char **
nm_utils_strsplit_set (const char *str, const char *delimiters, gboolean allow_escaping)
{
	const char **ptr, **ptr0;
	gsize alloc_size, plen, i;
	gsize str_len;
	char *s0;
	char *s;
	guint8 delimiters_table[256];
	gboolean escaped = FALSE;

	if (!str)
		return NULL;

	/* initialize lookup table for delimiter */
	if (!delimiters)
		delimiters = " \t\n";
	memset (delimiters_table, 0, sizeof (delimiters_table));
	for (i = 0; delimiters[i]; i++)
		delimiters_table[(guint8) delimiters[i]] = 1;

#define _is_delimiter(ch, delimiters_table, allow_esc, esc) \
	((delimiters_table)[(guint8) (ch)] != 0 && (!allow_esc || !esc))

#define next_char(p, esc) \
	G_STMT_START { \
		if (esc) \
			esc = FALSE; \
		else \
			esc = p[0] == '\\'; \
		p++; \
	} G_STMT_END

	/* skip initial delimiters, and return of the remaining string is
	 * empty. */
	while (_is_delimiter (str[0], delimiters_table, allow_escaping, escaped))
		next_char (str, escaped);

	if (!str[0])
		return NULL;

	str_len = strlen (str) + 1;
	alloc_size = 8;

	/* we allocate the buffer larger, so to copy @str at the
	 * end of it as @s0. */
	ptr0 = g_malloc ((sizeof (const char *) * (alloc_size + 1)) + str_len);
	s0 = (char *) &ptr0[alloc_size + 1];
	memcpy (s0, str, str_len);

	plen = 0;
	s = s0;
	ptr = ptr0;

	while (TRUE) {
		if (plen >= alloc_size) {
			const char **ptr_old = ptr;

			/* reallocate the buffer. Note that for now the string
			 * continues to be in ptr0/s0. We fix that at the end. */
			alloc_size *= 2;
			ptr = g_malloc ((sizeof (const char *) * (alloc_size + 1)) + str_len);
			memcpy (ptr, ptr_old, sizeof (const char *) * plen);
			if (ptr_old != ptr0)
				g_free (ptr_old);
		}

		ptr[plen++] = s;

		nm_assert (s[0] && !_is_delimiter (s[0], delimiters_table, allow_escaping, escaped));

		while (TRUE) {
			next_char (s, escaped);
			if (_is_delimiter (s[0], delimiters_table, allow_escaping, escaped))
				break;
			if (s[0] == '\0')
				goto done;
		}

		s[0] = '\0';
		next_char (s, escaped);
		while (_is_delimiter (s[0], delimiters_table, allow_escaping, escaped))
			next_char (s, escaped);
		if (s[0] == '\0')
			break;
	}
done:
	ptr[plen] = NULL;

	if (ptr != ptr0) {
		/* we reallocated the buffer. We must copy over the
		 * string @s0 and adjust the pointers. */
		s = (char *) &ptr[alloc_size + 1];
		memcpy (s, s0, str_len);
		for (i = 0; i < plen; i++)
			ptr[i] = &s[ptr[i] - s0];
		g_free (ptr0);
	}

	return ptr;
}

/**
 * nm_utils_strv_find_first:
 * @list: the strv list to search
 * @len: the length of the list, or a negative value if @list is %NULL terminated.
 * @needle: the value to search for. The search is done using strcmp().
 *
 * Searches @list for @needle and returns the index of the first match (based
 * on strcmp()).
 *
 * For convenience, @list has type 'char**' instead of 'const char **'.
 *
 * Returns: index of first occurrence or -1 if @needle is not found in @list.
 */
gssize
nm_utils_strv_find_first (char **list, gssize len, const char *needle)
{
	gssize i;

	if (len > 0) {
		g_return_val_if_fail (list, -1);

		if (!needle) {
			/* if we search a list with known length, %NULL is a valid @needle. */
			for (i = 0; i < len; i++) {
				if (!list[i])
					return i;
			}
		} else {
			for (i = 0; i < len; i++) {
				if (list[i] && !strcmp (needle, list[i]))
					return i;
			}
		}
	} else if (len < 0) {
		g_return_val_if_fail (needle, -1);

		if (list) {
			for (i = 0; list[i]; i++) {
				if (strcmp (needle, list[i]) == 0)
					return i;
			}
		}
	}
	return -1;
}

char **
_nm_utils_strv_cleanup (char **strv,
                        gboolean strip_whitespace,
                        gboolean skip_empty,
                        gboolean skip_repeated)
{
	guint i, j;

	if (!strv || !*strv)
		return strv;

	if (strip_whitespace) {
		for (i = 0; strv[i]; i++)
			g_strstrip (strv[i]);
	}
	if (!skip_empty && !skip_repeated)
		return strv;
	j = 0;
	for (i = 0; strv[i]; i++) {
		if (   (skip_empty && !*strv[i])
		    || (skip_repeated && nm_utils_strv_find_first (strv, j, strv[i]) >= 0))
			g_free (strv[i]);
		else
			strv[j++] = strv[i];
	}
	strv[j] = NULL;
	return strv;
}

/*****************************************************************************/

int
_nm_utils_ascii_str_to_bool (const char *str,
                             int default_value)
{
	gsize len;
	char *s = NULL;

	if (!str)
		return default_value;

	while (str[0] && g_ascii_isspace (str[0]))
		str++;

	if (!str[0])
		return default_value;

	len = strlen (str);
	if (g_ascii_isspace (str[len - 1])) {
		s = g_strdup (str);
		g_strchomp (s);
		str = s;
	}

	if (!g_ascii_strcasecmp (str, "true") || !g_ascii_strcasecmp (str, "yes") || !g_ascii_strcasecmp (str, "on") || !g_ascii_strcasecmp (str, "1"))
		default_value = TRUE;
	else if (!g_ascii_strcasecmp (str, "false") || !g_ascii_strcasecmp (str, "no") || !g_ascii_strcasecmp (str, "off") || !g_ascii_strcasecmp (str, "0"))
		default_value = FALSE;
	if (s)
		g_free (s);
	return default_value;
}

/*****************************************************************************/

NM_CACHED_QUARK_FCN ("nm-utils-error-quark", nm_utils_error_quark)

void
nm_utils_error_set_cancelled (GError **error,
                              gboolean is_disposing,
                              const char *instance_name)
{
	if (is_disposing) {
		g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_CANCELLED_DISPOSING,
		             "Disposing %s instance",
		             instance_name && *instance_name ? instance_name : "source");
	} else {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
		                     "Request cancelled");
	}
}

gboolean
nm_utils_error_is_cancelled (GError *error,
                             gboolean consider_is_disposing)
{
	if (error) {
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			return TRUE;
		if (   consider_is_disposing
		    && g_error_matches (error, NM_UTILS_ERROR, NM_UTILS_ERROR_CANCELLED_DISPOSING))
			return TRUE;
	}
	return FALSE;
}

/*****************************************************************************/

/**
 * nm_g_object_set_property:
 * @object: the target object
 * @property_name: the property name
 * @value: the #GValue to set
 * @error: (allow-none): optional error argument
 *
 * A reimplementation of g_object_set_property(), but instead
 * returning an error instead of logging a warning. All g_object_set*()
 * versions in glib require you to not pass invalid types or they will
 * log a g_warning() -- without reporting an error. We don't want that,
 * so we need to hack error checking around it.
 *
 * Returns: whether the value was successfully set.
 */
gboolean
nm_g_object_set_property (GObject *object,
                          const char   *property_name,
                          const GValue *value,
                          GError **error)
{
	GParamSpec *pspec;
	nm_auto_unset_gvalue GValue tmp_value = G_VALUE_INIT;
	GObjectClass *klass;

	g_return_val_if_fail (G_IS_OBJECT (object), FALSE);
	g_return_val_if_fail (property_name != NULL, FALSE);
	g_return_val_if_fail (G_IS_VALUE (value), FALSE);
	g_return_val_if_fail (!error || !*error, FALSE);

	/* g_object_class_find_property() does g_param_spec_get_redirect_target(),
	 * where we differ from a plain g_object_set_property(). */
	pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (object), property_name);

	if (!pspec) {
		g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_UNKNOWN,
		             _("object class '%s' has no property named '%s'"),
		             G_OBJECT_TYPE_NAME (object),
		             property_name);
		return FALSE;
	}
	if (!(pspec->flags & G_PARAM_WRITABLE)) {
		g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_UNKNOWN,
		             _("property '%s' of object class '%s' is not writable"),
		             pspec->name,
		             G_OBJECT_TYPE_NAME (object));
		return FALSE;
	}
	if ((pspec->flags & G_PARAM_CONSTRUCT_ONLY)) {
		g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_UNKNOWN,
		             _("construct property \"%s\" for object '%s' can't be set after construction"),
		             pspec->name, G_OBJECT_TYPE_NAME (object));
		return FALSE;
	}

	klass = g_type_class_peek (pspec->owner_type);
	if (klass == NULL) {
		g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_UNKNOWN,
		             _("'%s::%s' is not a valid property name; '%s' is not a GObject subtype"),
		            g_type_name (pspec->owner_type), pspec->name, g_type_name (pspec->owner_type));
		return FALSE;
	}

	/* provide a copy to work from, convert (if necessary) and validate */
	g_value_init (&tmp_value, pspec->value_type);
	if (!g_value_transform (value, &tmp_value)) {
		g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_UNKNOWN,
		             _("unable to set property '%s' of type '%s' from value of type '%s'"),
		             pspec->name,
		             g_type_name (pspec->value_type),
		             G_VALUE_TYPE_NAME (value));
		return FALSE;
	}
	if (   g_param_value_validate (pspec, &tmp_value)
	    && !(pspec->flags & G_PARAM_LAX_VALIDATION)) {
		gs_free char *contents = g_strdup_value_contents (value);

		g_set_error (error, NM_UTILS_ERROR, NM_UTILS_ERROR_UNKNOWN,
		             _("value \"%s\" of type '%s' is invalid or out of range for property '%s' of type '%s'"),
		             contents,
		             G_VALUE_TYPE_NAME (value),
		             pspec->name,
		             g_type_name (pspec->value_type));
		return FALSE;
	}

	g_object_set_property (object, property_name, &tmp_value);
	return TRUE;
}

gboolean
nm_g_object_set_property_boolean (GObject *object,
                                  const char   *property_name,
                                  gboolean value,
                                  GError **error)
{
	nm_auto_unset_gvalue GValue gvalue = { 0 };

	g_value_init (&gvalue, G_TYPE_BOOLEAN);
	g_value_set_boolean (&gvalue, !!value);
	return nm_g_object_set_property (object, property_name, &gvalue, error);
}

gboolean
nm_g_object_set_property_uint (GObject *object,
                               const char   *property_name,
                               guint value,
                               GError **error)
{
	nm_auto_unset_gvalue GValue gvalue = { 0 };

	g_value_init (&gvalue, G_TYPE_UINT);
	g_value_set_uint (&gvalue, value);
	return nm_g_object_set_property (object, property_name, &gvalue, error);
}

GParamSpec *
nm_g_object_class_find_property_from_gtype (GType gtype,
                                            const char *property_name)
{
	nm_auto_unref_gtypeclass GObjectClass *gclass = NULL;

	gclass = g_type_class_ref (gtype);
	return g_object_class_find_property (gclass, property_name);
}

/*****************************************************************************/

static void
_str_buf_append_c_escape_octal (NMStrBuf *strbuf,
                                char ch)
{
	nm_str_buf_append_c4 (strbuf,
	                      '\\',
	                      '0' + ((char) ((((guchar) ch) >> 6) & 07)),
	                      '0' + ((char) ((((guchar) ch) >> 3) & 07)),
	                      '0' + ((char) ((((guchar) ch)     ) & 07)));
}

gconstpointer
nm_utils_buf_utf8safe_unescape (const char *str, gsize *out_len, gpointer *to_free)
{
	NMStrBuf strbuf;
	gsize len;
	const char *s;

	g_return_val_if_fail (to_free, NULL);
	g_return_val_if_fail (out_len, NULL);

	if (!str) {
		*out_len = 0;
		*to_free = NULL;
		return NULL;
	}

	len = strlen (str);

	s = memchr (str, '\\', len);
	if (!s) {
		*out_len = len;
		*to_free = NULL;
		return str;
	}

	nm_str_buf_init (&strbuf, len, FALSE);

	nm_str_buf_append_len (&strbuf, str, s - str);
	str = s;

	for (;;) {
		char ch;
		guint v;

		nm_assert (str[0] == '\\');

		ch = (++str)[0];

		if (ch == '\0') {
			// error. Trailing '\\'
			break;
		}

		if (ch >= '0' && ch <= '9') {
			v = ch - '0';
			ch = (++str)[0];
			if (ch >= '0' && ch <= '7') {
				v = v * 8 + (ch - '0');
				ch = (++str)[0];
				if (ch >= '0' && ch <= '7') {
					/* technically, escape sequences larger than \3FF are out of range
					 * and invalid. We don't check for that, and do do the same as
					 * g_strcompress(): silently clip the value with & 0xFF. */
					v = v * 8 + (ch - '0');
					ch = (++str)[0];
				}
			}
			ch = v;
		} else {
			switch (ch) {
			case 'b': ch = '\b'; break;
			case 'f': ch = '\f'; break;
			case 'n': ch = '\n'; break;
			case 'r': ch = '\r'; break;
			case 't': ch = '\t'; break;
			case 'v': ch = '\v'; break;
			default:
				/* Here we handle "\\\\", but all other unexpected escape sequences are really a bug.
				 * Take them literally, after removing the escape character */
				break;
			}
			str++;
		}

		nm_str_buf_append_c (&strbuf, ch);

		s = strchr (str, '\\');
		if (!s) {
			nm_str_buf_append (&strbuf, str);
			break;
		}

		nm_str_buf_append_len (&strbuf, str, s - str);
		str = s;
	}

	return (*to_free = nm_str_buf_finalize (&strbuf,
	                                        out_len));
}

/**
 * nm_utils_buf_utf8safe_escape:
 * @buf: byte array, possibly in utf-8 encoding, may have NUL characters.
 * @buflen: the length of @buf in bytes, or -1 if @buf is a NUL terminated
 *   string.
 * @flags: #NMUtilsStrUtf8SafeFlags flags
 * @to_free: (out): return the pointer location of the string
 *   if a copying was necessary.
 *
 * Based on the assumption, that @buf contains UTF-8 encoded bytes,
 * this will return valid UTF-8 sequence, and invalid sequences
 * will be escaped with backslash (C escaping, like g_strescape()).
 * This is sanitize non UTF-8 characters. The result is valid
 * UTF-8.
 *
 * The operation can be reverted with nm_utils_buf_utf8safe_unescape().
 * Note that if, and only if @buf contains no NUL bytes, the operation
 * can also be reverted with g_strcompress().
 *
 * Depending on @flags, valid UTF-8 characters are not escaped at all
 * (except the escape character '\\'). This is the difference to g_strescape(),
 * which escapes all non-ASCII characters. This allows to pass on
 * valid UTF-8 characters as-is and can be directly shown to the user
 * as UTF-8 -- with exception of the backslash escape character,
 * invalid UTF-8 sequences, and other (depending on @flags).
 *
 * Returns: the escaped input buffer, as valid UTF-8. If no escaping
 *   is necessary, it returns the input @buf. Otherwise, an allocated
 *   string @to_free is returned which must be freed by the caller
 *   with g_free. The escaping can be reverted by g_strcompress().
 **/
const char *
nm_utils_buf_utf8safe_escape (gconstpointer buf, gssize buflen, NMUtilsStrUtf8SafeFlags flags, char **to_free)
{
	const char *const str = buf;
	const char *p = NULL;
	const char *s;
	gboolean nul_terminated = FALSE;
	NMStrBuf strbuf;

	g_return_val_if_fail (to_free, NULL);

	*to_free = NULL;

	if (buflen == 0)
		return NULL;

	if (buflen < 0) {
		if (!str)
			return NULL;
		buflen = strlen (str);
		if (buflen == 0)
			return str;
		nul_terminated = TRUE;
	}

	if (   g_utf8_validate (str, buflen, &p)
	    && nul_terminated) {
		/* note that g_utf8_validate() does not allow NUL character inside @str. Good.
		 * We can treat @str like a NUL terminated string. */
		if (!NM_STRCHAR_ANY (str, ch,
		                        (   ch == '\\' \
		                         || (   NM_FLAGS_HAS (flags, NM_UTILS_STR_UTF8_SAFE_FLAG_ESCAPE_CTRL) \
		                             && ch < ' ') \
		                         || (   NM_FLAGS_HAS (flags, NM_UTILS_STR_UTF8_SAFE_FLAG_ESCAPE_NON_ASCII) \
		                             && ((guchar) ch) >= 127))))
			return str;
	}

	nm_str_buf_init (&strbuf,
	                 buflen + 5,
	                 NM_FLAGS_HAS (flags, NM_UTILS_STR_UTF8_SAFE_FLAG_SECRET));

	s = str;
	do {
		buflen -= p - s;
		nm_assert (buflen >= 0);

		for (; s < p; s++) {
			char ch = s[0];

			nm_assert (ch);
			if (ch == '\\')
				nm_str_buf_append_c2 (&strbuf, '\\', '\\');
			else if (   (   NM_FLAGS_HAS (flags, NM_UTILS_STR_UTF8_SAFE_FLAG_ESCAPE_CTRL) \
			             && ch < ' ') \
			         || (   NM_FLAGS_HAS (flags, NM_UTILS_STR_UTF8_SAFE_FLAG_ESCAPE_NON_ASCII) \
			             && ((guchar) ch) >= 127))
				_str_buf_append_c_escape_octal (&strbuf, ch);
			else
				nm_str_buf_append_c (&strbuf, ch);
		}

		if (buflen <= 0)
			break;

		_str_buf_append_c_escape_octal (&strbuf, p[0]);

		buflen--;
		if (buflen == 0)
			break;

		s = &p[1];
		g_utf8_validate (s, buflen, &p);
	} while (TRUE);

	return (*to_free = nm_str_buf_finalize (&strbuf, NULL));
}

const char *
nm_utils_buf_utf8safe_escape_bytes (GBytes *bytes, NMUtilsStrUtf8SafeFlags flags, char **to_free)
{
	gconstpointer p;
	gsize l;

	if (bytes)
		p = g_bytes_get_data (bytes, &l);
	else {
		p = NULL;
		l = 0;
	}

	return nm_utils_buf_utf8safe_escape (p, l, flags, to_free);
}

/*****************************************************************************/

const char *
nm_utils_str_utf8safe_unescape (const char *str, char **to_free)
{
	gsize len;

	g_return_val_if_fail (to_free, NULL);

	return nm_utils_buf_utf8safe_unescape (str, &len, (gpointer *) to_free);
}

/**
 * nm_utils_str_utf8safe_escape:
 * @str: NUL terminated input string, possibly in utf-8 encoding
 * @flags: #NMUtilsStrUtf8SafeFlags flags
 * @to_free: (out): return the pointer location of the string
 *   if a copying was necessary.
 *
 * Returns the possible non-UTF-8 NUL terminated string @str
 * and uses backslash escaping (C escaping, like g_strescape())
 * to sanitize non UTF-8 characters. The result is valid
 * UTF-8.
 *
 * The operation can be reverted with g_strcompress() or
 * nm_utils_str_utf8safe_unescape().
 *
 * Depending on @flags, valid UTF-8 characters are not escaped at all
 * (except the escape character '\\'). This is the difference to g_strescape(),
 * which escapes all non-ASCII characters. This allows to pass on
 * valid UTF-8 characters as-is and can be directly shown to the user
 * as UTF-8 -- with exception of the backslash escape character,
 * invalid UTF-8 sequences, and other (depending on @flags).
 *
 * Returns: the escaped input string, as valid UTF-8. If no escaping
 *   is necessary, it returns the input @str. Otherwise, an allocated
 *   string @to_free is returned which must be freed by the caller
 *   with g_free. The escaping can be reverted by g_strcompress().
 **/
const char *
nm_utils_str_utf8safe_escape (const char *str, NMUtilsStrUtf8SafeFlags flags, char **to_free)
{
	return nm_utils_buf_utf8safe_escape (str, -1, flags, to_free);
}

/**
 * nm_utils_str_utf8safe_escape_cp:
 * @str: NUL terminated input string, possibly in utf-8 encoding
 * @flags: #NMUtilsStrUtf8SafeFlags flags
 *
 * Like nm_utils_str_utf8safe_escape(), except the returned value
 * is always a copy of the input and must be freed by the caller.
 *
 * Returns: the escaped input string in UTF-8 encoding. The returned
 *   value should be freed with g_free().
 *   The escaping can be reverted by g_strcompress().
 **/
char *
nm_utils_str_utf8safe_escape_cp (const char *str, NMUtilsStrUtf8SafeFlags flags)
{
	char *s;

	nm_utils_str_utf8safe_escape (str, flags, &s);
	return s ?: g_strdup (str);
}

char *
nm_utils_str_utf8safe_unescape_cp (const char *str)
{
	char *s;

	str = nm_utils_str_utf8safe_unescape (str, &s);
	return s ?: g_strdup (str);
}

char *
nm_utils_str_utf8safe_escape_take (char *str, NMUtilsStrUtf8SafeFlags flags)
{
	char *str_to_free;

	nm_utils_str_utf8safe_escape (str, flags, &str_to_free);
	if (str_to_free) {
		g_free (str);
		return str_to_free;
	}
	return str;
}

/*****************************************************************************/

/* taken from systemd's fd_wait_for_event(). Note that the timeout
 * is here in nano-seconds, not micro-seconds. */
int
nm_utils_fd_wait_for_event (int fd, int event, gint64 timeout_ns)
{
	struct pollfd pollfd = {
		.fd = fd,
		.events = event,
	};
	struct timespec ts, *pts;
	int r;

	if (timeout_ns < 0)
		pts = NULL;
	else {
		ts.tv_sec = (time_t) (timeout_ns / NM_UTILS_NS_PER_SECOND);
		ts.tv_nsec = (long int) (timeout_ns % NM_UTILS_NS_PER_SECOND);
		pts = &ts;
	}

	r = ppoll (&pollfd, 1, pts, NULL);
	if (r < 0)
		return -errno;
	if (r == 0)
		return 0;
	return pollfd.revents;
}

/* taken from systemd's loop_read() */
ssize_t
nm_utils_fd_read_loop (int fd, void *buf, size_t nbytes, bool do_poll)
{
	uint8_t *p = buf;
	ssize_t n = 0;

	g_return_val_if_fail (fd >= 0, -EINVAL);
	g_return_val_if_fail (buf, -EINVAL);

	/* If called with nbytes == 0, let's call read() at least
	 * once, to validate the operation */

	if (nbytes > (size_t) SSIZE_MAX)
		return -EINVAL;

	do {
		ssize_t k;

		k = read (fd, p, nbytes);
		if (k < 0) {
			if (errno == EINTR)
				continue;

			if (errno == EAGAIN && do_poll) {

				/* We knowingly ignore any return value here,
				 * and expect that any error/EOF is reported
				 * via read() */

				(void) nm_utils_fd_wait_for_event (fd, POLLIN, -1);
				continue;
			}

			return n > 0 ? n : -errno;
		}

		if (k == 0)
			return n;

		g_assert ((size_t) k <= nbytes);

		p += k;
		nbytes -= k;
		n += k;
	} while (nbytes > 0);

	return n;
}

/* taken from systemd's loop_read_exact() */
int
nm_utils_fd_read_loop_exact (int fd, void *buf, size_t nbytes, bool do_poll)
{
	ssize_t n;

	n = nm_utils_fd_read_loop (fd, buf, nbytes, do_poll);
	if (n < 0)
		return (int) n;
	if ((size_t) n != nbytes)
		return -EIO;

	return 0;
}

NMUtilsNamedValue *
nm_utils_named_values_from_str_dict (GHashTable *hash, guint *out_len)
{
	GHashTableIter iter;
	NMUtilsNamedValue *values;
	guint i, len;

	if (   !hash
	    || !(len = g_hash_table_size (hash))) {
		NM_SET_OUT (out_len, 0);
		return NULL;
	}

	i = 0;
	values = g_new (NMUtilsNamedValue, len + 1);
	g_hash_table_iter_init (&iter, hash);
	while (g_hash_table_iter_next (&iter,
	                               (gpointer *) &values[i].name,
	                               (gpointer *) &values[i].value_ptr))
		i++;
	nm_assert (i == len);
	values[i].name = NULL;
	values[i].value_ptr = NULL;

	if (len > 1) {
		g_qsort_with_data (values, len, sizeof (values[0]),
		                   nm_utils_named_entry_cmp_with_data, NULL);
	}

	NM_SET_OUT (out_len, len);
	return values;
}

gpointer *
nm_utils_hash_keys_to_array (GHashTable *hash,
                             GCompareDataFunc compare_func,
                             gpointer user_data,
                             guint *out_len)
{
	guint len;
	gpointer *keys;

	/* by convention, we never return an empty array. In that
	 * case, always %NULL. */
	if (   !hash
	    || g_hash_table_size (hash) == 0) {
		NM_SET_OUT (out_len, 0);
		return NULL;
	}

	keys = g_hash_table_get_keys_as_array (hash, &len);
	if (   len > 1
	    && compare_func) {
		g_qsort_with_data (keys,
		                   len,
		                   sizeof (gpointer),
		                   compare_func,
		                   user_data);
	}
	NM_SET_OUT (out_len, len);
	return keys;
}

char **
nm_utils_strv_make_deep_copied (const char **strv)
{
	gsize i;

	/* it takes a strv dictionary, and copies each
	 * strings. Note that this updates @strv *in-place*
	 * and returns it. */

	if (!strv)
		return NULL;
	for (i = 0; strv[i]; i++)
		strv[i] = g_strdup (strv[i]);

	return (char **) strv;
}

/*****************************************************************************/

gssize
nm_utils_ptrarray_find_binary_search (gconstpointer *list,
                                      gsize len,
                                      gconstpointer needle,
                                      GCompareDataFunc cmpfcn,
                                      gpointer user_data,
                                      gssize *out_idx_first,
                                      gssize *out_idx_last)
{
	gssize imin, imax, imid, i2min, i2max, i2mid;
	int cmp;

	g_return_val_if_fail (list || !len, ~((gssize) 0));
	g_return_val_if_fail (cmpfcn, ~((gssize) 0));

	imin = 0;
	if (len > 0) {
		imax = len - 1;

		while (imin <= imax) {
			imid = imin + (imax - imin) / 2;

			cmp = cmpfcn (list[imid], needle, user_data);
			if (cmp == 0) {
				/* we found a matching entry at index imid.
				 *
				 * Does the caller request the first/last index as well (in case that
				 * there are multiple entries which compare equal). */

				if (out_idx_first) {
					i2min = imin;
					i2max = imid + 1;
					while (i2min <= i2max) {
						i2mid = i2min + (i2max - i2min) / 2;

						cmp = cmpfcn (list[i2mid], needle, user_data);
						if (cmp == 0)
							i2max = i2mid -1;
						else {
							nm_assert (cmp < 0);
							i2min = i2mid + 1;
						}
					}
					*out_idx_first = i2min;
				}
				if (out_idx_last) {
					i2min = imid + 1;
					i2max = imax;
					while (i2min <= i2max) {
						i2mid = i2min + (i2max - i2min) / 2;

						cmp = cmpfcn (list[i2mid], needle, user_data);
						if (cmp == 0)
							i2min = i2mid + 1;
						else {
							nm_assert (cmp > 0);
							i2max = i2mid - 1;
						}
					}
					*out_idx_last = i2min - 1;
				}
				return imid;
			}

			if (cmp < 0)
				imin = imid + 1;
			else
				imax = imid - 1;
		}
	}

	/* return the inverse of @imin. This is a negative number, but
	 * also is ~imin the position where the value should be inserted. */
	imin = ~imin;
	NM_SET_OUT (out_idx_first, imin);
	NM_SET_OUT (out_idx_last, imin);
	return imin;
}

/*****************************************************************************/

/**
 * nm_utils_array_find_binary_search:
 * @list: the list to search. It must be sorted according to @cmpfcn ordering.
 * @elem_size: the size in bytes of each element in the list
 * @len: the number of elements in @list
 * @needle: the value that is searched
 * @cmpfcn: the compare function. The elements @list are passed as first
 *   argument to @cmpfcn, while @needle is passed as second. Usually, the
 *   needle is the same data type as inside the list, however, that is
 *   not necessary, as long as @cmpfcn takes care to cast the two arguments
 *   accordingly.
 * @user_data: optional argument passed to @cmpfcn
 *
 * Performs binary search for @needle in @list. On success, returns the
 * (non-negative) index where the compare function found the searched element.
 * On success, it returns a negative value. Note that the return negative value
 * is the bitwise inverse of the position where the element should be inserted.
 *
 * If the list contains multiple matching elements, an arbitrary index is
 * returned.
 *
 * Returns: the index to the element in the list, or the (negative, bitwise inverted)
 *   position where it should be.
 */
gssize
nm_utils_array_find_binary_search (gconstpointer list,
                                   gsize elem_size,
                                   gsize len,
                                   gconstpointer needle,
                                   GCompareDataFunc cmpfcn,
                                   gpointer user_data)
{
	gssize imin, imax, imid;
	int cmp;

	g_return_val_if_fail (list || !len, ~((gssize) 0));
	g_return_val_if_fail (cmpfcn, ~((gssize) 0));
	g_return_val_if_fail (elem_size > 0, ~((gssize) 0));

	imin = 0;
	if (len == 0)
		return ~imin;

	imax = len - 1;

	while (imin <= imax) {
		imid = imin + (imax - imin) / 2;

		cmp = cmpfcn (&((const char *) list)[elem_size * imid], needle, user_data);
		if (cmp == 0)
			return imid;

		if (cmp < 0)
			imin = imid + 1;
		else
			imax = imid - 1;
	}

	/* return the inverse of @imin. This is a negative number, but
	 * also is ~imin the position where the value should be inserted. */
	return ~imin;
}

/*****************************************************************************/

/**
 * nm_utils_hash_table_equal:
 * @a: one #GHashTable
 * @b: other #GHashTable
 * @treat_null_as_empty: if %TRUE, when either @a or @b is %NULL, it is
 *   treated like an empty hash. It means, a %NULL hash will compare equal
 *   to an empty hash.
 * @equal_func: the equality function, for comparing the values.
 *   If %NULL, the values are not compared. In that case, the function
 *   only checks, if both dictionaries have the same keys -- according
 *   to @b's key equality function.
 *   Note that the values of @a will be passed as first argument
 *   to @equal_func.
 *
 * Compares two hash tables, whether they have equal content.
 * This only makes sense, if @a and @b have the same key types and
 * the same key compare-function.
 *
 * Returns: %TRUE, if both dictionaries have the same content.
 */
gboolean
nm_utils_hash_table_equal (const GHashTable *a,
                           const GHashTable *b,
                           gboolean treat_null_as_empty,
                           NMUtilsHashTableEqualFunc equal_func)
{
	guint n;
	GHashTableIter iter;
	gconstpointer key, v_a, v_b;

	if (a == b)
		return TRUE;
	if (!treat_null_as_empty) {
		if (!a || !b)
			return FALSE;
	}

	n = a ? g_hash_table_size ((GHashTable *) a) : 0;
	if (n != (b ? g_hash_table_size ((GHashTable *) b) : 0))
		return FALSE;

	if (n > 0) {
		g_hash_table_iter_init (&iter, (GHashTable *) a);
		while (g_hash_table_iter_next (&iter, (gpointer *) &key, (gpointer *) &v_a)) {
			if (!g_hash_table_lookup_extended ((GHashTable *) b, key, NULL, (gpointer *) &v_b))
				return FALSE;
			if (   equal_func
			    && !equal_func (v_a, v_b))
				return FALSE;
		}
	}

	return TRUE;
}

/*****************************************************************************/

/**
 * nm_utils_get_start_time_for_pid:
 * @pid: the process identifier
 * @out_state: return the state character, like R, S, Z. See `man 5 proc`.
 * @out_ppid: parent process id
 *
 * Originally copied from polkit source (src/polkit/polkitunixprocess.c)
 * and adjusted.
 *
 * Returns: the timestamp when the process started (by parsing /proc/$PID/stat).
 * If an error occurs (e.g. the process does not exist), 0 is returned.
 *
 * The returned start time counts since boot, in the unit HZ (with HZ usually being (1/100) seconds)
 **/
guint64
nm_utils_get_start_time_for_pid (pid_t pid, char *out_state, pid_t *out_ppid)
{
	guint64 start_time;
	char filename[256];
	gs_free char *contents = NULL;
	size_t length;
	gs_free const char **tokens = NULL;
	char *p;
	char state = ' ';
	gint64 ppid = 0;

	start_time = 0;
	contents = NULL;

	g_return_val_if_fail (pid > 0, 0);

	nm_sprintf_buf (filename, "/proc/%"G_GUINT64_FORMAT"/stat", (guint64) pid);

	if (!g_file_get_contents (filename, &contents, &length, NULL))
		goto fail;

	/* start time is the token at index 19 after the '(process name)' entry - since only this
	 * field can contain the ')' character, search backwards for this to avoid malicious
	 * processes trying to fool us
	 */
	p = strrchr (contents, ')');
	if (!p)
		goto fail;
	p += 2; /* skip ') ' */
	if (p - contents >= (int) length)
		goto fail;

	state = p[0];

	tokens = nm_utils_strsplit_set (p, " ", FALSE);

	if (NM_PTRARRAY_LEN (tokens) < 20)
		goto fail;

	if (out_ppid) {
		ppid = _nm_utils_ascii_str_to_int64 (tokens[1], 10, 1, G_MAXINT, 0);
		if (ppid == 0)
			goto fail;
	}

	start_time = _nm_utils_ascii_str_to_int64 (tokens[19], 10, 1, G_MAXINT64, 0);
	if (start_time == 0)
		goto fail;

	NM_SET_OUT (out_state, state);
	NM_SET_OUT (out_ppid, ppid);
	return start_time;

fail:
	NM_SET_OUT (out_state, ' ');
	NM_SET_OUT (out_ppid, 0);
	return 0;
}

/*****************************************************************************/

/**
 * _nm_utils_strv_sort:
 * @strv: pointer containing strings that will be sorted
 *   in-place, %NULL is allowed, unless @len indicates
 *   that there are more elements.
 * @len: the number of elements in strv. If negative,
 *   strv must be a NULL terminated array and the length
 *   will be calculated first. If @len is a positive
 *   number, all first @len elements in @strv must be
 *   non-NULL, valid strings.
 *
 * Ascending sort of the array @strv inplace, using plain strcmp() string
 * comparison.
 */
void
_nm_utils_strv_sort (const char **strv, gssize len)
{
	gsize l;

	l = len < 0 ? (gsize) NM_PTRARRAY_LEN (strv) : (gsize) len;

	if (l <= 1)
		return;

	nm_assert (l <= (gsize) G_MAXINT);

	g_qsort_with_data (strv,
	                   l,
	                   sizeof (const char *),
	                   nm_strcmp_p_with_data,
	                   NULL);
}

/*****************************************************************************/

gpointer
_nm_utils_user_data_pack (int nargs, gconstpointer *args)
{
	int i;
	gpointer *data;

	nm_assert (nargs > 0);
	nm_assert (args);

	data = g_slice_alloc (((gsize) nargs) * sizeof (gconstpointer));
	for (i = 0; i < nargs; i++)
		data[i] = (gpointer) args[i];
	return data;
}

void
_nm_utils_user_data_unpack (gpointer user_data, int nargs, ...)
{
	gpointer *data = user_data;
	va_list ap;
	int i;

	nm_assert (data);
	nm_assert (nargs > 0);

	va_start (ap, nargs);
	for (i = 0; i < nargs; i++) {
		gpointer *dst;

		dst = va_arg (ap, gpointer *);
		nm_assert (dst);

		*dst = data[i];
	}
	va_end (ap);

	g_slice_free1 (((gsize) nargs) * sizeof (gconstpointer), user_data);
}

/*****************************************************************************/

#define IS_SPACE(c) NM_IN_SET ((c), ' ', '\t')

const char *
_nm_utils_escape_spaces (const char *str, char **to_free)
{
	const char *ptr = str;
	char *ret, *r;

	*to_free = NULL;

	if (!str)
		return NULL;

	while (TRUE) {
		if (!*ptr)
			return str;
		if (IS_SPACE (*ptr))
			break;
		ptr++;
	}

	ptr = str;
	ret = g_new (char, strlen (str) * 2 + 1);
	r = ret;
	*to_free = ret;
	while (*ptr) {
		if (IS_SPACE (*ptr))
			*r++ = '\\';
		*r++ = *ptr++;
	}
	*r = '\0';

	return ret;
}

char *
_nm_utils_unescape_spaces (char *str)
{
	guint i, j = 0;

	if (!str)
		return NULL;

	for (i = 0; str[i]; i++) {
		if (str[i] == '\\' && IS_SPACE (str[i+1]))
			i++;
		str[j++] = str[i];
	}
	str[j] = '\0';

	return str;
}

#undef IS_SPACE

/*****************************************************************************/

void
nm_explicit_bzero (void *s, gsize n)
{
	/* gracefully handle n == 0. This is important, callers rely on it. */
	if (n > 0) {
		nm_assert (s);
#if defined (HAVE_DECL_EXPLICIT_BZERO) && HAVE_DECL_EXPLICIT_BZERO
		explicit_bzero (s, n);
#else
		/* don't bother with a workaround. Use a reasonable glibc. */
		memset (s, 0, n);
#endif
	}
}

/*****************************************************************************/

char *
nm_secret_strchomp (char *secret)
{
	gsize len;

	g_return_val_if_fail (secret, NULL);

	/* it's actually identical to g_strchomp(). However,
	 * the glib function does not document, that it clears the
	 * memory. For @secret, we don't only want to truncate trailing
	 * spaces, we want to overwrite them with NUL. */

	len = strlen (secret);
	while (len--) {
		if (g_ascii_isspace ((guchar) secret[len]))
			secret[len] = '\0';
		else
			break;
	}

	return secret;
}

/*****************************************************************************/

GBytes *
nm_secret_copy_to_gbytes (gconstpointer mem, gsize mem_len)
{
	NMSecretBuf *b;

	if (mem_len == 0)
		return g_bytes_new_static ("", 0);

	nm_assert (mem);

	/* NUL terminate the buffer.
	 *
	 * The entire buffer is already malloc'ed and likely has some room for padding.
	 * Thus, in many situations, this additional byte will cause no overhead in
	 * practice.
	 *
	 * Even if it causes an overhead, do it just for safety. Yes, the returned
	 * bytes is not a NUL terminated string and no user must rely on this. Do
	 * not treat binary data as NUL terminated strings, unless you know what
	 * you are doing. Anyway, defensive FTW.
	 */

	b = nm_secret_buf_new (mem_len + 1);
	memcpy (b->bin, mem, mem_len);
	b->bin[mem_len] = 0;
	return nm_secret_buf_to_gbytes_take (b, mem_len);
}

/*****************************************************************************/

NMSecretBuf *
nm_secret_buf_new (gsize len)
{
	NMSecretBuf *secret;

	nm_assert (len > 0);

	secret = g_malloc (sizeof (NMSecretBuf) + len);
	*((gsize *) &(secret->len)) = len;
	return secret;
}

static void
_secret_buf_free (gpointer user_data)
{
	NMSecretBuf *secret = user_data;

	nm_assert (secret);
	nm_assert (secret->len > 0);

	nm_explicit_bzero (secret->bin, secret->len);
	g_free (user_data);
}

GBytes *
nm_secret_buf_to_gbytes_take (NMSecretBuf *secret, gssize actual_len)
{
	nm_assert (secret);
	nm_assert (secret->len > 0);
	nm_assert (actual_len == -1 || (actual_len >= 0 && actual_len <= secret->len));
	return g_bytes_new_with_free_func (secret->bin,
	                                   actual_len >= 0 ? (gsize) actual_len : secret->len,
	                                   _secret_buf_free,
	                                   secret);
}

/*****************************************************************************/

_nm_printf (3, 4)
static int
_get_contents_error (GError **error, int errsv, const char *format, ...)
{
	if (errsv < 0)
		errsv = -errsv;
	else if (!errsv)
		errsv = errno;

	if (error) {
		char *msg;
		va_list args;

		va_start (args, format);
		msg = g_strdup_vprintf (format, args);
		va_end (args);
		g_set_error (error,
		             G_FILE_ERROR,
		             g_file_error_from_errno (errsv),
		             "%s: %s",
		             msg, g_strerror (errsv));
		g_free (msg);
	}
	return -errsv;
}

/**
 * nm_utils_fd_get_contents:
 * @fd: open file descriptor to read. The fd will not be closed,
 *   but don't rely on its state afterwards.
 * @close_fd: if %TRUE, @fd will be closed by the function.
 *  Passing %TRUE here might safe a syscall for dup().
 * @max_length: allocate at most @max_length bytes. If the
 *   file is larger, reading will fail. Set to zero to use
 *   a very large default.
 *   WARNING: @max_length is here to avoid a crash for huge/unlimited files.
 *   For example, stat(/sys/class/net/enp0s25/ifindex) gives a filesize of
 *   4K, although the actual real is small. @max_length is the memory
 *   allocated in the process of reading the file, thus it must be at least
 *   the size reported by fstat.
 *   If you set it to 1K, read will fail because fstat() claims the
 *   file is larger.
 * @flags: %NMUtilsFileGetContentsFlags for reading the file.
 * @contents: the output buffer with the file read. It is always
 *   NUL terminated. The buffer is at most @max_length long, including
 *  the NUL byte. That is, it reads only files up to a length of
 *  @max_length - 1 bytes.
 * @length: optional output argument of the read file size.
 *
 * A reimplementation of g_file_get_contents() with a few differences:
 *   - accepts an open fd, instead of a path name. This allows you to
 *     use openat().
 *   - limits the maxium filesize to max_length.
 *
 * Returns: a negative error code on failure.
 */
int
nm_utils_fd_get_contents (int fd,
                          gboolean close_fd,
                          gsize max_length,
                          NMUtilsFileGetContentsFlags flags,
                          char **contents,
                          gsize *length,
                          GError **error)
{
	nm_auto_close int fd_keeper = close_fd ? fd : -1;
	struct stat stat_buf;
	gs_free char *str = NULL;
	const bool do_bzero_mem = NM_FLAGS_HAS (flags, NM_UTILS_FILE_GET_CONTENTS_FLAG_SECRET);

	g_return_val_if_fail (fd >= 0, -EINVAL);
	g_return_val_if_fail (contents, -EINVAL);
	g_return_val_if_fail (!error || !*error, -EINVAL);

	if (fstat (fd, &stat_buf) < 0)
		return _get_contents_error (error, 0, "failure during fstat");

	if (!max_length) {
		/* default to a very large size, but not extreme */
		max_length = 2 * 1024 * 1024;
	}

	if (   stat_buf.st_size > 0
	    && S_ISREG (stat_buf.st_mode)) {
		const gsize n_stat = stat_buf.st_size;
		ssize_t n_read;

		if (n_stat > max_length - 1)
			return _get_contents_error (error, EMSGSIZE, "file too large (%zu+1 bytes with maximum %zu bytes)", n_stat, max_length);

		str = g_try_malloc (n_stat + 1);
		if (!str)
			return _get_contents_error (error, ENOMEM, "failure to allocate buffer of %zu+1 bytes", n_stat);

		n_read = nm_utils_fd_read_loop (fd, str, n_stat, TRUE);
		if (n_read < 0) {
			if (do_bzero_mem)
				nm_explicit_bzero (str, n_stat);
			return _get_contents_error (error, n_read, "error reading %zu bytes from file descriptor", n_stat);
		}
		str[n_read] = '\0';

		if (n_read < n_stat) {
			if (!(str = nm_secret_mem_try_realloc_take (str, do_bzero_mem, n_stat + 1, n_read + 1)))
				return _get_contents_error (error, ENOMEM, "failure to reallocate buffer with %zu bytes", n_read + 1);
		}
		NM_SET_OUT (length, n_read);
	} else {
		nm_auto_fclose FILE *f = NULL;
		char buf[4096];
		gsize n_have, n_alloc;
		int fd2;

		if (fd_keeper >= 0)
			fd2 = nm_steal_fd (&fd_keeper);
		else {
			fd2 = fcntl (fd, F_DUPFD_CLOEXEC, 0);
			if (fd2 < 0)
				return _get_contents_error (error, 0, "error during dup");
		}

		if (!(f = fdopen (fd2, "r"))) {
			nm_close (fd2);
			return _get_contents_error (error, 0, "failure during fdopen");
		}

		n_have = 0;
		n_alloc = 0;

		while (!feof (f)) {
			int errsv;
			gsize n_read;

			n_read = fread (buf, 1, sizeof (buf), f);
			errsv = errno;
			if (ferror (f)) {
				if (do_bzero_mem)
					nm_explicit_bzero (buf, sizeof (buf));
				return _get_contents_error (error, errsv, "error during fread");
			}

			if (   n_have > G_MAXSIZE - 1 - n_read
			    || n_have + n_read + 1 > max_length) {
				if (do_bzero_mem)
					nm_explicit_bzero (buf, sizeof (buf));
				return _get_contents_error (error, EMSGSIZE, "file stream too large (%zu+1 bytes with maximum %zu bytes)",
				                            (n_have > G_MAXSIZE - 1 - n_read) ? G_MAXSIZE : n_have + n_read,
				                            max_length);
			}

			if (n_have + n_read + 1 >= n_alloc) {
				gsize old_n_alloc = n_alloc;

				if (n_alloc != 0) {
					nm_assert (str);
					if (n_alloc >= max_length / 2)
						n_alloc = max_length;
					else
						n_alloc *= 2;
				} else {
					nm_assert (!str);
					n_alloc = NM_MIN (n_read + 1, sizeof (buf));
				}

				if (!(str = nm_secret_mem_try_realloc_take (str, do_bzero_mem, old_n_alloc, n_alloc))) {
					if (do_bzero_mem)
						nm_explicit_bzero (buf, sizeof (buf));
					return _get_contents_error (error, ENOMEM, "failure to allocate buffer of %zu bytes", n_alloc);
				}
			}

			memcpy (str + n_have, buf, n_read);
			n_have += n_read;
		}

		if (do_bzero_mem)
			nm_explicit_bzero (buf, sizeof (buf));

		if (n_alloc == 0)
			str = g_new0 (char, 1);
		else {
			str[n_have] = '\0';
			if (n_have + 1 < n_alloc) {
				if (!(str = nm_secret_mem_try_realloc_take (str, do_bzero_mem, n_alloc, n_have + 1)))
					return _get_contents_error (error, ENOMEM, "failure to truncate buffer to %zu bytes", n_have + 1);
			}
		}

		NM_SET_OUT (length, n_have);
	}

	*contents = g_steal_pointer (&str);
	return 0;
}

/**
 * nm_utils_file_get_contents:
 * @dirfd: optional file descriptor to use openat(). If negative, use plain open().
 * @filename: the filename to open. Possibly relative to @dirfd.
 * @max_length: allocate at most @max_length bytes.
 *   WARNING: see nm_utils_fd_get_contents() hint about @max_length.
 * @flags: %NMUtilsFileGetContentsFlags for reading the file.
 * @contents: the output buffer with the file read. It is always
 *   NUL terminated. The buffer is at most @max_length long, including
 *  the NUL byte. That is, it reads only files up to a length of
 *  @max_length - 1 bytes.
 * @length: optional output argument of the read file size.
 *
 * A reimplementation of g_file_get_contents() with a few differences:
 *   - accepts an @dirfd to open @filename relative to that path via openat().
 *   - limits the maxium filesize to max_length.
 *   - uses O_CLOEXEC on internal file descriptor
 *
 * Returns: a negative error code on failure.
 */
int
nm_utils_file_get_contents (int dirfd,
                            const char *filename,
                            gsize max_length,
                            NMUtilsFileGetContentsFlags flags,
                            char **contents,
                            gsize *length,
                            GError **error)
{
	int fd;
	int errsv;

	g_return_val_if_fail (filename && filename[0], -EINVAL);

	if (dirfd >= 0) {
		fd = openat (dirfd, filename, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			errsv = errno;

			g_set_error (error,
			             G_FILE_ERROR,
			             g_file_error_from_errno (errsv),
			             "Failed to open file \"%s\" with openat: %s",
			             filename,
			             g_strerror (errsv));
			return -errsv;
		}
	} else {
		fd = open (filename, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			errsv = errno;

			g_set_error (error,
			             G_FILE_ERROR,
			             g_file_error_from_errno (errsv),
			             "Failed to open file \"%s\": %s",
			             filename,
			             g_strerror (errsv));
			return -errsv;
		}
	}
	return nm_utils_fd_get_contents (fd,
	                                 TRUE,
	                                 max_length,
	                                 flags,
	                                 contents,
	                                 length,
	                                 error);
}

/*****************************************************************************/

/*
 * Copied from GLib's g_file_set_contents() et al., but allows
 * specifying a mode for the new file.
 */
gboolean
nm_utils_file_set_contents (const char *filename,
                            const char *contents,
                            gssize length,
                            mode_t mode,
                            GError **error)
{
	gs_free char *tmp_name = NULL;
	struct stat statbuf;
	int errsv;
	gssize s;
	int fd;

	g_return_val_if_fail (filename, FALSE);
	g_return_val_if_fail (contents || !length, FALSE);
	g_return_val_if_fail (!error || !*error, FALSE);
	g_return_val_if_fail (length >= -1, FALSE);

	if (length == -1)
		length = strlen (contents);

	tmp_name = g_strdup_printf ("%s.XXXXXX", filename);
	fd = g_mkstemp_full (tmp_name, O_RDWR, mode);
	if (fd < 0) {
		errsv = errno;
		g_set_error (error,
		             G_FILE_ERROR,
		             g_file_error_from_errno (errsv),
		             "failed to create file %s: %s",
		             tmp_name,
		             g_strerror (errsv));
		return FALSE;
	}

	while (length > 0) {
		s = write (fd, contents, length);
		if (s < 0) {
			errsv = errno;
			if (errsv == EINTR)
				continue;

			nm_close (fd);
			unlink (tmp_name);

			g_set_error (error,
			             G_FILE_ERROR,
			             g_file_error_from_errno (errsv),
			             "failed to write to file %s: %s",
			             tmp_name,
			             g_strerror (errsv));
			return FALSE;
		}

		g_assert (s <= length);

		contents += s;
		length -= s;
	}

	/* If the final destination exists and is > 0 bytes, we want to sync the
	 * newly written file to ensure the data is on disk when we rename over
	 * the destination. Otherwise if we get a system crash we can lose both
	 * the new and the old file on some filesystems. (I.E. those that don't
	 * guarantee the data is written to the disk before the metadata.)
	 */
	if (   lstat (filename, &statbuf) == 0
	    && statbuf.st_size > 0
	    && fsync (fd) != 0) {
		errsv = errno;

		nm_close (fd);
		unlink (tmp_name);

		g_set_error (error,
		             G_FILE_ERROR,
		             g_file_error_from_errno (errsv),
		             "failed to fsync %s: %s",
		             tmp_name,
		             g_strerror (errsv));
		return FALSE;
	}

	nm_close (fd);

	if (rename (tmp_name, filename)) {
		errsv = errno;
		unlink (tmp_name);
		g_set_error (error,
		             G_FILE_ERROR,
		             g_file_error_from_errno (errsv),
		             "failed to rename %s to %s: %s",
		             tmp_name,
		             filename,
		             g_strerror (errsv));
		return FALSE;
	}

	return TRUE;
}
