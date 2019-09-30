// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2014-2019, NetApp, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef FUZZING
#include <sys/time.h>
#endif

#if !defined(PARTICLE) && !defined(RIOT_VERSION)
#include "krng.h"
#include <ifaddrs.h>
#include <net/if.h>
#elif defined(PARTICLE)
#include <rng_hal.h>

#define kr_srand_r(x, y)
#define kr_rand_r(x)                                                           \
    (uint64_t)(HAL_RNG_GetRandomNumber()) << 32 | HAL_RNG_GetRandomNumber()
#elif defined(RIOT_VERSION)
#include <random.h>
#include <xtimer.h>

#define kr_srand_r(x, y)
#define kr_rand_r(x) (uint64_t)(random_uint32()) << 32 | random_uint32()
#endif

#include <warpcore/warpcore.h>

#ifdef HAVE_ASAN
#include <sanitizer/asan_interface.h>
#endif

// #define DEBUG_BUFFERS

#include "backend.h"

#ifdef WITH_NETMAP
#include "eth.h"
#include "ip4.h"
#include "ip6.h"
#include "udp.h"
#endif

#include "neighbor.h"

#ifdef PARTICLE
typedef struct if_list if_list;

#include <ifapi.h>
#include <system_network.h>

#define getifaddrs if_get_if_addrs
#define freeifaddrs if_free_if_addrs
#define ifa_next next
#define ifa_name ifname
#define ifa_flags ifflags
#define ifa_addr if_addr->addr
#define ifa_netmask if_addr->netmask

#elif defined(RIOT_VERSION)
#include <lwip/dhcp.h>
#include <lwip/netif.h>
#include <lwip/timeouts.h>
#endif


#if !defined(PARTICLE) && !defined(RIOT_VERSION)
// w_init() must initialize this so that it is not all zero
static krng_t w_rand_state;
#endif

/// A global list of netmap engines that have been initialized for different
/// interfaces.
///
struct w_engines engines = sl_head_initializer(engines);


#ifdef DEBUG_BUFFERS
static void __attribute__((nonnull))
dump_bufs(const char * const label, const struct w_iov_sq * const q)
{
    char line[400] = "";
    int pos = 0;
    struct w_iov * v;
    uint32_t cnt = 0;
    sq_foreach (v, q, next) {
        cnt++;
        pos += snprintf(&line[pos], sizeof(line) - (size_t)pos, "%s%" PRIu32,
                        pos ? ", " : "", v->idx);
        if ((size_t)pos >= sizeof(line))
            break;
    }
    warn(DBG, "%s: %" PRIu " bufs: %s", label, w_iov_sq_cnt(q), line);
    ensure(cnt == w_iov_sq_cnt(q), "cnt mismatch");
}
#endif


/// Get the socket bound to the given four-tuple <source IP, source port,
/// destination IP, destination port>.
///
/// @param      w       Backend engine.
/// @param[in]  local   The local IP address and port.
/// @param[in]  remote  The remote IP address and port.
///
/// @return     The w_sock bound to the given four-tuple.
///
struct w_sock * w_get_sock(struct w_engine * const w,
                           const struct w_sockaddr * const local,
                           const struct w_sockaddr * const remote)
{
    struct w_socktuple tup = {.local = *local};
    if (remote)
        tup.remote = *remote;
    const khiter_t k = kh_get(sock, &w->sock, &tup);
    return unlikely(k == kh_end(&w->sock)) ? 0 : kh_val(&w->sock, k);
}


static void __attribute__((nonnull))
ins_sock(struct w_engine * const w, struct w_sock * const s)
{
    int ret;
    const khiter_t k = kh_put(sock, &w->sock, &s->tup, &ret);
    ensure(ret >= 1, "inserted is %d", ret);
    kh_val(&w->sock, k) = s;
}


static void __attribute__((nonnull))
rem_sock(struct w_engine * const w, struct w_sock * const s)
{
    const khiter_t k = kh_get(sock, &w->sock, &s->tup);
    ensure(k != kh_end(&w->sock), "found");
    kh_del(sock, &w->sock, k);
}


/// Return a spare w_iov from the pool of the given warpcore engine. Needs to be
/// returned to w->iov via sq_insert_head() or sq_concat().
///
/// @param      w     Backend engine.
/// @param[in]  len   The length of each @p buf.
/// @param[in]  off   Additional offset into the buffer.
///
/// @return     Spare w_iov.
///
struct w_iov *
w_alloc_iov(struct w_engine * const w, const uint16_t len, uint16_t off)
{
#ifdef DEBUG_BUFFERS
    warn(DBG, "w_alloc_iov len %u, off %u", len, off);
#endif
    struct w_iov * const v = w_alloc_iov_base(w);
    if (likely(v)) {
#ifdef WITH_NETMAP
        const size_t diff =
            sizeof(struct eth_hdr) +
            MAX(sizeof(struct ip4_hdr), sizeof(struct ip6_hdr)) +
            sizeof(struct udp_hdr);
        v->buf += diff;
        v->len -= diff;
#endif
        ensure(off == 0 || off <= v->len, "off %u > v->len %u", off, v->len);
        v->buf += off;
        v->len -= off;
        ensure(len <= v->len, "len %u > v->len %u", len, v->len);
        v->len = len ? len : v->len;
        // warn(DBG, "alloc w_iov off %u len %u", v->buf - v->base, v->len);
    }
#ifdef DEBUG_BUFFERS
    dump_bufs(__func__, &w->iov);
#endif
    return v;
}


/// Allocate a w_iov tail queue for @p plen payload bytes, for eventual use with
/// w_tx(). The tail queue must be later returned to warpcore w_free(). If a @p
/// len length is specified, limit the length of each buffer to the minimum of
/// the MTU and this value. If a @p off offset is specified, leave this much
/// extra space before @p buf in each w_iov. This is meant for upper-level
/// protocols that wish to reserve space for their headers.
///
/// If there aren't enough buffers available to fulfill the request, @p q will
/// be shorter than requested. It is up to the caller to check this.
///
/// @param      w     Backend engine.
/// @param[out] q     Tail queue of w_iov structs.
/// @param[in]  qlen  Amount of payload bytes in the returned tail queue.
/// @param[in]  len   The length of each @p buf.
/// @param[in]  off   Additional offset for @p buf.
///
void w_alloc_len(struct w_engine * const w,
                 struct w_iov_sq * const q,
                 const uint_t qlen,
                 const uint16_t len,
                 const uint16_t off)
{
#ifdef DEBUG_BUFFERS
    warn(DBG, "w_alloc_len qlen %" PRIu ", len %u, off %u", qlen, len, off);
    ensure(sq_empty(q), "q not empty");
#endif
    uint_t needed = qlen;
    while (likely(needed)) {
        struct w_iov * const v = w_alloc_iov(w, len, off);
        if (unlikely(v == 0))
            return;
        if (likely(needed > v->len))
            needed -= v->len;
        else {
            // warn(DBG, "adjust last to %u", needed);
            v->len = (uint16_t)needed;
            needed = 0;
        }
        sq_insert_tail(q, v, next);
    }
#ifdef DEBUG_BUFFERS
    dump_bufs(__func__, &w->iov);
    dump_bufs("allocated chain", q);
#endif
}


/// Allocate a w_iov tail queue of @p count packets, for eventual use with
/// w_tx(). The tail queue must be later returned to warpcore w_free(). If a @p
/// len length is specified, limit the length of each buffer to the minimum of
/// the MTU and this value. If a @p off offset is specified, leave this much
/// extra space before @p buf in each w_iov. This is meant for upper-level
/// protocols that wish to reserve space for their headers.
///
/// If there aren't enough buffers available to fulfill the request, @p q will
/// be shorter than requested. It is up to the caller to check this.
///
/// @param      w      Backend engine.
/// @param[out] q      Tail queue of w_iov structs.
/// @param[in]  count  Number of packets in the returned tail queue.
/// @param[in]  len    The length of each @p buf.
/// @param[in]  off    Additional offset for @p buf.
///
void w_alloc_cnt(struct w_engine * const w,
                 struct w_iov_sq * const q,
                 const uint_t count,
                 const uint16_t len,
                 const uint16_t off)
{
#ifdef DEBUG_BUFFERS
    warn(DBG, "w_alloc_cnt count %" PRIu ", len %u, off %u", count, len, off);
    ensure(sq_empty(q), "q not empty");
#endif
    for (uint_t needed = 0; likely(needed < count); needed++) {
        struct w_iov * const v = w_alloc_iov(w, len, off);
        if (unlikely(v == 0))
            return;
        sq_insert_tail(q, v, next);
    }
#ifdef DEBUG_BUFFERS
    dump_bufs(__func__, &w->iov);
    dump_bufs("allocated chain", q);
#endif
}


/// Return the total payload length of w_iov tail queue @p c.
///
/// @param[in]  q     The w_iov tail queue to compute the payload length of.
///
/// @return     Sum of the payload lengths of the w_iov structs in @p q.
///
uint_t w_iov_sq_len(const struct w_iov_sq * const q)
{
    uint_t l = 0;
    const struct w_iov * v;
    sq_foreach (v, q, next)
        l += v->len;
    return l;
}


/// Connect a bound socket to a remote IP address and port. Depending on the
/// backend, this function may block until a MAC address has been resolved with
/// ARP.
///
/// Calling w_connect() will make subsequent w_tx() operations on the w_sock
/// enqueue payload data towards that destination.
///
/// @param      s     w_sock to connect.
/// @param[in]  peer  The peer to connect to.
///
/// @return     Zero on success, @p errno otherwise.
///
int w_connect(struct w_sock * const s, const struct sockaddr * const peer)
{
    if (unlikely(w_connected(s))) {
        warn(ERR, "socket already connected");
        return EADDRINUSE;
    }

    if (unlikely(peer->sa_family != AF_INET && peer->sa_family != AF_INET6)) {
        warn(ERR, "peer address is not IP");
        return EAFNOSUPPORT;
    }

    rem_sock(s->w, s);
    set_ip(&s->tup.remote.addr, peer);
    s->tup.remote.port = sa_port(peer);
    const int e = backend_connect(s);
    if (unlikely(e)) {
        memset(&s->tup.remote, 0, sizeof(s->tup.remote));
        ins_sock(s->w, s);
        return e;
    }
    ins_sock(s->w, s);

    warn(DBG, "socket connected to %s:%d",
         w_ntop(&s->tup.remote.addr, (char[IP6_STRLEN]){""}, IP6_STRLEN),
         bswap16(s->tup.remote.port));

    return 0;
}


/// Bind a w_sock to the given local UDP port number.
///
/// @param      w         The w_sock to bind.
/// @param[in]  addr_idx  Index of the IP address to bind to.
/// @param[in]  port      The local port number to bind to, in network byte
///                       order. If port is zero, a random local port will be
///                       chosen.
/// @param[in]  opt       Socket options for this socket. Can be zero.
///
/// @return     Pointer to a bound w_sock.
///
struct w_sock * w_bind(struct w_engine * const w,
                       const uint16_t addr_idx,
                       const uint16_t port,
                       const struct w_sockopt * const opt)
{
    const struct w_sockaddr local = {.addr = w->ifaddr[addr_idx].addr,
                                     .port = port};
    struct w_sock * s = w_get_sock(w, &local, 0);
    if (unlikely(s)) {
        warn(INF, "UDP source port %d already in bound", bswap16(port));
        return 0;
    }

    if (unlikely(s = calloc(1, sizeof(*s))) == 0)
        goto fail;

    s->tup.local = local;
    s->w = w;
    sq_init(&s->iv);

    if (unlikely(backend_bind(s, opt) != 0))
        goto fail;

    warn(NTE, "socket bound to %s:%d",
         w_ntop(&local.addr, (char[IP6_STRLEN]){""}, IP6_STRLEN),
         bswap16(port));

    ins_sock(w, s);
    return s;

fail:
    if (s)
        free(s);
    return 0;
}


/// Close a warpcore socket.
///
/// @param      s     w_sock to close.
///
void w_close(struct w_sock * const s)
{
    backend_close(s);

    // remove the socket from list of sockets
    rem_sock(s->w, s);

    // free the socket
    free(s);
}


/// Shut a warpcore engine down cleanly. In addition to calling into the
/// backend-specific cleanup function, it frees up the extra buffers and other
/// memory structures.
///
/// @param      w     Backend engine.
///
void w_cleanup(struct w_engine * const w)
{
    warn(NTE, "warpcore shutting down");

    // close all sockets
    struct w_sock * s;
    kh_foreach_value(&w->sock, s, { w_close(s); });
    kh_release(sock, &w->sock);

    backend_cleanup(w);
    sl_remove(&engines, w, w_engine, next);
    free(w->b);
    free(w);
}


/// Init state for w_rand() and w_rand_uniform(). This **MUST** be called once
/// prior to calling any of these functions!
///
void w_init_rand(void)
{
    // init state for w_rand()
#if !defined(FUZZING) && !defined(PARTICLE) && !defined(RIOT_VERSION)
    struct timeval now;
    gettimeofday(&now, 0);
    const uint64_t seed = fnv1a_64(&now, sizeof(now));
    kr_srand_r(&w_rand_state, seed);
#else
    kr_srand_r(&w_rand_state, 0);
#endif
}


static bool __attribute__((nonnull))
skip_ipv6_addr(struct ifaddrs * const i, const bool is_loopback)
{
    const struct in6_addr * const ip6 =
        &((struct sockaddr_in6 *)(void *)i->ifa_addr)->sin6_addr;

    if (IN6_IS_ADDR_LINKLOCAL(ip6))
        return true;
    if (IN6_IS_ADDR_SITELOCAL(ip6))
        return true;
    if (IN6_IS_ADDR_V4MAPPED(ip6))
        return true;
    if (IN6_IS_ADDR_V4COMPAT(ip6))
        return true;
    if (is_loopback == false && IN6_IS_ADDR_LOOPBACK(ip6))
        return true;
    // if (IN6_IS_ADDR_UNSPECIFIED(ip6))
    //     return true;
    return false;
}


static uint8_t __attribute__((nonnull))
contig_mask_len(const int af, const void * const mask)
{
    uint8_t mask_len = 0;
    if (af == AF_IP4) {
        const uint32_t mask4 = bswap32(*(const uint32_t *)mask);
        mask_len = !(mask4 & (~mask4 >> 1));
    } else {
        uint128_t mask6;
        memcpy(&mask6, mask, IP6_LEN);
        mask6 = bswap128(mask6);
        mask_len = !(mask6 & (~mask6 >> 1));
    }

    if (mask_len == 0)
        return 0;

    uint8_t pos = 0;
    mask_len = 0;
    while ((pos < af_len(af)) && (((const uint8_t *)mask)[pos] == 0xff)) {
        mask_len += 8;
        pos++;
    }

    if (pos < af_len(af)) {
        uint8_t val = ((const uint8_t *)mask)[pos];
        while (val) {
            mask_len++;
            val = (uint8_t)(val << 1);
        }
    }

    return mask_len;
}


const char *
w_ntop(const struct w_addr * const addr, char * const dst, const size_t dst_len)
{
    return inet_ntop(addr->af,
                     (addr->af == AF_IP4 ? (const void *)&addr->ip4
                                         : (const void *)&addr->ip6),
                     dst, (socklen_t)dst_len);
}


/// Initialize a warpcore engine on the given interface. Ethernet and IP
/// source addresses and related information, such as the netmask, are taken
/// from the active OS configuration of the interface. A default router,
/// however, needs to be specified with @p rip, if communication over a WAN is
/// desired. @p nbufs controls how many packet buffers the engine will attempt
/// to allocate.
///
/// @param[in]  ifname  The OS name of the interface (e.g., "eth0").
/// @param[in]  rip     The default router to be used for non-local
///                     destinations. Can be zero.
/// @param[in]  nbufs   Number of extra packet buffers to allocate.
///
/// @return     Initialized warpcore engine.
///
struct w_engine * w_init(const char * const ifname,
                         const uint32_t rip __attribute__((unused)),
                         const uint_t nbufs)
{
    w_init_rand();

#if !defined(PARTICLE) && !defined(RIOT_VERSION)
    // construct interface name of a netmap pipe for this interface
    char pipe[IFNAMSIZ];
    snprintf(pipe, IFNAMSIZ, "warp-%s", ifname);
#endif

#ifdef RIOT_VERSION
    struct netif * const i = netif_get_by_index(netif_name_to_index(ifname));
    ensure(i, "could not find interface $s", ifname);
    w->mtu = i->mtu;
    memcpy(&w->mac, i->hwaddr, sizeof(w->mac));
    w->mbps = UINT32_MAX;
#endif

    // we mostly loop here because the link may be down
    uint16_t addr4_cnt = 0;
    uint16_t addr6_cnt = 0;
    struct ifaddrs * ifap = 0;
    while (addr4_cnt + addr6_cnt == 0) {
        // get interface config
        ensure(getifaddrs(&ifap) != -1, "%s: cannot get interface info",
               ifname);

        for (struct ifaddrs * i = ifap; i; i = i->ifa_next) {
            if (strcmp(i->ifa_name, ifname) != 0)
                continue;

            if (plat_get_link(i) == false)
                continue;

            if (i->ifa_addr->sa_family == AF_INET) {
                addr4_cnt++;
                continue;
            }

            if (i->ifa_addr->sa_family == AF_INET6 &&
                !skip_ipv6_addr(i, i->ifa_flags & IFF_LOOPBACK))
                addr6_cnt++;
        }
        if (addr4_cnt + addr6_cnt == 0) {
            freeifaddrs(ifap);
            // sleep for a bit, so we don't burn the CPU when link is down
            warn(WRN,
                 "%s: could not obtain required interface "
                 "information, retrying",
                 ifname);
            w_nanosleep(1 * NS_PER_S);
        }
    }

    // allocate engine struct with room for addresses
    struct w_engine * w;
    ensure((w = calloc(1, sizeof(*w) + (addr4_cnt + addr6_cnt) *
                                           sizeof(w->ifaddr[0]))) != 0,
           "cannot allocate struct w_engine");
    w->addr_cnt = addr4_cnt + addr6_cnt;

    // initialize lists of sockets and iovs
    sq_init(&w->iov);

    bool is_loopback = false;
    bool have_pipe = false;
    w->addr4_pos = addr4_cnt = addr6_cnt;
    addr6_cnt = 0;
    for (struct ifaddrs * i = ifap; i; i = i->ifa_next) {
#ifndef PARTICLE
        if (strcmp(i->ifa_name, pipe) == 0)
            have_pipe = true;
#endif
        if (strcmp(i->ifa_name, ifname) != 0)
            continue;

        switch (i->ifa_addr->sa_family) {
        case AF_LINK:
            is_loopback = i->ifa_flags & IFF_LOOPBACK;
            plat_get_mac(&w->mac, i);
            w->mtu = plat_get_mtu(i);
            // mpbs can be zero on generic platforms and loopback interfaces
            w->mbps = plat_get_mbps(i);
            plat_get_iface_driver(i, w->drvname, sizeof(w->drvname));
            warn(NTE, "%s MAC addr %s, MTU %d, speed %" PRIu32 "G", i->ifa_name,
                 eth_ntoa(&w->mac, (char[ETH_STRLEN]){""}), w->mtu,
                 w->mbps / 1000);
            break;

        case AF_INET6:
            if (skip_ipv6_addr(i, i->ifa_flags & IFF_LOOPBACK))
                continue;

            w->have_ip6 = true;
            struct w_ifaddr * ia = &w->ifaddr[addr6_cnt++];
            set_ip(&ia->addr, i->ifa_addr);
            const void * const sa_mask6 =
                &((const struct sockaddr_in6 *)(const void *)i->ifa_netmask)
                     ->sin6_addr;
            ia->prefix = contig_mask_len(ia->addr.af, sa_mask6);

            uint128_t mask6;
            memcpy(&mask6, sa_mask6, sizeof(mask6));
            ia->net6 = ia->addr.ip6 & mask6;
            ia->bcast6 = ia->addr.ip6 | ~mask6;
            break;

        case AF_INET:
            ia = &w->ifaddr[addr4_cnt++];

            w->have_ip4 = true;
            set_ip(&ia->addr, i->ifa_addr);
            const void * const sa_mask4 =
                &((const struct sockaddr_in *)(const void *)i->ifa_netmask)
                     ->sin_addr;
            ia->prefix = contig_mask_len(ia->addr.af, sa_mask4);

            uint32_t mask4;
            memcpy(&mask4, sa_mask4, sizeof(mask4));
            ia->net4 = ia->addr.ip4 & mask4;
            ia->bcast4 = ia->addr.ip4 | ~mask4;
            break;

        default:
            warn(NTE, "ignoring unknown addr family %d on %s",
                 i->ifa_addr->sa_family, i->ifa_name);
            break;
        }
    }
    freeifaddrs(ifap);

#if !defined(NDEBUG) || defined(NDEBUG_WITH_DLOG)
    for (uint16_t idx = 0; idx < w->addr_cnt; idx++) {
        struct w_ifaddr * const ia = &w->ifaddr[idx];
        warn(NTE, "%s IPv%d addr %s/%u", ifname, ia->addr.af == AF_IP4 ? 4 : 6,
             w_ntop(&ia->addr, (char[IP6_STRLEN]){""}, IP6_STRLEN), ia->prefix);
    }
#endif

    strncpy(w->ifname, ifname, sizeof(w->ifname));

    // set the IP address of our default router
    // w->rip = rip;

#if !defined(PARTICLE) && !defined(RIOT_VERSION)
    // some interfaces can have huge MTUs, so cap to something more sensible
    w->mtu = MIN(w->mtu, (uint16_t)getpagesize() / 2);
#endif

    // backend-specific init
    w->b = calloc(1, sizeof(*w->b));
    ensure(w->b, "cannot alloc backend");
    ensure(nbufs <= UINT32_MAX, "too many nbufs %" PRIu, nbufs);
    backend_init(w, (uint32_t)nbufs, is_loopback, !have_pipe);

    // store the initialized engine in our global list
    sl_insert_head(&engines, w, next);

    warn(INF, "%s/%s (%s) %s using %" PRIu " %u-byte bufs on %s", warpcore_name,
         w->backend_name, w->backend_variant, warpcore_version,
         w_iov_sq_cnt(&w->iov), w->mtu, w->ifname);
    return w;
}


/// Return the maximum size a given w_iov may have for the given engine.
/// Basically, subtracts the header space and any offset specified when
/// allocating the w_iov from the MTU.
///
/// @param[in]  v     The w_iov in question.
///
/// @return     Maximum length of the data in a w_iov for this engine.
///
uint16_t w_iov_max_len(const struct w_iov * const v)
{
    const uint16_t offset = (const uint16_t)(v->buf - v->base);
    return v->w->mtu - offset;
}


/// Return a w_iov tail queue obtained via w_alloc_len(), w_alloc_cnt() or
/// w_rx() back to warpcore.
///
/// @param      q     Tail queue of w_iov structs to return.
///
void w_free(struct w_iov_sq * const q)
{
    if (unlikely(sq_empty(q)))
        return;
    struct w_engine * const w = sq_first(q)->w;
#if !defined(NDEBUG) || defined(NDEBUG_WITH_DLOG)
    struct w_iov * v;
    sq_foreach (v, q, next) {
#ifdef DEBUG_BUFFERS
        warn(DBG, "w_free idx %" PRIu32, v->idx);
#endif
        ASAN_POISON_MEMORY_REGION(v->base, v->w->mtu);
    }
#endif
    sq_concat(&w->iov, q);
#ifdef DEBUG_BUFFERS
    dump_bufs(__func__, &w->iov);
#endif
}


/// Return a single w_iov obtained via w_alloc_len(), w_alloc_cnt() or
/// w_rx() back to warpcore.
///
/// @param      v     w_iov struct to return.
///
void w_free_iov(struct w_iov * const v)
{
#ifdef DEBUG_BUFFERS
    warn(DBG, "w_free_iov idx %" PRIu32, v->idx);
#endif
    ensure(sq_next(v, next) == 0,
           "idx %" PRIu32 " still linked to idx %" PRIu32, v->idx,
           sq_next(v, next)->idx);
#ifdef DEBUG_BUFFERS
    dump_bufs(__func__, &v->w->iov);
#endif
    sq_insert_head(&v->w->iov, v, next);
    ASAN_POISON_MEMORY_REGION(v->base, v->w->mtu);
#ifdef DEBUG_BUFFERS
    dump_bufs(__func__, &v->w->iov);
#endif
}


/// Return a 64-bit random number. Fast, but not cryptographically secure.
/// Implements xoroshiro128+; see
/// https://en.wikipedia.org/wiki/Xoroshiro128%2B.
///
/// @return     Random number.
///
uint64_t w_rand64(void)
{
    return kr_rand_r(&w_rand_state);
}


/// Return a 32-bit random number. Fast, but not cryptographically secure.
/// Truncates w_rand64() to 32 bits.
///
/// @return     Random number.
///
uint32_t w_rand32(void)
{
#if !defined(PARTICLE) && !defined(RIOT_VERSION)
    return (uint32_t)kr_rand_r(&w_rand_state);
#elif defined(PARTICLE)
    return HAL_RNG_GetRandomNumber();
#elif defined(RIOT_VERSION)
    return random_uint32();
#endif
}


/// Calculate a uniformly distributed random number in [0, upper_bound)
/// avoiding "modulo bias".
///
/// @param[in]  upper_bound  The upper bound
///
/// @return     Random number.
///
uint64_t w_rand_uniform64(const uint64_t upper_bound)
{
    if (unlikely(upper_bound < 2))
        return 0;

    // 2**64 % x == (2**64 - x) % x
    const uint64_t min = (UINT64_MAX - upper_bound) % upper_bound;

    // This could theoretically loop forever but each retry has p > 0.5
    // (worst case, usually far better) of selecting a number inside the
    // range we need, so it should rarely need to re-roll.
    uint64_t r;
    for (;;) {
        r = kr_rand_r(&w_rand_state);
        if (likely(r >= min))
            break;
    }

    return r % upper_bound;
}


/// Calculate a uniformly distributed random number in [0, upper_bound)
/// avoiding "modulo bias". This can be faster on platforms with crappy
/// 64-bit math.
///
/// @param[in]  upper_bound  The upper bound
///
/// @return     Random number.
///
uint32_t w_rand_uniform32(const uint32_t upper_bound)
{
    if (unlikely(upper_bound < 2))
        return 0;

    // 2**32 % x == (2**32 - x) % x
    const uint32_t min = (UINT32_MAX - upper_bound) % upper_bound;

    // This could theoretically loop forever but each retry has p > 0.5
    // (worst case, usually far better) of selecting a number inside the
    // range we need, so it should rarely need to re-roll.
    uint32_t r;
    for (;;) {
        r = w_rand32();
        if (likely(r >= min))
            break;
    }

    return r % upper_bound;
}


static void reinit_iov(struct w_iov * const v)
{
    v->buf = v->base;
    v->len = v->w->mtu;
    v->o = 0;
    sq_next(v, next) = 0;
}


void init_iov(struct w_engine * const w,
              struct w_iov * const v,
              const uint32_t idx)
{
    v->w = w;
    v->idx = idx;
    v->base = idx_to_buf(w, v->idx);
    reinit_iov(v);
}


struct w_iov * w_alloc_iov_base(struct w_engine * const w)
{
    struct w_iov * const v = sq_first(&w->iov);
    if (likely(v)) {
        sq_remove_head(&w->iov, next);
        reinit_iov(v);
        ASAN_UNPOISON_MEMORY_REGION(v->base, w->mtu);
    }
#ifdef DEBUG_BUFFERS
    warn(DBG, "w_alloc_iov_base idx %" PRIu32, v ? v->idx : UINT32_MAX);
#endif
    return v;
}


khint_t w_socktuple_hash(const struct w_socktuple * const tup)
{
    return w_addr_hash(&tup->local.addr) +
           fnv1a_32(&tup->local.port, sizeof(tup->local.port)) +
           (tup->remote.addr.af
                ? (w_addr_hash(&tup->remote.addr) +
                   fnv1a_32(&tup->local.port, sizeof(tup->local.port)))
                : 0);
}


khint_t w_socktuple_cmp(const struct w_socktuple * const a,
                        const struct w_socktuple * const b)
{
    return a->local.port == b->local.port && a->remote.port == b->remote.port &&
           w_addr_cmp(&a->local.addr, &b->local.addr) &&
           w_addr_cmp(&a->remote.addr, &b->remote.addr);
}


uint64_t w_now(void)
{
#if defined(PARTICLE)
    return HAL_Timer_Microseconds() * NS_PER_US;
#elif defined(RIOT_VERSION)
    return xtimer_now_usec64() * NS_PER_US;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * NS_PER_S + (uint64_t)now.tv_nsec;
#endif
}


void w_nanosleep(const uint64_t ns)
{
#ifdef PARTICLE
    HAL_Delay_Microseconds(ns / NS_PER_US);
#elif defined(RIOT_VERSION)
    xtimer_nanosleep(ns);
#else
    nanosleep(&(struct timespec){ns / NS_PER_S, (long)(ns % NS_PER_S)}, 0);
#endif
}
