/*	$OpenBSD: ip_carp.c,v 1.361 2024/02/13 12:22:09 bluhm Exp $	*/

/*
 * Copyright (c) 2002 Michael Shalayeff. All rights reserved.
 * Copyright (c) 2003 Ryan McBride. All rights reserved.
 * Copyright (c) 2006-2008 Marco Pfatschbacher. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR OR HIS RELATIVES BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF MIND, USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * TODO:
 *	- iface reconfigure
 *	- support for hardware checksum calculations;
 *
 */

#include "ether.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/timeout.h>
#include <sys/ioctl.h>
#include <sys/errno.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/refcnt.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/if_types.h>
#include <net/netisr.h>
#include <net/route.h>

#include <crypto/sha1.h>

#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/if_ether.h>
#include <netinet/ip_ipsp.h>

#include <net/if_dl.h>

#ifdef INET6
#include <netinet6/in6_var.h>
#include <netinet/icmp6.h>
#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>
#include <netinet6/nd6.h>
#include <netinet6/in6_ifattach.h>
#endif

#include "bpfilter.h"
#if NBPFILTER > 0
#include <net/bpf.h>
#endif

#include "vlan.h"
#if NVLAN > 0
#include <net/if_vlan_var.h>
#endif

#include <netinet/ip_carp.h>

struct carp_mc_entry {
	LIST_ENTRY(carp_mc_entry)	mc_entries;
	union {
		struct ether_multi	*mcu_enm;
	} mc_u;
	struct sockaddr_storage		mc_addr;
};
#define	mc_enm	mc_u.mcu_enm

enum { HMAC_ORIG=0, HMAC_NOV6LL=1, HMAC_MAX=2 };

struct carp_vhost_entry {
	SRPL_ENTRY(carp_vhost_entry) vhost_entries;
	struct refcnt vhost_refcnt;

	struct carp_softc *parent_sc;
	int vhe_leader;
	int vhid;
	int advskew;
	enum { INIT = 0, BACKUP, MASTER }	state;
	struct timeout ad_tmo;	/* advertisement timeout */
	struct timeout md_tmo;	/* master down timeout */
	struct timeout md6_tmo;	/* master down timeout */

	u_int64_t vhe_replay_cookie;

	/* authentication */
#define CARP_HMAC_PAD	64
	unsigned char vhe_pad[CARP_HMAC_PAD];
	SHA1_CTX vhe_sha1[HMAC_MAX];

	u_int8_t vhe_enaddr[ETHER_ADDR_LEN];
};

void	carp_vh_ref(void *, void *);
void	carp_vh_unref(void *, void *);

struct srpl_rc carp_vh_rc =
    SRPL_RC_INITIALIZER(carp_vh_ref, carp_vh_unref, NULL);

struct carp_softc {
	struct arpcom sc_ac;
#define	sc_if		sc_ac.ac_if
#define	sc_carpdevidx	sc_ac.ac_if.if_carpdevidx
	struct task sc_atask;
	struct task sc_ltask;
	struct task sc_dtask;
	struct ip_moptions sc_imo;
#ifdef INET6
	struct ip6_moptions sc_im6o;
#endif /* INET6 */

	SRPL_ENTRY(carp_softc) sc_list;
	struct refcnt sc_refcnt;

	int sc_suppress;
	int sc_bow_out;
	int sc_demote_cnt;

	int sc_sendad_errors;
#define CARP_SENDAD_MAX_ERRORS(sc) (3 * (sc)->sc_vhe_count)
	int sc_sendad_success;
#define CARP_SENDAD_MIN_SUCCESS(sc) (3 * (sc)->sc_vhe_count)

	char sc_curlladdr[ETHER_ADDR_LEN];

	SRPL_HEAD(, carp_vhost_entry) carp_vhosts;
	int sc_vhe_count;
	u_int8_t sc_vhids[CARP_MAXNODES];
	u_int8_t sc_advskews[CARP_MAXNODES];
	u_int8_t sc_balancing;

	int sc_naddrs;
	int sc_naddrs6;
	int sc_advbase;		/* seconds */

	/* authentication */
	unsigned char sc_key[CARP_KEY_LEN];

	u_int32_t sc_hashkey[2];
	u_int32_t sc_lsmask;		/* load sharing mask */
	int sc_lscount;			/* # load sharing interfaces (max 32) */
	int sc_delayed_arp;		/* delayed ARP request countdown */
	int sc_realmac;			/* using real mac */

	struct in_addr sc_peer;

	LIST_HEAD(__carp_mchead, carp_mc_entry)	carp_mc_listhead;
	struct carp_vhost_entry *cur_vhe; /* current active vhe */
};

void	carp_sc_ref(void *, void *);
void	carp_sc_unref(void *, void *);

struct srpl_rc carp_sc_rc =
    SRPL_RC_INITIALIZER(carp_sc_ref, carp_sc_unref, NULL);

int carp_opts[CARPCTL_MAXID] = { 0, 1, 0, LOG_CRIT };	/* XXX for now */
struct cpumem *carpcounters;

int	carp_send_all_recur = 0;

#define	CARP_LOG(l, sc, s)						\
	do {								\
		if (carp_opts[CARPCTL_LOG] >= l) {			\
			if (sc)						\
				log(l, "%s: ",				\
				    (sc)->sc_if.if_xname);		\
			else						\
				log(l, "carp: ");			\
			addlog s;					\
			addlog("\n");					\
		}							\
	} while (0)

void	carp_hmac_prepare(struct carp_softc *);
void	carp_hmac_prepare_ctx(struct carp_vhost_entry *, u_int8_t);
void	carp_hmac_generate(struct carp_vhost_entry *, u_int32_t *,
	    unsigned char *, u_int8_t);
int	carp_hmac_verify(struct carp_vhost_entry *, u_int32_t *,
	    unsigned char *);
void	carp_proto_input_c(struct ifnet *, struct mbuf *,
	    struct carp_header *, int, sa_family_t);
int	carp_proto_input_if(struct ifnet *, struct mbuf **, int *, int);
#ifdef INET6
int	carp6_proto_input_if(struct ifnet *, struct mbuf **, int *, int);
#endif
void	carpattach(int);
void	carpdetach(void *);
void	carp_prepare_ad(struct mbuf *, struct carp_vhost_entry *,
	    struct carp_header *);
void	carp_send_ad_all(void);
void	carp_vhe_send_ad_all(struct carp_softc *);
void	carp_timer_ad(void *);
void	carp_send_ad(struct carp_vhost_entry *);
void	carp_send_arp(struct carp_softc *);
void	carp_timer_down(void *);
void	carp_master_down(struct carp_vhost_entry *);
int	carp_ioctl(struct ifnet *, u_long, caddr_t);
int	carp_vhids_ioctl(struct carp_softc *, struct carpreq *);
int	carp_check_dup_vhids(struct carp_softc *, struct srpl *,
	    struct carpreq *);
void	carp_ifgroup_ioctl(struct ifnet *, u_long, caddr_t);
void	carp_ifgattr_ioctl(struct ifnet *, u_long, caddr_t);
void	carp_start(struct ifnet *);
int	carp_enqueue(struct ifnet *, struct mbuf *);
void	carp_transmit(struct carp_softc *, struct ifnet *, struct mbuf *);
void	carp_setrun_all(struct carp_softc *, sa_family_t);
void	carp_setrun(struct carp_vhost_entry *, sa_family_t);
void	carp_set_state_all(struct carp_softc *, int);
void	carp_set_state(struct carp_vhost_entry *, int);
void	carp_multicast_cleanup(struct carp_softc *);
int	carp_set_ifp(struct carp_softc *, struct ifnet *);
void	carp_set_enaddr(struct carp_softc *);
void	carp_set_vhe_enaddr(struct carp_vhost_entry *);
void	carp_addr_updated(void *);
int	carp_set_addr(struct carp_softc *, struct sockaddr_in *);
int	carp_join_multicast(struct carp_softc *);
#ifdef INET6
void	carp_send_na(struct carp_softc *);
int	carp_set_addr6(struct carp_softc *, struct sockaddr_in6 *);
int	carp_join_multicast6(struct carp_softc *);
#endif
int	carp_clone_create(struct if_clone *, int);
int	carp_clone_destroy(struct ifnet *);
int	carp_ether_addmulti(struct carp_softc *, struct ifreq *);
int	carp_ether_delmulti(struct carp_softc *, struct ifreq *);
void	carp_ether_purgemulti(struct carp_softc *);
int	carp_group_demote_count(struct carp_softc *);
void	carp_update_lsmask(struct carp_softc *);
int	carp_new_vhost(struct carp_softc *, int, int);
void	carp_destroy_vhosts(struct carp_softc *);
void	carp_del_all_timeouts(struct carp_softc *);
int	carp_vhe_match(struct carp_softc *, uint64_t);

struct if_clone carp_cloner =
    IF_CLONE_INITIALIZER("carp", carp_clone_create, carp_clone_destroy);

#define carp_cksum(_m, _l)	((u_int16_t)in_cksum((_m), (_l)))
#define CARP_IFQ_PRIO	6

void
carp_hmac_prepare(struct carp_softc *sc)
{
	struct carp_vhost_entry *vhe;
	u_int8_t i;

	KERNEL_ASSERT_LOCKED(); /* touching carp_vhosts */

	SRPL_FOREACH_LOCKED(vhe, &sc->carp_vhosts, vhost_entries) {
		for (i = 0; i < HMAC_MAX && i >= 0; i++) {  // Ensure no integer underflow
			if (vhe == NULL)  // Ensure vhe is not NULL
				break;
			if (i < UINT8_MAX)  // Ensure no overflow for u_int8_t
				carp_hmac_prepare_ctx(vhe, i);
			else
				break;
		}
	}
}

void
carp_hmac_prepare_ctx(struct carp_vhost_entry *vhe, u_int8_t ctx)
{
	struct carp_softc *sc = vhe->parent_sc;

	u_int8_t version = CARP_VERSION, type = CARP_ADVERTISEMENT;
	u_int8_t vhid = vhe->vhid & 0xff;
	SHA1_CTX sha1ctx;
	u_int32_t kmd[5];
	struct ifaddr *ifa;
	int i, found;
	struct in_addr last, cur, in;
#ifdef INET6
	struct in6_addr last6, cur6, in6;
#endif /* INET6 */

	/* compute ipad from key */
	memset(vhe->vhe_pad, 0, sizeof(vhe->vhe_pad));
	bcopy(sc->sc_key, vhe->vhe_pad, sizeof(sc->sc_key));
	for (i = 0; i < sizeof(vhe->vhe_pad); i++)
		vhe->vhe_pad[i] ^= 0x36;

	/* precompute first part of inner hash */
	SHA1Init(&vhe->vhe_sha1[ctx]);
	SHA1Update(&vhe->vhe_sha1[ctx], vhe->vhe_pad, sizeof(vhe->vhe_pad));
	SHA1Update(&vhe->vhe_sha1[ctx], (void *)&version, sizeof(version));
	SHA1Update(&vhe->vhe_sha1[ctx], (void *)&type, sizeof(type));

	/* generate a key for the arpbalance hash, before the vhid is hashed */
	if (vhe->vhe_leader) {
		bcopy(&vhe->vhe_sha1[ctx], &sha1ctx, sizeof(sha1ctx));
		SHA1Final((unsigned char *)kmd, &sha1ctx);
		sc->sc_hashkey[0] = kmd[0] ^ kmd[1];
		sc->sc_hashkey[1] = kmd[2] ^ kmd[3];
	}

	/* the rest of the precomputation */
	if (!sc->sc_realmac && vhe->vhe_leader &&
	    memcmp(sc->sc_ac.ac_enaddr, vhe->vhe_enaddr, ETHER_ADDR_LEN) != 0)
		SHA1Update(&vhe->vhe_sha1[ctx], sc->sc_ac.ac_enaddr,
		    ETHER_ADDR_LEN);

	SHA1Update(&vhe->vhe_sha1[ctx], (void *)&vhid, sizeof(vhid));

	/* Hash the addresses from smallest to largest, not interface order */
	cur.s_addr = 0;
	do {
		found = 0;
		last = cur;
		cur.s_addr = 0xffffffff;
		TAILQ_FOREACH(ifa, &sc->sc_if.if_addrlist, ifa_list) {
			if (ifa->ifa_addr->sa_family != AF_INET)
				continue;
			in.s_addr = ifatoia(ifa)->ia_addr.sin_addr.s_addr;
			if (ntohl(in.s_addr) > ntohl(last.s_addr) &&
			    ntohl(in.s_addr) < ntohl(cur.s_addr)) {
				cur.s_addr = in.s_addr;
				found++;
			}
		}
		if (found)
			SHA1Update(&vhe->vhe_sha1[ctx],
			    (void *)&cur, sizeof(cur));
	} while (found);
#ifdef INET6
	memset(&cur6, 0x00, sizeof(cur6));
	do {
		found = 0;
		last6 = cur6;
		memset(&cur6, 0xff, sizeof(cur6));
		TAILQ_FOREACH(ifa, &sc->sc_if.if_addrlist, ifa_list) {
			if (ifa->ifa_addr->sa_family != AF_INET6)
				continue;
			in6 = ifatoia6(ifa)->ia_addr.sin6_addr;
			if (IN6_IS_SCOPE_EMBED(&in6)) {
				if (ctx == HMAC_NOV6LL)
					continue;
				in6.s6_addr16[1] = 0;
			}
			if (memcmp(&in6, &last6, sizeof(in6)) > 0 &&
			    memcmp(&in6, &cur6, sizeof(in6)) < 0) {
				cur6 = in6;
				found++;
			}
		}
		if (found)
			SHA1Update(&vhe->vhe_sha1[ctx],
			    (void *)&cur6, sizeof(cur6));
	} while (found);
#endif /* INET6 */

	/* convert ipad to opad */
	for (i = 0; i < sizeof(vhe->vhe_pad); i++)
		vhe->vhe_pad[i] ^= 0x36 ^ 0x5c;
}

void
carp_hmac_generate(struct carp_vhost_entry *vhe, u_int32_t counter[2],
    unsigned char md[20], u_int8_t ctx)
{
    SHA1_CTX sha1ctx;

    /* Validate ctx to avoid out-of-bounds access */
    if (ctx >= sizeof(vhe->vhe_sha1) / sizeof(vhe->vhe_sha1[0])) {
        return; // or handle the error as appropriate
    }

    /* fetch first half of inner hash */
    bcopy(&vhe->vhe_sha1[ctx], &sha1ctx, sizeof(sha1ctx));

    /* Validate the size of counter to avoid overflow */
    if (sizeof(vhe->vhe_replay_cookie) > SIZE_MAX - sizeof(SHA1_CTX)) {
        return; // or handle the error as appropriate
    }

    SHA1Update(&sha1ctx, (void *)counter, sizeof(vhe->vhe_replay_cookie));
    SHA1Final(md, &sha1ctx);

    /* outer hash */
    SHA1Init(&sha1ctx);

    /* Ensure that sizeof(vhe->vhe_pad) does not cause overflow */
    if (sizeof(vhe->vhe_pad) > SIZE_MAX - sizeof(sha1ctx)) {
        return; // or handle the error as appropriate
    }

    SHA1Update(&sha1ctx, vhe->vhe_pad, sizeof(vhe->vhe_pad));

    /* Validate md size before updating */
    if (20 > SIZE_MAX - sizeof(sha1ctx)) {
        return; // or handle the error as appropriate
    }

    SHA1Update(&sha1ctx, md, 20);
    SHA1Final(md, &sha1ctx);
}

int
carp_hmac_verify(struct carp_vhost_entry *vhe, u_int32_t counter[2],
    unsigned char md[20])
{
	unsigned char md2[20];
	u_int8_t i;

	for (i = 0; i < HMAC_MAX; i++) {
		carp_hmac_generate(vhe, counter, md2, i);
		if (!timingsafe_bcmp(md, md2, sizeof(md2)))
			return (0);
	}
	return (1);
}

int
carp_proto_input(struct mbuf **mp, int *offp, int proto, int af)
{
	struct ifnet *ifp;

	if (mp == NULL || *mp == NULL || offp == NULL) {
		if (mp != NULL && *mp != NULL) {
			m_freemp(mp);
		}
		return IPPROTO_DONE;
	}

	ifp = if_get((*mp)->m_pkthdr.ph_ifidx);
	if (ifp == NULL) {
		m_freemp(mp);
		return IPPROTO_DONE;
	}

	proto = carp_proto_input_if(ifp, mp, offp, proto);
	if_put(ifp);
	return proto;
}

/*
 * process input packet.
 * we have rearranged checks order compared to the rfc,
 * but it seems more efficient this way or not possible otherwise.
 */
int
carp_proto_input_if(struct ifnet *ifp, struct mbuf **mp, int *offp, int proto)
{
	struct mbuf *m = *mp;
	struct ip *ip = mtod(m, struct ip *);
	struct carp_softc *sc = NULL;
	struct carp_header *ch;
	int iplen, len, ismulti;

	carpstat_inc(carps_ipackets);

	if (!carp_opts[CARPCTL_ALLOW]) {
		m_freem(m);
		return IPPROTO_DONE;
	}

	ismulti = IN_MULTICAST(ip->ip_dst.s_addr);

	/* check if received on a valid carp interface */
	switch (ifp->if_type) {
	case IFT_CARP:
		break;
	case IFT_ETHER:
		if (ismulti || !SRPL_EMPTY_LOCKED(&ifp->if_carp))
			break;
		/* FALLTHROUGH */
	default:
		carpstat_inc(carps_badif);
		CARP_LOG(LOG_INFO, sc,
		    ("packet received on non-carp interface: %s",
		     ifp->if_xname));
		m_freem(m);
		return IPPROTO_DONE;
	}

	/* verify that the IP TTL is 255.  */
	if (ip->ip_ttl != CARP_DFLTTL) {
		carpstat_inc(carps_badttl);
		CARP_LOG(LOG_NOTICE, sc, ("received ttl %d != %d on %s",
		    ip->ip_ttl, CARP_DFLTTL, ifp->if_xname));
		m_freem(m);
		return IPPROTO_DONE;
	}

	/*
	 * verify that the received packet length is
	 * equal to the CARP header
	 */
	iplen = ip->ip_hl << 2;
	if (iplen < sizeof(struct ip)) {
		carpstat_inc(carps_badlen);
		CARP_LOG(LOG_INFO, sc, ("invalid IP header length %d on %s",
		    iplen, ifp->if_xname));
		m_freem(m);
		return IPPROTO_DONE;
	}
	if (iplen > m->m_pkthdr.len) {
		carpstat_inc(carps_badlen);
		CARP_LOG(LOG_INFO, sc, ("packet too short %d on %s",
		    m->m_pkthdr.len, ifp->if_xname));
		m_freem(m);
		return IPPROTO_DONE;
	}
	len = iplen + sizeof(*ch);
	if (len < iplen || len > m->m_pkthdr.len) { // Check for integer overflow and bounds
		carpstat_inc(carps_badlen);
		CARP_LOG(LOG_INFO, sc, ("packet length %d invalid on %s",
		    m->m_pkthdr.len, ifp->if_xname));
		m_freem(m);
		return IPPROTO_DONE;
	}

	if ((m = *mp = m_pullup(m, len)) == NULL) {
		carpstat_inc(carps_hdrops);
		return IPPROTO_DONE;
	}
	ip = mtod(m, struct ip *);
	ch = (struct carp_header *)(mtod(m, caddr_t) + iplen);

	/* verify the CARP checksum */
	m->m_data += iplen;
	if (carp_cksum(m, len - iplen)) {
		carpstat_inc(carps_badsum);
		CARP_LOG(LOG_INFO, sc, ("checksum failed on %s",
		    ifp->if_xname));
		m_freem(m);
		return IPPROTO_DONE;
	}
	m->m_data -= iplen;

	KERNEL_LOCK();
	carp_proto_input_c(ifp, m, ch, ismulti, AF_INET);
	KERNEL_UNLOCK();
	return IPPROTO_DONE;
}

#ifdef INET6
int
carp6_proto_input(struct mbuf **mp, int *offp, int proto, int af)
{
	struct ifnet *ifp;

	if (mp == NULL || *mp == NULL || offp == NULL)
		return IPPROTO_DONE;

	ifp = if_get((*mp)->m_pkthdr.ph_ifidx);
	if (ifp == NULL) {
		m_freemp(mp);
		return IPPROTO_DONE;
	}

	proto = carp6_proto_input_if(ifp, mp, offp, proto);
	if_put(ifp);
	return proto;
}

int
carp6_proto_input_if(struct ifnet *ifp, struct mbuf **mp, int *offp, int proto)
{
	struct mbuf *m = *mp;
	struct carp_softc *sc = NULL;
	struct ip6_hdr *ip6;
	struct carp_header *ch;
	u_int len;

	/* Ensure mbuf is large enough to contain an IP6 header */
	if (m->m_len < sizeof(struct ip6_hdr)) {
		m_freem(m);
		return IPPROTO_DONE;
	}

	ip6 = mtod(m, struct ip6_hdr *);

	carpstat_inc(carps_ipackets6);

	if (!carp_opts[CARPCTL_ALLOW]) {
		m_freem(m);
		return IPPROTO_DONE;
	}

	/* check if received on a valid carp interface */
	if (ifp->if_type != IFT_CARP) {
		carpstat_inc(carps_badif);
		CARP_LOG(LOG_INFO, sc, ("packet received on non-carp interface: %s",
		    ifp->if_xname));
		m_freem(m);
		return IPPROTO_DONE;
	}

	/* verify that the IP TTL is 255 */
	if (ip6->ip6_hlim != CARP_DFLTTL) {
		carpstat_inc(carps_badttl);
		CARP_LOG(LOG_NOTICE, sc, ("received ttl %d != %d on %s",
		    ip6->ip6_hlim, CARP_DFLTTL, ifp->if_xname));
		m_freem(m);
		return IPPROTO_DONE;
	}

	/* verify that we have a complete carp packet */
	len = m->m_len;
	if (len < *offp + sizeof(*ch)) {
		carpstat_inc(carps_badlen);
		CARP_LOG(LOG_INFO, sc, ("packet size %u too small", len));
		m_freem(m);
		return IPPROTO_DONE;
	}

	if ((m = *mp = m_pullup(m, *offp + sizeof(*ch))) == NULL) {
		carpstat_inc(carps_badlen);
		CARP_LOG(LOG_INFO, sc, ("packet size %u too small", len));
		return IPPROTO_DONE;
	}

	/* Ensure that m_pullup succeeded in providing the necessary data */
	if (m->m_len < *offp + sizeof(*ch)) {
		carpstat_inc(carps_badlen);
		m_freem(m);
		return IPPROTO_DONE;
	}

	ch = (struct carp_header *)(mtod(m, caddr_t) + *offp);

	/* verify the CARP checksum */
	m->m_data += *offp;
	if (carp_cksum(m, sizeof(*ch))) {
		carpstat_inc(carps_badsum);
		CARP_LOG(LOG_INFO, sc, ("checksum failed, on %s",
		    ifp->if_xname));
		m_freem(m);
		return IPPROTO_DONE;
	}
	m->m_data -= *offp;

	KERNEL_LOCK();
	carp_proto_input_c(ifp, m, ch, 1, AF_INET6);
	KERNEL_UNLOCK();
	return IPPROTO_DONE;
}
#endif /* INET6 */

void
carp_proto_input_c(struct ifnet *ifp, struct mbuf *m, struct carp_header *ch,
    int ismulti, sa_family_t af)
{
	struct carp_softc *sc;
	struct ifnet *ifp0;
	struct carp_vhost_entry *vhe;
	struct timeval sc_tv, ch_tv;
	struct srpl *cif;

	KERNEL_ASSERT_LOCKED(); /* touching if_carp + carp_vhosts */

	ifp0 = if_get(ifp->if_carpdevidx);

	if (ifp->if_type == IFT_CARP) {
		/*
		 * If the parent of this carp(4) got destroyed while
		 * `m' was being processed, silently drop it.
		 */
		if (ifp0 == NULL)
			goto rele;
		cif = &ifp0->if_carp;
	} else
		cif = &ifp->if_carp;

	SRPL_FOREACH_LOCKED(sc, cif, sc_list) {
		if (af == AF_INET &&
		    ismulti != IN_MULTICAST(sc->sc_peer.s_addr))
			continue;
		SRPL_FOREACH_LOCKED(vhe, &sc->carp_vhosts, vhost_entries) {
			if (vhe->vhid == ch->carp_vhid)
				goto found;
		}
	}
 found:

	if (!sc || (sc->sc_if.if_flags & (IFF_UP|IFF_RUNNING)) !=
	    (IFF_UP|IFF_RUNNING)) {
		carpstat_inc(carps_badvhid);
		goto rele;
	}

	getmicrotime(&sc->sc_if.if_lastchange);

	/* verify the CARP version. */
	if (ch->carp_version != CARP_VERSION) {
		carpstat_inc(carps_badver);
		sc->sc_if.if_ierrors++;
		CARP_LOG(LOG_NOTICE, sc, ("invalid version %d != %d",
		    ch->carp_version, CARP_VERSION));
		goto rele;
	}

	/* verify the hash */
	if (carp_hmac_verify(vhe, ch->carp_counter, ch->carp_md)) {
		carpstat_inc(carps_badauth);
		sc->sc_if.if_ierrors++;
		CARP_LOG(LOG_INFO, sc, ("incorrect hash"));
		goto rele;
	}

	if (!memcmp(&vhe->vhe_replay_cookie, ch->carp_counter,
	    sizeof(ch->carp_counter))) {
		struct ifnet *ifp2;

		ifp2 = if_get(sc->sc_carpdevidx);
		/* Do not log duplicates from non simplex interfaces */
		if (ifp2 && ifp2->if_flags & IFF_SIMPLEX) {
			carpstat_inc(carps_badauth);
			sc->sc_if.if_ierrors++;
			CARP_LOG(LOG_WARNING, sc,
			    ("replay or network loop detected"));
		}
		if_put(ifp2);
		goto rele;
	}

	sc_tv.tv_sec = sc->sc_advbase;
	sc_tv.tv_usec = vhe->advskew * 1000000 / 256;
	ch_tv.tv_sec = ch->carp_advbase;
	ch_tv.tv_usec = ch->carp_advskew * 1000000 / 256;

	switch (vhe->state) {
	case INIT:
		break;
	case MASTER:
		/*
		 * If we receive an advertisement from a master who's going to
		 * be more frequent than us, and whose demote count is not higher
		 * than ours, go into BACKUP state. If his demote count is lower,
		 * also go into BACKUP.
		 */
		if (((timercmp(&sc_tv, &ch_tv, >) ||
		    timercmp(&sc_tv, &ch_tv, ==)) &&
		    (ch->carp_demote <= carp_group_demote_count(sc))) ||
		    ch->carp_demote < carp_group_demote_count(sc)) {
			timeout_del(&vhe->ad_tmo);
			carp_set_state(vhe, BACKUP);
			carp_setrun(vhe, 0);
		}
		break;
	case BACKUP:
		/*
		 * If we're pre-empting masters who advertise slower than us,
		 * and do not have a better demote count, treat them as down.
		 *
		 */
		if (carp_opts[CARPCTL_PREEMPT] &&
		    timercmp(&sc_tv, &ch_tv, <) &&
		    ch->carp_demote >= carp_group_demote_count(sc)) {
			carp_master_down(vhe);
			break;
		}

		/*
		 * Take over masters advertising with a higher demote count,
		 * regardless of CARPCTL_PREEMPT.
		 */
		if (ch->carp_demote > carp_group_demote_count(sc)) {
			carp_master_down(vhe);
			break;
		}

		/*
		 *  If the master is going to advertise at such a low frequency
		 *  that he's guaranteed to time out, we'd might as well just
		 *  treat him as timed out now.
		 */
		sc_tv.tv_sec = sc->sc_advbase * 3;
		if (sc->sc_advbase && timercmp(&sc_tv, &ch_tv, <)) {
			carp_master_down(vhe);
			break;
		}

		/*
		 * Otherwise, we reset the counter and wait for the next
		 * advertisement.
		 */
		carp_setrun(vhe, af);
		break;
	}

rele:
	if_put(ifp0);
	m_freem(m);
	return;
}

int
carp_sysctl_carpstat(void *oldp, size_t *oldlenp, void *newp)
{
    struct carpstats carpstat;

    CTASSERT(sizeof(carpstat) == (carps_ncounters * sizeof(uint64_t)));
    memset(&carpstat, 0, sizeof carpstat);

    if (carps_ncounters > (SIZE_MAX / sizeof(uint64_t))) {
        return -1; // Avoid integer overflow
    }

    counters_read(carpcounters, (uint64_t *)&carpstat, carps_ncounters, NULL);

    if (oldlenp == NULL || *oldlenp < sizeof(carpstat)) {
        return -1; // Avoid out-of-bounds read/write
    }

    return (sysctl_rdstruct(oldp, oldlenp, newp, &carpstat, sizeof(carpstat)));
}

int
carp_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen)
{
	int error;

	/* All sysctl names at this level are terminal. */
	if (namelen != 1)
		return (ENOTDIR);

	if (name == NULL || oldlenp == NULL)
		return (EINVAL);

	if (*oldlenp > SSIZE_MAX || newlen > SSIZE_MAX)
		return (EOVERFLOW);

	switch (name[0]) {
	case CARPCTL_STATS:
		return (carp_sysctl_carpstat(oldp, oldlenp, newp));
	default:
		if (name[0] <= 0 || name[0] >= CARPCTL_MAXID)
			return (ENOPROTOOPT);
		NET_LOCK();
		error = sysctl_int(oldp, oldlenp, newp, newlen,
		    &carp_opts[name[0]]);
		NET_UNLOCK();
		return (error);
	}
}

/*
 * Interface side of the CARP implementation.
 */

void
carpattach(int n)
{
	if (n < 0 || n > INT_MAX) {
		return;  // Invalid input check.
	}

	if_creategroup("carp");  /* keep around even if empty */
	if_clone_attach(&carp_cloner);

	if (carps_ncounters <= 0 || carps_ncounters > INT_MAX) {
		return;  // Avoid integer overflow in counters_alloc.
	}

	carpcounters = counters_alloc(carps_ncounters);
}

int
carp_clone_create(struct if_clone *ifc, int unit)
{
	struct carp_softc *sc;
	struct ifnet *ifp;

	/* Check for integer overflow in malloc size calculation */
	if (sizeof(*sc) > SIZE_MAX / sizeof(struct carp_softc))
		return (ENOMEM);

	sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK|M_ZERO);
	if (sc == NULL)
		return (ENOMEM);

	refcnt_init(&sc->sc_refcnt);

	SRPL_INIT(&sc->carp_vhosts);
	sc->sc_vhe_count = 0;
	if (carp_new_vhost(sc, 0, 0)) {
		free(sc, M_DEVBUF, sizeof(*sc));
		return (ENOMEM);
	}

	task_set(&sc->sc_atask, carp_addr_updated, sc);
	task_set(&sc->sc_ltask, carp_carpdev_state, sc);
	task_set(&sc->sc_dtask, carpdetach, sc);

	sc->sc_suppress = 0;
	sc->sc_advbase = CARP_DFLTINTV;
	sc->sc_naddrs = sc->sc_naddrs6 = 0;
#ifdef INET6
	sc->sc_im6o.im6o_hlim = CARP_DFLTTL;
#endif /* INET6 */

	/* Check for integer overflow in mallocarray size calculation */
	if (IP_MIN_MEMBERSHIPS > SIZE_MAX / sizeof(struct in_multi *))
		return (ENOMEM);

	sc->sc_imo.imo_membership = mallocarray(IP_MIN_MEMBERSHIPS,
	    sizeof(struct in_multi *), M_IPMOPTS, M_WAITOK|M_ZERO);
	if (sc->sc_imo.imo_membership == NULL) {
		free(sc, M_DEVBUF, sizeof(*sc));
		return (ENOMEM);
	}
	sc->sc_imo.imo_max_memberships = IP_MIN_MEMBERSHIPS;

	LIST_INIT(&sc->carp_mc_listhead);
	ifp = &sc->sc_if;
	ifp->if_softc = sc;

	/* Check ifc->ifc_name length to avoid buffer overflow */
	if (strlen(ifc->ifc_name) > IFNAMSIZ - 1)
		return (EINVAL);

	snprintf(ifp->if_xname, sizeof ifp->if_xname, "%s%d", ifc->ifc_name,
	    unit);
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_ioctl = carp_ioctl;
	ifp->if_start = carp_start;
	ifp->if_enqueue = carp_enqueue;
	ifp->if_xflags = IFXF_CLONED;
	if_counters_alloc(ifp);
	if_attach(ifp);
	ether_ifattach(ifp);
	ifp->if_type = IFT_CARP;
	ifp->if_sadl->sdl_type = IFT_CARP;
	ifp->if_output = carp_output;
	ifp->if_priority = IF_CARP_DEFAULT_PRIORITY;
	ifp->if_link_state = LINK_STATE_INVALID;

	/* Hook carp_addr_updated to cope with address and route changes. */
	if_addrhook_add(&sc->sc_if, &sc->sc_atask);

	return (0);
}

int
carp_new_vhost(struct carp_softc *sc, int vhid, int advskew)
{
	struct carp_vhost_entry *vhe, *vhe0;

	if (sc == NULL || vhid < 0 || vhid > 255 || advskew < 0 || advskew > 255)
		return (EINVAL);

	vhe = malloc(sizeof(*vhe), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (vhe == NULL)
		return (ENOMEM);

	refcnt_init(&vhe->vhost_refcnt);
	carp_sc_ref(NULL, sc); /* give a sc ref to the vhe */
	vhe->parent_sc = sc;
	vhe->vhid = vhid;
	vhe->advskew = advskew;
	vhe->state = INIT;
	timeout_set_proc(&vhe->ad_tmo, carp_timer_ad, vhe);
	timeout_set_proc(&vhe->md_tmo, carp_timer_down, vhe);
	timeout_set_proc(&vhe->md6_tmo, carp_timer_down, vhe);

	KERNEL_ASSERT_LOCKED(); /* touching carp_vhosts */

	/* mark the first vhe as leader */
	if (SRPL_EMPTY_LOCKED(&sc->carp_vhosts)) {
		vhe->vhe_leader = 1;
		SRPL_INSERT_HEAD_LOCKED(&carp_vh_rc, &sc->carp_vhosts,
		    vhe, vhost_entries);
		sc->sc_vhe_count = 1;
		return (0);
	}

	SRPL_FOREACH_LOCKED(vhe0, &sc->carp_vhosts, vhost_entries) {
		if (SRPL_NEXT_LOCKED(vhe0, vhost_entries) == NULL)
			break;
	}

	SRPL_INSERT_AFTER_LOCKED(&carp_vh_rc, vhe0, vhe, vhost_entries);
	if (sc->sc_vhe_count == INT_MAX)
		return (ERANGE);
	sc->sc_vhe_count++;

	return (0);
}

int
carp_clone_destroy(struct ifnet *ifp)
{
	struct carp_softc *sc = ifp->if_softc;

	if_addrhook_del(&sc->sc_if, &sc->sc_atask);

	NET_LOCK();
	carpdetach(sc);
	NET_UNLOCK();

	ether_ifdetach(ifp);
	if_detach(ifp);
	carp_destroy_vhosts(ifp->if_softc);
	refcnt_finalize(&sc->sc_refcnt, "carpdtor");
	free(sc->sc_imo.imo_membership, M_IPMOPTS,
	    sc->sc_imo.imo_max_memberships * sizeof(struct in_multi *));
	free(sc, M_DEVBUF, sizeof(*sc));
	return (0);
}

void
carp_del_all_timeouts(struct carp_softc *sc)
{
	struct carp_vhost_entry *vhe;
	uintptr_t sc_base = (uintptr_t)sc;
	uintptr_t sc_end = sc_base + sizeof(*sc);

	KERNEL_ASSERT_LOCKED(); /* touching carp_vhosts */
	SRPL_FOREACH_LOCKED(vhe, &sc->carp_vhosts, vhost_entries) {
		uintptr_t vhe_base = (uintptr_t)vhe;
		uintptr_t vhe_end = vhe_base + sizeof(*vhe);
		
        if (vhe_base < sc_base || vhe_end > sc_end || vhe_base > vhe_end) {
            break;
        }

		timeout_del(&vhe->ad_tmo);
		timeout_del(&vhe->md_tmo);
		timeout_del(&vhe->md6_tmo);
	}
}

void
carpdetach(void *arg)
{
    struct carp_softc *sc = arg;
    struct ifnet *ifp0;
    struct srpl *cif;

    carp_del_all_timeouts(sc);

    if (sc->sc_demote_cnt)
        carp_group_demote_adj(&sc->sc_if, -sc->sc_demote_cnt, "detach");
    sc->sc_suppress = 0;
    sc->sc_sendad_errors = 0;

    carp_set_state_all(sc, INIT);
    sc->sc_if.if_flags &= ~IFF_UP;
    carp_setrun_all(sc, 0);
    carp_multicast_cleanup(sc);

    if (sc->sc_carpdevidx < 0)  // Ensure sc_carpdevidx is non-negative
        return;
    ifp0 = if_get(sc->sc_carpdevidx);
    if (ifp0 == NULL)
        return;

    KERNEL_ASSERT_LOCKED(); /* touching if_carp */

    cif = &ifp0->if_carp;

    SRPL_REMOVE_LOCKED(&carp_sc_rc, cif, sc, carp_softc, sc_list);
    sc->sc_carpdevidx = 0;

    if_linkstatehook_del(ifp0, &sc->sc_ltask);
    if_detachhook_del(ifp0, &sc->sc_dtask);
    ifpromisc(ifp0, 0);
    if_put(ifp0);
}

void
carp_destroy_vhosts(struct carp_softc *sc)
{
	/* XXX bow out? */
	struct carp_vhost_entry *vhe;

	KERNEL_ASSERT_LOCKED(); /* touching carp_vhosts */

	while ((vhe = SRPL_FIRST_LOCKED(&sc->carp_vhosts)) != NULL) {
		SRPL_REMOVE_LOCKED(&carp_vh_rc, &sc->carp_vhosts, vhe,
		    carp_vhost_entry, vhost_entries);
		carp_vh_unref(NULL, vhe); /* drop last ref */
	}
	sc->sc_vhe_count = 0;
}

void
carp_prepare_ad(struct mbuf *m, struct carp_vhost_entry *vhe,
    struct carp_header *ch)
{
	if (!vhe->vhe_replay_cookie) {
		arc4random_buf(&vhe->vhe_replay_cookie,
		    sizeof(vhe->vhe_replay_cookie));
	}

	if (sizeof(vhe->vhe_replay_cookie) > sizeof(ch->carp_counter)) {
		/* Handle error appropriately, e.g., log error, return, etc. */
		return;
	}

	bcopy(&vhe->vhe_replay_cookie, ch->carp_counter,
	    sizeof(vhe->vhe_replay_cookie));

	/*
	 * For the time being, do not include the IPv6 linklayer addresses
	 * in the HMAC.
	 */
	carp_hmac_generate(vhe, ch->carp_counter, ch->carp_md, HMAC_NOV6LL);
}

void
carp_send_ad_all(void)
{
    struct ifnet *ifp0;
    struct srpl *cif;
    struct carp_softc *vh;

    KERNEL_ASSERT_LOCKED(); /* touching if_carp */

    if (carp_send_all_recur > 0 || carp_send_all_recur == INT_MAX)
        return;
    ++carp_send_all_recur;
    TAILQ_FOREACH(ifp0, &ifnetlist, if_list) {
        if (ifp0->if_type != IFT_ETHER)
            continue;

        cif = &ifp0->if_carp;
        SRPL_FOREACH_LOCKED(vh, cif, sc_list) {
            if ((vh->sc_if.if_flags & (IFF_UP|IFF_RUNNING)) ==
                (IFF_UP|IFF_RUNNING)) {
                carp_vhe_send_ad_all(vh);
            }
        }
    }
    --carp_send_all_recur;
}

void
carp_vhe_send_ad_all(struct carp_softc *sc)
{
	struct carp_vhost_entry *vhe;

	KERNEL_ASSERT_LOCKED(); /* touching carp_vhosts */

	SRPL_FOREACH_LOCKED(vhe, &sc->carp_vhosts, vhost_entries) {
		if (vhe != NULL && vhe->state == MASTER)
			carp_send_ad(vhe);
	}
}

void
carp_timer_ad(void *v)
{
	if (v == NULL) {
		return; // Avoids dereferencing a NULL pointer
	}

	if (v < (void*)0x1000 || v > (void*)ULONG_MAX) {
		return; // Avoids potential out-of-bounds by checking a valid memory range
	}

	NET_LOCK();
	carp_send_ad(v);
	NET_UNLOCK();
}

void
carp_send_ad(struct carp_vhost_entry *vhe)
{
	struct carp_header ch;
	struct timeval tv;
	struct carp_softc *sc = vhe->parent_sc;
	struct carp_header *ch_ptr;
	struct mbuf *m;
	int error, len, advbase, advskew;
	struct ifnet *ifp;
	struct ifaddr *ifa;
	struct sockaddr sa;

	NET_ASSERT_LOCKED();

	if ((ifp = if_get(sc->sc_carpdevidx)) == NULL) {
		sc->sc_if.if_oerrors++;
		return;
	}

	/* bow out if we've gone to backup (the carp interface is going down) */
	if (sc->sc_bow_out) {
		advbase = 255;
		advskew = 255;
	} else {
		advbase = sc->sc_advbase;
		advskew = vhe->advskew;
		tv.tv_sec = advbase;
		if (advbase == 0 && advskew == 0)
			tv.tv_usec = 1 * 1000000 / 256;
		else
			tv.tv_usec = advskew * 1000000 / 256;
	}

	ch.carp_version = CARP_VERSION;
	ch.carp_type = CARP_ADVERTISEMENT;
	ch.carp_vhid = vhe->vhid;
	ch.carp_demote = carp_group_demote_count(sc) & 0xff;
	ch.carp_advbase = advbase;
	ch.carp_advskew = advskew;
	ch.carp_authlen = 7;	/* XXX DEFINE */
	ch.carp_cksum = 0;

	sc->cur_vhe = vhe; /* we need the vhe later on the output path */

	if (sc->sc_naddrs) {
		struct ip *ip;

		MGETHDR(m, M_DONTWAIT, MT_HEADER);
		if (m == NULL) {
			sc->sc_if.if_oerrors++;
			carpstat_inc(carps_onomem);
			/* XXX maybe less ? */
			goto retry_later;
		}
		len = sizeof(*ip) + sizeof(ch);
		m->m_pkthdr.pf.prio = CARP_IFQ_PRIO;
		m->m_pkthdr.ph_rtableid = sc->sc_if.if_rdomain;
		m->m_pkthdr.len = len;
		m->m_len = len;
		m_align(m, len);
		ip = mtod(m, struct ip *);
		ip->ip_v = IPVERSION;
		ip->ip_hl = sizeof(*ip) >> 2;
		ip->ip_tos = IPTOS_LOWDELAY;
		ip->ip_len = htons(len);
		ip->ip_id = htons(ip_randomid());
		ip->ip_off = htons(IP_DF);
		ip->ip_ttl = CARP_DFLTTL;
		ip->ip_p = IPPROTO_CARP;
		ip->ip_sum = 0;

		memset(&sa, 0, sizeof(sa));
		sa.sa_family = AF_INET;
		/* Prefer addresses on the parent interface as source for AD. */
		ifa = ifaof_ifpforaddr(&sa, ifp);
		if (ifa == NULL)
			ifa = ifaof_ifpforaddr(&sa, &sc->sc_if);
		KASSERT(ifa != NULL);
		ip->ip_src.s_addr = ifatoia(ifa)->ia_addr.sin_addr.s_addr;
		ip->ip_dst.s_addr = sc->sc_peer.s_addr;
		if (IN_MULTICAST(ip->ip_dst.s_addr))
			m->m_flags |= M_MCAST;

		ch_ptr = (struct carp_header *)(ip + 1);
		bcopy(&ch, ch_ptr, sizeof(ch));
		carp_prepare_ad(m, vhe, ch_ptr);

		m->m_data += sizeof(*ip);
		ch_ptr->carp_cksum = carp_cksum(m, len - sizeof(*ip));
		m->m_data -= sizeof(*ip);

		getmicrotime(&sc->sc_if.if_lastchange);
		carpstat_inc(carps_opackets);

		error = ip_output(m, NULL, NULL, IP_RAWOUTPUT, &sc->sc_imo,
		    NULL, 0);
		if (error &&
		    /* when unicast, the peer's down is not our fault */
		    !(!IN_MULTICAST(sc->sc_peer.s_addr) && error == EHOSTDOWN)){
			if (error == ENOBUFS)
				carpstat_inc(carps_onomem);
			else
				CARP_LOG(LOG_WARNING, sc,
				    ("ip_output failed: %d", error));
			sc->sc_if.if_oerrors++;
			if (sc->sc_sendad_errors < INT_MAX)
				sc->sc_sendad_errors++;
			if (sc->sc_sendad_errors == CARP_SENDAD_MAX_ERRORS(sc))
				carp_group_demote_adj(&sc->sc_if, 1,
				    "> snderrors");
			sc->sc_sendad_success = 0;
		} else {
			if (sc->sc_sendad_errors >= CARP_SENDAD_MAX_ERRORS(sc)) {
				if (++sc->sc_sendad_success >=
				    CARP_SENDAD_MIN_SUCCESS(sc)) {
					carp_group_demote_adj(&sc->sc_if, -1,
					    "< snderrors");
					sc->sc_sendad_errors = 0;
				}
			} else
				sc->sc_sendad_errors = 0;
		}
		if (vhe->vhe_leader) {
			if (sc->sc_delayed_arp > 0)
				sc->sc_delayed_arp--;
			if (sc->sc_delayed_arp == 0) {
				carp_send_arp(sc);
				sc->sc_delayed_arp = -1;
			}
		}
	}
#ifdef INET6
	if (sc->sc_naddrs6) {
		struct ip6_hdr *ip6;
		size_t ip6_size = sizeof(*ip6);
		size_t ch_size = sizeof(ch);

		if (ip6_size > SIZE_MAX - ch_size) {
			sc->sc_if.if_oerrors++;
			carpstat_inc(carps_onomem);
			goto retry_later;
		}
		
		size_t len = ip6_size + ch_size;

		MGETHDR(m, M_DONTWAIT, MT_HEADER);
		if (m == NULL || len > MCLBYTES) {  // Check for null pointer and ensure len does not exceed maximum cluster size.
			sc->sc_if.if_oerrors++;
			carpstat_inc(carps_onomem);
			goto retry_later;
		}
		m->m_pkthdr.pf.prio = CARP_IFQ_PRIO;
		m->m_pkthdr.ph_rtableid = sc->sc_if.if_rdomain;
		m->m_pkthdr.len = len;
		m->m_len = len;
		m_align(m, len);
		m->m_flags |= M_MCAST;
		ip6 = mtod(m, struct ip6_hdr *);
		memset(ip6, 0, sizeof(*ip6));
		ip6->ip6_vfc |= IPV6_VERSION;
		ip6->ip6_hlim = CARP_DFLTTL;
		ip6->ip6_nxt = IPPROTO_CARP;

		/* set the source address */
		memset(&sa, 0, sizeof(sa));
		sa.sa_family = AF_INET6;
		/* Prefer addresses on the parent interface as source for AD. */
		ifa = ifaof_ifpforaddr(&sa, ifp);
		if (ifa == NULL)
			ifa = ifaof_ifpforaddr(&sa, &sc->sc_if);
		KASSERT(ifa != NULL);
		
		struct in6_addr *src_addr = &ifatoia6(ifa)->ia_addr.sin6_addr;
		if (src_addr == NULL) {
			sc->sc_if.if_oerrors++;
			carpstat_inc(carps_onomem);
			m_freem(m);
			goto retry_later;
		}
		bcopy(src_addr, &ip6->ip6_src, sizeof(struct in6_addr));
		
		/* set the multicast destination */
		ip6->ip6_dst.s6_addr16[0] = htons(0xff02);
		ip6->ip6_dst.s6_addr16[1] = htons(ifp->if_index);
		ip6->ip6_dst.s6_addr8[15] = 0x12;

		ch_ptr = (struct carp_header *)(ip6 + 1);
		bcopy(&ch, ch_ptr, sizeof(ch));
		carp_prepare_ad(m, vhe, ch_ptr);

		if (m->m_data + sizeof(*ip6) > m->m_ext.ext_buf + MCLBYTES) {
			sc->sc_if.if_oerrors++;
			carpstat_inc(carps_onomem);
			m_freem(m);
			goto retry_later;
		}
		m->m_data += sizeof(*ip6);
		ch_ptr->carp_cksum = carp_cksum(m, len - sizeof(*ip6));
		m->m_data -= sizeof(*ip6);

		getmicrotime(&sc->sc_if.if_lastchange);
		carpstat_inc(carps_opackets6);

		error = ip6_output(m, NULL, NULL, 0, &sc->sc_im6o, NULL);
		if (error) {
			if (error == ENOBUFS)
				carpstat_inc(carps_onomem);
			else
				CARP_LOG(LOG_WARNING, sc, ("ip6_output failed: %d", error));
			sc->sc_if.if_oerrors++;
			if (sc->sc_sendad_errors < INT_MAX)
				sc->sc_sendad_errors++;
			if (sc->sc_sendad_errors == CARP_SENDAD_MAX_ERRORS(sc))
				carp_group_demote_adj(&sc->sc_if, 1, "> snd6errors");
			sc->sc_sendad_success = 0;
		} else {
			if (sc->sc_sendad_errors >= CARP_SENDAD_MAX_ERRORS(sc)) {
				if (++sc->sc_sendad_success >= CARP_SENDAD_MIN_SUCCESS(sc)) {
					carp_group_demote_adj(&sc->sc_if, -1, "< snd6errors");
					sc->sc_sendad_errors = 0;
				}
			} else
				sc->sc_sendad_errors = 0;
		}
	}
#endif /* INET6 */

retry_later:
	sc->cur_vhe = NULL;
	if (advbase != 255 || advskew != 255)
		timeout_add_tv(&vhe->ad_tmo, &tv);
	if_put(ifp);
}

/*
 * Broadcast a gratuitous ARP request containing
 * the virtual router MAC address for each IP address
 * associated with the virtual router.
 */
void
carp_send_arp(struct carp_softc *sc)
{
	struct ifaddr *ifa;
	in_addr_t in;

	TAILQ_FOREACH(ifa, &sc->sc_if.if_addrlist, ifa_list) {

		if (ifa->ifa_addr->sa_family != AF_INET)
			continue;

		in = ifatoia(ifa)->ia_addr.sin_addr.s_addr;
		arprequest(&sc->sc_if, &in, &in, sc->sc_ac.ac_enaddr);
	}
}

#ifdef INET6
void
carp_send_na(struct carp_softc *sc)
{
	struct ifaddr *ifa;
	struct in6_addr *in6;
	static struct in6_addr mcast = IN6ADDR_LINKLOCAL_ALLNODES_INIT;

	TAILQ_FOREACH(ifa, &sc->sc_if.if_addrlist, ifa_list) {

		if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET6)
			continue;

		struct in6_ifaddr *ia6 = ifatoia6(ifa);
		if (ia6 == NULL)
			continue;
			
		in6 = &ia6->ia_addr.sin6_addr;
		if (in6 == NULL)
			continue;

		nd6_na_output(&sc->sc_if, &mcast, in6,
		    ND_NA_FLAG_OVERRIDE |
		    (ip6_forwarding ? ND_NA_FLAG_ROUTER : 0), 1, NULL);
	}
}
#endif /* INET6 */

void
carp_update_lsmask(struct carp_softc *sc)
{
	struct carp_vhost_entry *vhe;
	int count;

	if (sc->sc_balancing == CARP_BAL_NONE)
		return;

	sc->sc_lsmask = 0;
	count = 0;

	KERNEL_ASSERT_LOCKED(); /* touching carp_vhosts */
	SRPL_FOREACH_LOCKED(vhe, &sc->carp_vhosts, vhost_entries) {
		if (vhe->state == MASTER && count < sizeof(sc->sc_lsmask) * 8 && count >= 0)
			sc->sc_lsmask |= 1 << count;
		if (count == INT_MAX) {
			// Prevent integer overflow
			break;
		}
		count++;
	}
	sc->sc_lscount = count;
	CARP_LOG(LOG_DEBUG, sc, ("carp_update_lsmask: %x", sc->sc_lsmask));
}

int
carp_iamatch(struct ifnet *ifp)
{
	struct carp_softc *sc = ifp->if_softc;
	struct carp_vhost_entry *vhe;
	struct srp_ref sr;
	int match = 0;

	if (sc == NULL) return 0; // Check for NULL pointer
	vhe = SRPL_FIRST(&sr, &sc->carp_vhosts);
	if (vhe == NULL) return 0; // Check for NULL pointer
	if (vhe->state == MASTER)
		match = 1;
	SRPL_LEAVE(&sr);

	return (match);
}

int
carp_ourether(struct ifnet *ifp, uint8_t *ena)
{
	struct srpl *cif = &ifp->if_carp;
	struct carp_softc *sc;
	struct srp_ref sr;
	int match = 0;
	uint64_t dst;

	KASSERT(ifp->if_type == IFT_ETHER);

	/* Ensure ena is not NULL before proceeding */
	if (ena == NULL) {
		return 0;
	}

	dst = ether_addr_to_e64((struct ether_addr *)ena);

	SRPL_FOREACH(sc, &sr, cif, sc_list) {
		if ((sc->sc_if.if_flags & (IFF_UP|IFF_RUNNING)) !=
		    (IFF_UP|IFF_RUNNING))
			continue;
		if (carp_vhe_match(sc, dst)) {
			match = 1;
			break;
		}
	}
	SRPL_LEAVE(&sr);

	return (match);
}

int
carp_vhe_match(struct carp_softc *sc, uint64_t dst)
{
	struct carp_vhost_entry *vhe;
	struct srp_ref sr;
	int active = 0;

	vhe = SRPL_FIRST(&sr, &sc->carp_vhosts);
	active = (vhe->state == MASTER || sc->sc_balancing >= CARP_BAL_IP);
	SRPL_LEAVE(&sr);

	return (active && (dst ==
	    ether_addr_to_e64((struct ether_addr *)sc->sc_ac.ac_enaddr)));
}

struct mbuf *
carp_input(struct ifnet *ifp0, struct mbuf *m, uint64_t dst)
{
	struct srpl *cif;
	struct carp_softc *sc;
	struct srp_ref sr;

	// Validate input parameters
	if (ifp0 == NULL || m == NULL) {
		return NULL;
	}
	
	cif = &ifp0->if_carp;

	SRPL_FOREACH(sc, &sr, cif, sc_list) {
		if ((sc->sc_if.if_flags & (IFF_UP|IFF_RUNNING)) !=
		    (IFF_UP|IFF_RUNNING))
			continue;

		if (carp_vhe_match(sc, dst)) {
			/*
			 * These packets look like layer 2 multicast but they
			 * are unicast at layer 3. With help of the tag the
			 * mbuf's M_MCAST flag can be removed by carp_lsdrop()
			 * after we have passed layer 2.
			 */
			if (sc->sc_balancing == CARP_BAL_IP) {
				struct m_tag *mtag;
				mtag = m_tag_get(PACKET_TAG_CARP_BAL_IP, 0,
				    M_NOWAIT);
				if (mtag == NULL) {
					m_freem(m);
					goto out;
				}
				m_tag_prepend(m, mtag);
			}
			break;
		}
	}

	if (sc == NULL) {
		SRPL_LEAVE(&sr);

		if (!ETH64_IS_MULTICAST(dst))
			return (m);

		/*
		 * XXX Should really check the list of multicast addresses
		 * for each CARP interface _before_ copying.
		 */
		SRPL_FOREACH(sc, &sr, cif, sc_list) {
			struct mbuf *m0;

			if (!(sc->sc_if.if_flags & IFF_UP))
				continue;

			m0 = m_dup_pkt(m, ETHER_ALIGN, M_DONTWAIT);
			if (m0 == NULL)
				continue;

			if_vinput(&sc->sc_if, m0);
		}
		SRPL_LEAVE(&sr);

		return (m);
	}

	if_vinput(&sc->sc_if, m);
out:
	SRPL_LEAVE(&sr);

	return (NULL);
}

int
carp_lsdrop(struct ifnet *ifp, struct mbuf *m, sa_family_t af, u_int32_t *src,
    u_int32_t *dst, int drop)
{
	struct carp_softc *sc;
	u_int32_t fold;
	struct m_tag *mtag;

	if (ifp == NULL || m == NULL || src == NULL || dst == NULL)
		return 0;

	if (ifp->if_type != IFT_CARP)
		return 0;
	sc = ifp->if_softc;
	if (sc == NULL || sc->sc_lscount > 32) // Assuming sc_lscount is within a reasonable range
		return 0;
	if (sc->sc_balancing == CARP_BAL_NONE)
		return 0;

	if (m->m_flags & M_MCAST &&
	    (mtag = m_tag_find(m, PACKET_TAG_CARP_BAL_IP, NULL))) {
		m_tag_delete(m, mtag);
		m->m_flags &= ~M_MCAST;
	}

	if (!drop)
		return 0;

	if (m->m_flags & (M_BCAST|M_MCAST))
		return 0;

	fold = src[0] ^ dst[0];
#ifdef INET6
	if (af == AF_INET6) {
		int i;
		for (i = 1; i < 4; i++) {
			fold ^= src[i] ^ dst[i];
		}
	}
#endif
	if (sc->sc_lscount == 0) /* just to be safe */
		return 1;

	u_int32_t index = ntohl(fold) % sc->sc_lscount;
	if (index >= 32) // Bounds check to ensure no overflow
		return 1;

	return ((1 << index) & sc->sc_lsmask) == 0;
}

void
carp_timer_down(void *v)
{
	NET_LOCK();
	carp_master_down(v);
	NET_UNLOCK();
}

void
carp_master_down(struct carp_vhost_entry *vhe)
{
    struct carp_softc *sc;

    // Validate the pointers to avoid null pointer dereferences
    if (vhe == NULL || vhe->parent_sc == NULL) {
        return;
    }

    sc = vhe->parent_sc;

    NET_ASSERT_LOCKED();

    // Prevent out-of-bounds by ensuring the state is within valid values
    if (vhe->state < INIT || vhe->state > BACKUP) {
        return;
    }

    switch (vhe->state) {
    case INIT:
        printf("%s: master_down event in INIT state\n",
            sc->sc_if.if_xname);
        break;
    case MASTER:
        break;
    case BACKUP:
        carp_set_state(vhe, MASTER);
        carp_send_ad(vhe);
        if (sc->sc_balancing == CARP_BAL_NONE && vhe->vhe_leader) {
            carp_send_arp(sc);
            // Schedule a delayed ARP to deal w/ some L3 switches
            sc->sc_delayed_arp = 2;
#ifdef INET6
            carp_send_na(sc);
#endif /* INET6 */
        }
        carp_setrun(vhe, 0);
        carpstat_inc(carps_preempt);
        break;
    }
}

void
carp_setrun_all(struct carp_softc *sc, sa_family_t af)
{
	struct carp_vhost_entry *vhe;

	KERNEL_ASSERT_LOCKED(); /* touching carp_vhost */
	SRPL_FOREACH_LOCKED(vhe, &sc->carp_vhosts, vhost_entries) {
		carp_setrun(vhe, af);
	}
}

/*
 * When in backup state, af indicates whether to reset the master down timer
 * for v4 or v6. If it's set to zero, reset the ones which are already pending.
 */
void
carp_setrun(struct carp_vhost_entry *vhe, sa_family_t af)
{
	struct ifnet *ifp;
	struct timeval tv;
	struct carp_softc *sc = vhe->parent_sc;

	if ((ifp = if_get(sc->sc_carpdevidx)) == NULL) {
		sc->sc_if.if_flags &= ~IFF_RUNNING;
		carp_set_state_all(sc, INIT);
		return;
	}

	if (memcmp(((struct arpcom *)ifp)->ac_enaddr,
	    sc->sc_ac.ac_enaddr, ETHER_ADDR_LEN) == 0)
		sc->sc_realmac = 1;
	else
		sc->sc_realmac = 0;

	if_put(ifp);

	if (sc->sc_if.if_flags & IFF_UP && vhe->vhid > 0 &&
	    (sc->sc_naddrs || sc->sc_naddrs6) && !sc->sc_suppress) {
		sc->sc_if.if_flags |= IFF_RUNNING;
	} else {
		sc->sc_if.if_flags &= ~IFF_RUNNING;
		return;
	}

	switch (vhe->state) {
	case INIT:
		carp_set_state(vhe, BACKUP);
		carp_setrun(vhe, 0);
		break;
	case BACKUP:
		timeout_del(&vhe->ad_tmo);
		tv.tv_sec = 3 * sc->sc_advbase;
		if (sc->sc_advbase == 0 && vhe->advskew == 0)
			tv.tv_usec = 3 * 1000000 / 256;
		else if (sc->sc_advbase == 0)
			tv.tv_usec = 3 * vhe->advskew * 1000000 / 256;
		else
			tv.tv_usec = vhe->advskew * 1000000 / 256;
		if (tv.tv_usec < 0) tv.tv_usec = 0; // Prevent integer underflow
		if (tv.tv_sec < 0) tv.tv_sec = 0; // Prevent integer underflow
		if (vhe->vhe_leader)
			sc->sc_delayed_arp = -1;
		switch (af) {
		case AF_INET:
			timeout_add_tv(&vhe->md_tmo, &tv);
			break;
#ifdef INET6
		case AF_INET6:
			timeout_add_tv(&vhe->md6_tmo, &tv);
			break;
#endif /* INET6 */
		default:
			if (sc->sc_naddrs)
				timeout_add_tv(&vhe->md_tmo, &tv);
			if (sc->sc_naddrs6)
				timeout_add_tv(&vhe->md6_tmo, &tv);
			break;
		}
		break;
	case MASTER:
		tv.tv_sec = sc->sc_advbase;
		if (sc->sc_advbase == 0 && vhe->advskew == 0)
			tv.tv_usec = 1 * 1000000 / 256;
		else
			tv.tv_usec = vhe->advskew * 1000000 / 256;
		if (tv.tv_usec < 0) tv.tv_usec = 0; // Prevent integer underflow
		if (tv.tv_sec < 0) tv.tv_sec = 0; // Prevent integer underflow
		timeout_add_tv(&vhe->ad_tmo, &tv);
		break;
	}
}

void
carp_multicast_cleanup(struct carp_softc *sc)
{
	struct ip_moptions *imo = &sc->sc_imo;
#ifdef INET6
	struct ip6_moptions *im6o = &sc->sc_im6o;
#endif
	u_int16_t n = imo->imo_num_memberships;

	/* Clean up our own multicast memberships */
	while (n-- > 0) {
		if (imo->imo_membership != NULL && n < IP_MAX_MEMBERSHIPS && imo->imo_membership[n] != NULL) {
			in_delmulti(imo->imo_membership[n]);
			imo->imo_membership[n] = NULL;
		}
	}
	imo->imo_num_memberships = 0;
	imo->imo_ifidx = 0;

#ifdef INET6
	while (!LIST_EMPTY(&im6o->im6o_memberships)) {
		struct in6_multi_mship *imm =
		    LIST_FIRST(&im6o->im6o_memberships);

		LIST_REMOVE(imm, i6mm_chain);
		in6_leavegroup(imm);
	}
	im6o->im6o_ifidx = 0;
#endif

	/* And any other multicast memberships */
	carp_ether_purgemulti(sc);
}

int
carp_set_ifp(struct carp_softc *sc, struct ifnet *ifp0)
{
	struct srpl *cif;
	struct carp_softc *vr, *last = NULL, *after = NULL;
	int myself = 0, error = 0;

	if (ifp0->if_index < 0 || ifp0->if_index >= INT_MAX || sc->sc_carpdevidx < 0 || sc->sc_carpdevidx >= INT_MAX) {
		return (EINVAL);
	}
	KASSERT(ifp0->if_index != sc->sc_carpdevidx);
	KERNEL_ASSERT_LOCKED(); /* touching if_carp */

	if ((ifp0->if_flags & IFF_MULTICAST) == 0)
		return (EADDRNOTAVAIL);

	if (ifp0->if_type != IFT_ETHER)
		return (EINVAL);

	cif = &ifp0->if_carp;
	if (carp_check_dup_vhids(sc, cif, NULL))
		return (EINVAL);

	if ((error = ifpromisc(ifp0, 1)))
		return (error);

	/* detach from old interface */
	if (sc->sc_carpdevidx != 0)
		carpdetach(sc);

	/* attach carp interface to physical interface */
	if_detachhook_add(ifp0, &sc->sc_dtask);
	if_linkstatehook_add(ifp0, &sc->sc_ltask);

	sc->sc_carpdevidx = ifp0->if_index;
	sc->sc_if.if_capabilities = ifp0->if_capabilities &
	    (IFCAP_CSUM_MASK | IFCAP_TSOv4 | IFCAP_TSOv6);

	SRPL_FOREACH_LOCKED(vr, cif, sc_list) {
		struct carp_vhost_entry *vrhead, *schead;
		last = vr;

		if (vr == sc)
			myself = 1;

		vrhead = SRPL_FIRST_LOCKED(&vr->carp_vhosts);
		schead = SRPL_FIRST_LOCKED(&sc->carp_vhosts);
		if (vrhead->vhid < schead->vhid)
			after = vr;
	}

	if (!myself) {
		/* We're trying to keep things in order */
		if (last == NULL) {
			SRPL_INSERT_HEAD_LOCKED(&carp_sc_rc, cif,
			    sc, sc_list);
		} else if (after == NULL) {
    if (last != NULL && sc != NULL) {
        SRPL_INSERT_AFTER_LOCKED(&carp_sc_rc, last, sc, sc_list);
    }
} else {
			SRPL_INSERT_AFTER_LOCKED(&carp_sc_rc, after,
			    sc, sc_list);
		}
	}
	if (sc->sc_naddrs || sc->sc_naddrs6)
		sc->sc_if.if_flags |= IFF_UP;
	carp_set_enaddr(sc);

	carp_carpdev_state(sc);

	return (0);
}

void
carp_set_vhe_enaddr(struct carp_vhost_entry *vhe)
{
	struct carp_softc *sc = vhe->parent_sc;

	if (vhe->vhid != 0 && sc->sc_carpdevidx != 0) {
		if (vhe->vhe_leader && sc->sc_balancing == CARP_BAL_IP)
			vhe->vhe_enaddr[0] = 1;
		else
			vhe->vhe_enaddr[0] = 0;
		vhe->vhe_enaddr[1] = 0;
		vhe->vhe_enaddr[2] = 0x5e;
		vhe->vhe_enaddr[3] = 0;
		vhe->vhe_enaddr[4] = 1;
		if (vhe->vhid <= 0xFF) { // Ensures vhid fits in one byte
			vhe->vhe_enaddr[5] = vhe->vhid;
		} else {
			vhe->vhe_enaddr[5] = 0; // Default value if out-of-bounds
		}
	} else
		memset(vhe->vhe_enaddr, 0, ETHER_ADDR_LEN);
}

void
carp_set_enaddr(struct carp_softc *sc)
{
	struct carp_vhost_entry *vhe;

	KERNEL_ASSERT_LOCKED(); /* touching carp_vhosts */
	SRPL_FOREACH_LOCKED(vhe, &sc->carp_vhosts, vhost_entries)
		carp_set_vhe_enaddr(vhe);

	vhe = SRPL_FIRST_LOCKED(&sc->carp_vhosts);

	/*
	 * Use the carp lladdr if the running one isn't manually set.
	 * Only compare static parts of the lladdr.
	 */
	if (vhe && sc) {
		if ((memcmp(sc->sc_ac.ac_enaddr + 1, vhe->vhe_enaddr + 1,
		    ETHER_ADDR_LEN - 2) == 0) ||
		    (!sc->sc_ac.ac_enaddr[0] && !sc->sc_ac.ac_enaddr[1] &&
		    !sc->sc_ac.ac_enaddr[2] && !sc->sc_ac.ac_enaddr[3] &&
		    !sc->sc_ac.ac_enaddr[4] && !sc->sc_ac.ac_enaddr[5])) {
			bcopy(vhe->vhe_enaddr, sc->sc_ac.ac_enaddr, ETHER_ADDR_LEN);
		}

		/* Make sure the enaddr has changed before further twiddling. */
		if (memcmp(sc->sc_ac.ac_enaddr, sc->sc_curlladdr, ETHER_ADDR_LEN) != 0) {
			bcopy(sc->sc_ac.ac_enaddr, LLADDR(sc->sc_if.if_sadl), ETHER_ADDR_LEN);
			bcopy(sc->sc_ac.ac_enaddr, sc->sc_curlladdr, ETHER_ADDR_LEN);
#ifdef INET6
			/*
			 * (re)attach a link-local address which matches
			 * our new MAC address.
			 */
			if (sc->sc_naddrs6)
				in6_ifattach_linklocal(&sc->sc_if, NULL);
#endif
			carp_set_state_all(sc, INIT);
			carp_setrun_all(sc, 0);
		}
	}
}

void
carp_addr_updated(void *v)
{
    struct carp_softc *sc = (struct carp_softc *) v;
    struct ifaddr *ifa;
    int new_naddrs = 0, new_naddrs6 = 0;

    // Check for valid `sc` pointer to avoid dereference of a null pointer
    if (sc == NULL) return;

    TAILQ_FOREACH(ifa, &sc->sc_if.if_addrlist, ifa_list) {
        if (ifa->ifa_addr->sa_family == AF_INET) {
            // Ensure new_naddrs does not overflow
            if (new_naddrs < INT_MAX)
                new_naddrs++;
        }
#ifdef INET6
        else if (ifa->ifa_addr->sa_family == AF_INET6) {
            // Ensure new_naddrs6 does not overflow
            if (new_naddrs6 < INT_MAX)
                new_naddrs6++;
        }
#endif /* INET6 */
    }

    /* We received address changes from if_addrhooks callback */
    if (new_naddrs != sc->sc_naddrs || new_naddrs6 != sc->sc_naddrs6) {

        sc->sc_naddrs = new_naddrs;
        sc->sc_naddrs6 = new_naddrs6;

        /* Re-establish multicast membership removed by in_control */
        if (IN_MULTICAST(sc->sc_peer.s_addr)) {
            if (!in_hasmulti(&sc->sc_peer, &sc->sc_if)) {
                struct in_multi **imm = sc->sc_imo.imo_membership;
                u_int16_t maxmem = sc->sc_imo.imo_max_memberships;

                memset(&sc->sc_imo, 0, sizeof(sc->sc_imo));
                sc->sc_imo.imo_membership = imm;
                sc->sc_imo.imo_max_memberships = maxmem;

                if (sc->sc_carpdevidx != 0 && sc->sc_naddrs > 0)
                    carp_join_multicast(sc);
            }
        }

        if (sc->sc_naddrs == 0 && sc->sc_naddrs6 == 0) {
            sc->sc_if.if_flags &= ~IFF_UP;
            carp_set_state_all(sc, INIT);
        } else
            carp_hmac_prepare(sc);
    }

    carp_setrun_all(sc, 0);
}

int
carp_set_addr(struct carp_softc *sc, struct sockaddr_in *sin)
{
	struct in_addr *in;
	int error;

	/* Check for null pointers to avoid dereferencing */
	if (sc == NULL || sin == NULL) {
		return (EINVAL); /* Invalid argument error */
	}

	in = &sin->sin_addr;

	KASSERT(sc->sc_carpdevidx != 0);

	/* XXX is this necessary? */
	if (in->s_addr == INADDR_ANY) {
		carp_setrun_all(sc, 0);
		return (0);
	}

	/* Check for possible integer overflow in sc_naddrs */
	if (sc->sc_naddrs == 0 && (error = carp_join_multicast(sc)) != 0)
		return (error);

	carp_set_state_all(sc, INIT);

	return (0);
}

int
carp_join_multicast(struct carp_softc *sc)
{
	struct ip_moptions *imo = &sc->sc_imo;
	struct in_multi *imm;
	struct in_addr addr;

	if (!IN_MULTICAST(sc->sc_peer.s_addr))
		return (0);

	addr.s_addr = sc->sc_peer.s_addr;
	if ((imm = in_addmulti(&addr, &sc->sc_if)) == NULL)
		return (ENOBUFS);

	if (imo->imo_num_memberships >= IP_MAX_MEMBERSHIPS) /* Ensure no out-of-bounds access */
		return (ENOBUFS);

	imo->imo_membership[0] = imm;

	if (imo->imo_num_memberships + 1 < imo->imo_num_memberships) /* Check for integer overflow */
		return (EOVERFLOW);

	imo->imo_num_memberships = 1;
	imo->imo_ifidx = sc->sc_if.if_index;

	if (sc->sc_if.if_index > INT_MAX) /* Ensure ifindex is within valid range */
		return (EINVAL);

	imo->imo_ttl = CARP_DFLTTL;
	imo->imo_loop = 0;
	return (0);
}


#ifdef INET6
int
carp_set_addr6(struct carp_softc *sc, struct sockaddr_in6 *sin6)
{
	int error;

	KASSERT(sc->sc_carpdevidx != 0);

	if (IN6_IS_ADDR_UNSPECIFIED(&sin6->sin6_addr)) {
		carp_setrun_all(sc, 0);
		return (0);
	}

	if (sc->sc_naddrs6 == 0 && (error = carp_join_multicast6(sc)) != 0)
		return (error);

	carp_set_state_all(sc, INIT);

	return (0);
}

int
carp_join_multicast6(struct carp_softc *sc)
{
	struct in6_multi_mship *imm, *imm2;
	struct ip6_moptions *im6o = &sc->sc_im6o;
	struct sockaddr_in6 addr6;
	int error;

	/* Join IPv6 CARP multicast group */
	memset(&addr6, 0, sizeof(addr6));
	addr6.sin6_family = AF_INET6;
	addr6.sin6_len = sizeof(addr6);
	if (sc->sc_if.if_index > 0xffff) {
		return (EINVAL);
	}
	addr6.sin6_addr.s6_addr16[0] = htons(0xff02);
	addr6.sin6_addr.s6_addr16[1] = htons(sc->sc_if.if_index);
	addr6.sin6_addr.s6_addr8[15] = 0x12;
	if ((imm = in6_joingroup(&sc->sc_if,
	    &addr6.sin6_addr, &error)) == NULL) {
		return (error);
	}
	/* join solicited multicast address */
	memset(&addr6.sin6_addr, 0, sizeof(addr6.sin6_addr));
	addr6.sin6_addr.s6_addr16[0] = htons(0xff02);
	addr6.sin6_addr.s6_addr16[1] = htons(sc->sc_if.if_index);
	addr6.sin6_addr.s6_addr32[1] = 0;
	addr6.sin6_addr.s6_addr32[2] = htonl(1);
	addr6.sin6_addr.s6_addr32[3] = 0;
	addr6.sin6_addr.s6_addr8[12] = 0xff;
	if ((imm2 = in6_joingroup(&sc->sc_if,
	    &addr6.sin6_addr, &error)) == NULL) {
		in6_leavegroup(imm);
		return (error);
	}

	/* apply v6 multicast membership */
	im6o->im6o_ifidx = sc->sc_if.if_index;
	if (imm)
		LIST_INSERT_HEAD(&im6o->im6o_memberships, imm,
		    i6mm_chain);
	if (imm2)
		LIST_INSERT_HEAD(&im6o->im6o_memberships, imm2,
		    i6mm_chain);

	return (0);
}

#endif /* INET6 */

int
carp_ioctl(struct ifnet *ifp, u_long cmd, caddr_t addr)
{
	struct proc *p = curproc;	/* XXX */
	struct carp_softc *sc = ifp->if_softc;
	struct carp_vhost_entry *vhe;
	struct carpreq carpr;
	struct ifaddr *ifa = (struct ifaddr *)addr;
	struct ifreq *ifr = (struct ifreq *)addr;
	struct ifnet *ifp0 = NULL;
	int i, error = 0;

	switch (cmd) {
	case SIOCSIFADDR:
		if (sc->sc_carpdevidx == 0)
			return (EINVAL);

		switch (ifa->ifa_addr->sa_family) {
		case AF_INET:
			sc->sc_if.if_flags |= IFF_UP;
			error = carp_set_addr(sc, satosin(ifa->ifa_addr));
			break;
#ifdef INET6
		case AF_INET6:
			sc->sc_if.if_flags |= IFF_UP;
			error = carp_set_addr6(sc, satosin6(ifa->ifa_addr));
			break;
#endif /* INET6 */
		default:
			error = EAFNOSUPPORT;
			break;
		}
		break;

	case SIOCSIFFLAGS:
		KERNEL_ASSERT_LOCKED(); /* touching carp_vhosts */
		vhe = SRPL_FIRST_LOCKED(&sc->carp_vhosts);
		if (vhe->state != INIT && !(ifr->ifr_flags & IFF_UP)) {
			carp_del_all_timeouts(sc);

			/* we need the interface up to bow out */
			sc->sc_if.if_flags |= IFF_UP;
			sc->sc_bow_out = 1;
			carp_vhe_send_ad_all(sc);
			sc->sc_bow_out = 0;

			sc->sc_if.if_flags &= ~IFF_UP;
			carp_set_state_all(sc, INIT);
			carp_setrun_all(sc, 0);
		} else if (vhe->state == INIT && (ifr->ifr_flags & IFF_UP)) {
			sc->sc_if.if_flags |= IFF_UP;
			carp_setrun_all(sc, 0);
		}
		break;

	case SIOCSVH:
		KERNEL_ASSERT_LOCKED(); /* touching carp_vhosts */
		vhe = SRPL_FIRST_LOCKED(&sc->carp_vhosts);
		if ((error = suser(p)) != 0)
			break;
		if ((error = copyin(ifr->ifr_data, &carpr, sizeof carpr)))
			break;
		error = 1;
		if (carpr.carpr_carpdev[0] != '\0' &&
		    (ifp0 = if_unit(carpr.carpr_carpdev)) == NULL)
			return (EINVAL);
		if (carpr.carpr_peer.s_addr == 0)
			sc->sc_peer.s_addr = INADDR_CARP_GROUP;
		else
			sc->sc_peer.s_addr = carpr.carpr_peer.s_addr;
		if (ifp0 != NULL && ifp0->if_index != sc->sc_carpdevidx) {
			if ((error = carp_set_ifp(sc, ifp0))) {
				if_put(ifp0);
				return (error);
			}
		}
		if_put(ifp0);
		if (vhe->state != INIT && carpr.carpr_state != vhe->state) {
			switch (carpr.carpr_state) {
			case BACKUP:
				timeout_del(&vhe->ad_tmo);
				carp_set_state_all(sc, BACKUP);
				carp_setrun_all(sc, 0);
				break;
			case MASTER:
				KERNEL_ASSERT_LOCKED();
				/* touching carp_vhosts */
				SRPL_FOREACH_LOCKED(vhe, &sc->carp_vhosts,
				    vhost_entries)
					carp_master_down(vhe);
				break;
			default:
				break;
			}
		}
		if ((error = carp_vhids_ioctl(sc, &carpr)))
			return (error);
		if (carpr.carpr_advbase >= 0) {
			if (carpr.carpr_advbase > 255) {
				error = EINVAL;
				break;
			}
			sc->sc_advbase = carpr.carpr_advbase;
			error--;
		}
		if (memcmp(sc->sc_advskews, carpr.carpr_advskews,
		    sizeof(sc->sc_advskews))) {
			i = 0;
			KERNEL_ASSERT_LOCKED(); /* touching carp_vhosts */
			SRPL_FOREACH_LOCKED(vhe, &sc->carp_vhosts,
			    vhost_entries)
				vhe->advskew = carpr.carpr_advskews[i++];
			bcopy(carpr.carpr_advskews, sc->sc_advskews,
			    sizeof(sc->sc_advskews));
		}
		if (sc->sc_balancing != carpr.carpr_balancing) {
			if (carpr.carpr_balancing > CARP_BAL_MAXID) {
				error = EINVAL;
				break;
			}
			sc->sc_balancing = carpr.carpr_balancing;
			carp_set_enaddr(sc);
			carp_update_lsmask(sc);
		}
		bcopy(carpr.carpr_key, sc->sc_key, sizeof(sc->sc_key));
		if (error > 0)
			error = EINVAL;
		else {
			error = 0;
			carp_hmac_prepare(sc);
			carp_setrun_all(sc, 0);
		}
		break;

	case SIOCGVH:
		memset(&carpr, 0, sizeof(carpr));
		if ((ifp0 = if_get(sc->sc_carpdevidx)) != NULL)
			strlcpy(carpr.carpr_carpdev, ifp0->if_xname, IFNAMSIZ);
		if_put(ifp0);
		i = 0;
		KERNEL_ASSERT_LOCKED(); /* touching carp_vhosts */
		SRPL_FOREACH_LOCKED(vhe, &sc->carp_vhosts, vhost_entries) {
			carpr.carpr_vhids[i] = vhe->vhid;
			carpr.carpr_advskews[i] = vhe->advskew;
			carpr.carpr_states[i] = vhe->state;
			i++;
		}
		carpr.carpr_advbase = sc->sc_advbase;
		carpr.carpr_balancing = sc->sc_balancing;
		if (suser(p) == 0)
			bcopy(sc->sc_key, carpr.carpr_key,
			    sizeof(carpr.carpr_key));
		carpr.carpr_peer.s_addr = sc->sc_peer.s_addr;
		error = copyout(&carpr, ifr->ifr_data, sizeof(carpr));
		break;

	case SIOCADDMULTI:
		error = carp_ether_addmulti(sc, ifr);
		break;

	case SIOCDELMULTI:
		error = carp_ether_delmulti(sc, ifr);
		break;
	case SIOCAIFGROUP:
	case SIOCDIFGROUP:
		if (sc->sc_demote_cnt)
			carp_ifgroup_ioctl(ifp, cmd, addr);
		break;
	case SIOCSIFGATTR:
		carp_ifgattr_ioctl(ifp, cmd, addr);
		break;
	default:
		error = ENOTTY;
	}

	if (memcmp(sc->sc_ac.ac_enaddr, sc->sc_curlladdr, ETHER_ADDR_LEN) != 0)
		carp_set_enaddr(sc);
	return (error);
}

int
carp_check_dup_vhids(struct carp_softc *sc, struct srpl *cif,
    struct carpreq *carpr)
{
	struct carp_softc *vr;
	struct carp_vhost_entry *vhe, *vhe0;
	int i;

	KERNEL_ASSERT_LOCKED(); /* touching if_carp + carp_vhosts */

	SRPL_FOREACH_LOCKED(vr, cif, sc_list) {
		if (vr == sc)
			continue;
		SRPL_FOREACH_LOCKED(vhe, &vr->carp_vhosts, vhost_entries) {
			if (carpr) {
				for (i = 0; i < INT_MAX && carpr->carpr_vhids[i]; i++) {
					if (vhe->vhid == carpr->carpr_vhids[i])
						return (EINVAL);
				}
				/* Ensure that the loop terminates in case of malformed input */
				if (i == INT_MAX) {
					return (EINVAL);
				}
			}
			SRPL_FOREACH_LOCKED(vhe0, &sc->carp_vhosts,
			    vhost_entries) {
				if (vhe->vhid == vhe0->vhid)
					return (EINVAL);
			}
		}
	}
	return (0);
}

int
carp_vhids_ioctl(struct carp_softc *sc, struct carpreq *carpr)
{
	int i, j;
	u_int8_t taken_vhids[256];

	if (carpr->carpr_vhids[0] == 0 ||
	    !memcmp(sc->sc_vhids, carpr->carpr_vhids, sizeof(sc->sc_vhids)))
		return (0);

	memset(taken_vhids, 0, sizeof(taken_vhids));
	for (i = 0; carpr->carpr_vhids[i]; i++) {
		struct ifnet *ifp;

		if (i >= sizeof(taken_vhids))  // Check for out-of-bounds access
			return (EINVAL);

		if (taken_vhids[carpr->carpr_vhids[i]])
			return (EINVAL);
		taken_vhids[carpr->carpr_vhids[i]] = 1;

		if ((ifp = if_get(sc->sc_carpdevidx)) != NULL) {
			struct srpl *cif;
			cif = &ifp->if_carp;
			if (carp_check_dup_vhids(sc, cif, carpr)) {
				if_put(ifp);
				return (EINVAL);
			}
		}
		if_put(ifp);
		if (carpr->carpr_advskews[i] >= 255)
			return (EINVAL);
	}
	/* set sane balancing defaults */
	if (i <= 1)
		carpr->carpr_balancing = CARP_BAL_NONE;
	else if (carpr->carpr_balancing == CARP_BAL_NONE &&
	    sc->sc_balancing == CARP_BAL_NONE)
		carpr->carpr_balancing = CARP_BAL_IP;

	/* destroy all */
	carp_del_all_timeouts(sc);
	carp_destroy_vhosts(sc);
	memset(sc->sc_vhids, 0, sizeof(sc->sc_vhids));

	/* sort vhosts list by vhid */
	for (j = 1; j <= 255; j++) {
		for (i = 0; carpr->carpr_vhids[i]; i++) {
			if (i >= sizeof(sc->sc_vhids))  // Check for out-of-bounds access
				return (EINVAL);

			if (carpr->carpr_vhids[i] != j)
				continue;
			if (carp_new_vhost(sc, carpr->carpr_vhids[i],
			    carpr->carpr_advskews[i]))
				return (ENOMEM);
			sc->sc_vhids[i] = carpr->carpr_vhids[i];
			sc->sc_advskews[i] = carpr->carpr_advskews[i];
		}
	}
	carp_set_enaddr(sc);
	carp_set_state_all(sc, INIT);
	return (0);
}

void
carp_ifgroup_ioctl(struct ifnet *ifp, u_long cmd, caddr_t addr)
{
	struct ifgroupreq *ifgr = (struct ifgroupreq *)addr;
	struct ifg_list	*ifgl;
	int *dm, adj;

	if (!strcmp(ifgr->ifgr_group, IFG_ALL))
		return;
	adj = ((struct carp_softc *)ifp->if_softc)->sc_demote_cnt;
	if (cmd == SIOCDIFGROUP)
		adj = adj * -1;

	TAILQ_FOREACH(ifgl, &ifp->if_groups, ifgl_next)
		if (!strcmp(ifgl->ifgl_group->ifg_group, ifgr->ifgr_group)) {
			dm = &ifgl->ifgl_group->ifg_carp_demoted;

			if ((adj > 0 && *dm <= INT_MAX - adj) || (adj < 0 && *dm >= INT_MIN - adj)) {
				if (*dm + adj >= 0)
					*dm += adj;
				else
					*dm = 0;
			}
		}
}

void
carp_ifgattr_ioctl(struct ifnet *ifp, u_long cmd, caddr_t addr)
{
	struct ifgroupreq *ifgr;
	struct carp_softc *sc;

	if (addr == NULL || ifp == NULL)
		return;

	ifgr = (struct ifgroupreq *)addr;
	sc = ifp->if_softc;

	if (sc == NULL)
		return;

	if (ifgr->ifgr_attrib.ifg_carp_demoted > 0 && (sc->sc_if.if_flags &
	    (IFF_UP|IFF_RUNNING)) == (IFF_UP|IFF_RUNNING))
		carp_vhe_send_ad_all(sc);
}

void
carp_start(struct ifnet *ifp)
{
	struct carp_softc *sc = ifp->if_softc;
	struct ifnet *ifp0;
	struct mbuf *m;

	if (sc == NULL || sc->sc_carpdevidx < 0) {
		ifq_purge(&ifp->if_snd);
		return;
	}

	if ((ifp0 = if_get(sc->sc_carpdevidx)) == NULL) {
		ifq_purge(&ifp->if_snd);
		return;
	}

	while ((m = ifq_dequeue(&ifp->if_snd)) != NULL)
		carp_transmit(sc, ifp0, m);
	if_put(ifp0);
}

void
carp_transmit(struct carp_softc *sc, struct ifnet *ifp0, struct mbuf *m)
{
	struct ifnet *ifp = &sc->sc_if;

#if NBPFILTER > 0
	{
		caddr_t if_bpf = ifp->if_bpf;
		if (if_bpf)
			bpf_mtap_ether(if_bpf, m, BPF_DIRECTION_OUT);
	}
#endif /* NBPFILTER > 0 */

	if (!ISSET(ifp0->if_flags, IFF_RUNNING)) {
		counters_inc(ifp->if_counters, ifc_oerrors);
		m_freem(m);
		return;
	}

	/*
	 * Do not leak the multicast address when sending
	 * advertisements in 'ip' and 'ip-stealth' balancing
	 * modes.
	 */
	if (sc->sc_balancing == CARP_BAL_IP ||
	    sc->sc_balancing == CARP_BAL_IPSTEALTH) {
		struct ether_header *eh = mtod(m, struct ether_header *);
		if (m->m_len < sizeof(struct ether_header)) {
			counters_inc(ifp->if_counters, ifc_oerrors);
			m_freem(m);
			return;
		}
		memcpy(eh->ether_shost, sc->sc_ac.ac_enaddr,
		    sizeof(eh->ether_shost));
	}

	if (if_enqueue(ifp0, m))
		counters_inc(ifp->if_counters, ifc_oerrors);
}

int
carp_enqueue(struct ifnet *ifp, struct mbuf *m)
{
    struct carp_softc *sc;
    struct ifnet *ifp0;

    if (ifp == NULL || m == NULL || (sc = ifp->if_softc) == NULL) {
        if (m != NULL) {
            m_freem(m);
        }
        return (0);
    }

    if ((ifp0 = if_get(sc->sc_carpdevidx)) == NULL) {
        m_freem(m);
        return (0);
    }

    if (m->m_pkthdr.len < 0 || m->m_pkthdr.len > INT_MAX) {
        m_freem(m);
        if_put(ifp0);
        return (0);
    }

    counters_pkt(ifp->if_counters,
        ifc_opackets, ifc_obytes, m->m_pkthdr.len);
    carp_transmit(sc, ifp0, m);
    if_put(ifp0);

    return (0);
}

int
carp_output(struct ifnet *ifp, struct mbuf *m, struct sockaddr *sa,
    struct rtentry *rt)
{
	if (ifp == NULL || m == NULL || sa == NULL || rt == NULL)
		return (EINVAL);

	struct carp_softc *sc = ((struct carp_softc *)ifp->if_softc);
	if (sc == NULL)
		return (EINVAL);

	struct carp_vhost_entry *vhe;
	struct srp_ref sr;
	int ismaster;

	if (sc->cur_vhe == NULL) {
		vhe = SRPL_FIRST(&sr, &sc->carp_vhosts);
		if (vhe == NULL) {
			return (EINVAL); // Ensure vhe is not NULL before dereferencing
		}
		ismaster = (vhe->state == MASTER);
		SRPL_LEAVE(&sr);
	} else {
		if (sc->cur_vhe == NULL) {
			return (EINVAL); // Ensure cur_vhe is not NULL before dereferencing
		}
		ismaster = (sc->cur_vhe->state == MASTER);
	}

	if ((sc->sc_balancing == CARP_BAL_NONE && !ismaster)) {
		m_freem(m);
		return (ENETUNREACH);
	}

	return (ether_output(ifp, m, sa, rt));
}

void
carp_set_state_all(struct carp_softc *sc, int state)
{
	struct carp_vhost_entry *vhe;

	KERNEL_ASSERT_LOCKED(); /* touching carp_vhosts */

	if (sc == NULL || state < 0)
		return;

	SRPL_FOREACH_LOCKED(vhe, &sc->carp_vhosts, vhost_entries) {
		if (vhe == NULL || vhe->state < 0)
			continue;

		if (vhe->state == state)
			continue;

		carp_set_state(vhe, state);
	}
}

void
carp_set_state(struct carp_vhost_entry *vhe, int state)
{
	struct carp_softc *sc = vhe->parent_sc;
	static const char *carp_states[] = { CARP_STATES };
	int loglevel;
	struct carp_vhost_entry *vhe0;

	KASSERT(vhe->state != state);

	if (vhe->state == INIT || state == INIT)
		loglevel = LOG_WARNING;
	else
		loglevel = LOG_CRIT;

	if (sc->sc_vhe_count > 1) {
		/* Ensure state indices are within bounds */
		if (vhe->state >= 0 && vhe->state < sizeof(carp_states) / sizeof(carp_states[0]) &&
		    state >= 0 && state < sizeof(carp_states) / sizeof(carp_states[0])) {
			CARP_LOG(loglevel, sc,
			    ("state transition (vhid %d): %s -> %s", vhe->vhid,
			    carp_states[vhe->state], carp_states[state]));
		}
	} else {
		/* Ensure state indices are within bounds */
		if (vhe->state >= 0 && vhe->state < sizeof(carp_states) / sizeof(carp_states[0]) &&
		    state >= 0 && state < sizeof(carp_states) / sizeof(carp_states[0])) {
			CARP_LOG(loglevel, sc,
			    ("state transition: %s -> %s",
			    carp_states[vhe->state], carp_states[state]));
		}
	}

	vhe->state = state;
	carp_update_lsmask(sc);

	KERNEL_ASSERT_LOCKED(); /* touching carp_vhosts */

	sc->sc_if.if_link_state = LINK_STATE_INVALID;
	SRPL_FOREACH_LOCKED(vhe0, &sc->carp_vhosts, vhost_entries) {
		/*
		 * Link must be up if at least one vhe is in state MASTER to
		 * bring or keep route up.
		 */
		if (vhe0->state == MASTER) {
			sc->sc_if.if_link_state = LINK_STATE_UP;
			break;
		} else if (vhe0->state == BACKUP) {
			sc->sc_if.if_link_state = LINK_STATE_DOWN;
		}
	}
	if_link_state_change(&sc->sc_if);
}

void
carp_group_demote_adj(struct ifnet *ifp, int adj, char *reason)
{
	struct ifg_list	*ifgl;
	int *dm, need_ad;
	struct carp_softc *nil = NULL;

	if (ifp->if_type == IFT_CARP) {
		dm = &((struct carp_softc *)ifp->if_softc)->sc_demote_cnt;
		if (adj > 0 && *dm > INT_MAX - adj)
			*dm = INT_MAX;
		else if (adj < 0 && *dm < INT_MIN - adj)
			*dm = 0;
		else if (*dm + adj >= 0)
			*dm += adj;
		else
			*dm = 0;
	}

	need_ad = 0;
	TAILQ_FOREACH(ifgl, &ifp->if_groups, ifgl_next) {
		if (!strcmp(ifgl->ifgl_group->ifg_group, IFG_ALL))
			continue;
		dm = &ifgl->ifgl_group->ifg_carp_demoted;

		if (adj > 0 && *dm > INT_MAX - adj)
			*dm = INT_MAX;
		else if (adj < 0 && *dm < INT_MIN - adj)
			*dm = 0;
		else if (*dm + adj >= 0)
			*dm += adj;
		else
			*dm = 0;

		if (adj > 0 && *dm == 1)
			need_ad = 1;
		CARP_LOG(LOG_ERR, nil,
		    ("%s demoted group %s by %d to %d (%s)",
		    ifp->if_xname, ifgl->ifgl_group->ifg_group,
		    adj, *dm, reason));
	}
	if (need_ad)
		carp_send_ad_all();
}

int
carp_group_demote_count(struct carp_softc *sc)
{
	struct ifg_list	*ifgl;
	int count = 0;

	if (sc == NULL)
		return 0;

	TAILQ_FOREACH(ifgl, &sc->sc_if.if_groups, ifgl_next) {
		if (ifgl == NULL || ifgl->ifgl_group == NULL)
			continue;

		if (__builtin_add_overflow(count, ifgl->ifgl_group->ifg_carp_demoted, &count)) {
			count = 255;
			break;
		}
	}

	if (count == 0 && sc->sc_demote_cnt) {
		if (sc->sc_demote_cnt > 255)
			count = 255;
		else
			count = sc->sc_demote_cnt;
	}

	return (count > 255 ? 255 : count);
}

void
carp_carpdev_state(void *v)
{
	struct carp_softc *sc = v;
	struct ifnet *ifp0;
	int suppressed;

	/* Check for null pointer to avoid dereferencing */
	if (sc == NULL)
		return;

	/* Ensure sc_carpdevidx is within valid bounds, check for integer overflow */
	if (sc->sc_carpdevidx < 0 || sc->sc_carpdevidx > INT_MAX)
		return;

	suppressed = sc->sc_suppress;

	if ((ifp0 = if_get(sc->sc_carpdevidx)) == NULL)
		return;

	if (ifp0->if_link_state == LINK_STATE_DOWN ||
	    !(ifp0->if_flags & IFF_UP)) {
		sc->sc_if.if_flags &= ~IFF_RUNNING;
		carp_del_all_timeouts(sc);
		carp_set_state_all(sc, INIT);
		sc->sc_suppress = 1;
		carp_setrun_all(sc, 0);
		if (!suppressed)
			carp_group_demote_adj(&sc->sc_if, 1, "carpdev");
	} else if (suppressed) {
		carp_set_state_all(sc, INIT);
		sc->sc_suppress = 0;
		carp_setrun_all(sc, 0);
		carp_group_demote_adj(&sc->sc_if, -1, "carpdev");
	}

	if_put(ifp0);
}

int
carp_ether_addmulti(struct carp_softc *sc, struct ifreq *ifr)
{
	struct ifnet *ifp0;
	struct carp_mc_entry *mc;
	u_int8_t addrlo[ETHER_ADDR_LEN], addrhi[ETHER_ADDR_LEN];
	int error;

	ifp0 = if_get(sc->sc_carpdevidx);
	if (ifp0 == NULL)
		return (EINVAL);

	error = ether_addmulti(ifr, (struct arpcom *)&sc->sc_ac);
	if (error != ENETRESET) {
		if_put(ifp0);
		return (error);
	}

	/*
	 * This is new multicast address.  We have to tell parent
	 * about it.  Also, remember this multicast address so that
	 * we can delete them on unconfigure.
	 */
	mc = malloc(sizeof(*mc), M_DEVBUF, M_NOWAIT);
	if (mc == NULL) {
		error = ENOMEM;
		goto alloc_failed;
	}

	/*
	 * As ether_addmulti() returns ENETRESET, following two
	 * statement shouldn't fail.
	 */
	(void)ether_multiaddr(&ifr->ifr_addr, addrlo, addrhi);
	ETHER_LOOKUP_MULTI(addrlo, addrhi, &sc->sc_ac, mc->mc_enm);
	memcpy(&mc->mc_addr, &ifr->ifr_addr, ifr->ifr_addr.sa_len);
	LIST_INSERT_HEAD(&sc->carp_mc_listhead, mc, mc_entries);

	error = (*ifp0->if_ioctl)(ifp0, SIOCADDMULTI, (caddr_t)ifr);
	if (error != 0)
		goto ioctl_failed;

	if_put(ifp0);

	return (error);

 ioctl_failed:
	LIST_REMOVE(mc, mc_entries);
	free(mc, M_DEVBUF, sizeof(*mc));
 alloc_failed:
	(void)ether_delmulti(ifr, (struct arpcom *)&sc->sc_ac);
	if_put(ifp0);

	return (error);
}

int
carp_ether_delmulti(struct carp_softc *sc, struct ifreq *ifr)
{
    struct ifnet *ifp0;
    struct ether_multi *enm;
    struct carp_mc_entry *mc;
    u_int8_t addrlo[ETHER_ADDR_LEN], addrhi[ETHER_ADDR_LEN];
    int error;

    if (sc == NULL || ifr == NULL)
        return (EINVAL);

    ifp0 = if_get(sc->sc_carpdevidx);
    if (ifp0 == NULL)
        return (EINVAL);

    /*
     * Find a key to lookup carp_mc_entry.  We have to do this
     * before calling ether_delmulti for obvious reason.
     */
    if ((error = ether_multiaddr(&ifr->ifr_addr, addrlo, addrhi)) != 0)
        goto rele;
    ETHER_LOOKUP_MULTI(addrlo, addrhi, &sc->sc_ac, enm);
    if (enm == NULL) {
        error = EINVAL;
        goto rele;
    }

    LIST_FOREACH(mc, &sc->carp_mc_listhead, mc_entries)
        if (mc->mc_enm == enm)
            break;

    /* We won't delete entries we didn't add */
    if (mc == NULL) {
        error = EINVAL;
        goto rele;
    }

    error = ether_delmulti(ifr, (struct arpcom *)&sc->sc_ac);
    if (error != ENETRESET)
        goto rele;

    /* We no longer use this multicast address.  Tell parent so. */
    error = (*ifp0->if_ioctl)(ifp0, SIOCDELMULTI, (caddr_t)ifr);
    if (error == 0) {
        /* And forget about this address. */
        LIST_REMOVE(mc, mc_entries);
        free(mc, M_DEVBUF, sizeof(*mc));
    } else {
        (void)ether_addmulti(ifr, (struct arpcom *)&sc->sc_ac);
    }
rele:
    if_put(ifp0);
    return (error);
}

/*
 * Delete any multicast address we have asked to add from parent
 * interface.  Called when the carp is being unconfigured.
 */
void
carp_ether_purgemulti(struct carp_softc *sc)
{
	struct ifnet *ifp0;		/* Parent. */
	struct carp_mc_entry *mc;
	union {
		struct ifreq ifreq;
		struct {
			char ifr_name[IFNAMSIZ];
			struct sockaddr_storage ifr_ss;
		} ifreq_storage;
	} u;
	struct ifreq *ifr = &u.ifreq;

	if ((ifp0 = if_get(sc->sc_carpdevidx)) == NULL)
		return;

	memcpy(ifr->ifr_name, ifp0->if_xname, IFNAMSIZ);
	while ((mc = LIST_FIRST(&sc->carp_mc_listhead)) != NULL) {
		if (mc->mc_addr.ss_len > sizeof(ifr->ifr_addr))
			break;
		memcpy(&ifr->ifr_addr, &mc->mc_addr, mc->mc_addr.ss_len);
		(void)(*ifp0->if_ioctl)(ifp0, SIOCDELMULTI, (caddr_t)ifr);
		LIST_REMOVE(mc, mc_entries);
		free(mc, M_DEVBUF, sizeof(*mc));
	}

	if_put(ifp0);
}

void
carp_vh_ref(void *null, void *v)
{
	struct carp_vhost_entry *vhe = v;

	refcnt_take(&vhe->vhost_refcnt);
}

void
carp_vh_unref(void *null, void *v)
{
	struct carp_vhost_entry *vhe = v;

	if (refcnt_rele(&vhe->vhost_refcnt)) {
		carp_sc_unref(NULL, vhe->parent_sc);
		free(vhe, M_DEVBUF, sizeof(*vhe));
	}
}

void
carp_sc_ref(void *null, void *s)
{
	struct carp_softc *sc = s;

	refcnt_take(&sc->sc_refcnt);
}

void
carp_sc_unref(void *null, void *s)
{
	struct carp_softc *sc = s;

	refcnt_rele_wake(&sc->sc_refcnt);
}
