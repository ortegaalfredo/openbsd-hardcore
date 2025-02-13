/*	$OpenBSD: tcp_timer.c,v 1.76 2024/01/28 20:34:25 bluhm Exp $	*/
/*	$NetBSD: tcp_timer.c,v 1.14 1996/02/13 23:44:09 christos Exp $	*/

/*
 * Copyright (c) 1982, 1986, 1988, 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)tcp_timer.c	8.1 (Berkeley) 6/10/93
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/protosw.h>
#include <sys/kernel.h>
#include <sys/pool.h>

#include <net/route.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/in_pcb.h>
#include <netinet/ip_var.h>
#include <netinet/tcp.h>
#include <netinet/tcp_fsm.h>
#include <netinet/tcp_timer.h>
#include <netinet/tcp_var.h>
#include <netinet/tcp_debug.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp_seq.h>

/*
 * Locks used to protect struct members in this file:
 *	T	tcp_timer_mtx		global tcp timer data structures
 */

int	tcp_always_keepalive;
int	tcp_keepidle;
int	tcp_keepintvl;
int	tcp_maxpersistidle;	/* max idle time in persist */
int	tcp_maxidle;		/* [T] max idle time for keep alive */

/*
 * Time to delay the ACK.  This is initialized in tcp_init(), unless
 * its patched.
 */
int	tcp_delack_msecs;

void	tcp_timer_rexmt(void *);
void	tcp_timer_persist(void *);
void	tcp_timer_keep(void *);
void	tcp_timer_2msl(void *);
void	tcp_timer_reaper(void *);
void	tcp_timer_delack(void *);

const tcp_timer_func_t tcp_timer_funcs[TCPT_NTIMERS] = {
	tcp_timer_rexmt,
	tcp_timer_persist,
	tcp_timer_keep,
	tcp_timer_2msl,
	tcp_timer_reaper,
	tcp_timer_delack,
};

/*
 * Timer state initialization, called from tcp_init().
 */
void
tcp_timer_init(void)
{

    if (tcp_keepidle < 0 || tcp_keepidle > INT_MAX)
        return;
    if (tcp_keepidle == 0)
        tcp_keepidle = TCPTV_KEEP_IDLE;

    if (tcp_keepintvl < 0 || tcp_keepintvl > INT_MAX)
        return;
    if (tcp_keepintvl == 0)
        tcp_keepintvl = TCPTV_KEEPINTVL;

    if (tcp_maxpersistidle < 0 || tcp_maxpersistidle > INT_MAX)
        return;
    if (tcp_maxpersistidle == 0)
        tcp_maxpersistidle = TCPTV_KEEP_IDLE;

    if (tcp_delack_msecs < 0 || tcp_delack_msecs > INT_MAX)
        return;
    if (tcp_delack_msecs == 0)
        tcp_delack_msecs = TCP_DELACK_MSECS;
}

/*
 * Callout to process delayed ACKs for a TCPCB.
 */
void
tcp_timer_delack(void *arg)
{
	struct tcpcb *otp = NULL, *tp = arg;
	short ostate;

	if (tp == NULL || tp->t_inpcb == NULL || tp->t_inpcb->inp_socket == NULL) {
		return; // Early exit if any critical structure is NULL
	}

	/*
	 * If tcp_output() wasn't able to transmit the ACK
	 * for whatever reason, it will restart the delayed
	 * ACK callout.
	 */
	NET_LOCK();
	/* Ignore canceled timeouts or timeouts that have been rescheduled. */
	if (!ISSET((tp)->t_flags, TF_TMR_DELACK) ||
	    timeout_pending(&tp->t_timer[TCPT_DELACK]))
		goto out;
	CLR((tp)->t_flags, TF_TMR_DELACK);

	if (tp->t_inpcb->inp_socket->so_options & SO_DEBUG) {
		otp = tp;
		ostate = tp->t_state;
	}
	tp->t_flags |= TF_ACKNOW;
	(void) tcp_output(tp);
	if (otp)
		tcp_trace(TA_TIMER, ostate, tp, otp, NULL, TCPT_DELACK, 0);
 out:
	NET_UNLOCK();
}

/*
 * Tcp protocol timeout routine called every 500 ms.
 * Updates the timers in all active tcb's and
 * causes finite state machine actions if timers expire.
 */
void
tcp_slowtimo(void)
{
	mtx_enter(&tcp_timer_mtx);

	if (tcp_keepintvl > 0 && TCPTV_KEEPCNT <= INT_MAX / tcp_keepintvl) {
		tcp_maxidle = TCPTV_KEEPCNT * tcp_keepintvl;
	} else {
		mtx_leave(&tcp_timer_mtx);
		return; // or handle error
	}

	if (TCP_ISSINCR2 > 0 && PR_SLOWHZ > 0 && TCP_ISSINCR2 / PR_SLOWHZ <= INT_MAX - tcp_iss) {
		tcp_iss += TCP_ISSINCR2 / PR_SLOWHZ;
	} else {
		mtx_leave(&tcp_timer_mtx);
		return; // or handle error
	}

	mtx_leave(&tcp_timer_mtx);
}

/*
 * Cancel all timers for TCP tp.
 */
void
tcp_canceltimers(struct tcpcb *tp)
{
	int i;

	for (i = 0; i < TCPT_NTIMERS && i >= 0; i++)
		if (tp != NULL && i < TCPT_NTIMERS)
			TCP_TIMER_DISARM(tp, i);
}

int	tcp_backoff[TCP_MAXRXTSHIFT + 1] =
    { 1, 2, 4, 8, 16, 32, 64, 64, 64, 64, 64, 64, 64 };

int tcp_totbackoff = 511;	/* sum of tcp_backoff[] */

/*
 * TCP timer processing.
 */

void	tcp_timer_freesack(struct tcpcb *);

void
tcp_timer_freesack(struct tcpcb *tp)
{
	struct sackhole *p, *q;
	/*
	 * Free SACK holes for 2MSL and REXMT timers.
	 */
	if (tp == NULL) {
		return;
	}
	q = tp->snd_holes;
	while (q != NULL) {
		p = q;
		q = q->next;
		p->next = NULL; // Clear the next pointer to prevent use-after-free
		pool_put(&sackhl_pool, p);
	}
	tp->snd_holes = NULL;
}

void
tcp_timer_rexmt(void *arg)
{
	struct tcpcb *otp = NULL, *tp = arg;
	struct inpcb *inp;
	uint32_t rto;
	short ostate;

	NET_LOCK();
	inp = tp->t_inpcb;

	/* Ignore canceled timeouts or timeouts that have been rescheduled. */
	if (!ISSET((tp)->t_flags, TF_TMR_REXMT) ||
	    timeout_pending(&tp->t_timer[TCPT_REXMT]))
		goto out;
	CLR((tp)->t_flags, TF_TMR_REXMT);

	if ((tp->t_flags & TF_PMTUD_PEND) && inp &&
	    SEQ_GEQ(tp->t_pmtud_th_seq, tp->snd_una) &&
	    SEQ_LT(tp->t_pmtud_th_seq, (int)(tp->snd_una + tp->t_maxseg))) {
		struct sockaddr_in sin;
		struct icmp icmp;

		/* TF_PMTUD_PEND is set in tcp_ctlinput() which is IPv4 only */
		KASSERT(!ISSET(inp->inp_flags, INP_IPV6));
		tp->t_flags &= ~TF_PMTUD_PEND;

		/* XXX create fake icmp message with relevant entries */
		icmp.icmp_nextmtu = tp->t_pmtud_nextmtu;
		icmp.icmp_ip.ip_len = tp->t_pmtud_ip_len;
		icmp.icmp_ip.ip_hl = tp->t_pmtud_ip_hl;
		icmp.icmp_ip.ip_dst = inp->inp_faddr;
		icmp_mtudisc(&icmp, inp->inp_rtableid);

		/*
		 * Notify all connections to the same peer about
		 * new mss and trigger retransmit.
		 */
		bzero(&sin, sizeof(sin));
		sin.sin_len = sizeof(sin);
		sin.sin_family = AF_INET;
		sin.sin_addr = inp->inp_faddr;
		in_pcbnotifyall(&tcbtable, &sin, inp->inp_rtableid, EMSGSIZE,
		    tcp_mtudisc);
		goto out;
	}

	tcp_timer_freesack(tp);
	if (++tp->t_rxtshift > TCP_MAXRXTSHIFT) {
		tp->t_rxtshift = TCP_MAXRXTSHIFT;
		tcpstat_inc(tcps_timeoutdrop);
		tp = tcp_drop(tp, tp->t_softerror ?
		    tp->t_softerror : ETIMEDOUT);
		goto out;
	}
	if (inp->inp_socket->so_options & SO_DEBUG) {
		otp = tp;
		ostate = tp->t_state;
	}
	tcpstat_inc(tcps_rexmttimeo);
	rto = TCP_REXMTVAL(tp);
	if (rto < tp->t_rttmin)
		rto = tp->t_rttmin;
	TCPT_RANGESET(tp->t_rxtcur,
	    rto * tcp_backoff[tp->t_rxtshift],
	    tp->t_rttmin, TCPTV_REXMTMAX);
	TCP_TIMER_ARM(tp, TCPT_REXMT, tp->t_rxtcur);

	/*
	 * If we are losing and we are trying path MTU discovery,
	 * try turning it off.  This will avoid black holes in
	 * the network which suppress or fail to send "packet
	 * too big" ICMP messages.  We should ideally do
	 * lots more sophisticated searching to find the right
	 * value here...
	 */
	if (ip_mtudisc && inp &&
	    TCPS_HAVEESTABLISHED(tp->t_state) &&
	    tp->t_rxtshift > TCP_MAXRXTSHIFT / 6) {
		struct rtentry *rt = NULL;

		/* No data to send means path mtu is not a problem */
		if (!inp->inp_socket->so_snd.sb_cc)
			goto leave;

		rt = in_pcbrtentry(inp);
		/* Check if path MTU discovery is disabled already */
		if (rt && (rt->rt_flags & RTF_HOST) &&
		    (rt->rt_locks & RTV_MTU))
			goto leave;

		rt = NULL;
		switch(tp->pf) {
#ifdef INET6
		case PF_INET6:
			/*
			 * We can not turn off path MTU for IPv6.
			 * Do nothing for now, maybe lower to
			 * minimum MTU.
			 */
			break;
#endif
		case PF_INET:
			rt = icmp_mtudisc_clone(inp->inp_faddr,
			    inp->inp_rtableid, 0);
			break;
		}
		if (rt != NULL) {
			/* Disable path MTU discovery */
			if ((rt->rt_locks & RTV_MTU) == 0) {
				rt->rt_locks |= RTV_MTU;
				in_rtchange(inp, 0);
			}

			rtfree(rt);
		}
	leave:
		;
	}

	/*
	 * If losing, let the lower level know and try for
	 * a better route.  Also, if we backed off this far,
	 * our srtt estimate is probably bogus.  Clobber it
	 * so we'll take the next rtt measurement as our srtt;
	 * move the current srtt into rttvar to keep the current
	 * retransmit times until then.
	 */
	if (tp->t_rxtshift > TCP_MAXRXTSHIFT / 4) {
		in_losing(inp);
		tp->t_rttvar += (tp->t_srtt >> TCP_RTT_SHIFT);
		tp->t_srtt = 0;
	}
	tp->snd_nxt = tp->snd_una;
	/*
	 * Note:  We overload snd_last to function also as the
	 * snd_last variable described in RFC 2582
	 */
	tp->snd_last = tp->snd_max;
	/*
	 * If timing a segment in this window, stop the timer.
	 */
	tp->t_rtttime = 0;
#ifdef TCP_ECN
	/*
	 * if ECN is enabled, there might be a broken firewall which
	 * blocks ecn packets.  fall back to non-ecn.
	 */
	if ((tp->t_state == TCPS_SYN_SENT || tp->t_state == TCPS_SYN_RECEIVED)
	    && tcp_do_ecn && !(tp->t_flags & TF_DISABLE_ECN))
		tp->t_flags |= TF_DISABLE_ECN;
#endif
	/*
	 * Close the congestion window down to one segment
	 * (we'll open it by one segment for each ack we get).
	 * Since we probably have a window's worth of unacked
	 * data accumulated, this "slow start" keeps us from
	 * dumping all that data as back-to-back packets (which
	 * might overwhelm an intermediate gateway).
	 *
	 * There are two phases to the opening: Initially we
	 * open by one mss on each ack.  This makes the window
	 * size increase exponentially with time.  If the
	 * window is larger than the path can handle, this
	 * exponential growth results in dropped packet(s)
	 * almost immediately.  To get more time between
	 * drops but still "push" the network to take advantage
	 * of improving conditions, we switch from exponential
	 * to linear window opening at some threshold size.
	 * For a threshold, we use half the current window
	 * size, truncated to a multiple of the mss.
	 *
	 * (the minimum cwnd that will give us exponential
	 * growth is 2 mss.  We don't allow the threshold
	 * to go below this.)
	 */
	{
		u_long win;

		win = ulmin(tp->snd_wnd, tp->snd_cwnd) / 2 / tp->t_maxseg;
		if (win < 2)
			win = 2;
		tp->snd_cwnd = tp->t_maxseg;
		tp->snd_ssthresh = win * tp->t_maxseg;
		tp->t_dupacks = 0;
#ifdef TCP_ECN
		tp->snd_last = tp->snd_max;
		tp->t_flags |= TF_SEND_CWR;
#endif
#if 1 /* TCP_ECN */
		tcpstat_inc(tcps_cwr_timeout);
#endif
	}
	(void) tcp_output(tp);
	if (otp)
		tcp_trace(TA_TIMER, ostate, tp, otp, NULL, TCPT_REXMT, 0);
 out:
	NET_UNLOCK();
}

void
tcp_timer_persist(void *arg)
{
	struct tcpcb *otp = NULL, *tp = arg;
	uint32_t rto;
	short ostate;
	uint64_t now;

	if (tp == NULL || tp->t_inpcb == NULL || tp->t_inpcb->inp_socket == NULL) {
		return;
	}

	NET_LOCK();
	/* Ignore canceled timeouts or timeouts that have been rescheduled. */
	if (!ISSET((tp)->t_flags, TF_TMR_PERSIST) ||
	    timeout_pending(&tp->t_timer[TCPT_PERSIST]))
		goto out;
	CLR((tp)->t_flags, TF_TMR_PERSIST);

	if (TCP_TIMER_ISARMED(tp, TCPT_REXMT))
		goto out;

	if (tp->t_inpcb->inp_socket->so_options & SO_DEBUG) {
		otp = tp;
		ostate = tp->t_state;
	}
	tcpstat_inc(tcps_persisttimeo);
	/*
	 * Hack: if the peer is dead/unreachable, we do not
	 * time out if the window is closed.  After a full
	 * backoff, drop the connection if the idle time
	 * (no responses to probes) reaches the maximum
	 * backoff that we would use if retransmitting.
	 */
	rto = TCP_REXMTVAL(tp);
	if (rto < tp->t_rttmin)
		rto = tp->t_rttmin;
	now = tcp_now();
	if (now < tp->t_rcvtime) {
		goto out;
	}
	if (tp->t_rxtshift == TCP_MAXRXTSHIFT &&
	    ((now - tp->t_rcvtime) >= tcp_maxpersistidle ||
	    (now - tp->t_rcvtime) >= rto * tcp_totbackoff)) {
		tcpstat_inc(tcps_persistdrop);
		tp = tcp_drop(tp, ETIMEDOUT);
		goto out;
	}
	tcp_setpersist(tp);
	tp->t_force = 1;
	(void) tcp_output(tp);
	tp->t_force = 0;
	if (otp)
		tcp_trace(TA_TIMER, ostate, tp, otp, NULL, TCPT_PERSIST, 0);
 out:
	NET_UNLOCK();
}

void
tcp_timer_keep(void *arg)
{
	struct tcpcb *otp = NULL, *tp = arg;
	short ostate;

	if (tp == NULL) return; // Check for null pointer

	NET_LOCK();
	/* Ignore canceled timeouts or timeouts that have been rescheduled. */
	if (!ISSET((tp)->t_flags, TF_TMR_KEEP) ||
	    timeout_pending(&tp->t_timer[TCPT_KEEP]))
		goto out;
	CLR((tp)->t_flags, TF_TMR_KEEP);

	if (tp->t_inpcb && tp->t_inpcb->inp_socket && (tp->t_inpcb->inp_socket->so_options & SO_DEBUG)) {
		otp = tp;
		ostate = tp->t_state;
	}
	tcpstat_inc(tcps_keeptimeo);
	if (TCPS_HAVEESTABLISHED(tp->t_state) == 0)
		goto dropit;
	if (tp->t_inpcb && tp->t_inpcb->inp_socket &&
	    (tcp_always_keepalive ||
	    tp->t_inpcb->inp_socket->so_options & SO_KEEPALIVE) &&
	    tp->t_state <= TCPS_CLOSING) {
		int maxidle;
		uint64_t now;

		maxidle = READ_ONCE(tcp_maxidle);
		now = tcp_now();

		if (maxidle > 0 && now >= tp->t_rcvtime) {
			if ((now - tp->t_rcvtime) >= (uint64_t)tcp_keepidle + maxidle)
				goto dropit;
		}

		if (tp->t_template != NULL) { // Check if t_template is not NULL
			tcpstat_inc(tcps_keepprobe);
			tcp_respond(tp, mtod(tp->t_template, caddr_t),
			    NULL, tp->rcv_nxt, tp->snd_una - 1, 0, 0, now);
			TCP_TIMER_ARM(tp, TCPT_KEEP, tcp_keepintvl);
		}
	} else
		TCP_TIMER_ARM(tp, TCPT_KEEP, tcp_keepidle);
	if (otp)
		tcp_trace(TA_TIMER, ostate, tp, otp, NULL, TCPT_KEEP, 0);
 out:
	NET_UNLOCK();
	return;

 dropit:
	tcpstat_inc(tcps_keepdrops);
	tp = tcp_drop(tp, ETIMEDOUT);
	NET_UNLOCK();
}

void
tcp_timer_2msl(void *arg)
{
    struct tcpcb *otp = NULL, *tp = arg;
    short ostate;
    int maxidle;
    uint64_t now;

    if (tp == NULL || tp->t_inpcb == NULL || tp->t_inpcb->inp_socket == NULL) {
        return; // Return if tp or any referenced structures are NULL
    }

    NET_LOCK();
    /* Ignore canceled timeouts or timeouts that have been rescheduled. */
    if (!ISSET((tp)->t_flags, TF_TMR_2MSL) ||
        timeout_pending(&tp->t_timer[TCPT_2MSL]))
        goto out;
    CLR((tp)->t_flags, TF_TMR_2MSL);

    if (tp->t_inpcb->inp_socket->so_options & SO_DEBUG) {
        otp = tp;
        ostate = tp->t_state;
    }
    tcp_timer_freesack(tp);

    maxidle = READ_ONCE(tcp_maxidle);
    now = tcp_now();
    if (tp->t_state != TCPS_TIME_WAIT &&
        ((maxidle == 0) || ((now >= tp->t_rcvtime) && ((now - tp->t_rcvtime) <= (uint64_t)maxidle))))
        TCP_TIMER_ARM(tp, TCPT_2MSL, tcp_keepintvl);
    else
        tp = tcp_close(tp);
    if (otp)
        tcp_trace(TA_TIMER, ostate, tp, otp, NULL, TCPT_2MSL, 0);
 out:
    NET_UNLOCK();
}

void
tcp_timer_reaper(void *arg)
{
	if (arg == NULL) {
		return;
	}

	struct tcpcb *tp = arg;

	if ((uintptr_t)tp <= 0 || (uintptr_t)tp >= UINTPTR_MAX) {
		return;
	}

	pool_put(&tcpcb_pool, tp);

	if (tcps_closed < INT_MAX) {
		tcpstat_inc(tcps_closed);
	}
}
