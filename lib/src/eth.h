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

#pragma once

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef PARTICLE
#include <netinet/if_ether.h>
#endif

#ifdef WITH_NETMAP
// IWYU pragma: no_include <net/netmap.h>
#include <net/netmap_user.h> // IWYU pragma: keep
#endif

#include <warpcore/warpcore.h>

#include "arp.h"

struct netmap_slot;
struct netmap_slot;


/// An [Ethernet II MAC
/// header](https://en.wikipedia.org/wiki/Ethernet_frame#Ethernet_II).
///
struct eth_hdr {
    struct ether_addr dst; ///< Destination MAC address.
    struct ether_addr src; ///< Source MAC address.
    uint16_t type;         ///< EtherType of the payload data.
} __attribute__((aligned(1)));


#define ETH_TYPE_IP bswap16(0x0800)  ///< EtherType for IPv4.
#define ETH_TYPE_ARP bswap16(0x0806) ///< EtherType for ARP.

/// Return a pointer to the first data byte inside the Ethernet frame in @p buf.
///
/// @param      buf   The buffer to find data in.
///
/// @return     Pointer to the first data byte inside @p buf.
///
static inline __attribute__((always_inline, nonnull)) uint8_t *
eth_data(uint8_t * const buf)
{
    return buf + sizeof(struct eth_hdr);
}


extern bool __attribute__((nonnull)) eth_rx(struct w_engine * const w,
                                            struct netmap_slot * const s,
                                            uint8_t * const buf);

#ifdef WITH_NETMAP

static inline void __attribute__((nonnull))
mk_eth_hdr(const struct w_sock * const s, struct w_iov * const v)
{
    struct eth_hdr * const eth = (void *)v->base;
    const struct sockaddr_in * const addr4 = (struct sockaddr_in *)&v->addr;
    eth->dst =
        w_connected(s) ? s->dmac : arp_who_has(s->w, addr4->sin_addr.s_addr);
    eth->src = v->w->mac;
    eth->type = ETH_TYPE_IP;
}

extern bool __attribute__((nonnull))
eth_tx(struct w_iov * const v, const uint16_t len);

#endif
