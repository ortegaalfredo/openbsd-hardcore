/*	$OpenBSD: inet_nat64.c,v 1.2 2015/03/14 03:38:51 jsg Exp $	*/
/*	$vantronix: inet_nat64.c,v 1.2 2011/02/28 14:57:58 mike Exp $	*/

/*
 * Copyright (c) 2011 Reyk Floeter <reyk@vantronix.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/socket.h>
#ifdef _KERNEL
#include <sys/mbuf.h>
#else
#include <errno.h>
#endif

union inet_nat64_addr {
	u_int32_t	 u32[4];
	u_int8_t	 u8[16];
};

u_int32_t inet_nat64_mask(u_int32_t, u_int32_t, u_int8_t);

int	  inet_nat64(int, const void *, void *, const void *, u_int8_t);
int	  inet_nat64_inet(const void *, void *, const void *, u_int8_t);
int	  inet_nat64_inet6(const void *, void *, const void *, u_int8_t);

int	  inet_nat46(int, const void *, void *, const void *, u_int8_t);
int	  inet_nat46_inet(const void *, void *, const void *, u_int8_t);
int	  inet_nat46_inet6(const void *, void *, const void *, u_int8_t);

u_int32_t
inet_nat64_mask(u_int32_t src, u_int32_t pfx, u_int8_t pfxlen)
{
	u_int32_t	u32;
	if (pfxlen == 0)
		return (src);
	else if (pfxlen > 32)
		pfxlen = 32;
	u32 =
	    (src & ~htonl(0xffffffff << (32 - pfxlen)) & 0xffffffff) |
	    (pfx & htonl(0xffffffff << (32 - pfxlen)) & 0xffffffff);
	return (u32);
}

int
inet_nat64(int af, const void *src, void *dst,
    const void *pfx, u_int8_t pfxlen)
{
    if (src == NULL || dst == NULL || pfx == NULL || pfxlen == 0) {
#ifndef _KERNEL
        errno = EINVAL;
#endif
        return (-1);
    }
    switch (af) {
    case AF_INET:
        return (inet_nat64_inet(src, dst, pfx, pfxlen));
    case AF_INET6:
        return (inet_nat64_inet6(src, dst, pfx, pfxlen));
    default:
#ifndef _KERNEL
        errno = EAFNOSUPPORT;
#endif
        return (-1);
    }
    /* NOTREACHED */
}

int
inet_nat64_inet(const void *src, void *dst, const void *pfx, u_int8_t pfxlen)
{
	const union inet_nat64_addr	*s = src;
	const union inet_nat64_addr	*p = pfx;
	union inet_nat64_addr		*d = dst;
	int				 i, j;

	if (!src || !dst || !pfx) {
#ifndef _KERNEL
		errno = EINVAL;
#endif
		return (-1);
	}

	switch (pfxlen) {
	case 32:
	case 40:
	case 48:
	case 56:
	case 64:
	case 96:
		i = pfxlen / 8;
		break;
	default:
		if (pfxlen < 96 || pfxlen > 128) {
#ifndef _KERNEL
			errno = EINVAL;
#endif
			return (-1);
		}

		/* as an extension, mask out any other bits */
		d->u32[0] = inet_nat64_mask(s->u32[3], p->u32[3],
		    (u_int8_t)(32 - (128 - pfxlen)));
		return (0);
	}

	/* fill the octets with the source and skip reserved octet 8 */
	for (j = 0; j < 4; j++) {
		if (i == 8)
			i++;
		if (i >= 16 || j >= 16) { /* Ensure no out-of-bounds access */
#ifndef _KERNEL
			errno = ERANGE;
#endif
			return (-1);
		}
		d->u8[j] = s->u8[i++];
	}

	return (0);
}

int
inet_nat64_inet6(const void *src, void *dst, const void *pfx, u_int8_t pfxlen)
{
	const union inet_nat64_addr	*s = src;
	const union inet_nat64_addr	*p = pfx;
	union inet_nat64_addr		*d = dst;
	int				 i, j;

	/* Validate prefix length */
	if (pfxlen > 128) {
#ifndef _KERNEL
		errno = EINVAL;
#endif
		return (-1);
	}

	/* first copy the prefix octets to the destination */
	*d = *p;

	switch (pfxlen) {
	case 32:
	case 40:
	case 48:
	case 56:
	case 64:
	case 96:
		i = pfxlen / 8;
		break;
	default:
		if (pfxlen < 96 || pfxlen > 128) {
#ifndef _KERNEL
			errno = EINVAL;
#endif
			return (-1);
		}

		/* as an extension, mask out any other bits */
		d->u32[3] = inet_nat64_mask(s->u32[0], p->u32[3],
		    (u_int8_t)(32 - (128 - pfxlen)));
		return (0);
	}

	/* octet 8 is reserved and must be set to zero */
	d->u8[8] = 0;

	/* Ensure we do not go out of bounds when filling other octets */
	for (j = 0; j < 4; j++) {
		if (i < 16) {
			if (i == 8)
				i++;
			if (i < 16)
				d->u8[i++] = s->u8[j];
		}
	}

	return (0);
}

int
inet_nat46(int af, const void *src, void *dst,
    const void *pfx, u_int8_t pfxlen)
{
	if (pfxlen > 32 || src == NULL || dst == NULL || pfx == NULL) {
#ifndef _KERNEL
		errno = EINVAL;
#endif
		return (-1);
	}

	switch (af) {
	case AF_INET:
		return (inet_nat46_inet(src, dst, pfx, pfxlen));
	case AF_INET6:
		return (inet_nat46_inet6(src, dst, pfx, pfxlen));
	default:
#ifndef _KERNEL
		errno = EAFNOSUPPORT;
#endif
		return (-1);
	}
	/* NOTREACHED */
}

int
inet_nat46_inet(const void *src, void *dst, const void *pfx, u_int8_t pfxlen)
{
	const union inet_nat64_addr	*s = src;
	const union inet_nat64_addr	*p = pfx;
	union inet_nat64_addr		*d = dst;

	if (pfxlen > 32) return -1; // pfxlen should be within 0-32 for valid masking.
	
	/* set the remaining bits to the source */
	d->u32[0] = inet_nat64_mask(s->u32[3], p->u32[0], pfxlen);

	return (0);
}

int
inet_nat46_inet6(const void *src, void *dst, const void *pfx, u_int8_t pfxlen)
{
	const union inet_nat64_addr	*s = src;
	const union inet_nat64_addr	*p = pfx;
	union inet_nat64_addr		*d = dst;

	/* Ensure pfxlen is within valid range */
	if (pfxlen > 32) {
		return (-1); // Invalid prefix length
	}

	/* set the initial octets to zero */
	d->u32[0] = d->u32[1] = d->u32[2] = 0;

	/* now set the remaining bits to the source */
	d->u32[3] = inet_nat64_mask(s->u32[0], p->u32[0], pfxlen);

	return (0);
}
