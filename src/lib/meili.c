#include <stdlib.h>
#include <stdio.h>

#include "meili.h"
#include "../runtime/pipeline.h"

#include "./net/meili_pkt.h"
#include "./regex/meili_regex.h"

/* pkt_trans
*   - Run a packet transformation operation specified by UCO.  
*/
void pkt_trans(struct pipeline_stage *self, int (*trans)(struct pipeline_stage *self, meili_pkt *pkt), meili_pkt *pkt){
    if(!trans){
        return;   
    }
    trans(self, pkt);
};

/* pkt_flt
*   - Filter packets with the operation specified by UCO.
*/
void pkt_flt(struct pipeline_stage *self, int (*check)(struct pipeline_stage *self, meili_pkt *pkt), meili_pkt *pkt){
    // printf("Meili api pkt_lt called\n");
    if(!check){
        return; 
    }
    int flag = check(self, pkt);

    if(flag == 1){
        /* filter the packet */
        ;
    }
}

/* flow_ext
*   - Construct flows from a stream based on UCO.
*/
void flow_ext(){};

/* flow_trans
*   - Run a flow transformation operation specified by UCO.
*/
void flow_trans(){};

/* reg_sock
*   - Register an established socket to Meili.
*/
void reg_sock(){};

/* epoll
*   - Process an event on the socket with the operation specified by UCO.
*/
void epoll(){};

/* regex
*   - The built-in Regular Expression API.   
*/
void regex(struct pipeline_stage *self, meili_pkt *pkt){
    
    int qid = self->worker_qid;
    struct pipeline *pl = (struct pipeline *)(self->pl);
    regex_stats_t temp_stats;
    pl_conf *run_conf = &(pl->conf);

	int to_send = 0;
	int ret;
    int nb_dequeued_op = 0;


    /* Prepare ops in regex_dev_search_live */
    to_send = regex_dev_search_live(run_conf, qid, pkt, &temp_stats);
    // if (ret)
    //     return ret;

    /* If to_send signal is set, push the batch( and pull at the same time to avoid full queue) */
    if (to_send) {
        regex_dev_force_batch_push(run_conf, qid, &temp_stats, &nb_dequeued_op, NULL);
    }	
	else{
		/* If batch is not full, pull finished ops */
		regex_dev_force_batch_pull(run_conf, qid, &temp_stats, &nb_dequeued_op, NULL);	
	}
	return;        
};

/* AES
*   - The built-in AES Encryption API.
*/
void AES(){};

/* compression
*   - The built-in Compression API.
*/
void compression(){};

int register_meili_apis(){
    printf("register meili apis\n");
    Meili.pkt_trans     = pkt_trans;
    Meili.pkt_flt       = pkt_flt;
    Meili.flow_ext      = flow_ext;
    Meili.flow_trans    = flow_trans; 
    Meili.reg_sock      = reg_sock;
    Meili.epoll         = epoll;
    Meili.regex         = regex;
    Meili.AES           = AES;
    Meili.compression   = compression;
    return 0;
}