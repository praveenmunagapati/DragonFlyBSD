/*
 * Copyright (c) 1988, 1991, 1993
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
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
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
 *	@(#)rtsock.c	8.7 (Berkeley) 10/12/95
 * $FreeBSD: src/sys/net/rtsock.c,v 1.44.2.11 2002/12/04 14:05:41 ru Exp $
 * $DragonFly: src/sys/net/rtsock.c,v 1.19 2005/01/06 09:14:13 hsu Exp $
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/domain.h>

#include <machine/stdarg.h>

#include <net/if.h>
#include <net/route.h>
#include <net/raw_cb.h>

MALLOC_DEFINE(M_RTABLE, "routetbl", "routing tables");

static struct route_cb {
	int	ip_count;
	int	ip6_count;
	int	ipx_count;
	int	ns_count;
	int	any_count;
} route_cb;

static struct	sockaddr route_dst = { 2, PF_ROUTE, };
static struct	sockaddr route_src = { 2, PF_ROUTE, };
static struct	sockaddr sa_zero   = { sizeof sa_zero, AF_INET, };
static struct	sockproto route_proto = { PF_ROUTE, };

struct walkarg {
	int	w_tmemsize;
	int	w_op, w_arg;
	caddr_t	w_tmem;
	struct sysctl_req *w_req;
};

static struct mbuf *
		rt_msg1 (int, struct rt_addrinfo *);
static int	rt_msg2 (int, struct rt_addrinfo *, caddr_t, struct walkarg *);
static int	rt_xaddrs (char *, char *, struct rt_addrinfo *);
static int	sysctl_dumpentry (struct radix_node *rn, void *vw);
static int	sysctl_iflist (int af, struct walkarg *w);
static int	route_output(struct mbuf *, struct socket *, ...);
static void	rt_setmetrics (u_long, struct rt_metrics *,
			       struct rt_metrics *);

/*
 * It really doesn't make any sense at all for this code to share much
 * with raw_usrreq.c, since its functionality is so restricted.  XXX
 */
static int
rts_abort(struct socket *so)
{
	int s, error;

	s = splnet();
	error = raw_usrreqs.pru_abort(so);
	splx(s);
	return error;
}

/* pru_accept is EOPNOTSUPP */

static int
rts_attach(struct socket *so, int proto, struct pru_attach_info *ai)
{
	struct rawcb *rp;
	int s, error;

	if (sotorawcb(so) != NULL)
		return EISCONN;	/* XXX panic? */

	MALLOC(rp, struct rawcb *, sizeof *rp, M_PCB, M_WAITOK|M_ZERO);
	if (rp == NULL)
		return ENOBUFS;

	/*
	 * The splnet() is necessary to block protocols from sending
	 * error notifications (like RTM_REDIRECT or RTM_LOSING) while
	 * this PCB is extant but incompletely initialized.
	 * Probably we should try to do more of this work beforehand and
	 * eliminate the spl.
	 */
	s = splnet();
	so->so_pcb = rp;
	error = raw_attach(so, proto, ai->sb_rlimit);
	rp = sotorawcb(so);
	if (error) {
		splx(s);
		free(rp, M_PCB);
		return error;
	}
	switch(rp->rcb_proto.sp_protocol) {
	case AF_INET:
		route_cb.ip_count++;
		break;
	case AF_INET6:
		route_cb.ip6_count++;
		break;
	case AF_IPX:
		route_cb.ipx_count++;
		break;
	case AF_NS:
		route_cb.ns_count++;
		break;
	}
	rp->rcb_faddr = &route_src;
	route_cb.any_count++;
	soisconnected(so);
	so->so_options |= SO_USELOOPBACK;
	splx(s);
	return 0;
}

static int
rts_bind(struct socket *so, struct sockaddr *nam, struct thread *td)
{
	int s, error;

	s = splnet();
	error = raw_usrreqs.pru_bind(so, nam, td); /* xxx just EINVAL */
	splx(s);
	return error;
}

static int
rts_connect(struct socket *so, struct sockaddr *nam, struct thread *td)
{
	int s, error;

	s = splnet();
	error = raw_usrreqs.pru_connect(so, nam, td); /* XXX just EINVAL */
	splx(s);
	return error;
}

/* pru_connect2 is EOPNOTSUPP */
/* pru_control is EOPNOTSUPP */

static int
rts_detach(struct socket *so)
{
	struct rawcb *rp = sotorawcb(so);
	int s, error;

	s = splnet();
	if (rp != NULL) {
		switch(rp->rcb_proto.sp_protocol) {
		case AF_INET:
			route_cb.ip_count--;
			break;
		case AF_INET6:
			route_cb.ip6_count--;
			break;
		case AF_IPX:
			route_cb.ipx_count--;
			break;
		case AF_NS:
			route_cb.ns_count--;
			break;
		}
		route_cb.any_count--;
	}
	error = raw_usrreqs.pru_detach(so);
	splx(s);
	return error;
}

static int
rts_disconnect(struct socket *so)
{
	int s, error;

	s = splnet();
	error = raw_usrreqs.pru_disconnect(so);
	splx(s);
	return error;
}

/* pru_listen is EOPNOTSUPP */

static int
rts_peeraddr(struct socket *so, struct sockaddr **nam)
{
	int s, error;

	s = splnet();
	error = raw_usrreqs.pru_peeraddr(so, nam);
	splx(s);
	return error;
}

/* pru_rcvd is EOPNOTSUPP */
/* pru_rcvoob is EOPNOTSUPP */

static int
rts_send(struct socket *so, int flags, struct mbuf *m, struct sockaddr *nam,
	 struct mbuf *control, struct thread *td)
{
	int s, error;

	s = splnet();
	error = raw_usrreqs.pru_send(so, flags, m, nam, control, td);
	splx(s);
	return error;
}

/* pru_sense is null */

static int
rts_shutdown(struct socket *so)
{
	int s, error;

	s = splnet();
	error = raw_usrreqs.pru_shutdown(so);
	splx(s);
	return error;
}

static int
rts_sockaddr(struct socket *so, struct sockaddr **nam)
{
	int s, error;

	s = splnet();
	error = raw_usrreqs.pru_sockaddr(so, nam);
	splx(s);
	return error;
}

static struct pr_usrreqs route_usrreqs = {
	rts_abort, pru_accept_notsupp, rts_attach, rts_bind, rts_connect,
	pru_connect2_notsupp, pru_control_notsupp, rts_detach, rts_disconnect,
	pru_listen_notsupp, rts_peeraddr, pru_rcvd_notsupp, pru_rcvoob_notsupp,
	rts_send, pru_sense_null, rts_shutdown, rts_sockaddr,
	sosend, soreceive, sopoll
};

/*ARGSUSED*/
static int
route_output(struct mbuf *m, struct socket *so, ...)
{
	struct rt_msghdr *rtm = NULL;
	struct rtentry *rt = NULL;
	struct rtentry *saved_nrt = NULL;
	struct radix_node_head *rnh;
	struct ifnet *ifp = NULL;
	struct ifaddr *ifa = NULL;
	struct rawcb *rp = NULL;
	struct pr_output_info *oi;
	struct rt_addrinfo info;
	int len, error = 0;
	__va_list ap;

	__va_start(ap, so);
	oi = __va_arg(ap, struct pr_output_info *);
	__va_end(ap);

#define gotoerr(e) { error = e; goto flush;}
	if (m == NULL || ((m->m_len < sizeof(long)) &&
		       (m = m_pullup(m, sizeof(long))) == NULL))
		return (ENOBUFS);
	if (!(m->m_flags & M_PKTHDR))
		panic("route_output");
	len = m->m_pkthdr.len;
	if (len < sizeof *rtm ||
	    len != mtod(m, struct rt_msghdr *)->rtm_msglen) {
		info.sa_dst = NULL;
		gotoerr(EINVAL);
	}
	R_Malloc(rtm, struct rt_msghdr *, len);
	if (rtm == NULL) {
		info.sa_dst = NULL;
		gotoerr(ENOBUFS);
	}
	m_copydata(m, 0, len, (caddr_t)rtm);
	if (rtm->rtm_version != RTM_VERSION) {
		info.sa_dst = NULL;
		gotoerr(EPROTONOSUPPORT);
	}
	rtm->rtm_pid = oi->p_pid;
	bzero(&info, sizeof info);
	info.rti_addrs = rtm->rtm_addrs;
	if (rt_xaddrs((char *)(rtm + 1), len + (char *)rtm, &info)) {
		info.sa_dst = NULL;
		gotoerr(EINVAL);
	}
	info.rti_flags = rtm->rtm_flags;
	if (info.sa_dst == NULL || info.sa_dst->sa_family >= AF_MAX ||
	    (info.sa_gateway != NULL && (info.sa_gateway->sa_family >= AF_MAX)))
		gotoerr(EINVAL);

	if (info.sa_genmask != NULL) {
		struct radix_node *t;
		int klen;

		t = rn_addmask((char *)info.sa_genmask, TRUE, 1);
		if (t != NULL &&
		    info.sa_genmask->sa_len >= (klen = *(u_char *)t->rn_key) &&
		    bcmp((char *)info.sa_genmask + 1, (char *)t->rn_key + 1,
		         klen - 1) == 0)
			info.sa_genmask = (struct sockaddr *)(t->rn_key);
		else
			gotoerr(ENOBUFS);
	}

	/*
	 * Verify that the caller has the appropriate privilege; RTM_GET
	 * is the only operation the non-superuser is allowed.
	 */
	if (rtm->rtm_type != RTM_GET && suser_cred(so->so_cred, 0) != 0)
		gotoerr(EPERM);

	switch (rtm->rtm_type) {

	case RTM_ADD:
		if (info.sa_gateway == NULL)
			gotoerr(EINVAL);
		error = rtrequest1(RTM_ADD, &info, &saved_nrt);
		if (error == 0 && saved_nrt != NULL) {
			rt_setmetrics(rtm->rtm_inits,
				&rtm->rtm_rmx, &saved_nrt->rt_rmx);
			saved_nrt->rt_rmx.rmx_locks &= ~(rtm->rtm_inits);
			saved_nrt->rt_rmx.rmx_locks |=
				(rtm->rtm_inits & rtm->rtm_rmx.rmx_locks);
			saved_nrt->rt_refcnt--;
			saved_nrt->rt_genmask = info.sa_genmask;
		}
		break;

	case RTM_DELETE:
		error = rtrequest1(RTM_DELETE, &info, &saved_nrt);
		if (error == 0) {
			if ((rt = saved_nrt))
				rt->rt_refcnt++;
			goto report;
		}
		break;

	case RTM_GET:
	case RTM_CHANGE:
	case RTM_LOCK:
		if ((rnh = rt_tables[info.sa_dst->sa_family]) == NULL) {
			gotoerr(EAFNOSUPPORT);
		} else if ((rt = (struct rtentry *) rnh->rnh_lookup(
		    (char *)info.sa_dst, (char *)info.sa_netmask, rnh)) != NULL)
			rt->rt_refcnt++;
		else
			gotoerr(ESRCH);
		switch(rtm->rtm_type) {

		case RTM_GET:
		report:
			info.sa_dst = rt_key(rt);
			info.sa_gateway = rt->rt_gateway;
			info.sa_netmask = rt_mask(rt);
			info.sa_genmask = rt->rt_genmask;
			if (rtm->rtm_addrs & (RTA_IFP | RTA_IFA)) {
				ifp = rt->rt_ifp;
				if (ifp) {
					info.sa_ifpaddr =
					    TAILQ_FIRST(&ifp->if_addrhead)->
						ifa_addr;
					info.sa_ifaaddr = rt->rt_ifa->ifa_addr;
					if (ifp->if_flags & IFF_POINTOPOINT)
						info.sa_bcastaddr =
						    rt->rt_ifa->ifa_dstaddr;
					rtm->rtm_index = ifp->if_index;
				} else {
					info.sa_ifpaddr = NULL;
					info.sa_ifaaddr = NULL;
			    }
			}
			len = rt_msg2(rtm->rtm_type, &info, NULL, NULL);
			if (len > rtm->rtm_msglen) {
				struct rt_msghdr *new_rtm;
				R_Malloc(new_rtm, struct rt_msghdr *, len);
				if (new_rtm == NULL)
					gotoerr(ENOBUFS);
				bcopy(rtm, new_rtm, rtm->rtm_msglen);
				Free(rtm); rtm = new_rtm;
			}
			rt_msg2(rtm->rtm_type, &info, (caddr_t)rtm, NULL);
			rtm->rtm_flags = rt->rt_flags;
			rtm->rtm_rmx = rt->rt_rmx;
			rtm->rtm_addrs = info.rti_addrs;
			break;

		case RTM_CHANGE:
			/*
			 * new gateway could require new ifaddr, ifp;
			 * flags may also be different; ifp may be specified
			 * by ll sockaddr when protocol address is ambiguous
			 */
			if (((rt->rt_flags & RTF_GATEWAY) &&
			     info.sa_gateway != NULL) ||
			    info.sa_ifpaddr != NULL ||
			    (info.sa_ifaaddr != NULL &&
			     sa_equal(info.sa_ifaaddr, rt->rt_ifa->ifa_addr))) {
				if ((error = rt_getifa(&info)) != 0)
					gotoerr(error);
			}
			if (info.sa_gateway != NULL &&
			    (error = rt_setgate(rt, rt_key(rt),
						info.sa_gateway)) != 0)
				gotoerr(error);
			if ((ifa = info.rti_ifa) != NULL) {
				struct ifaddr *oifa = rt->rt_ifa;

				if (oifa != ifa) {
					if (oifa && oifa->ifa_rtrequest)
						oifa->ifa_rtrequest(RTM_DELETE,
								    rt, &info);
					IFAFREE(rt->rt_ifa);
					rt->rt_ifa = ifa;
					IFAREF(ifa);
					rt->rt_ifp = info.rti_ifp;
				}
			}
			rt_setmetrics(rtm->rtm_inits, &rtm->rtm_rmx,
			    &rt->rt_rmx);
			if (rt->rt_ifa && rt->rt_ifa->ifa_rtrequest)
			       rt->rt_ifa->ifa_rtrequest(RTM_ADD, rt, &info);
			if (info.sa_genmask != NULL)
				rt->rt_genmask = info.sa_genmask;
			/*
			 * Fall into
			 */
		case RTM_LOCK:
			rt->rt_rmx.rmx_locks &= ~(rtm->rtm_inits);
			rt->rt_rmx.rmx_locks |=
				(rtm->rtm_inits & rtm->rtm_rmx.rmx_locks);
			break;
		}
		break;

	default:
		gotoerr(EOPNOTSUPP);
	}

flush:
	if (rtm) {
		if (error)
			rtm->rtm_errno = error;
		else
			rtm->rtm_flags |= RTF_DONE;
	}
	if (rt)
		rtfree(rt);
	/*
	 * Check to see if we don't want our own messages.
	 */
	if (!(so->so_options & SO_USELOOPBACK)) {
		if (route_cb.any_count <= 1) {
			if (rtm)
				Free(rtm);
			m_freem(m);
			return (error);
		}
		/* There is another listener, so construct message */
		rp = sotorawcb(so);
	}
	if (rtm) {
		m_copyback(m, 0, rtm->rtm_msglen, (caddr_t)rtm);
		if (m->m_pkthdr.len < rtm->rtm_msglen) {
			m_freem(m);
			m = NULL;
		} else if (m->m_pkthdr.len > rtm->rtm_msglen)
			m_adj(m, rtm->rtm_msglen - m->m_pkthdr.len);
		Free(rtm);
	}
	if (rp != NULL)
		rp->rcb_proto.sp_family = 0; /* Avoid us */
	if (info.sa_dst != NULL)
		route_proto.sp_protocol = info.sa_dst->sa_family;
	if (m != NULL)
		raw_input(m, &route_proto, &route_src, &route_dst);
	if (rp != NULL)
		rp->rcb_proto.sp_family = PF_ROUTE;
	return (error);
}

static void
rt_setmetrics(u_long which, struct rt_metrics *in, struct rt_metrics *out)
{
#define setmetric(flag, elt) if (which & (flag)) out->elt = in->elt;
	setmetric(RTV_RPIPE, rmx_recvpipe);
	setmetric(RTV_SPIPE, rmx_sendpipe);
	setmetric(RTV_SSTHRESH, rmx_ssthresh);
	setmetric(RTV_RTT, rmx_rtt);
	setmetric(RTV_RTTVAR, rmx_rttvar);
	setmetric(RTV_HOPCOUNT, rmx_hopcount);
	setmetric(RTV_MTU, rmx_mtu);
	setmetric(RTV_EXPIRE, rmx_expire);
#undef setmetric
}

#define ROUNDUP(a) \
	((a) > 0 ? (1 + (((a) - 1) | (sizeof(long) - 1))) : sizeof(long))
#define ADVANCE(x, n) (x += ROUNDUP((n)->sa_len))

/*
 * Extract the addresses of the passed sockaddrs.
 * Do a little sanity checking so as to avoid bad memory references.
 * This data is derived straight from userland.
 */
static int
rt_xaddrs(char *cp, char *cplim, struct rt_addrinfo *rtinfo)
{
	struct sockaddr *sa;
	int i;

	for (i = 0; (i < RTAX_MAX) && (cp < cplim); i++) {
		if ((rtinfo->rti_addrs & (1 << i)) == 0)
			continue;
		sa = (struct sockaddr *)cp;
		/*
		 * It won't fit.
		 */
		if ((cp + sa->sa_len) > cplim) {
			return (EINVAL);
		}

		/*
		 * There are no more...  Quit now.
		 * If there are more bits, they are in error.
		 * I've seen this.  route(1) can evidently generate these. 
		 * This causes kernel to core dump.
		 * For compatibility, if we see this, point to a safe address.
		 */
		if (sa->sa_len == 0) {
			rtinfo->rti_info[i] = &sa_zero;
			return (0); /* should be EINVAL but for compat */
		}

		/* Accept the sockaddr. */
		rtinfo->rti_info[i] = sa;
		ADVANCE(cp, sa);
	}
	return (0);
}

static struct mbuf *
rt_msg1(int type, struct rt_addrinfo *rtinfo)
{
	struct rt_msghdr *rtm;
	struct mbuf *m;
	int i;
	struct sockaddr *sa;
	int len, dlen;

	switch (type) {

	case RTM_DELADDR:
	case RTM_NEWADDR:
		len = sizeof(struct ifa_msghdr);
		break;

	case RTM_DELMADDR:
	case RTM_NEWMADDR:
		len = sizeof(struct ifma_msghdr);
		break;

	case RTM_IFINFO:
		len = sizeof(struct if_msghdr);
		break;

	case RTM_IFANNOUNCE:
		len = sizeof(struct if_announcemsghdr);
		break;

	default:
		len = sizeof(struct rt_msghdr);
	}
	if (len > MCLBYTES)
		panic("rt_msg1");
	m = m_gethdr(MB_DONTWAIT, MT_DATA);
	if (m && len > MHLEN) {
		MCLGET(m, MB_DONTWAIT);
		if (!(m->m_flags & M_EXT)) {
			m_free(m);
			m = NULL;
		}
	}
	if (m == NULL)
		return (m);
	m->m_pkthdr.len = m->m_len = len;
	m->m_pkthdr.rcvif = NULL;
	rtm = mtod(m, struct rt_msghdr *);
	bzero(rtm, len);
	for (i = 0; i < RTAX_MAX; i++) {
		if ((sa = rtinfo->rti_info[i]) == NULL)
			continue;
		rtinfo->rti_addrs |= (1 << i);
		dlen = ROUNDUP(sa->sa_len);
		m_copyback(m, len, dlen, (caddr_t)sa);
		len += dlen;
	}
	if (m->m_pkthdr.len != len) {
		m_freem(m);
		return (NULL);
	}
	rtm->rtm_msglen = len;
	rtm->rtm_version = RTM_VERSION;
	rtm->rtm_type = type;
	return (m);
}

static int
rt_msg2(int type, struct rt_addrinfo *rtinfo, caddr_t cp, struct walkarg *w)
{
	int i;
	int len, dlen;
	boolean_t second_time = FALSE;
	caddr_t cp0;

	rtinfo->rti_addrs = NULL;
again:
	switch (type) {

	case RTM_DELADDR:
	case RTM_NEWADDR:
		len = sizeof(struct ifa_msghdr);
		break;

	case RTM_IFINFO:
		len = sizeof(struct if_msghdr);
		break;

	default:
		len = sizeof(struct rt_msghdr);
	}
	cp0 = cp;
	if (cp != NULL)
		cp += len;

	for (i = 0; i < RTAX_MAX; i++) {
		struct sockaddr *sa;

		if ((sa = rtinfo->rti_info[i]) == NULL)
			continue;
		rtinfo->rti_addrs |= (1 << i);
		dlen = ROUNDUP(sa->sa_len);
		if (cp != NULL) {
			bcopy(sa, cp, dlen);
			cp += dlen;
		}
		len += dlen;
	}
	len = ALIGN(len);
	if (cp == NULL && w != NULL && !second_time) {
		struct walkarg *rw = w;

		if (rw->w_req != NULL) {
			if (rw->w_tmemsize < len) {
				if (rw->w_tmem)
					free(rw->w_tmem, M_RTABLE);
				rw->w_tmem = malloc(len, M_RTABLE,
						    M_INTWAIT | M_NULLOK);
				if (rw->w_tmem)
					rw->w_tmemsize = len;
			}
			if (rw->w_tmem != NULL) {
				cp = rw->w_tmem;
				second_time = TRUE;
				goto again;
			}
		}
	}
	if (cp != NULL) {
		struct rt_msghdr *rtm = (struct rt_msghdr *)cp0;

		rtm->rtm_version = RTM_VERSION;
		rtm->rtm_type = type;
		rtm->rtm_msglen = len;
	}
	return (len);
}

/*
 * This routine is called to generate a message from the routing
 * socket indicating that a redirect has occurred, a routing lookup
 * has failed, or that a protocol has detected timeouts to a particular
 * destination.
 */
void
rt_missmsg(int type, struct rt_addrinfo *rtinfo, int flags, int error)
{
	struct sockaddr *dst = rtinfo->rti_info[RTAX_DST];
	struct rt_msghdr *rtm;
	struct mbuf *m;

	if (route_cb.any_count == 0)
		return;
	m = rt_msg1(type, rtinfo);
	if (m == NULL)
		return;
	rtm = mtod(m, struct rt_msghdr *);
	rtm->rtm_flags = RTF_DONE | flags;
	rtm->rtm_errno = error;
	rtm->rtm_addrs = rtinfo->rti_addrs;
	route_proto.sp_protocol = (dst != NULL) ? dst->sa_family : 0;
	raw_input(m, &route_proto, &route_src, &route_dst);
}

/*
 * This routine is called to generate a message from the routing
 * socket indicating that the status of a network interface has changed.
 */
void
rt_ifmsg(struct ifnet *ifp)
{
	struct if_msghdr *ifm;
	struct mbuf *m;
	struct rt_addrinfo info;

	if (route_cb.any_count == 0)
		return;
	bzero(&info, sizeof info);
	m = rt_msg1(RTM_IFINFO, &info);
	if (m == NULL)
		return;
	ifm = mtod(m, struct if_msghdr *);
	ifm->ifm_index = ifp->if_index;
	ifm->ifm_flags = (u_short)ifp->if_flags;
	ifm->ifm_data = ifp->if_data;
	ifm->ifm_addrs = NULL;
	route_proto.sp_protocol = 0;
	raw_input(m, &route_proto, &route_src, &route_dst);
}

static void
rt_ifamsg(int cmd, struct ifaddr *ifa)
{
	struct ifa_msghdr *ifam;
	struct rt_addrinfo info;
	struct mbuf *m;
	struct sockaddr *sa;
	struct ifnet *ifp = ifa->ifa_ifp;

	bzero(&info, sizeof info);
	info.sa_ifaaddr = sa = ifa->ifa_addr;
	info.sa_ifpaddr = TAILQ_FIRST(&ifp->if_addrhead)->ifa_addr;
	info.sa_netmask = ifa->ifa_netmask;
	info.sa_bcastaddr = ifa->ifa_dstaddr;

	m = rt_msg1(cmd, &info);
	if (m == NULL)
		return;

	ifam = mtod(m, struct ifa_msghdr *);
	ifam->ifam_index = ifp->if_index;
	ifam->ifam_metric = ifa->ifa_metric;
	ifam->ifam_flags = ifa->ifa_flags;
	ifam->ifam_addrs = info.rti_addrs;

	route_proto.sp_protocol = sa ? sa->sa_family : 0;

	raw_input(m, &route_proto, &route_src, &route_dst);
}

static void
rt_rtmsg(int cmd, struct ifaddr *ifa, int error, struct rtentry *rt)
{
	struct rt_msghdr *rtm;
	struct rt_addrinfo info;
	struct mbuf *m;
	struct sockaddr *sa;
	struct ifnet *ifp = ifa->ifa_ifp;

	if (rt == NULL)
		return;

	bzero(&info, sizeof info);
	info.sa_netmask = rt_mask(rt);
	info.sa_dst = sa = rt_key(rt);
	info.sa_gateway = rt->rt_gateway;

	m = rt_msg1(cmd, &info);
	if (m == NULL)
		return;

	rtm = mtod(m, struct rt_msghdr *);
	rtm->rtm_index = ifp->if_index;
	rtm->rtm_flags |= rt->rt_flags;
	rtm->rtm_errno = error;
	rtm->rtm_addrs = info.rti_addrs;

	route_proto.sp_protocol = sa ? sa->sa_family : 0;

	raw_input(m, &route_proto, &route_src, &route_dst);
}

/*
 * This is called to generate messages from the routing socket
 * indicating a network interface has had addresses associated with it.
 * if we ever reverse the logic and replace messages TO the routing
 * socket indicate a request to configure interfaces, then it will
 * be unnecessary as the routing socket will automatically generate
 * copies of it.
 */
void
rt_newaddrmsg(int cmd, struct ifaddr *ifa, int error, struct rtentry *rt)
{
	if (route_cb.any_count == 0)
		return;

	if (cmd == RTM_ADD) {
		rt_ifamsg(RTM_NEWADDR, ifa);
		rt_rtmsg(RTM_ADD, ifa, error, rt);
	} else {
		KASSERT((cmd == RTM_DELETE), ("unknown cmd %d", cmd));
		rt_rtmsg(RTM_DELETE, ifa, error, rt);
		rt_ifamsg(RTM_DELADDR, ifa);
	}
}

/*
 * This is the analogue to the rt_newaddrmsg which performs the same
 * function but for multicast group memberhips.  This is easier since
 * there is no route state to worry about.
 */
void
rt_newmaddrmsg(int cmd, struct ifmultiaddr *ifma)
{
	struct rt_addrinfo info;
	struct mbuf *m = NULL;
	struct ifnet *ifp = ifma->ifma_ifp;
	struct ifma_msghdr *ifmam;

	if (route_cb.any_count == 0)
		return;

	bzero(&info, sizeof info);
	info.sa_ifaaddr = ifma->ifma_addr;
	if (ifp != NULL && TAILQ_FIRST(&ifp->if_addrhead) != NULL)
		info.sa_ifpaddr = TAILQ_FIRST(&ifp->if_addrhead)->ifa_addr;
	else
		info.sa_ifpaddr = NULL;
	/*
	 * If a link-layer address is present, present it as a ``gateway''
	 * (similarly to how ARP entries, e.g., are presented).
	 */
	info.sa_gateway = ifma->ifma_lladdr;

	m = rt_msg1(cmd, &info);
	if (m == NULL)
		return;

	ifmam = mtod(m, struct ifma_msghdr *);
	ifmam->ifmam_index = ifp->if_index;
	ifmam->ifmam_addrs = info.rti_addrs;
	route_proto.sp_protocol = ifma->ifma_addr->sa_family;

	raw_input(m, &route_proto, &route_src, &route_dst);
}

/*
 * This is called to generate routing socket messages indicating
 * network interface arrival and departure.
 */
void
rt_ifannouncemsg(ifp, what)
	struct ifnet *ifp;
	int what;
{
	struct if_announcemsghdr *ifan;
	struct mbuf *m;
	struct rt_addrinfo info;

	if (route_cb.any_count == 0)
		return;

	bzero(&info, sizeof info);

	m = rt_msg1(RTM_IFANNOUNCE, &info);
	if (m == NULL)
		return;

	ifan = mtod(m, struct if_announcemsghdr *);
	ifan->ifan_index = ifp->if_index;
	strlcpy(ifan->ifan_name, ifp->if_xname, sizeof ifan->ifan_name);
	ifan->ifan_what = what;

	route_proto.sp_protocol = 0;

	raw_input(m, &route_proto, &route_src, &route_dst);
 }

/*
 * This is used in dumping the kernel table via sysctl().
 */
int
sysctl_dumpentry(rn, vw)
	struct radix_node *rn;
	void *vw;
{
	struct walkarg *w = vw;
	struct rtentry *rt = (struct rtentry *)rn;
	int error = 0, size;
	struct rt_addrinfo info;

	if (w->w_op == NET_RT_FLAGS && !(rt->rt_flags & w->w_arg))
		return 0;

	bzero(&info, sizeof info);
	info.sa_dst = rt_key(rt);
	info.sa_gateway = rt->rt_gateway;
	info.sa_netmask = rt_mask(rt);
	info.sa_genmask = rt->rt_genmask;
	if (rt->rt_ifp != NULL) {
		info.sa_ifpaddr =
		    TAILQ_FIRST(&rt->rt_ifp->if_addrhead)->ifa_addr;
		info.sa_ifaaddr = rt->rt_ifa->ifa_addr;
		if (rt->rt_ifp->if_flags & IFF_POINTOPOINT)
			info.sa_bcastaddr = rt->rt_ifa->ifa_dstaddr;
	}
	size = rt_msg2(RTM_GET, &info, NULL, w);
	if (w->w_req != NULL && w->w_tmem != NULL) {
		struct rt_msghdr *rtm = (struct rt_msghdr *)w->w_tmem;

		rtm->rtm_flags = rt->rt_flags;
		rtm->rtm_use = rt->rt_use;
		rtm->rtm_rmx = rt->rt_rmx;
		rtm->rtm_index = rt->rt_ifp->if_index;
		rtm->rtm_errno = rtm->rtm_pid = rtm->rtm_seq = 0;
		rtm->rtm_addrs = info.rti_addrs;
		error = SYSCTL_OUT(w->w_req, rtm, size);
		return (error);
	}
	return (error);
}

int
sysctl_iflist(af, w)
	int	af;
	struct	walkarg *w;
{
	struct ifnet *ifp;
	struct ifaddr *ifa;
	struct	rt_addrinfo info;
	int	len, error = 0;

	bzero(&info, sizeof info);
	TAILQ_FOREACH(ifp, &ifnet, if_link) {
		if (w->w_arg && w->w_arg != ifp->if_index)
			continue;
		ifa = TAILQ_FIRST(&ifp->if_addrhead);
		info.sa_ifpaddr = ifa->ifa_addr;
		len = rt_msg2(RTM_IFINFO, &info, NULL, w);
		info.sa_ifpaddr = NULL;
		if (w->w_req != NULL && w->w_tmem != NULL) {
			struct if_msghdr *ifm;

			ifm = (struct if_msghdr *)w->w_tmem;
			ifm->ifm_index = ifp->if_index;
			ifm->ifm_flags = (u_short)ifp->if_flags;
			ifm->ifm_data = ifp->if_data;
			ifm->ifm_addrs = info.rti_addrs;
			error = SYSCTL_OUT(w->w_req, ifm, len);
			if (error)
				return (error);
		}
		while ((ifa = TAILQ_NEXT(ifa, ifa_link)) != NULL) {
			if (af && af != ifa->ifa_addr->sa_family)
				continue;
			if (curproc->p_ucred->cr_prison && prison_if(curthread, ifa->ifa_addr))
				continue;
			info.sa_ifaaddr = ifa->ifa_addr;
			info.sa_netmask = ifa->ifa_netmask;
			info.sa_bcastaddr = ifa->ifa_dstaddr;
			len = rt_msg2(RTM_NEWADDR, &info, NULL, w);
			if (w->w_req && w->w_tmem) {
				struct ifa_msghdr *ifam;

				ifam = (struct ifa_msghdr *)w->w_tmem;
				ifam->ifam_index = ifa->ifa_ifp->if_index;
				ifam->ifam_flags = ifa->ifa_flags;
				ifam->ifam_metric = ifa->ifa_metric;
				ifam->ifam_addrs = info.rti_addrs;
				error = SYSCTL_OUT(w->w_req, w->w_tmem, len);
				if (error)
					return (error);
			}
		}
		info.sa_netmask = info.sa_ifaaddr = info.sa_bcastaddr = NULL;
	}
	return (0);
}

static int
sysctl_rtsock(SYSCTL_HANDLER_ARGS)
{
	int	*name = (int *)arg1;
	u_int	namelen = arg2;
	struct radix_node_head *rnh;
	int	i, s, error = EINVAL;
	u_char  af;
	struct	walkarg w;

	name ++;
	namelen--;
	if (req->newptr)
		return (EPERM);
	if (namelen != 3)
		return (EINVAL);
	af = name[0];
	bzero(&w, sizeof w);
	w.w_op = name[1];
	w.w_arg = name[2];
	w.w_req = req;

	s = splnet();
	switch (w.w_op) {

	case NET_RT_DUMP:
	case NET_RT_FLAGS:
		for (i = 1; i <= AF_MAX; i++)
			if ((rnh = rt_tables[i]) && (af == 0 || af == i) &&
			    (error = rnh->rnh_walktree(rnh,
						       sysctl_dumpentry, &w)))
				break;
		break;

	case NET_RT_IFLIST:
		error = sysctl_iflist(af, &w);
	}
	splx(s);
	if (w.w_tmem)
		free(w.w_tmem, M_RTABLE);
	return (error);
}

SYSCTL_NODE(_net, PF_ROUTE, routetable, CTLFLAG_RD, sysctl_rtsock, "");

/*
 * Definitions of protocols supported in the ROUTE domain.
 */

extern struct domain routedomain;		/* or at least forward */

static struct protosw routesw[] = {
{ SOCK_RAW,	&routedomain,	0,		PR_ATOMIC|PR_ADDR,
  0,		route_output,	raw_ctlinput,	0,
  cpu0_soport,
  raw_init,	0,		0,		0,
  &route_usrreqs
}
};

static struct domain routedomain =
    { PF_ROUTE, "route", 0, 0, 0,
      routesw, &routesw[(sizeof routesw)/(sizeof routesw[0])] };

DOMAIN_SET(route);
