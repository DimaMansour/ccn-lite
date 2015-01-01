/*
 * @f ccn-lite-relay.c
 * @b user space CCN relay
 *
 * Copyright (C) 2011-14, Christian Tschudin, University of Basel
 *
 * Permission to use, copy, modify, and/or distribute this software for any
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
 *
 * File history:
 * 2011-11-22 created
 */

#include <dirent.h>
#include <fnmatch.h>
#include <regex.h>
#include <sys/stat.h>
#include <sys/types.h>

#define CCNL_UNIX

#define USE_CCNxDIGEST
#define USE_DEBUG                      // must select this for USE_MGMT
#define USE_DEBUG_MALLOC
// #define USE_FRAG
#define USE_ETHERNET
#define USE_HTTP_STATUS
#define USE_MGMT
// #define USE_NACK
// #define USE_NFN
#define USE_NFN_DEFAULT_ROUTE
#define USE_NFN_NSTRANS
// #define USE_NFN_MONITOR
// #define USE_SCHEDULER
#define USE_SUITE_CCNB                 // must select this for USE_MGMT
#define USE_SUITE_CCNTLV
#define USE_SUITE_IOTTLV
#define USE_SUITE_NDNTLV
#define USE_SUITE_LOCALRPC
#define USE_UNIXSOCKET
// #define USE_SIGNATURES

#include "ccnl-os-includes.h"

#include "ccnl-defs.h"
#include "ccnl-core.h"

#include "ccnl-ext.h"
#include "ccnl-ext-debug.c"
#include "ccnl-os-time.c"
#include "ccnl-ext-logging.c"

#define ccnl_app_RX(x,y)                do{}while(0)
#define ccnl_print_stats(x,y)           do{}while(0)

#include "ccnl-core.c"

#include "ccnl-ext-http.c"
#include "ccnl-ext-localrpc.c"
#include "ccnl-ext-mgmt.c"
#include "ccnl-ext-nfn.c"
#include "ccnl-ext-nfnmonitor.c"
#include "ccnl-ext-sched.c"
#include "ccnl-ext-frag.c"
#include "ccnl-ext-crypto.c"

// ----------------------------------------------------------------------

struct ccnl_relay_s theRelay;
char suite = CCNL_SUITE_DEFAULT; 

struct timeval*
ccnl_run_events()
{
    static struct timeval now;
    long usec;

    gettimeofday(&now, 0);
    while (eventqueue) {
        struct ccnl_timer_s *t = eventqueue;

        usec = timevaldelta(&(t->timeout), &now);
        if (usec >= 0) {
            now.tv_sec = usec / 1000000;
            now.tv_usec = usec % 1000000;
            return &now;
        }
        if (t->fct)
            (t->fct)(t->node, t->intarg);
        else if (t->fct2)
            (t->fct2)(t->aux1, t->aux2);
        eventqueue = t->next;
        ccnl_free(t);
    }

    return NULL;
}

// ----------------------------------------------------------------------

#ifdef USE_ETHERNET
int
ccnl_open_ethdev(char *devname, struct sockaddr_ll *sll, int ethtype)
{
    struct ifreq ifr;
    int s;

    DEBUGMSG(TRACE, "ccnl_open_ethdev %s 0x%04x\n", devname, ethtype);

    s = socket(AF_PACKET, SOCK_RAW, htons(ethtype));
    if (s < 0) {
        perror("eth socket");
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, (char*) devname, IFNAMSIZ);
    if(ioctl(s, SIOCGIFHWADDR, (void *) &ifr) < 0 ) {
        perror("ethsock ioctl get hw addr");
        return -1;
    }

    sll->sll_family = AF_PACKET;
    memcpy(sll->sll_addr, &ifr.ifr_hwaddr.sa_data, ETH_ALEN);
    if(ioctl(s, SIOCGIFINDEX, (void *) &ifr) < 0 ) {
        perror("ethsock ioctl get index");
        return -1;
    }
    sll->sll_ifindex = ifr.ifr_ifindex;
    sll->sll_protocol = htons(ethtype);
    if (bind(s, (struct sockaddr*) sll, sizeof(*sll)) < 0) {
        perror("ethsock bind");
        return -1;
    }

    return s;
}
#endif // USE_ETHERNET

#ifdef USE_UNIXSOCKET
int
ccnl_open_unixpath(char *path, struct sockaddr_un *ux)
{
  int sock, bufsize;

    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("opening datagram socket");
        return -1;
    }

    unlink(path);
    ux->sun_family = AF_UNIX;
    strcpy(ux->sun_path, path);

    if (bind(sock, (struct sockaddr *) ux, sizeof(struct sockaddr_un))) {
        perror("binding name to datagram socket");
        close(sock);
        return -1;
    }

    bufsize = 4 * CCNL_MAX_PACKET_SIZE;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

    return sock;

}
#endif // USE_UNIXSOCKET


int
ccnl_open_udpdev(int port, struct sockaddr_in *si)
{
    int s;
    unsigned int len;

    s = socket(PF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        perror("udp socket");
        return -1;
    }

    si->sin_addr.s_addr = INADDR_ANY;
    si->sin_port = htons(port);
    si->sin_family = PF_INET;
    if(bind(s, (struct sockaddr *)si, sizeof(*si)) < 0) {
        perror("udp sock bind");
        return -1;
    }
    len = sizeof(*si);
    getsockname(s, (struct sockaddr*) si, &len);

    return s;
}

#ifdef USE_ETHERNET
int
ccnl_eth_sendto(int sock, unsigned char *dst, unsigned char *src,
                unsigned char *data, int datalen)
{
    short type = htons(CCNL_ETH_TYPE);
    unsigned char buf[2000];
    int hdrlen;

    strcpy((char*)buf, eth2ascii(dst));
    DEBUGMSG(TRACE, "ccnl_eth_sendto %d bytes (src=%s, dst=%s)\n",
             datalen, eth2ascii(src), buf);

    hdrlen = 14;
    if ((datalen+hdrlen) > sizeof(buf))
            datalen = sizeof(buf) - hdrlen;
    memcpy(buf, dst, 6);
    memcpy(buf+6, src, 6);
    memcpy(buf+12, &type, sizeof(type));
    memcpy(buf+hdrlen, data, datalen);

    return sendto(sock, buf, hdrlen + datalen, 0, 0, 0);
}
#endif // USE_ETHERNET

void
ccnl_ll_TX(struct ccnl_relay_s *ccnl, struct ccnl_if_s *ifc,
           sockunion *dest, struct ccnl_buf_s *buf)
{
    int rc;

    switch(dest->sa.sa_family) {
    case AF_INET:
        rc = sendto(ifc->sock,
                    buf->data, buf->datalen, 0,
                    (struct sockaddr*) &dest->ip4, sizeof(struct sockaddr_in));
        DEBUGMSG(DEBUG, "udp sendto %s/%d returned %d\n",
                 inet_ntoa(dest->ip4.sin_addr), ntohs(dest->ip4.sin_port), rc);
        break;
#ifdef USE_ETHERNET
    case AF_PACKET:
        rc = ccnl_eth_sendto(ifc->sock,
                             dest->eth.sll_addr,
                             ifc->addr.eth.sll_addr,
                             buf->data, buf->datalen);
        DEBUGMSG(DEBUG, "eth_sendto %s returned %d\n",
                 eth2ascii(dest->eth.sll_addr), rc);
        break;
#endif
#ifdef USE_UNIXSOCKET
    case AF_UNIX:
        rc = sendto(ifc->sock,
                    buf->data, buf->datalen, 0,
                    (struct sockaddr*) &dest->ux, sizeof(struct sockaddr_un));
        DEBUGMSG(DEBUG, "unix sendto %s returned %d\n",
                 dest->ux.sun_path, rc);
        break;
#endif
    default:
        DEBUGMSG(WARNING, "unknown transport\n");
        break;
    }
    rc = 0; // just to silence a compiler warning (if USE_DEBUG is not set)
}

void
ccnl_close_socket(int s)
{
    struct sockaddr_un su;
    socklen_t len = sizeof(su);

    if (!getsockname(s, (struct sockaddr*) &su, &len) &&
                                        su.sun_family == AF_UNIX) {
        unlink(su.sun_path);
    }
    close(s);
}

// ----------------------------------------------------------------------

static int inter_ccn_interval = 0; // in usec
static int inter_pkt_interval = 0; // in usec

#ifdef USE_SCHEDULER
struct ccnl_sched_s*
ccnl_relay_defaultFaceScheduler(struct ccnl_relay_s *ccnl,
                                void(*cb)(void*,void*))
{
    return ccnl_sched_pktrate_new(cb, ccnl, inter_ccn_interval);
}

struct ccnl_sched_s*
ccnl_relay_defaultInterfaceScheduler(struct ccnl_relay_s *ccnl,
                                     void(*cb)(void*,void*))
{
    return ccnl_sched_pktrate_new(cb, ccnl, inter_pkt_interval);
}
/*
#else
# define ccnl_relay_defaultFaceScheduler       NULL
# define ccnl_relay_defaultInterfaceScheduler  NULL
*/
#endif // USE_SCHEDULER


void ccnl_ageing(void *relay, void *aux)
{
    ccnl_do_ageing(relay, aux);
    ccnl_set_timer(1000000, ccnl_ageing, relay, 0);
}

// ----------------------------------------------------------------------

void
ccnl_relay_config(struct ccnl_relay_s *relay, char *ethdev, int udpport,
                  int httpport, char *uxpath, int suite, int max_cache_entries,
                  char *crypto_face_path)
{
    struct ccnl_if_s *i;

    DEBUGMSG(INFO, "configuring relay\n");

    relay->max_cache_entries = max_cache_entries;       
#ifdef USE_SCHEDULER
    relay->defaultFaceScheduler = ccnl_relay_defaultFaceScheduler;
    relay->defaultInterfaceScheduler = ccnl_relay_defaultInterfaceScheduler;
#endif
    
#ifdef USE_ETHERNET
    // add (real) eth0 interface with index 0:
    if (ethdev) {
        i = &relay->ifs[relay->ifcount];
        i->sock = ccnl_open_ethdev(ethdev, &i->addr.eth, CCNL_ETH_TYPE);
        i->mtu = 1500;
        i->reflect = 1;
        i->fwdalli = 1;
        if (i->sock >= 0) {
            relay->ifcount++;
            DEBUGMSG(INFO, "ETH interface (%s %s) configured\n",
                     ethdev, ccnl_addr2ascii(&i->addr));
            if (relay->defaultInterfaceScheduler)
                i->sched = relay->defaultInterfaceScheduler(relay,
                                                        ccnl_interface_CTS);
        } else
            DEBUGMSG(WARNING, "sorry, could not open eth device\n");
    }
#endif // USE_ETHERNET

    if (udpport > 0) {
        i = &relay->ifs[relay->ifcount];
        i->sock = ccnl_open_udpdev(udpport, &i->addr.ip4);
//      i->frag = CCNL_DGRAM_FRAG_NONE;

#ifdef USE_SUITE_CCNB
        if (suite == CCNL_SUITE_CCNB)
            i->mtu = CCN_DEFAULT_MTU;
#endif
#ifdef USE_SUITE_NDNTLV
        if (suite == CCNL_SUITE_NDNTLV)
            i->mtu = NDN_DEFAULT_MTU;
#endif
        i->fwdalli = 1;
        if (i->sock >= 0) {
            relay->ifcount++;
            DEBUGMSG(INFO, "UDP interface (%s) configured\n",
                     ccnl_addr2ascii(&i->addr));
            if (relay->defaultInterfaceScheduler)
                i->sched = relay->defaultInterfaceScheduler(relay,
                                                        ccnl_interface_CTS);
        } else
            DEBUGMSG(WARNING, "sorry, could not open udp device (port %d)\n",
                udpport);
    }

#ifdef USE_HTTP_STATUS
    if (httpport > 0) {
        relay->http = ccnl_http_new(relay, httpport);
    }
#endif // USE_HTTP_STATUS

#ifdef USE_NFN
    relay->km = ccnl_calloc(1, sizeof(struct ccnl_krivine_s));
    relay->km->configid = -1;
#endif

#ifdef USE_UNIXSOCKET
    if (uxpath) {
        i = &relay->ifs[relay->ifcount];
        i->sock = ccnl_open_unixpath(uxpath, &i->addr.ux);
        i->mtu = 4096;
        if (i->sock >= 0) {
            relay->ifcount++;
            DEBUGMSG(INFO, "UNIX interface (%s) configured\n",
                     ccnl_addr2ascii(&i->addr));
            if (relay->defaultInterfaceScheduler)
                i->sched = relay->defaultInterfaceScheduler(relay,
                                                        ccnl_interface_CTS);
        } else
            DEBUGMSG(WARNING, "sorry, could not open unix datagram device\n");
    }
#ifdef USE_SIGNATURES
    if(crypto_face_path) {
        char h[1024];
        //sending interface + face
        i = &relay->ifs[relay->ifcount];
        i->sock = ccnl_open_unixpath(crypto_face_path, &i->addr.ux);
        i->mtu = 4096;
        if (i->sock >= 0) {
            relay->ifcount++;
            DEBUGMSG(INFO, "new UNIX interface (%s) configured\n",
                     ccnl_addr2ascii(&i->addr));
            if (relay->defaultInterfaceScheduler)
                i->sched = relay->defaultInterfaceScheduler(relay,
                                                        ccnl_interface_CTS);
            ccnl_crypto_create_ccnl_crypto_face(relay, crypto_face_path);       
            relay->crypto_path = crypto_face_path;
        } else
            DEBUGMSG(WARNING, "sorry, could not open unix datagram device\n");
        
        //receiving interface
        memset(h,0,sizeof(h));
        sprintf(h,"%s-2",crypto_face_path);
        i = &relay->ifs[relay->ifcount];
        i->sock = ccnl_open_unixpath(h, &i->addr.ux);
        i->mtu = 4096;
        if (i->sock >= 0) {
            relay->ifcount++;
            DEBUGMSG(INFO, "new UNIX interface (%s) configured\n",
                     ccnl_addr2ascii(&i->addr));
            if (relay->defaultInterfaceScheduler)
                i->sched = relay->defaultInterfaceScheduler(relay,
                                                        ccnl_interface_CTS);
            //create_ccnl_crypto_face(relay, crypto_face_path);       
        } else
            DEBUGMSG(WARNING, "sorry, could not open unix datagram device\n");
    }
#endif //USE_SIGNATURES
#endif // USE_UNIXSOCKET

    ccnl_set_timer(1000000, ccnl_ageing, relay, 0);
}

// ----------------------------------------------------------------------

int
ccnl_io_loop(struct ccnl_relay_s *ccnl)
{
    int i, len, maxfd = -1, rc;
    fd_set readfs, writefs;
    unsigned char buf[CCNL_MAX_PACKET_SIZE];
    
    if (ccnl->ifcount == 0) {
        DEBUGMSG(ERROR, "no socket to work with, not good, quitting\n");
        exit(EXIT_FAILURE);
    }
    for (i = 0; i < ccnl->ifcount; i++)
        if (ccnl->ifs[i].sock > maxfd)
            maxfd = ccnl->ifs[i].sock;
    maxfd++;

    DEBUGMSG(INFO, "starting main event and IO loop\n");
    while (!ccnl->halt_flag) {
        struct timeval *timeout;

        FD_ZERO(&readfs);
        FD_ZERO(&writefs);

#ifdef USE_HTTP_STATUS
        ccnl_http_anteselect(ccnl, ccnl->http, &readfs, &writefs, &maxfd);
#endif
        for (i = 0; i < ccnl->ifcount; i++) {
            FD_SET(ccnl->ifs[i].sock, &readfs);
            if (ccnl->ifs[i].qlen > 0)
                FD_SET(ccnl->ifs[i].sock, &writefs);
        }

        timeout = ccnl_run_events();
        rc = select(maxfd, &readfs, &writefs, NULL, timeout);

        if (rc < 0) {
            perror("select(): ");
            exit(EXIT_FAILURE);
        }

#ifdef USE_HTTP_STATUS
        ccnl_http_postselect(ccnl, ccnl->http, &readfs, &writefs);
#endif
        for (i = 0; i < ccnl->ifcount; i++) {
            if (FD_ISSET(ccnl->ifs[i].sock, &readfs)) {
                sockunion src_addr;
                socklen_t addrlen = sizeof(sockunion);
                if ((len = recvfrom(ccnl->ifs[i].sock, buf, sizeof(buf), 0,
                                (struct sockaddr*) &src_addr, &addrlen)) > 0) {
                    
                    if (src_addr.sa.sa_family == AF_INET) {
                        ccnl_core_RX(ccnl, i, buf, len,
                                     &src_addr.sa, sizeof(src_addr.ip4));
                    }
#ifdef USE_ETHERNET
                    else if (src_addr.sa.sa_family == AF_PACKET) {
                        if (len > 14)
                            ccnl_core_RX(ccnl, i, buf+14, len-14,
                                         &src_addr.sa, sizeof(src_addr.eth));
                    }
#endif
#ifdef USE_UNIXSOCKET
                    else if (src_addr.sa.sa_family == AF_UNIX) {
                        ccnl_core_RX(ccnl, i, buf, len,
                                     &src_addr.sa, sizeof(src_addr.ux));
                    }
#endif
                }
            }

            if (FD_ISSET(ccnl->ifs[i].sock, &writefs)) {
              ccnl_interface_CTS(ccnl, ccnl->ifs + i);
            }
        }
    }

    return 0;
}


void
ccnl_populate_cache(struct ccnl_relay_s *ccnl, char *path)
{
    DIR *dir;
    struct dirent *de;

    dir = opendir(path);
    if (!dir) {
        DEBUGMSG(ERROR, "could not open directory %s\n", path);
        return;
    }

    DEBUGMSG(INFO, "populating cache from directory %s\n", path);

    while ((de = readdir(dir))) {
        char fname[1000];
        struct stat s;
        struct ccnl_buf_s *buf = 0, *nonce=0, *ppkd=0, *pkt = 0;
        struct ccnl_prefix_s *prefix = 0;
        struct ccnl_content_s *c = 0;
        unsigned char *content, *data;
        int fd, contlen, datalen, typ, len, suite, skip;

        if (de->d_name[0] == '.')
            continue;

        strcpy(fname, path);
        strcat(fname, "/");
        strcat(fname, de->d_name);

        if (stat(fname, &s)) { 
            perror("stat"); 
            continue; 
        }

        DEBUGMSG(INFO, "loading file %s, %d bytes\n", de->d_name,
                 (int) s.st_size);

        fd = open(fname, O_RDONLY);
        if (!fd) { 
            perror("open"); 
            continue; 
        }

        buf = (struct ccnl_buf_s *) ccnl_malloc(sizeof(*buf) + s.st_size);
        if (buf)
            datalen = read(fd, buf->data, s.st_size);
        else
            datalen = -1;
        close(fd);

        if (!buf || datalen != s.st_size || datalen < 2) {
            DEBUGMSG(WARNING, "size mismatch for file %s, %d/%d bytes\n",
                     de->d_name, datalen, (int) s.st_size);
            continue;
        }
        buf->datalen = datalen;
        suite = ccnl_pkt2suite(buf->data, datalen, &skip);

        switch (suite) {
#ifdef USE_SUITE_CCNB
        case CCNL_SUITE_CCNB:
            if (buf->data[0+skip] != 0x04 || buf->data[1+skip] != 0x82)
                goto notacontent;
            data = buf->data + 2 + skip;
            datalen -= 2 + skip;
            pkt = ccnl_ccnb_extract(&data, &datalen, 0, 0, 0, 0,
                                    &prefix, &nonce, &ppkd, &content, &contlen);
            break;
#endif
#ifdef USE_SUITE_CCNTLV
        case CCNL_SUITE_CCNTLV:
            // ccntlv_extract expects the data pointer 
            // at the start of the message. Move past the fixed header.
            data = buf->data + skip;
            datalen -= 8 + skip;
            data += 8;
            pkt = ccnl_ccntlv_extract(8, // hdrlen
                                      &data, &datalen, &prefix, 0, 0, 0,
                                      &content, &contlen);
            break;
#endif 
#ifdef USE_SUITE_IOTTLV
        case CCNL_SUITE_IOTTLV:
            data = buf->data + skip;
            datalen -= skip;
            if (ccnl_iottlv_dehead(&data, &datalen, &typ, &len) ||
                                                       typ != IOT_TLV_Reply)
                goto notacontent;
            pkt = ccnl_iottlv_extract(buf->data + skip, &data, &datalen,
                                      &prefix, NULL, &content, &contlen);
            break;
#endif
#ifdef USE_SUITE_NDNTLV
        case CCNL_SUITE_NDNTLV:
            data = buf->data + skip;
            datalen -= skip;
            if (ccnl_ndntlv_dehead(&data, &datalen, &typ, &len) ||
                                                       typ != NDN_TLV_Data)
                goto notacontent;
            pkt = ccnl_ndntlv_extract(data - buf->data, &data, &datalen,
                                      0, 0, 0, 0, NULL, &prefix, NULL,
                                      &nonce, &ppkd, &content, &contlen);
            break;
#endif
        default:
            DEBUGMSG(WARNING, "unknown packet format (%s)\n", de->d_name);
            goto Done;
        }
        if (!pkt) {
            DEBUGMSG(WARNING, "parsing error (%s)\n", de->d_name);
            goto Done;
        }
        if (!prefix) {
            DEBUGMSG(WARNING, "missing prefix (%s)\n", de->d_name);
            goto Done;
        }

        c = ccnl_content_new(ccnl, suite, &pkt, &prefix,
                             &ppkd, content, contlen);
        if (!c) {
            DEBUGMSG(WARNING, "could not create content (%s)\n", de->d_name);
            goto Done;
        }
        ccnl_content_add2cache(ccnl, c);
        c->flags |= CCNL_CONTENT_FLAGS_STATIC;
Done:
        free_prefix(prefix);
        ccnl_free(buf);
        ccnl_free(pkt);
        ccnl_free(nonce);
        ccnl_free(ppkd);
        continue;
notacontent:
        DEBUGMSG(WARNING, "not a content object (%s)\n", de->d_name);
        ccnl_free(buf);
    }

    closedir(dir);
}

// ----------------------------------------------------------------------

int
main(int argc, char **argv)
{
    int opt, max_cache_entries = -1, udpport = -1, httpport = -1;
    char *datadir = NULL, *ethdev = NULL, *crypto_sock_path = NULL;
#if defined(USE_NFN_DEFAULT_ROUTE)
    char* def_route;
#endif
#ifdef USE_UNIXSOCKET
    char *uxpath = CCNL_DEFAULT_UNIXSOCKNAME;
#else
    char *uxpath = NULL;
#endif

    time(&theRelay.startup_time);
    srandom(time(NULL));

    while ((opt = getopt(argc, argv, "hc:d:e:g:i:s:t:u:v:x:p:n:")) != -1) {
        switch (opt) {
#if defined(USE_NFN_DEFAULT_ROUTE)
	case 'n':
	    def_route = optarg; 
	    break;
#endif
        case 'c':
            max_cache_entries = atoi(optarg);
            break;
        case 'd':
            datadir = optarg;
            break;
        case 'e':
            ethdev = optarg;
            break;
        case 'g':
            inter_pkt_interval = atoi(optarg);
            break;
        case 'i':
            inter_ccn_interval = atoi(optarg);
            break;
        case 's':
            suite = ccnl_str2suite(optarg);
            if (suite < 0 || suite >= CCNL_SUITE_LAST)
                goto usage;
            break;
        case 't':
            httpport = atoi(optarg);
            break;
        case 'u':
            udpport = atoi(optarg);
            break;
        case 'v':
#ifdef USE_LOGGING
            if (isdigit(optarg[0]))
                debug_level = atoi(optarg);
            else
                debug_level = ccnl_debug_str2level(optarg);
#endif
            break;
        case 'x':
            uxpath = optarg;
            break;
        case 'p':
            crypto_sock_path = optarg;
            break;
        case 'h':
        default:
usage:
            fprintf(stderr,
                    "usage: %s [options]\n"
#if !defined(USE_NFN) && defined(USE_NFN_DEFAULT_ROUTE)
		    "  -n default nfn route ip/port: x.x.x.x/xxxx"
#endif
                    "  -c MAX_CONTENT_ENTRIES\n"
                    "  -d databasedir\n"
                    "  -e ethdev\n"
                    "  -g MIN_INTER_PACKET_INTERVAL\n"
                    "  -h\n"
                    "  -i MIN_INTER_CCNMSG_INTERVAL\n"
                    "  -p crypto_face_ux_socket\n"
                    "  -s SUITE (ccnb, ccnx2014, iot2014, ndn2013)\n"
                    "  -t tcpport (for HTML status page)\n"
                    "  -u udpport\n"

#ifdef USE_LOGGING
                    "  -v DEBUG_LEVEL (fatal, error, warning, info, debug, trace, verbose)\n"
#endif
#ifdef USE_UNIXSOCKET
                    "  -x unixpath\n"
#endif
                    , argv[0]);
            exit(EXIT_FAILURE);
        }
    }

#define setPorts(PORT)  if (udpport < 0) udpport = PORT; \
                        if (httpport < 0) httpport = PORT

    switch (suite) {
#ifdef USE_SUITE_CCNB
    case CCNL_SUITE_CCNB:
        setPorts(CCN_UDP_PORT);
        break;
#endif
#ifdef USE_SUITE_CCNTLV
    case CCNL_SUITE_CCNTLV:
        setPorts(CCN_UDP_PORT);
        break;
#endif
#ifdef USE_SUITE_NDNTLV
    case CCNL_SUITE_NDNTLV:
#endif
    default:
        setPorts(NDN_UDP_PORT);
        break;
    }

    ccnl_core_init();

    DEBUGMSG(INFO, "This is ccn-lite-relay, starting at %s",
             ctime(&theRelay.startup_time) + 4);
    DEBUGMSG(INFO, "  ccnl-core: %s\n", CCNL_VERSION);
    DEBUGMSG(INFO, "  compile time: %s %s\n", __DATE__, __TIME__);
    DEBUGMSG(INFO, "  compile options: %s\n", compile_string());
    DEBUGMSG(INFO, "using suite %s\n", ccnl_suite2str(suite));

    ccnl_relay_config(&theRelay, ethdev, udpport, httpport,
                      uxpath, suite, max_cache_entries, crypto_sock_path);

#if defined(USE_NFN_DEFAULT_ROUTE)
    if(def_route){
	
    	struct ccnl_if_s *i;
	char host[16];
	char port[6];
	char *ptr = strtok(def_route, "/");
	while(ptr != NULL){
	    ptr = strtok(NULL, "/");
	    break;
	}
	memset(host,'\0', sizeof(host));
	memset(port,'\0', sizeof(port));
	DEBUGMSG(VERBOSE, "ptr: %s %p; %s %p\n", ptr, ptr, def_route, def_route);
	strncpy(host, def_route, ptr-def_route);
	strcpy(port, ptr);

	DEBUGMSG(VERBOSE, "Creating default nfn route: %s/%s\n", host, port);

	i = ccnl_malloc(sizeof(*i));
        i->sock = ccnl_open_udpdev(strtol((char*)port, NULL, 0), &i->addr.ip4);
        i->mtu = CCN_DEFAULT_MTU;
        i->reflect = 0;
        i->fwdalli = 1;

        if (theRelay.defaultInterfaceScheduler){
            i->sched = theRelay.defaultInterfaceScheduler(&theRelay, ccnl_interface_CTS);
	}

        sockunion su;
        su.sa.sa_family = AF_INET;
        inet_aton((const char*)host, &su.ip4.sin_addr);
        su.ip4.sin_port = htons(strtol((const char*)port, NULL, 0));

	theRelay.nfn_default_face = ccnl_get_face_or_create(&theRelay, -1, &su.sa, sizeof(struct sockaddr_in));
    }
    else{
	theRelay.nfn_default_face = 0;    
    }
#endif
    if (datadir)
        ccnl_populate_cache(&theRelay, datadir);
    
    ccnl_io_loop(&theRelay);

    while (eventqueue)
        ccnl_rem_timer(eventqueue);
    
    ccnl_core_cleanup(&theRelay);
#ifdef USE_HTTP_STATUS
    theRelay.http = ccnl_http_cleanup(theRelay.http);
#endif
#ifdef USE_DEBUG_MALLOC
    debug_memdump();
#endif

    return 0;
}

// eof
