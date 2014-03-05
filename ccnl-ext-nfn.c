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
 * 2014-02-06 <christopher.scherb@unibas.ch>created 
 */
#ifndef CCNL_EXT_NFN_C
#define CCNL_EXT_NFN_C


#include "ccnl-core.h"

#include "krivine.c"

int 
ccnl_nfn_resume_comp(struct ccnl_relay_s *ccnl, char *expression, struct ccnl_interest_s *i){
    
    DEBUGMSG(99, "Computation content received: %s\n", expression);
    if(!strncmp(expression, "CFG|", 4)){
        Krivine_reduction(ccnl, expression, 0);
    }
    else{
         Krivine_reduction(ccnl, i->comp_config, 1); 
    }
}

int 
ccnl_nfn(struct ccnl_relay_s *ccnl, struct ccnl_buf_s *orig,
	  struct ccnl_prefix_s *prefix, struct ccnl_face_s *from, int compute)
{
    DEBUGMSG(49, "ccnl_nfn(%p, %p, %p, %p)\n", ccnl, orig, prefix, from);
    DEBUGMSG(99, "NFN-engine\n"); 
    if(!memcmp(prefix->comp[prefix->compcnt-2], "THUNK", 5))
    {
        DEBUGMSG(99, "  Thunk-request, currently not implementd\n"); 
    }
    char str[1000];
    
    int i, len = 0;
    for(i = 0; i < prefix->compcnt-1; ++i){
        //DEBUGMSG(99, "%s\n", prefix->comp[i]);
        len += sprintf(str + len, " %s", prefix->comp[i]);
    }
    //search for result here... if found return...
    char *res = Krivine_reduction(ccnl, str, compute);
    //stores result if computed
    if(res){
        DEBUGMSG(2,"Computation finshed: %s\n", res);
        struct ccnl_content_s *c = add_computation_to_cache(ccnl, prefix, res, strlen(res));
            
        c->flags = CCNL_CONTENT_FLAGS_STATIC;
        ccnl_content_add2cache(ccnl, c);
        ccnl_content_serve_pending(ccnl,c);
    }
    return 0;
}

#endif