/*
 *  OpenVPN -- An application to securely tunnel IP networks
 *             over a single TCP/UDP port, with support for SSL/TLS-based
 *             session authentication and key exchange,
 *             packet encryption, packet authentication, and
 *             packet compression.
 *
 *  Copyright (C) 2002-2022 OpenVPN Inc <sales@openvpn.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#elif defined(_MSC_VER)
#include "config-msvc.h"
#endif

#include "syshead.h"

#include "multi.h"
#include <inttypes.h>
#include "forward.h"

#include "memdbg.h"
#include "ssl_pkt.h"

#ifdef HAVE_SYS_INOTIFY_H
#include <sys/inotify.h>
#endif

static bool
do_pre_decrypt_check(struct multi_context *m)
{
    ASSERT(m->top.c2.tls_auth_standalone);

    enum first_packet_verdict verdict;
    struct tls_pre_decrypt_state state = {0};

    verdict = tls_pre_decrypt_lite(m->top.c2.tls_auth_standalone, &state,
                                   &m->top.c2.from, &m->top.c2.buf);

    free_tls_pre_decrypt_state(&state);

    if (verdict == VERDICT_INVALID || verdict == VERDICT_VALID_CONTROL_V1)
    {
        return false;
    }
    return true;
}

/*
 * Get a client instance based on real address.  If
 * the instance doesn't exist, create it while
 * maintaining real address hash table atomicity.
 */

struct multi_instance *
multi_get_create_instance_udp(struct multi_context *m, bool *floated)
{
    struct gc_arena gc = gc_new();
    struct mroute_addr real;
    struct multi_instance *mi = NULL;
    struct hash *hash = m->hash;

    if (mroute_extract_openvpn_sockaddr(&real, &m->top.c2.from.dest, true)
        && m->top.c2.buf.len > 0)
    {
        struct hash_element *he;
        const uint32_t hv = hash_value(hash, &real);
        struct hash_bucket *bucket = hash_bucket(hash, hv);
        uint8_t *ptr = BPTR(&m->top.c2.buf);
        uint8_t op = ptr[0] >> P_OPCODE_SHIFT;
        bool v2 = (op == P_DATA_V2) && (m->top.c2.buf.len >= (1 + 3));
        bool peer_id_disabled = false;

        /* make sure buffer has enough length to read opcode (1 byte) and peer-id (3 bytes) */
        if (v2)
        {
            uint32_t peer_id = ntohl(*(uint32_t *)ptr) & 0xFFFFFF;
            peer_id_disabled = (peer_id == MAX_PEER_ID);

            if (!peer_id_disabled && (peer_id < m->max_clients) && (m->instances[peer_id]))
            {
                mi = m->instances[peer_id];

                *floated = !link_socket_actual_match(&mi->context.c2.from, &m->top.c2.from);

                if (*floated)
                {
                    /* reset prefix, since here we are not sure peer is the one it claims to be */
                    ungenerate_prefix(mi);
                    msg(D_MULTI_MEDIUM, "Float requested for peer %" PRIu32 " to %s", peer_id,
                        mroute_addr_print(&real, &gc));
                }
            }
        }
        if (!v2 || peer_id_disabled)
        {
            he = hash_lookup_fast(hash, bucket, &real, hv);
            if (he)
            {
                mi = (struct multi_instance *) he->value;
            }
        }
        if (!mi)
        {
            if (do_pre_decrypt_check(m))
            {
                if (frequency_limit_event_allowed(m->new_connection_limiter))
                {
                    mi = multi_create_instance(m, &real);
                    if (mi)
                    {
                        hash_add_fast(hash, bucket, &mi->real, hv, mi);
                        mi->did_real_hash = true;
                        multi_assign_peer_id(m, mi);
                    }
                }
                else
                {
                    msg(D_MULTI_ERRORS,
                        "MULTI: Connection from %s would exceed new connection frequency limit as controlled by --connect-freq",
                        mroute_addr_print(&real, &gc));
                }
            }
        }

#ifdef ENABLE_DEBUG
        if (check_debug_level(D_MULTI_DEBUG))
        {
            const char *status = mi ? "[ok]" : "[failed]";

            dmsg(D_MULTI_DEBUG, "GET INST BY REAL: %s %s",
                 mroute_addr_print(&real, &gc),
                 status);
        }
#endif
    }

    gc_free(&gc);
    ASSERT(!(mi && mi->halt));
    return mi;
}

/*
 * Send a packet to TCP/UDP socket.
 */
static inline void
multi_process_outgoing_link(struct multi_context *m, const unsigned int mpp_flags)
{
    struct multi_instance *mi = multi_process_outgoing_link_pre(m);
    if (mi)
    {
        multi_process_outgoing_link_dowork(m, mi, mpp_flags);
    }
}

/*
 * Process an I/O event.
 */
static void
multi_process_io_udp(struct multi_context *m)
{
    const unsigned int status = m->top.c2.event_set_status;
    const unsigned int mpp_flags = m->top.c2.fast_io
                                   ? (MPP_CONDITIONAL_PRE_SELECT | MPP_CLOSE_ON_SIGNAL)
                                   : (MPP_PRE_SELECT | MPP_CLOSE_ON_SIGNAL);

#ifdef MULTI_DEBUG_EVENT_LOOP
    char buf[16];
    buf[0] = 0;
    if (status & SOCKET_READ)
    {
        strcat(buf, "SR/");
    }
    else if (status & SOCKET_WRITE)
    {
        strcat(buf, "SW/");
    }
    else if (status & TUN_READ)
    {
        strcat(buf, "TR/");
    }
    else if (status & TUN_WRITE)
    {
        strcat(buf, "TW/");
    }
    else if (status & FILE_CLOSED)
    {
        strcat(buf, "FC/");
    }
    printf("IO %s\n", buf);
#endif /* ifdef MULTI_DEBUG_EVENT_LOOP */

#ifdef ENABLE_MANAGEMENT
    if (status & (MANAGEMENT_READ|MANAGEMENT_WRITE))
    {
        ASSERT(management);
        management_io(management);
    }
#endif

    /* UDP port ready to accept write */
    if (status & SOCKET_WRITE)
    {
        multi_process_outgoing_link(m, mpp_flags);
    }
    /* TUN device ready to accept write */
    else if (status & TUN_WRITE)
    {
        multi_process_outgoing_tun(m, mpp_flags);
    }
    /* Incoming data on UDP port */
    else if (status & SOCKET_READ)
    {
        read_incoming_link(&m->top);
        if (!IS_SIG(&m->top))
        {
            multi_process_incoming_link(m, NULL, mpp_flags);
        }
    }
    /* Incoming data on TUN device */
    else if (status & TUN_READ)
    {
        read_incoming_tun(&m->top);
        if (!IS_SIG(&m->top))
        {
            multi_process_incoming_tun(m, mpp_flags);
        }
    }
#ifdef ENABLE_ASYNC_PUSH
    /* INOTIFY callback */
    else if (status & FILE_CLOSED)
    {
        multi_process_file_closed(m, mpp_flags);
    }
#endif
}

/*
 * Return the io_wait() flags appropriate for
 * a point-to-multipoint tunnel.
 */
static inline unsigned int
p2mp_iow_flags(const struct multi_context *m)
{
    unsigned int flags = IOW_WAIT_SIGNAL;
    if (m->pending)
    {
        if (TUN_OUT(&m->pending->context))
        {
            flags |= IOW_TO_TUN;
        }
        if (LINK_OUT(&m->pending->context))
        {
            flags |= IOW_TO_LINK;
        }
    }
    else if (mbuf_defined(m->mbuf))
    {
        flags |= IOW_MBUF;
    }
    else
    {
        flags |= IOW_READ;
    }
#ifdef _WIN32
    if (tuntap_ring_empty(m->top.c1.tuntap))
    {
        flags &= ~IOW_READ_TUN;
    }
#endif
    return flags;
}


void
tunnel_server_udp(struct context *top)
{
    struct multi_context multi;

    top->mode = CM_TOP;
    context_clear_2(top);

    /* initialize top-tunnel instance */
    init_instance_handle_signals(top, top->es, CC_HARD_USR1_TO_HUP);
    if (IS_SIG(top))
    {
        return;
    }

    /* initialize global multi_context object */
    multi_init(&multi, top, false);

    /* initialize our cloned top object */
    multi_top_init(&multi, top);

    /* initialize management interface */
    init_management_callback_multi(&multi);

    /* finished with initialization */
    initialization_sequence_completed(top, ISC_SERVER); /* --mode server --proto udp */

#ifdef ENABLE_ASYNC_PUSH
    multi.top.c2.inotify_fd = inotify_init();
    if (multi.top.c2.inotify_fd < 0)
    {
        msg(D_MULTI_ERRORS | M_ERRNO, "MULTI: inotify_init error");
    }
#endif

    /* per-packet event loop */
    while (true)
    {
        perf_push(PERF_EVENT_LOOP);

        /* set up and do the io_wait() */
        multi_get_timeout(&multi, &multi.top.c2.timeval);
        io_wait(&multi.top, p2mp_iow_flags(&multi));
        MULTI_CHECK_SIG(&multi);

        /* check on status of coarse timers */
        multi_process_per_second_timers(&multi);

        /* timeout? */
        if (multi.top.c2.event_set_status == ES_TIMEOUT)
        {
            multi_process_timeout(&multi, MPP_PRE_SELECT|MPP_CLOSE_ON_SIGNAL);
        }
        else
        {
            /* process I/O */
            multi_process_io_udp(&multi);
            MULTI_CHECK_SIG(&multi);
        }

        perf_pop();
    }

#ifdef ENABLE_ASYNC_PUSH
    close(top->c2.inotify_fd);
#endif

    /* shut down management interface */
    uninit_management_callback();

    /* save ifconfig-pool */
    multi_ifconfig_pool_persist(&multi, true);

    /* tear down tunnel instance (unless --persist-tun) */
    multi_uninit(&multi);
    multi_top_free(&multi);
    close_instance(top);
}

