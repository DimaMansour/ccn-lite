/*
 * @f util/ccn-lite-mkC.c
 * @b CLI mkContent, write to Stdout
 *
 * Copyright (C) 2013, Christian Tschudin, University of Basel
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
 * 2013-07-06  created
 */

#define USE_SUITE_CCNB
#define USE_SUITE_CCNTLV
#define USE_SUITE_IOTTLV
#define USE_SUITE_NDNTLV
#define USE_SIGNATURES

#include "ccnl-common.c"
#include "ccnl-crypto.c"


// ----------------------------------------------------------------------

char *private_key_path; 
char *witness;

// ----------------------------------------------------------------------

int
main(int argc, char *argv[])
{

    // char *private_key_path; 
    // char *witness;
    unsigned char body[64*1024];
    unsigned char out[65*1024];
    char *publisher = 0;
    char *infname = 0, *outfname = 0;
    unsigned int chunknum = UINT_MAX, lastchunknum = UINT_MAX;
    int f, len, opt, plen, offs = 0;
    struct ccnl_prefix_s *name;
    int suite = CCNL_SUITE_DEFAULT;
    private_key_path = 0;
    witness = 0;

    while ((opt = getopt(argc, argv, "hi:k:l:n:o:p:s:v:w:")) != -1) {
        switch (opt) {
        case 'i':
            infname = optarg;
            break;
        case 'k':
            private_key_path = optarg;
            break;
        case 'o':
            outfname = optarg;
            break;
        case 'p':
            publisher = optarg;
            plen = unescape_component(publisher);
            if (plen != 32) {
                DEBUGMSG(ERROR,
                  "publisher key digest has wrong length (%d instead of 32)\n",
                  plen);
                exit(-1);
            }
            break;
        case 'l':
            lastchunknum = atoi(optarg);
            break;
        case 'n':
            chunknum = atoi(optarg);
            break;
        case 'v':
#ifdef USE_LOGGING
            if (isdigit(optarg[0]))
                debug_level = atoi(optarg);
            else
                debug_level = ccnl_debug_str2level(optarg);
#endif
            break;

        case 'w':
            witness = optarg;
            break;
        case 's':
            suite = ccnl_str2suite(optarg);
            if (suite < 0 || suite >= CCNL_SUITE_LAST) {
                DEBUGMSG(ERROR, "Unsupported suite %d\n", suite);
                goto Usage;
            }
            break;
        case 'h':
        default:
Usage:
        fprintf(stderr, "usage: %s [options] URI [NFNexpr]\n"
        "  -i FNAME    input file (instead of stdin)\n"
        "  -k FNAME    publisher private key (CCNB)\n"
        "  -l LASTCHUNKNUM number of last chunk\n"       
        "  -n CHUNKNUM chunknum\n"
        "  -o FNAME    output file (instead of stdout)\n"
        "  -p DIGEST   publisher fingerprint\n"
        "  -s SUITE    (ccnb, ccnx2014, iot2014, ndn2013)\n"
#ifdef USE_LOGGING
        "  -v DEBUG_LEVEL (fatal, error, warning, info, debug, trace, verbose)\n"
#endif
        "  -w STRING   witness\n"
        "Examples:\n"
        "%% mkC /ndn/edu/wustl/ping             (classic lookup)\n"
        "%% mkC /th/ere  \"lambda expr\"          (lambda expr, in-net)\n"
        "%% mkC \"\" \"add 1 1\"                    (lambda expr, local)\n"
        "%% mkC /rpc/site \"call 1 /test/data\"   (lambda RPC, directed)\n",
        argv[0]);
        exit(1);
        }
    }

    if (!argv[optind]) 
        goto Usage;

    if (infname) {
        f = open(infname, O_RDONLY);
        if (f < 0)
            perror("file open:");
    } else
        f = 0;
    len = read(f, body, sizeof(body));
    close(f);

    name = ccnl_URItoPrefix(argv[optind], suite, argv[optind+1],
                            chunknum == UINT_MAX ? NULL : &chunknum);

    switch (suite) {
    case CCNL_SUITE_CCNB:
        len = ccnl_ccnb_fillContent(name, body, len, NULL, out);
        break;
    case CCNL_SUITE_CCNTLV:
        offs = CCNL_MAX_PACKET_SIZE;
        len = ccnl_ccntlv_prependContentWithHdr(name, body, len, 
            lastchunknum == UINT_MAX ? NULL : &lastchunknum, 
            &offs, 
            NULL, // Int *contentpos
            out);
        break;
    case CCNL_SUITE_IOTTLV:
        offs = CCNL_MAX_PACKET_SIZE;
        if (ccnl_iottlv_prependReply(name, body, len, &offs, NULL,
                   lastchunknum == UINT_MAX ? NULL : &lastchunknum, out) < 0
              || ccnl_switch_prependCoding(CCNL_ENC_IOT2014, &offs, out) < 0)
            return -1;
        len = CCNL_MAX_PACKET_SIZE - offs;
        break;
    case CCNL_SUITE_NDNTLV:
        offs = CCNL_MAX_PACKET_SIZE;
        len = ccnl_ndntlv_prependContent(name, body, len, &offs,
                                         NULL, 
                                         lastchunknum == UINT_MAX ? NULL : &lastchunknum, 
                                         out);
        break;
    default:
        break;
    }

    if (outfname) {
        f = creat(outfname, 0666);
        if (f < 0)
            perror("file open:");
    } else
        f = 1;
    write(f, out + offs, len);
    close(f);

    return 0;
}

// eof
