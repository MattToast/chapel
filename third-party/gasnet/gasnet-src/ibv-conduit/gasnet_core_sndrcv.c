/*   $Source: bitbucket.org:berkeleylab/gasnet.git/ibv-conduit/gasnet_core_sndrcv.c $
 * Description: GASNet ibv conduit implementation, transport send/receive logic
 * Copyright 2003, LBNL
 * Terms of use are as specified in license.txt
 */

#include <gasnet_internal.h>
#include <gasnet_am.h>
#include <gasnet_ibv.h>

#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>

/* ------------------------------------------------------------------------------------ *
 *  Configuration                                                                       *
 * ------------------------------------------------------------------------------------ */

/* If running w/ threads (locks) we want to coalesce calls to
     gasnetc_lifo_push(&gasnetc_bbuf_freelist,*)
   and
     firehose_release().
   However, when no threads (no locks) are present, we don't
   want to pay the overhead for coalescing.
*/
#if GASNETC_ANY_PAR
  #define GASNETC_SND_REAP_COLLECT 1
#else
  #define GASNETC_SND_REAP_COLLECT 0
#endif

/* ------------------------------------------------------------------------------------ *
 *  Global variables                                                                    *
 * ------------------------------------------------------------------------------------ */
size_t					gasnetc_fh_align;
size_t					gasnetc_fh_align_mask;
size_t                                  gasnetc_inline_limit;
size_t                   		gasnetc_nonbulk_bounce_limit;
size_t					gasnetc_packedlong_limit; // TODO-EX: adjust w/ nargs?
size_t                                  gasnetc_put_stripe_sz, gasnetc_put_stripe_split;
size_t                                  gasnetc_get_stripe_sz, gasnetc_get_stripe_split;
#if !GASNETC_PIN_SEGMENT
  size_t				gasnetc_putinmove_limit;
#endif
int					gasnetc_use_rcv_thread = GASNETC_USE_RCV_THREAD;
int					gasnetc_use_snd_thread = GASNETC_USE_SND_THREAD;
#if GASNETC_FH_OPTIONAL
  int					gasnetc_use_firehose = 1;
#endif
#if GASNETC_IBV_SRQ
  int					gasnetc_use_srq = 1;
#endif
#if GASNETC_IBV_XRC
  int					gasnetc_use_xrc = 1;
#endif
#if GASNETC_IBV_ODP
  int					gasnetc_use_odp = 1;
#endif
#if GASNETC_HAVE_FENCED_PUTS
  int                                   gasnetc_use_fenced_puts = 0;
#endif
int					gasnetc_am_credits_slack;
int					gasnetc_am_credits_slack_orig;
int					gasnetc_alloc_qps;
int					gasnetc_num_qps;
#if GASNETC_USE_RCV_THREAD && GASNETC_SERIALIZE_POLL_CQ
int                                     gasnetc_rcv_thread_poll_serialize = -1;
int                                     gasnetc_rcv_thread_poll_exclusive = -1;
#endif
#if GASNETC_USE_SND_THREAD && GASNETC_SERIALIZE_POLL_CQ
int                                     gasnetc_snd_thread_poll_serialize = -1;
int                                     gasnetc_snd_thread_poll_exclusive = -1;
#endif

// TODO: multi-client will need this to be per-client
#define GASNETC_MAX_PROGRESS_THREADS (GASNET_IBV_MAX_HCAS * (GASNETC_USE_RCV_THREAD + GASNETC_USE_SND_THREAD))
static unsigned int gasnetc_progress_thread_count;
static gex_ProgressThreadInfo_t gasnetc_progress_thread_info[GASNETC_MAX_PROGRESS_THREADS+1]; // +1 avoid potential errors on 0

#if GASNETC_PIN_SEGMENT
  // Rkeys for non-primordial remote EPs
  // One dense array per ep_index, allocated lazily
  // TODO: more scalable storage (bug 4088)
  uint32_t *gasnetc_np_rkeys[GASNET_MAXEPS] = {NULL, };
#endif

/* ------------------------------------------------------------------------------------ *
 *  File-scoped types                                                                   *
 * ------------------------------------------------------------------------------------ */

/* Per-thread data
 * Unlike gasneti_threaddata_t, this is associated w/ conduit-internal threads as well.
 */
typedef struct {
  /* Thread-local list of sreq's. */
  gasnetc_sreq_t	*sreqs;
  
  /* Nothing else yet, but lockfree algorithms for (at least) x84_64 will also need
   * some thread-local data if they are ever implemented. */
} gasnetc_per_thread_t;

/* ------------------------------------------------------------------------------------ *
 *  File-scoped variables
 * ------------------------------------------------------------------------------------ */

#if GASNETC_HAVE_FENCED_PUTS
static int gasnetc_op_needs_fence_mask;
#endif

#if !GASNETC_PIN_SEGMENT
  static size_t                         gasnetc_putinmove_limit_adjusted = 0;
#endif

static gasnetc_lifo_head_t		gasnetc_bbuf_freelist = GASNETC_LIFO_INITIALIZER;

static gasnetc_sema_t			*gasnetc_cq_semas = NULL;

/* Shared between gasnetc_sndrcv_{limits,init}() */
static int gasnetc_op_oust_per_qp;
static int gasnetc_am_rqst_per_qp;
static int gasnetc_rbuf_spares;

/* ------------------------------------------------------------------------------------ *
 *  File-scoped completion callbacks
 * ------------------------------------------------------------------------------------ */

// EOP completion callbacks
extern void gasnetc_cb_eop_alc(gasnetc_atomic_val_t *p) {
  gasnete_eop_t *eop = gasneti_container_of(p, gasnete_eop_t, initiated_alc);
  gasnete_eop_check(eop);
  (void) gasnetc_complete_eop(eop, gasnetc_comptype_eop_alc);
}
extern void gasnetc_cb_eop_put(gasnetc_atomic_val_t *p) {
  gasnete_eop_t *eop = gasneti_container_of(p, gasnete_eop_t, initiated_cnt);
  gasnete_eop_check(eop);
  (void) gasnetc_complete_eop(eop, gasnetc_comptype_eop_put);
}
extern void gasnetc_cb_eop_get(gasnetc_atomic_val_t *p) {
  gasnete_eop_t *eop = gasneti_container_of(p, gasnete_eop_t, initiated_cnt);
  gasnete_eop_check(eop);
  (void) gasnetc_complete_eop(eop, gasnetc_comptype_eop_get);
}

// NAR (nbi-accessregion) completion callbacks
extern void gasnetc_cb_nar_alc(gasnetc_atomic_val_t *p) {
  gasnete_iop_t *iop = gasneti_container_of(p, gasnete_iop_t, initiated_alc_cnt);
  gasnete_iop_check(iop);
  GASNETE_IOP_CNT_FINISH_REG(iop, alc, 1, GASNETI_ATOMIC_NONE);
}
extern void gasnetc_cb_nar_put(gasnetc_atomic_val_t *p) {
  gasnete_iop_t *iop = gasneti_container_of(p, gasnete_iop_t, initiated_put_cnt);
  gasnete_iop_check(iop);
  GASNETE_IOP_CNT_FINISH_REG(iop, put, 1, GASNETI_ATOMIC_NONE);
}
extern void gasnetc_cb_nar_get(gasnetc_atomic_val_t *p) {
  gasnete_iop_t *iop = gasneti_container_of(p, gasnete_iop_t, initiated_get_cnt);
  gasnete_iop_check(iop);
  GASNETE_IOP_CNT_FINISH_REG(iop, get, 1, GASNETI_ATOMIC_REL);
}

// IOP (non accessregion) completion callbacks
extern void gasnetc_cb_iop_alc(gasnetc_atomic_val_t *p) {
  gasnete_iop_t *iop = gasneti_container_of(p, gasnete_iop_t, initiated_alc_cnt);
  gasnete_iop_check(iop);
  GASNETE_IOP_CNT_FINISH_INT(iop, alc, 1, GASNETI_ATOMIC_NONE);
}
extern void gasnetc_cb_iop_put(gasnetc_atomic_val_t *p) {
  gasnete_iop_t *iop = gasneti_container_of(p, gasnete_iop_t, initiated_put_cnt);
  gasnete_iop_check(iop);
  GASNETE_IOP_CNT_FINISH_INT(iop, put, 1, GASNETI_ATOMIC_NONE);
}
extern void gasnetc_cb_iop_get(gasnetc_atomic_val_t *p) {
  gasnete_iop_t *iop = gasneti_container_of(p, gasnete_iop_t, initiated_get_cnt);
  gasnete_iop_check(iop);
  GASNETE_IOP_CNT_FINISH_INT(iop, get, 1, GASNETI_ATOMIC_REL);
}

// gasnetc_counter_t completion callbacks
extern void gasnetc_cb_counter(gasnetc_atomic_val_t *cnt) {
  gasnetc_counter_t *counter = gasneti_container_of(cnt, gasnetc_counter_t, initiated);
  gasnetc_atomic_increment(&counter->completed, 0);
}
extern void gasnetc_cb_counter_rel(gasnetc_atomic_val_t *cnt) {
  gasnetc_counter_t *counter = gasneti_container_of(cnt, gasnetc_counter_t, initiated);
  gasnetc_atomic_increment(&counter->completed, GASNETI_ATOMIC_REL);
}


#if GASNETC_HAVE_FENCED_PUTS
/* ------------------------------------------------------------------------------------ *
 * AuxSeg space for dummy Atomic ops used to fence multi-rail Puts
 * TODO: this use of auxseg is yet another an O(ranks) table we must seek to eliminate
 * ------------------------------------------------------------------------------------ */
static gasnet_seginfo_t *gasnetc_fence_auxseg = NULL;
gasneti_auxseg_request_t gasnetc_fence_auxseg_alloc(gasnet_seginfo_t *auxseg_info) {
  gasneti_auxseg_request_t retval;

  if (!gasnetc_use_fenced_puts) {
    // No allocation if fenced puts are not enabled
    retval.minsz = retval.optimalsz = 0;
  } else {
    // One cache line each for use as initiator and target
    // TODO: distinct cache lines for each HCA (via `cep` argument to macros, below)
    retval.minsz = retval.optimalsz = 2 * GASNETI_CACHE_LINE_BYTES;
  }

  if (gasnetc_use_fenced_puts && auxseg_info) { /* auxseg granted */
    gasneti_assert(!gasnetc_fence_auxseg);
    gasnetc_fence_auxseg = gasneti_malloc(gasneti_nodes*sizeof(gasnet_seginfo_t));
    GASNETI_MEMCPY(gasnetc_fence_auxseg, auxseg_info, gasneti_nodes*sizeof(gasnet_seginfo_t));
  }

  return retval;
}
#define GASNETC_FENCE_ADDR_(jobrank,n) \
            ((uintptr_t)gasnetc_fence_auxseg[(jobrank)].addr + (n << GASNETI_CACHE_LINE_SHIFT))
#define GASNETC_FENCE_REM_ADDR(cep) GASNETC_FENCE_ADDR_(gasnetc_epid2node(cep->epid), 0)
#define GASNETC_FENCE_LOC_ADDR(cep) GASNETC_FENCE_ADDR_(gasneti_mynode, 1)
#endif

/* ------------------------------------------------------------------------------------ *
 *  File-scoped functions and macros                                                    *
 * ------------------------------------------------------------------------------------ */

#if GASNETI_MAX_THREADS > 1
  /* Note: first word of thread data is reserved for core */
  #define gasnetc_my_perthread() (gasnetc_per_thread_t *)(*(void**)(GASNETI_MYTHREAD))
  /* Since we use system-level AMs between gasnetc_sndrcv_init and gasnete_init(),
     the TLD will get initialized at first use in the AM Request path anyway. */
  #define gasnetc_per_thread_setup() ((void)0)
#else
  static gasnetc_per_thread_t gasnetc_per_thread;
  #define gasnetc_my_perthread() (&gasnetc_per_thread)
  #define gasnetc_per_thread_setup() gasnetc_per_thread_init(&gasnetc_per_thread)
#endif

static void gasnetc_free_aligned(void *ptr) {
  gasneti_free_aligned(ptr);
}

#define GASNETC_SREQS_GROWTHCNT 32 /* sreq list always grown by this size increment */

static void gasnetc_free_sreqs(void *_ptr) {
  gasnetc_sreq_t *ptr = (gasnetc_sreq_t *)_ptr;
  int i;
  /* sreqs for AM sends may still be live on the adapter and thus unsafe to free
   */
  for (i = 0; i < GASNETC_SREQS_GROWTHCNT; i++) {
    while (ptr->opcode != GASNETC_OP_FREE) {
      gasnetc_snd_reap(1);
      if (ptr->opcode != GASNETC_OP_FREE) gasneti_sched_yield();
    }
    ptr = (gasnetc_sreq_t *)GASNETI_ALIGNUP(ptr+1, GASNETI_CACHE_LINE_BYTES);
  }
  gasneti_free_aligned(_ptr);
}

GASNETI_INLINE(gasnetc_alloc_sreqs)
void gasnetc_alloc_sreqs(gasnetc_sreq_t **head_p, gasnetc_sreq_t **tail_p GASNETI_THREAD_FARG)
{
  const int count = GASNETC_SREQS_GROWTHCNT;
  size_t bytes = GASNETI_ALIGNUP(sizeof(gasnetc_sreq_t), GASNETI_CACHE_LINE_BYTES);
  gasnetc_sreq_t *ptr = gasneti_malloc_aligned(GASNETI_CACHE_LINE_BYTES, count * bytes);
  int i;
  gasneti_leak_aligned(ptr);
  gasnete_register_threadcleanup(gasnetc_free_sreqs, ptr);
  *head_p = ptr;
  for (i = 1; i < count; ++i, ptr = ptr->next) {
    ptr->next = (gasnetc_sreq_t *)((uintptr_t)ptr + bytes);
    ptr->opcode = GASNETC_OP_FREE;
  }
  ptr->opcode = GASNETC_OP_FREE;
  *tail_p = ptr;
  GASNETC_STAT_EVENT_VAL(ALLOC_SREQ, count);
}

static
void gasnetc_per_thread_init(gasnetc_per_thread_t *td)
{
  GASNET_BEGIN_FUNCTION(); // OK - not a critical-path
  gasnetc_sreq_t *tail;
  gasnetc_alloc_sreqs(&td->sreqs, &tail GASNETI_THREAD_PASS);
  tail->next = td->sreqs;
}

extern void
gasnetc_new_threaddata_callback(void **core_threadinfo) {
    gasnetc_per_thread_t *result;
#if GASNETI_MAX_THREADS > 1
    result = gasneti_malloc_aligned(GASNETI_CACHE_LINE_BYTES,
                                    GASNETI_ALIGNUP(sizeof(gasnetc_per_thread_t),
                                                    GASNETI_CACHE_LINE_BYTES));
    gasneti_leak_aligned(result);
    gasnete_register_threadcleanup(gasnetc_free_aligned, result);
#else
    result = &gasnetc_per_thread;
#endif
    gasnetc_per_thread_init(result);
    *core_threadinfo = (void*) result;
}

/* ------------------------------------------------------------------------------------ */

extern int
gasnetc_create_cq(struct ibv_context * hca_hndl, int req_size,
		  struct ibv_cq * *cq_p, int *act_size,
		  gasnetc_progress_thread_t *pthr_p)
{
  struct ibv_comp_channel * compl = NULL;
  struct ibv_cq * result;
 #if GASNETI_CONDUIT_THREADS
  if (pthr_p) {
    compl = ibv_create_comp_channel(hca_hndl);
    GASNETC_IBV_CHECK_PTR(compl, "from ibv_create_comp_channel");
  }
 #endif
  result = ibv_create_cq(hca_hndl, req_size, NULL, compl, 0);
  GASNETC_IBV_CHECK_PTR(result, "from ibv_create_cq()");
 #if GASNETI_CONDUIT_THREADS
  if (pthr_p) {
    int rc = ibv_req_notify_cq(result, 0);
    GASNETC_IBV_CHECK(rc, "while requesting cq events");
    memset(pthr_p, 0, sizeof(*pthr_p));
    pthr_p->compl = compl;
    pthr_p->cq = result;
  }
 #endif
  if_pt (result != NULL) {
    *cq_p = result;
    *act_size = result->cqe;
    return 0;
  } else {
    return 1;
  }
}


// Simple round-robin (w/ a harmless multi-thread race)
// Note use of casts to volatile ensure the compiler will not move the accesses
// (reads earlier, or writes later) so as to defeat the entire purpose of
// advancing a counter to be observed by other threads.
#if GASNETC_ANY_PAR
  #define GASNETC_WEAK_COUNTER_DECL(var,val) \
    static struct {                                     \
       char pad0[GASNETI_CACHE_LINE_BYTES];             \
       int  cntr;                                       \
       char pad1[GASNETI_CACHE_LINE_BYTES-sizeof(int)]; \
    } var = {{0},(val),{0}}
  #define GASNETC_WEAK_COUNTER_READ(var) \
    (*(volatile int *)(&(var).cntr))
  #define GASNETC_WEAK_COUNTER_WRITE(var,val) \
    do { *(volatile int *)(&(var).cntr) = (val); } while (0)
#else
  #define GASNETC_WEAK_COUNTER_DECL(var,val) \
    static int var = (val)
  #define GASNETC_WEAK_COUNTER_READ(var) \
    (*(volatile int *)&(var))
  #define GASNETC_WEAK_COUNTER_WRITE(var,val) \
    do { *(volatile int *)&(var) = (val); } while (0)
#endif

#if GASNETC_IB_MAX_HCAS > 1
  #define GASNETC_HCA_IDX(_cep)		((_cep)->hca_index)
#else
  #define GASNETC_HCA_IDX(_cep)		0
#endif
#define GASNETC_FH_RKEY(_cep, _fhptr)	((_fhptr)->client.rkey[GASNETC_HCA_IDX(_cep)])
#define GASNETC_FH_LKEY(_cep, _fhptr)	((_fhptr)->client.lkey[GASNETC_HCA_IDX(_cep)])

// TODO-EX: following functions are a hack for multi-segment keys, which
// should be replaced with more general multi-registration support later.
#if GASNETC_PIN_SEGMENT
  // idx = -1 is aux segment
  // idx = 0 is primordial segment
  // idx > 0 is non-primordial segment
  GASNETI_INLINE(gasnetc_seg_rkey)
  uint32_t gasnetc_seg_rkey(gasnetc_cep_t *cep, int idx)
  {
    if (!idx) {
      return GASNETC_SEG_RKEY(cep);
    } else if (idx < 0) {
      return cep->hca->aux_rkeys[gasnetc_epid2node(cep->epid)];
    } else {
      gasneti_assume(idx < GASNET_MAXEPS);
      gasneti_assert(gasnetc_np_rkeys[idx]);
      size_t offset = gasnetc_num_hcas * gasnetc_epid2node(cep->epid) + GASNETC_HCA_IDX(cep);
      return gasnetc_np_rkeys[idx][offset];
    }
  }
  GASNETI_INLINE(gasnetc_seg_lkey)
  uint32_t gasnetc_seg_lkey(gasnetc_EP_t ep, gasnetc_cep_t *cep, int is_aux)
  {
    gasneti_assert(ep);
    return ( ! is_aux ) ? GASNETC_SEG_LKEY(ep, cep)
                        : cep->hca->aux_reg.handle->lkey;
  }
#endif // GASNETC_PIN_SEGMENT


/* This limits the amount we ask for in a firehose_{local,remote}_pin() call,
 * to enourage a steady-state layout of firehoses that has start and end addresses
 * at multpiles of gasnetc_fh_align and length 2*gasnetc_fh_align.
 * This will look sort of like two courses of bricks.
 */
GASNETI_INLINE(gasnetc_fh_aligned_len)
size_t gasnetc_fh_aligned_len(uintptr_t start, size_t len) {
  size_t result = 2 * gasnetc_fh_align - (start & gasnetc_fh_align_mask);
  return MIN(len, result);
}

GASNETI_INLINE(gasnetc_fh_aligned_local_pin)
const firehose_request_t *gasnetc_fh_aligned_local_pin(uintptr_t start, size_t len) {
  const firehose_request_t *result = firehose_local_pin(start, gasnetc_fh_aligned_len(start, len), NULL);
  if_pf (! gasneti_valid_client_t(& result->client)) {
    // Failed memory registration (e.g. read-only memory)
    firehose_release(&result, 1);
    return NULL;
  }
  gasneti_assume(result != NULL);
  return result;
}

GASNETI_INLINE(gasnetc_fh_try_local_pin)
const firehose_request_t *gasnetc_fh_try_local_pin(uintptr_t start, size_t len) {
  const firehose_request_t *result = firehose_try_local_pin(start, len, NULL);
  if_pf (result && ! gasneti_valid_client_t(& result->client)) {
    // Failed memory registration (e.g. read-only memory)
    firehose_release(&result, 1);
    return NULL;
  }
  return result;
}

/* Post a work request to the receive queue of the given endpoint */
GASNETI_INLINE(gasnetc_rcv_post)
void gasnetc_rcv_post(gasnetc_cep_t *cep, gasnetc_rbuf_t *rbuf) {
  int vstat;

  gasneti_assert(cep);
  gasneti_assert(rbuf);

  // In the absence of SRQ, check for attempted intra-nbrhd traffic
  // With SRQ, however, initialization occurs via the first cep per HCA, which maybe in-nbrhd
  gasneti_assert(gasnetc_use_srq || !GASNETI_NBRHD_JOBRANK_IS_LOCAL(gasnetc_epid2node(cep->epid)));
  
  rbuf->cep = cep;
  rbuf->rr_sg.lkey = GASNETC_RCV_LKEY(cep);
  GASNETI_TRACE_PRINTF(D,("POST_RR rbuf=%p hca=%d lkey=0x%08x", 
			  (void *)rbuf, GASNETC_HCA_IDX(cep),
			  (unsigned int)(rbuf->rr_sg.lkey)));
  if (!gasnetc_use_srq) {
    GASNETI_TRACE_PRINTF(D,("POST_RR rbuf=%p peer=%d qp=%d",
			    (void *)rbuf, gasnetc_epid2node(cep->epid),
			    gasnetc_epid2qpi(cep->epid) - 1));
  }

  {
    struct ibv_recv_wr *bad_wr;
  #if GASNETC_IBV_SRQ
    if (cep->srq) { /* Equivalent to gasnetc_use_srq, but we need this value anyway */
      vstat = ibv_post_srq_recv(cep->srq, &rbuf->rr_desc, &bad_wr);
    } else
  #endif
    {
      vstat = ibv_post_recv(cep->qp_handle, &rbuf->rr_desc, &bad_wr);
    }
  }

  if_pt (vstat == 0) {
    /* normal return */
    return;
  } else if (GASNETC_IS_EXITING()) {
    /* disconnected by another thread */
    gasnetc_exit(0);
  } else {
    /* unexpected error */
    GASNETC_IBV_CHECK(vstat, "while posting a receive work request");
  }
}

/* GASNETI_INLINE(gasnetc_processPacket) */
void gasnetc_processPacket(gasnetc_cep_t *cep, gasnetc_rbuf_t *rbuf, uint32_t flags GASNETI_THREAD_FARG) {
  gasnetc_EP_t ep = rbuf->rr_ep;
  gasneti_assert(ep == gasnetc_ep0);
  gasnetc_buffer_t * const buf = (gasnetc_buffer_t *)(uintptr_t)(rbuf->rr_sg.addr);
  const gex_AM_Index_t handler_id = GASNETC_MSG_HANDLERID(flags);
  const gex_AM_Entry_t * const handler_entry = &ep->_amtbl[handler_id];
  const gex_AM_Fn_t handler_fn = handler_entry->gex_fnptr;
  const gasneti_category_t category = GASNETC_MSG_CATEGORY(flags);
  const int isreq = GASNETC_MSG_ISREQUEST(flags);
  int full_numargs = GASNETC_MSG_NUMARGS(flags);
  int user_numargs = full_numargs;
  const gex_Token_t token = (gex_Token_t)rbuf;
  gex_AM_Arg_t *args;

  gasneti_assert(!GASNETI_NBRHD_JOBRANK_IS_LOCAL(GASNETC_MSG_SRCIDX(flags)));
  gasneti_assert(cep != NULL);

#if GASNETI_THREADINFO_OPT
  rbuf->rbuf_threadinfo = GASNETI_MYTHREAD;
#endif
  rbuf->rbuf_needReply = isreq;
#if GASNET_DEBUG
  rbuf->rbuf_handlerRunning = 1;
#endif
  rbuf->rbuf_flags = flags;

  /* Locate arguments */
  switch (category) {
    case gasneti_Short:
      args = buf->shortmsg.args;
      break;

    case gasneti_Medium:
      args = buf->medmsg.args;
      break;

    case gasneti_Long:
      args = buf->longmsg.args;
      break;

    default: gasneti_unreachable_error(("Invalid category in gasnetc_processPacket: 0x%x",(int)category));
  }

  { /* Process any flow control info */
    int credits = 0;

    if (full_numargs == GASNETC_MAX_ARGS) {
      credits = GASNETC_HIDDEN_ARG_CREDITS(args);
      full_numargs = GASNETC_HIDDEN_ARG_FULL_NARGS(args);
      user_numargs = full_numargs - 1;

      gasneti_assert(!gasnetc_use_srq || !credits);

      GASNETI_TRACE_PRINTF(C,("RCV_AM_CREDITS credits=%d\n", credits));

      args += 1;
    }

    /* Available remotely posted (request) buffers */
    credits += (isreq ^ 1); /* Credit for self if this is a reply */
    if (credits) {
      gasnetc_sema_up_n(&cep->am_rem, credits);
    }
  }
  
  /* Ack? */
  if (!handler_id) return;

  gasneti_amtbl_check(handler_entry, user_numargs, category, isreq);

  /* Run the handler */
  switch (category) {
    case gasneti_Short:
      { 
        GASNETI_RUN_HANDLER_SHORT(isreq,handler_id,handler_fn,token,args,user_numargs);
      }
      break;

    case gasneti_Medium:
      {
        void * data = GASNETC_MSG_MED_DATA(buf, full_numargs);
        size_t nbytes = buf->medmsg.nBytes;
        GASNETI_RUN_HANDLER_MEDIUM(isreq,handler_id,handler_fn,token,args,user_numargs,data,nbytes);
      }
      break;

    case gasneti_Long:
      { 
        void * data = (void *)(buf->longmsg.destLoc);
	size_t nbytes = buf->longmsg.nBytes & 0x7fffffff;
	if (buf->longmsg.nBytes & 0x80000000) {
	  /* Must relocate the payload which is packed like a Medium. */
	  gasneti_assert(nbytes <= GASNETC_MAX_PACKEDLONG_(user_numargs));
	  GASNETI_MEMCPY(data, GASNETC_MSG_LONG_DATA(buf, full_numargs), (size_t)nbytes);
	}
        GASNETI_RUN_HANDLER_LONG(isreq,handler_id,handler_fn,token,args,user_numargs,data,(size_t)nbytes);
      }
      break;
    default: gasneti_unreachable_error(("Invalid category in gasnetc_processPacket: 0x%x",(int)category));
  }
  
#if GASNET_DEBUG
  rbuf->rbuf_handlerRunning = 0;
#endif
}

#if GASNETC_IBV_SRQ
  // SRQ needs a secondary source of buffers reserved for construction of
  // outbound AM Replies, to prevent starvation when outbound Request
  // experience backpressure.
  // However, we don't want to double resource usage via static partition.
  // Actual use is rare and just a single buffer is sufficient to avoid deadlock.
  // So *one* is all we provide, keeping the critical paths simple such that
  // freeing buffers requires only a single pointer read of extra work in the
  // common case that the spare buffer is not in-use.
  static gasnetc_atomic_ptr_t gasnetc_spare_reply_bbuf = gasnetc_atomic_ptr_init(0xcafef00d);

  // Allocates the spare outbound reply buffer, if enabled and available
  GASNETI_INLINE(gasnetc_alloc_spare_reply_bbuf) GASNETI_MALLOC
  gasnetc_buffer_t *gasnetc_alloc_spare_reply_bbuf(void) {
   gasnetc_buffer_t *result = NULL;
    if (gasnetc_use_srq && gasnetc_atomic_ptr_read(&gasnetc_spare_reply_bbuf, 0)) {
      result = (gasnetc_buffer_t *)(uintptr_t)gasnetc_atomic_ptr_swap(&gasnetc_spare_reply_bbuf, 0, 0);
      if (result) GASNETC_STAT_EVENT(SPARE_REPLY_BBUF);
    }
    return result;
  }

  // Makes the passed buffer the new spare if SRQ is enabled and there is not one already.
  // Note that gasnetc_spare_reply_bbuf will be non-NULL when !gasnetc_use_srq.
  // Returns zero if there was already a spare or !gasnetc_use_srq, non-zero otherwise.
  GASNETI_INLINE(gasnetc_maybe_restore_spare_reply_bbuf)
  int gasnetc_maybe_restore_spare_reply_bbuf(gasnetc_buffer_t *ptr) {
    gasneti_assert(ptr);
    if_pt (gasnetc_atomic_ptr_read(&gasnetc_spare_reply_bbuf, 0)) {
      return 0;
    } else {
      gasneti_assert(gasnetc_use_srq);
      return gasnetc_atomic_ptr_compare_and_swap(&gasnetc_spare_reply_bbuf, 0, (uintptr_t)ptr, 0);
    }
  }

  // For SRQ-only we need to special-case Reply buffers
  GASNETI_INLINE(gasnetc_bbuf_pop_helper)
  gasnetc_buffer_t *gasnetc_bbuf_pop_helper(const int is_reply) {
    gasnetc_buffer_t *result = gasnetc_lifo_pop(&gasnetc_bbuf_freelist);
    if (result) return result;
    return is_reply ? gasnetc_alloc_spare_reply_bbuf() : NULL;
  }
#else
  #define gasnetc_alloc_spare_reply_bbuf() gasneti_unreachable_error(("logic error"));
  #define gasnetc_maybe_restore_spare_reply_bbuf(x)  0
  #define gasnetc_bbuf_pop_helper(is_reply) gasnetc_lifo_pop(&gasnetc_bbuf_freelist)
#endif


#if GASNETC_SND_REAP_COLLECT
  #define GASNETC_COLLECT_DECLS \
    int collect_fh_num = 0; \
    void *collect_bbuf_dummy; \
    void *collect_bbuf_tail = &collect_bbuf_dummy; \
    const firehose_request_t *collect_fh_ptrs[GASNETC_SND_REAP_LIMIT * GASNETC_MAX_FH];
  #define GASNETC_COLLECT_FARGS \
    , const int collect \
    , int *collect_fh_num_p \
    , void **collect_bbuf_tail_p \
    , const firehose_request_t **collect_fh_ptrs
  #define GASNETC_COLLECT_MANY \
    , 1, &collect_fh_num, &collect_bbuf_tail, collect_fh_ptrs
  #define GASNETC_COLLECT_ONE \
    , 0, NULL, NULL, NULL
  #define GASNETC_COLLECT_BBUF_multi(_bbuf) do { \
    gasnetc_lifo_link(*collect_bbuf_tail_p, _tmp); \
    *collect_bbuf_tail_p = _tmp; \
  } while (0)
  #define GASNETC_COLLECT_FHS_multi(_sreq) do { \
    int _tmp = *collect_fh_num_p; \
    for (int i=0; i<_sreq->fh_count; ++i, ++_tmp) { \
      collect_fh_ptrs[_tmp] = _sreq->fh_ptr[i]; \
    } \
    *collect_fh_num_p = _tmp; \
  } while (0)
  #define GASNETC_COLLECT_FINALIZE() do { \
    if (collect_bbuf_tail != &collect_bbuf_dummy) { \
      gasnetc_lifo_push_many(&gasnetc_bbuf_freelist, gasnetc_lifo_next(&collect_bbuf_dummy), collect_bbuf_tail); \
    } \
    if (collect_fh_num) {          \
      gasneti_assert(collect_fh_num <= GASNETC_SND_REAP_LIMIT * GASNETC_MAX_FH); \
      firehose_release(collect_fh_ptrs, collect_fh_num); \
    } \
  } while (0)
#else
  #define GASNETC_COLLECT_DECLS // empty
  #define GASNETC_COLLECT_FARGS // empty
  #define GASNETC_COLLECT_MANY // empty
  #define GASNETC_COLLECT_ONE // empty
  #define GASNETC_COLLECT_FINALIZE() // empty
  #define GASNETC_COLLECT_BBUF_multi(_bbuf) gasneti_unreachable()
  #define GASNETC_COLLECT_FHS_multi(_sreq)  gasneti_unreachable()
#endif

#define GASNETC_COLLECT_BBUF(_collect,_bbuf) do { \
    void *_tmp = (void*)(_bbuf);                \
    gasneti_assert(_tmp != NULL);               \
    if (!gasnetc_maybe_restore_spare_reply_bbuf(_tmp)) { \
      if (_collect) {                           \
        GASNETC_COLLECT_BBUF_multi(_tmp);       \
      } else {                                  \
        gasnetc_lifo_push(&gasnetc_bbuf_freelist,_tmp); \
      }                                         \
    }                                           \
  } while(0)
#define GASNETC_COLLECT_FHS(_collect,_sreq) do {         \
    gasneti_assert_int(_sreq->fh_count ,>=, 0);          \
    gasneti_assert_int(_sreq->fh_count ,<=, GASNETC_MAX_FH); \
    if (_collect) {                                      \
      GASNETC_COLLECT_FHS_multi(_sreq);                  \
    } else {                                             \
      firehose_release(_sreq->fh_ptr, _sreq->fh_count);  \
    }                                                    \
  } while(0)

#if HAVE_IBV_WC_STATUS_STR
  #define gasnetc_ibv_wc_status_str(status) ibv_wc_status_str(status)
#else
  // Defensive.  Not expected to be reached in modern libibverbs
  #define gasnetc_ibv_wc_status_str(status) "N/A"
#endif

const char *gasnetc_opcode_str(gasnetc_sreq_opcode_t opcode)
{
  switch (opcode) {
  #define OPCASE(op) case GASNETC_OP_##op: return #op; break;
    OPCASE(FREE)
    OPCASE(AM)
    OPCASE(ATOMIC)
    OPCASE(GET_ZEROCP)
#if GASNETC_PIN_SEGMENT && GASNETC_FH_OPTIONAL
    OPCASE(GET_BOUNCE)
#endif
#if GASNETC_HAVE_FENCED_PUTS
    OPCASE(FENCE)
#endif
#if !GASNETC_PIN_SEGMENT
    OPCASE(PUT_INMOVE)
#endif
    OPCASE(LONG_ZEROCP)
    OPCASE(LONG_BOUNCE)
    OPCASE(PUT_INLINE)
    OPCASE(PUT_ZEROCP)
    OPCASE(PUT_BOUNCE)
    OPCASE(INVALID)
  #undef OPCASE
    default: {
      static char opcode_str[32];
      snprintf(opcode_str, sizeof(opcode_str), "UNKNOWN (%d)", opcode);
      return opcode_str;
    }
  }
}

static void gasnetc_dump_cqe(struct ibv_wc *comp, gasnetc_hca_t *hca, const int is_snd)
{
  static char msg_buffer[1024];  // safe due to serialization in the single caller
  int remain = sizeof(msg_buffer) - 1;
  char *msg = msg_buffer;
  msg_buffer[0] = '\0';

  // No assertions, since already on a failure path
  #define MSG_APPEND(format,...) do { \
    if (remain > 0) {                                       \
      int len = snprintf(msg, remain, format, __VA_ARGS__); \
      if (len > 0) { msg += len; remain -= len; }           \
    }                                                       \
    *msg = '\0';                                            \
  } while (0)

#if GASNETC_DYNAMIC_CONNECT && !GASNETC_USE_CONN_THREAD
  if (comp->wr_id & 1) { // UD for dynamic connection
    MSG_APPEND("%s"," operation=CONN");
  } else
#endif
  if (is_snd) {
    gasnetc_sreq_t *sreq = (gasnetc_sreq_t *)(uintptr_t)comp->wr_id;
    MSG_APPEND(" op=%s dest=(proc:%d, qpi:%d)",
               gasnetc_opcode_str(sreq->opcode),
               gasnetc_epid2node(sreq->cep->epid),
               gasnetc_epid2qpi(sreq->cep->epid) - 1);

  #if GASNET_DEBUG
    MSG_APPEND(" flags=%s", (sreq->send_flags & IBV_SEND_INLINE) ? "INLINE" : "0");
    switch(sreq->wr_opcode) {
      case IBV_WR_RDMA_READ:
      case IBV_WR_RDMA_WRITE:
        MSG_APPEND(" loc_addr=" GASNETI_LADDRFMT
                   " rem_addr=" GASNETI_LADDRFMT
                   " length=%u",
                   GASNETI_LADDRSTR(sreq->args.rdma.loc_addr),
                   GASNETI_LADDRSTR(sreq->args.rdma.rem_addr),
                   (unsigned int)sreq->args.rdma.length);
        break;

      case IBV_WR_SEND_WITH_IMM: {
        uint32_t flags = sreq->args.am.imm_data;
        int category = GASNETC_MSG_CATEGORY(flags);
        int nargs = GASNETC_MSG_NUMARGS(flags);
        const char *cat_name = "????";
        gex_AM_Arg_t *args = NULL;
        uint32_t nbytes = (uint32_t)-1;
        switch (category) {
          case gasneti_Short:
            cat_name = "Short";
            if (sreq->am_buff) {
              args = sreq->am_buff->shortmsg.args;
            }
            break;
          case gasneti_Medium:
            cat_name = "Medium";
            if (sreq->am_buff) {
              args = sreq->am_buff->medmsg.args;
              nbytes = sreq->am_buff->medmsg.nBytes;
            }
            break;
          case gasneti_Long:
            cat_name = "Long";
            if (sreq->am_buff) {
              args = sreq->am_buff->longmsg.args;
              nbytes = sreq->am_buff->longmsg.nBytes;
            }
            break;
        }
        if (nargs == GASNETC_MAX_ARGS) {
          // Decode actual nargs when carrying a hidden flow control arg
          if (args) {
            nargs = GASNETC_HIDDEN_ARG_FULL_NARGS(args) - 1;
          }
          // TODO: in absence of the original `args` array, Short and Long
          // cases _might_ be able to reconstruct from message length, but
          // padding to 8-byte boundary prevents that for Medium.
        }
        MSG_APPEND(" Re%s%s: nargs=%s handler=%d",
                   GASNETC_MSG_ISREPLY(flags)?"ply":"quest",
                   cat_name,
                   (nargs == GASNETC_MAX_ARGS)?"unknown":gasneti_dynsprintf("%d",nargs),
                   GASNETC_MSG_HANDLERID(flags));
        // May use a second scatter-gather entry for med or packed-long payloads
        int num_sge = MIN(sreq->args.am.num_sge, 2); // longer than 2 is erroneous
        for (int i = 0; i < num_sge; ++i) {
          MSG_APPEND(" sge[%d]=(addr:" GASNETI_LADDRFMT ", length:%u)",
                     i, GASNETI_LADDRSTR(sreq->args.am.addr[i]),
                     (unsigned int)sreq->args.am.length[i]);
        }
        // Payload length in this xfer, if any
        if (nbytes != (uint32_t)-1) {
          if (category == gasneti_Long) {
            // For Long, only report packed payload bytes
            nbytes = (nbytes & 0x80000000) ? (nbytes & 0x7fffffff) : 0;
          }
          if (nbytes) {
            MSG_APPEND(", includes %u bytes payload", (unsigned int)nbytes);
          }
        }
        break;
      }

      default: break; // Avoid warning about missing enum values
    }
  #endif // DEBUG
  }

  #undef MSG_APPEND
  const char *label = is_snd ? "snd" : "rcv";
  gasneti_console_message("ERROR", "%s status=%d(%s) vendor_err=0x%x qp_num=0x%x hca=%s%s",
                          label, comp->status, gasnetc_ibv_wc_status_str(comp->status),
                          comp->vendor_err, comp->qp_num, hca->hca_id, msg_buffer);
}

GASNETI_NEVER_INLINE(gasnetc_dump_cqs,
void gasnetc_dump_cqs(struct ibv_wc *comp, gasnetc_hca_t *hca, const int is_snd)) {
  static gex_HSL_t lock = GEX_HSL_INITIALIZER;

  gex_HSL_Lock(&lock);

  gasnetc_dump_cqe(comp, hca, is_snd);

  if (comp->status == IBV_WC_WR_FLUSH_ERR) {
    // Since the caller's CQE failed with "Work Request Flushed Error",
    // the other CQ might contain the original error (else it was remote).
    int max_cqe;
    const char *label;
    struct ibv_cq *cq;
    if (is_snd) {
      max_cqe = hca->qps * 2 * gasnetc_am_oust_pp * (gasneti_nodes - 1);
      label = "rcv";
      cq = hca->rcv_cq;
    } else {
      max_cqe = hca->qps * gasnetc_op_oust_pp;
      label = "snd";
      cq = hca->snd_cq;
    }

    // Drain the other CQ. Skip SUCCESS. Print the first error.
    int count = 0;
    while (1 == ibv_poll_cq(cq, 1, comp)) {
      if (comp->status != IBV_WC_SUCCESS) {
        gasnetc_dump_cqe(comp, hca, !is_snd);
        break; // done, success
      }
      if (++count > max_cqe) {
        gasneti_console_message("ERROR", "%s CQ: impossibly large WCE count > %d",
                                label, max_cqe);
        break; // done, failure
      }
    }
  }

  gex_HSL_Unlock(&lock);
}

GASNETI_INLINE(gasnetc_snd_reap_one)
void gasnetc_snd_reap_one(struct ibv_wc *comp_p, gasnetc_hca_t *hca GASNETC_COLLECT_FARGS) {
#if !GASNETC_SND_REAP_COLLECT
  const int collect = 0;
#endif

  if_pt (comp_p->status == IBV_WC_SUCCESS) {
    gasnetc_sreq_t *sreq = (gasnetc_sreq_t *)(uintptr_t)comp_p->wr_id;
  #if GASNETC_DYNAMIC_CONNECT && !GASNETC_USE_CONN_THREAD
    if_pf (comp_p->wr_id & 1) {
      gasnetc_conn_snd_wc(comp_p);
    } else
  #endif
    if_pt (sreq) {
      gasnetc_sema_up(hca->snd_cq_sema_p);
    again:
      gasnetc_sema_up(GASNETC_CEP_SQ_SEMA(sreq->cep));

      switch (sreq->opcode) {
      #if GASNETC_PIN_SEGMENT && GASNETC_FH_OPTIONAL
      case GASNETC_OP_GET_BOUNCE: // Bounce-buffer GET
        gasneti_assert(sreq->comp.cb != NULL);
        gasneti_assert(!GASNETC_USE_FIREHOSE); // Only possible when firehose disabled
        gasneti_assert(sreq->bb_buff != NULL);
        gasneti_assert(sreq->bb_addr != NULL);
        gasneti_assert(sreq->bb_len > 0);
        GASNETI_MEMCPY(sreq->bb_addr, sreq->bb_buff, sreq->bb_len);
        sreq->comp.cb(sreq->comp.data);
        GASNETC_COLLECT_BBUF(collect, sreq->bb_buff);
        break;
      #endif

      case GASNETC_OP_GET_ZEROCP: // Zero-copy GET
        gasneti_assert(sreq->comp.cb != NULL);
        sreq->comp.cb(sreq->comp.data);
        GASNETC_COLLECT_FHS(collect, sreq);
        break;

      case GASNETC_OP_PUT_BOUNCE:  // Bounce-buffer PUT
      case GASNETC_OP_LONG_BOUNCE: // Bounce-buffer Long payload
        if (sreq->comp.cb != NULL) {
          sreq->comp.cb(sreq->comp.data);
        }
        #if GASNETC_PIN_SEGMENT
        gasneti_assert(sreq->bb_buff);
        GASNETC_COLLECT_BBUF(collect, sreq->bb_buff);
        #else
        gasneti_assert(sreq->fh_bbuf);
        GASNETC_COLLECT_BBUF(collect, sreq->fh_bbuf);
        GASNETC_COLLECT_FHS(collect, sreq);
        #endif
        break;

      case GASNETC_OP_PUT_INLINE: // Inline PUT
        if (sreq->comp.cb != NULL) {
          sreq->comp.cb(sreq->comp.data);
        }
        #if GASNETC_PIN_SEGMENT
        gasneti_assert_int(sreq->fh_count ,==, 0);
        #else
        GASNETC_COLLECT_FHS(collect, sreq);
        #endif
        break;

      case GASNETC_OP_PUT_ZEROCP:  // Zero-copy PUT
      case GASNETC_OP_LONG_ZEROCP: // Zero-copy Long payload
        if (sreq->comp.cb != NULL) {
          sreq->comp.cb(sreq->comp.data);
        }
        GASNETC_COLLECT_FHS(collect, sreq);
        break;

      case GASNETC_OP_AM: // AM send
        if (sreq->comp.cb != NULL) {
          sreq->comp.cb(sreq->comp.data);
        }
        if (sreq->am_buff != NULL) {
          GASNETC_COLLECT_BBUF(collect, sreq->am_buff);
        }
        break;

      case GASNETC_OP_ATOMIC:
        if (sreq->comp.cb != NULL) {
          sreq->comp.cb(sreq->comp.data);
        }
        break;

      #if GASNETC_HAVE_FENCED_PUTS
      case GASNETC_OP_FENCE:        // Atomic after PUT, with descriptor chaining
        sreq->opcode = GASNETC_OP_FREE;
        sreq = sreq->fence_sreq;
        #if GASNET_DEBUG
          gasneti_assert(sreq);
          gasneti_assert(sreq->opcode & GASNETC_OP_NEEDS_FENCE);
          comp_p->opcode = IBV_WC_RDMA_WRITE;
        #endif
        goto again;
      #endif

      default:
        gasneti_unreachable_error(("Reaped send with invalid/unknown opcode %d", (int)sreq->opcode));
      }

      // Mark sreq free
      sreq->opcode = GASNETC_OP_FREE;
    } else {
      gasneti_fatalerror("snd_reap reaped NULL sreq");
      return;
    }
  } else if (GASNETC_IS_EXITING()) {
    return;
  } else if (!gasneti_attach_done) {
    gasneti_fatalerror("failed to connect (snd) status=%d", comp_p->status);
    return;
  } else {
    gasnetc_dump_cqs(comp_p, hca, 1);
    gasneti_fatalerror("aborting on reap of failed send");
    return;
  }
}

/* Try to pull completed entries (if any) from the send CQ(s). */
int gasnetc_snd_reap(int limit) {
  int count;
  struct ibv_wc comp;
  GASNETC_COLLECT_DECLS

  gasnetc_hca_t *hca;
#if GASNETC_IB_MAX_HCAS > 1
  if (gasnetc_snd_poll_multi_hcas) {
    GASNETC_WEAK_COUNTER_DECL(index, 0);
    int tmp = GASNETC_WEAK_COUNTER_READ(index);
    GASNETC_WEAK_COUNTER_WRITE(index, ((tmp == 0) ? gasnetc_num_hcas : tmp) - 1);
    hca = &gasnetc_hca[tmp];
  } else
#endif
  hca = &gasnetc_hca[0];

  gasneti_assert(limit <= GASNETC_SND_REAP_LIMIT);

  for (count = 0; count < limit; ++count) {
    if (GASNETC_POLL_CQ_TRYDOWN_SND(hca)) break; // another thread is polling this CQ
    int rc = ibv_poll_cq(hca->snd_cq, 1, &comp);
    GASNETC_POLL_CQ_UP_SND(hca);
    if (rc == 0) break; // CQ empty - we are done
    gasnetc_snd_reap_one(&comp, hca GASNETC_COLLECT_MANY);
  }

  if (count) {
    // Release any firehoses and bounce buffers we've collected
    GASNETC_COLLECT_FINALIZE();

    GASNETC_STAT_EVENT_VAL(SND_REAP,count);
  }

  return count;
}

/* Take *unbound* epid, return a qp number */
gasnetc_epid_t gasnetc_epid_select_qpi(gasnetc_cep_t *ceps, gasnetc_epid_t epid)
{
  gasnetc_epid_t qpi = gasnetc_epid2qpi(epid);

  if_pt (qpi == 0) {
 #if 0
    /* Select by largest space avail */
    uint32_t best_space = gasnetc_sema_read(GASNETC_CEP_SQ_SEMA(ceps+0));
    for (int i = 1; i < gasnetc_num_qps; ++i) {
      uint32_t space = gasnetc_sema_read(GASNETC_CEP_SQ_SEMA(ceps+i));
      if (space > best_space) {
        best_space = space;
        qpi = i;
      }
    }
 #else
    GASNETC_WEAK_COUNTER_DECL(prev, 0);
    qpi = GASNETC_WEAK_COUNTER_READ(prev);
    qpi = ((qpi == 0) ? gasnetc_num_qps : qpi) - 1;
    GASNETC_WEAK_COUNTER_WRITE(prev, qpi);
 #endif
    gasneti_assert(qpi < gasnetc_num_qps);
  } else {
    --qpi; /* offset */
    gasneti_assert(qpi < gasnetc_alloc_qps);
  }

  return qpi;
}

/* Take and sreq and bind it to a specific (not wildcard) qp */
#if GASNETC_DYNAMIC_CONNECT || GASNETC_IBV_SRQ
gasnetc_cep_t *gasnetc_bind_cep_inner(gasnetc_EP_t ep, gasnetc_epid_t epid, gasnetc_sreq_t *sreq, int is_reply GASNETI_THREAD_FARG)
#else
gasnetc_cep_t *gasnetc_bind_cep_inner(gasnetc_EP_t ep, gasnetc_epid_t epid, gasnetc_sreq_t *sreq)
#endif
{
  gasnetc_cep_t *ceps = gasnetc_get_cep(ep, gasnetc_epid2node(epid));
  gasnetc_cep_t *cep;
  gasnetc_epid_t qpi;

  /* Loop until space is available on the selected SQ for 1 new entry.
   * If we hold the last one then threads sending to the same node will stall. */
  qpi = gasnetc_epid_select_qpi(ceps, epid);
  cep = &ceps[qpi];
  if_pf (!gasnetc_sema_trydown(GASNETC_CEP_SQ_SEMA(cep))) {
    GASNETC_TRACE_WAIT_BEGIN();

  #if GASNETC_DYNAMIC_CONNECT
    /* Close the one dynamic connection race condition. */
    if (GASNETT_PREDICT_FALSE(GASNETC_CEP_SQ_SEMA(cep) == &gasnetc_zero_sema) && is_reply) {
      /* We are in the "gap" between RTR and RTS and waiting for the ACK.
       * However, since we are trying to send an AM Reply we KNOW that
       * the ACK was sent since we only Reply in response to a Request.
       * The Passive node reaches RTS at the time it sends its ACK and
       * thus cannot send us a Request until ready to send the ACK.
       */
      gasnetc_conn_implied_ack(ep, gasnetc_epid2node(epid));
    }
  #endif

#if GASNETC_IBV_SRQ
  // When using SRQ, rcv buffers for AM Requests may be under-provisioned,
  // leading to back-pressure on the injecting SQ.  When encountering this
  // as an injector, we must not become inattentive to the rcv CQ or we
  // risk deadlock (bug 4157).  So we may need to process inbound traffic
  // as well.  This includes RDMA Put of a RequestLong payload, in addition
  // to the send of the header.
  //
  // Note that the AM Request traffic is on a distinct channel (different
  // injecting QP) from RMA traffic and AM Reply traffic; and that the receive
  // buffer pool for Replies is always *fully* provisioned.  Thus there is no
  // need to poll the rcv CQ for anything other than the injection of an AM
  // Request header or a RequestLong payload.  In fact, doing so for Reply
  // injection would risk recursion and deadlock due to resources already held.
  //
  // As defined below, `should_poll_rcv` fully identifies the conditions under
  // which the poll is needed by checking for use of a bound (non-zero) qpi in
  // the Request-specific "upper half" of the qpi space.  This implicitly
  // checks for use of SRQ, since no operations are bound to those qpi values
  // otherwise.
  gasnetc_epid_t orig_qpi = gasnetc_epid2qpi(epid);
  const int should_poll_rcv = orig_qpi && GASNETC_QPI_IS_REQ(orig_qpi - 1);
  if (is_reply) gasneti_assert(!should_poll_rcv); // sanity check

  // This mess is needed because one cannot use `#if` inside the arguments
  // to a macro such as GASNETI_SPIN_DOUNTIL()
  #if GASNET_PSHM
    #define MAYBE_POLL_RCV_PSHM() gasneti_AMPSHMPoll(0 GASNETI_THREAD_PASS)
  #else
    #define MAYBE_POLL_RCV_PSHM() ((void)0)
  #endif
  // Note: It is NOT safe to run progress functions for the case of an AM
  // Request header because the caller holds resources (at least an AM credit
  // and often a bounce buffer).  That means running _communicating_ progress
  // functions here could lead to deadlock.  However, Put of a RequestLong
  // payload can (and so does) safely execute progress functions here.
  // This code is not reached for AM Reply (subject of "sanity check" above).
  // TODO: revisit GASNETC_OP_AM if/when we distinguish NON-communicating PFs.
  #define MAYBE_POLL_RCV(_ep, _cep) do { \
      if (should_poll_rcv && !gasnetc_sema_read(GASNETC_CEP_SQ_SEMA(_cep))) {  \
        gasnetc_poll_rcv_all(_ep, GASNETC_RCV_REAP_LIMIT GASNETI_THREAD_PASS); \
        MAYBE_POLL_RCV_PSHM();                                                 \
        if (sreq->opcode != GASNETC_OP_AM) { GASNETI_PROGRESSFNS_RUN(); }      \
      }                                                                        \
    } while (0)
#else
  #define MAYBE_POLL_RCV(_ep, _cep) ((void)0)
#endif

    GASNETI_SPIN_DOUNTIL(
      gasnetc_sema_trydown(GASNETC_CEP_SQ_SEMA(cep)),
      {
        gasnetc_snd_reap(1);
        /* Redo load balancing choice */
        qpi = gasnetc_epid_select_qpi(ceps, epid);
        cep = &ceps[qpi];
        MAYBE_POLL_RCV(ep, cep);
      });
    GASNETC_TRACE_WAIT_END(POST_SR_STALL_SQ);

#undef MAYBE_POLL_RCV
#undef MAYBE_POLL_RCV_PSHM
  }
  cep->used = 1;

  sreq->epid = gasnetc_epid(epid, qpi);
  sreq->cep = cep;

  return cep;
}

GASNETI_INLINE (gasnetc_ack)
void gasnetc_ack(gasnetc_rbuf_t *rbuf) {
  #if GASNET_DEBUG
    rbuf->rbuf_handlerRunning = 1; /* To satisfy assertion on Reply path */
  #endif
    GASNETI_SAFE(gasnetc_ReplySysShort((gex_Token_t)rbuf, NULL,
                                       gasneti_handleridx(gasnetc_ack), 0));
}

GASNETI_INLINE (gasnetc_hidden_ack)
void gasnetc_hidden_ack(gasnetc_rbuf_t *rbuf, gasnetc_cep_t *cep) {
  /* A race might result in sending non-coalesced ACKs if a Request
   * or Reply in another thread picks up one we expect to find.
   * However, we'll always send the correct total number of credits
   * and we'll never have more than gasnetc_am_credits_slack delayed.
   */
  uint32_t old;
  do {
    old = gasnetc_atomic_read(&cep->am_flow.credit, 0);
    if (old >= gasnetc_am_credits_slack) {
      /* MUST send back a reply */
      gasnetc_ack(rbuf);
      break;
    }
    gasneti_assert(!gasnetc_use_srq); /* No coalescing when using SRQ */
  } while (!gasnetc_atomic_compare_and_swap(&cep->am_flow.credit, old, old+1, 0));
}

GASNETI_INLINE(gasnetc_rcv_am)
void gasnetc_rcv_am(const struct ibv_wc *comp, gasnetc_rbuf_t **spare_p GASNETI_THREAD_FARG) {
  gasnetc_rbuf_t emergency_spare;
  gasnetc_rbuf_t *rbuf = (gasnetc_rbuf_t *)(uintptr_t)comp->wr_id;
  const uint32_t flags = comp->imm_data;
  gasnetc_cep_t *cep = rbuf->cep;
  gasnetc_rbuf_t *spare;
  const int isrep = GASNETC_MSG_ISREPLY(flags);

  GASNETC_STAT_EVENT(RCV_AM);

#if GASNETC_IBV_SRQ
  if (gasnetc_use_srq) {
    gasnetc_cep_t *orig_cep = cep;
    gasnetc_hca_t *hca = cep->hca;

    /* SRQ means rbuf->cep is "inexact", so must reconstruct */
    cep = GASNETC_NODE2CEP(rbuf->rr_ep, GASNETC_MSG_SRCIDX(flags));
    if (!isrep) {
      cep += gasnetc_num_qps; /* Search top half of table */
    }
    if (gasnetc_num_qps > 1) {
      int i;
      for (i=0; i<gasnetc_num_qps; ++i, ++cep) {
        if ((cep->rcv_qpn == comp->qp_num) && (cep->hca == hca)) break;
      }
      gasneti_assert(i < gasnetc_num_qps);
    }

    /* All flow-control and any Reply belong to opposite member of the pair */
    if (isrep) {
      cep += gasnetc_num_qps;
    } else {
      cep -= gasnetc_num_qps;
    }
    rbuf->cep = cep;

    /* Process and repost w/o any fancy tricks to keep credits perfectly accurate */
    gasnetc_processPacket(cep, rbuf, flags GASNETI_THREAD_PASS);
    if_pf (rbuf->rbuf_needReply) {
      /* MUST send back a reply - no coallescing */
      gasnetc_ack(rbuf);
    }

    gasnetc_rcv_post(orig_cep, rbuf);
    if (isrep) {
      gasnetc_sema_up(&hca->am_sema);
    }
  } else
#endif
  if (isrep) {
    /* Now process the packet */
    gasnetc_processPacket(cep, rbuf, flags GASNETI_THREAD_PASS);

    // Repost the rcv buffer
    gasnetc_rcv_post(cep, rbuf);
  } else {
    /* Post a replacement buffer before processing the request.
     * This ensures that the credit sent with the reply will
     * have a corresponding buffer available at this end. */
    spare = (*spare_p) ? (*spare_p) : gasnetc_lifo_pop(cep->rbuf_freelist);
    if_pt (spare) {
      /* This is the normal case */
      gasnetc_rcv_post(cep, spare);
      *spare_p = rbuf;	/* recv'd rbuf becomes the spare for next pass (if any) */
    } else {
      /* Because we don't have any "spare" rbuf available to post we copy the recvd
       * message to a temporary (non-pinned) buffer so we can repost rbuf.
       */
      gasnetc_buffer_t *buf = gasneti_malloc(sizeof(gasnetc_buffer_t));
      GASNETI_MEMCPY(buf, (void *)(uintptr_t)rbuf->rr_sg.addr, sizeof(gasnetc_buffer_t));
      emergency_spare.rr_sg.addr = (uintptr_t)buf;
      emergency_spare.rr_ep = rbuf->rr_ep;
      emergency_spare.cep = rbuf->cep;
  
      gasnetc_rcv_post(cep, rbuf);

      rbuf = &emergency_spare;
      GASNETC_STAT_EVENT(ALLOC_AM_SPARE);
      GASNETI_TRACE_PRINTF(C,("ALLOC_AM_SPARE\n"));
    }

    /* Now process the packet */
    gasnetc_processPacket(cep, rbuf, flags GASNETI_THREAD_PASS);

    /* Finalize flow control */
    if_pf (rbuf->rbuf_needReply) {
      gasnetc_hidden_ack(rbuf, cep);
    }

    /* Free the temporary buffer, if any */
    if_pf (!spare) {
      gasneti_free((void *)(uintptr_t)emergency_spare.rr_sg.addr);
    }
  }
}

static int gasnetc_rcv_reap(gasnetc_hca_t *hca, const int limit, gasnetc_rbuf_t **spare_p GASNETI_THREAD_FARG) {
  struct ibv_wc comp;
  int count;

  for (count = 0; count < limit; ++count) {
    if (GASNETC_POLL_CQ_TRYDOWN_RCV(hca)) break;
    int rc = ibv_poll_cq(hca->rcv_cq, 1, &comp);
    GASNETC_POLL_CQ_UP_RCV(hca);
    if (rc == 0) break; // CQ empty - we are done
    if_pt (rc == 1) {
      if_pt (comp.status == IBV_WC_SUCCESS) {
      #if GASNETC_DYNAMIC_CONNECT && !GASNETC_USE_CONN_THREAD
        if_pf (comp.wr_id & 1) {
          gasnetc_conn_rcv_wc(&comp);
          break; /* lower latency (and fewer implied ACKS) if we cease polling */
        }
      #endif
        gasnetc_rcv_am(&comp, spare_p GASNETI_THREAD_PASS);
      } else if (GASNETC_IS_EXITING()) {
        /* disconnected */
	break;	/* can't exit since we can be called in exit path */
      } else if (!gasneti_attach_done) {
        gasneti_fatalerror("failed to connect (rcv) status=%d", comp.status);
        break;
      } else {
	gasnetc_dump_cqs(&comp, hca, 0);
        gasneti_fatalerror("aborting on reap of failed recv");
	break;
      }
    } else if (GASNETC_IS_EXITING()) {
      /* disconnected by another thread */
      gasnetc_exit(0);
    } else {
      GASNETC_IBV_CHECK(rc, "while reaping the recv queue");
    }
  } 

  if (count) {
    GASNETC_STAT_EVENT_VAL(RCV_REAP,count);

    #if !GASNETC_PIN_SEGMENT
    /* Handler might have queued work for firehose */
    firehose_poll();
    #endif
  }

  return count;
}

void gasnetc_poll_rcv_hca(gasnetc_EP_t ep, gasnetc_hca_t *hca, int limit GASNETI_THREAD_FARG) {
  // Poll for AM in recv CQ
  gasnetc_rbuf_t *spare = NULL;
  (void)gasnetc_rcv_reap(hca, limit, &spare GASNETI_THREAD_PASS);
  if (spare) {
    gasnetc_lifo_push(&hca->rbuf_freelist, spare);
  }
}

void gasnetc_poll_rcv_all(gasnetc_EP_t ep, int limit GASNETI_THREAD_FARG) {
  gasnetc_hca_t *hca;
#if GASNETC_IB_MAX_HCAS > 1
  if (gasnetc_rcv_poll_multi_hcas) {
    GASNETC_WEAK_COUNTER_DECL(index, 0);
    int tmp = GASNETC_WEAK_COUNTER_READ(index);
    GASNETC_WEAK_COUNTER_WRITE(index, ((tmp == 0) ? gasnetc_num_hcas : tmp) - 1);
    hca = &gasnetc_hca[tmp];
  } else
#endif
  hca = &gasnetc_hca[0];

    gasnetc_poll_rcv_hca(ep, hca, GASNETC_RCV_REAP_LIMIT GASNETI_THREAD_PASS);
  #if GASNET_PSHM
    gasneti_AMPSHMPoll(0 GASNETI_THREAD_PASS);
  #endif
}

/* helper for allocation of a send request structure */
GASNETI_NEVER_INLINE(gasnetc_get_sreq_miss,
gasnetc_sreq_t *gasnetc_get_sreq_miss(gasnetc_sreq_t * const oldest GASNETI_THREAD_FARG))
{
    gasnetc_sreq_t *sreq = oldest;

    /* 2) Next poll all CQs and then check the oldest sreq again */
    int h;
    GASNETC_FOR_ALL_HCA_INDEX(h) {
      (void)gasnetc_snd_reap(1);
    }
    if_pf (sreq->opcode != GASNETC_OP_FREE) {
      /* 3) Next scan ahead, skipping over in-flight firehose misses for instance */
      do {
        sreq = sreq->next;
      } while ((sreq->opcode != GASNETC_OP_FREE) && (sreq != oldest));
      if_pf (sreq->opcode != GASNETC_OP_FREE) {
        /* 4) Finally allocate more */
        gasnetc_sreq_t *head, *tail;
        gasneti_assert(sreq == oldest);
        gasnetc_alloc_sreqs(&head, &tail GASNETI_THREAD_PASS);
        tail->next = sreq->next;
        sreq = (sreq->next = head);
      }
    }

    return sreq;
}

/* allocate a send request structure */
gasnetc_sreq_t *gasnetc_get_sreq(gasnetc_sreq_opcode_t opcode GASNETI_THREAD_FARG) {
  gasnetc_per_thread_t *td = gasnetc_my_perthread();
  gasnetc_sreq_t *sreq;

  /* 1) First try the oldest sreq in our list */
  sreq = td->sreqs;
  gasneti_assert(sreq != NULL);
  if_pf (sreq->opcode != GASNETC_OP_FREE) {
    /* steps 2...4 above */
    sreq = gasnetc_get_sreq_miss(sreq GASNETI_THREAD_PASS);
  }

  td->sreqs = sreq->next;
  gasneti_assert(td->sreqs != NULL);

  #if GASNET_DEBUG
    /* invalidate field(s) which should always be set by caller */
    sreq->epid = ~0;
    sreq->cep = NULL;
    sreq->fh_count = -1;
    #if !GASNETC_PIN_SEGMENT
    sreq->fh_len = ~0;
    sreq->fh_ep = NULL;
    #endif
  #endif

  /* Assume no counters */
  sreq->comp.cb = NULL;
  #if !GASNETC_PIN_SEGMENT
    sreq->fh_lc_cb = NULL;
    sreq->fh_oust = NULL;
  #endif

  gasneti_assert(sreq->opcode == GASNETC_OP_FREE);
  gasneti_assert(opcode != GASNETC_OP_FREE);
  sreq->opcode = opcode;

  return sreq;
}

GASNETI_INLINE(gasnetc_get_bbuf_inner)
gasnetc_buffer_t *gasnetc_get_bbuf_inner(const int is_reply, const int block GASNETI_THREAD_FARG) {
  gasnetc_buffer_t *bbuf = NULL;

  GASNETC_TRACE_WAIT_BEGIN();
  GASNETC_STAT_EVENT(GET_BBUF);

  bbuf = gasnetc_bbuf_pop_helper(is_reply);
  if_pt (bbuf) {
    // done
  } else if (block) {
    GASNETI_SPIN_DOUNTIL(bbuf, {
        gasnetc_poll_snd();
        bbuf = gasnetc_bbuf_pop_helper(is_reply);
      });
    GASNETC_TRACE_WAIT_END(GET_BBUF_STALL);
  } else {
    gasnetc_poll_snd();
    bbuf = gasnetc_bbuf_pop_helper(is_reply);
  }
  gasneti_assert((bbuf != NULL) || !block);

  return bbuf;
}

// Allocate a pre-pinned bounce buffer using helpers above
gasnetc_buffer_t *gasnetc_get_bbuf(int block GASNETI_THREAD_FARG) {
  return gasnetc_get_bbuf_inner(0, block GASNETI_THREAD_PASS);
}

#if GASNETC_IBV_SRQ
// Allocate a pre-pinned bounce buffer, with special case for reply
gasnetc_buffer_t *gasnetc_get_bbuf_srq(int is_reply, int block GASNETI_THREAD_FARG) {
  return gasnetc_get_bbuf_inner(is_reply, block GASNETI_THREAD_PASS);
}
#endif

#if GASNET_TRACE || GASNET_DEBUG
GASNETI_INLINE(gasnetc_snd_validate)
void gasnetc_snd_validate(gasnetc_sreq_t *sreq, struct ibv_send_wr *sr_desc, int count, const char *type) {
  gasneti_assert(sreq);
  gasneti_assert(sreq->cep);
  gasneti_assert(sr_desc);
  gasneti_assert(sr_desc->num_sge >= 1);
  gasneti_assert(sr_desc->num_sge <= GASNETC_SND_SG);
  gasneti_assert(count > 0);
  gasneti_assert(type);

  GASNETI_TRACE_PRINTF(D,("%s sreq=%p peer=%d qp=%d hca=%d\n", type, (void *)sreq,
			  gasnetc_epid2node(sreq->cep->epid),
			  gasnetc_epid2qpi(sreq->cep->epid) - 1,
			  GASNETC_HCA_IDX(sreq->cep)));
  for (int i = 0; i < count; ++i, ++sr_desc) {
    uintptr_t r_addr = sr_desc->wr.rdma.remote_addr;

  #if GASNET_DEBUG
    sreq->send_flags = sr_desc->send_flags;
    sreq->wr_opcode = sr_desc->opcode;
  #endif

    switch (sr_desc->opcode) {
    case IBV_WR_SEND_WITH_IMM:
      GASNETI_TRACE_PRINTF(D,("%s op=SND\n", type));
      for (int j = 0; j < sr_desc->num_sge; ++j) {
        uintptr_t l_addr = sr_desc->sg_list[j].addr;
        size_t    len    = sr_desc->sg_list[j].length;
	unsigned  lkey   = sr_desc->sg_list[j].lkey;
        GASNETI_TRACE_PRINTF(D,("  %i: lkey=0x%08x len=%"PRIuPTR" local=[%p-%p] remote=N/A\n",
			        j, lkey, (uintptr_t)len,
			        (void *)l_addr, (void *)(l_addr + (len - 1))));
      }
    #if GASNET_DEBUG
      gasneti_assert(sr_desc->num_sge == 1 || sr_desc->num_sge == 2);
      sreq->args.am.imm_data = sr_desc->imm_data;
      sreq->args.am.num_sge = sr_desc->num_sge;
      sreq->args.am.addr[0] = sr_desc->sg_list[0].addr;
      sreq->args.am.length[0] = sr_desc->sg_list[0].length;
      sreq->args.am.addr[1] = sr_desc->sg_list[1].addr;
      sreq->args.am.length[1] = sr_desc->sg_list[1].length;
    #endif
      break;

    case IBV_WR_RDMA_WRITE:
      GASNETI_TRACE_PRINTF(D,("%s op=PUT rkey=0x%08x\n", type, (unsigned int)sr_desc->wr.rdma.rkey));
      for (int j = 0; j < sr_desc->num_sge; ++j) {
        uintptr_t l_addr = sr_desc->sg_list[j].addr;
        size_t    len    = sr_desc->sg_list[j].length;
	unsigned  lkey   = sr_desc->sg_list[j].lkey;
        GASNETI_TRACE_PRINTF(D,("  %i: lkey=0x%08x len=%"PRIuPTR" local=[%p-%p] remote=[%p-%p]\n",
			        j, lkey, (uintptr_t)len,
				(void *)l_addr, (void *)(l_addr + (len - 1)),
				(void *)r_addr, (void *)(r_addr + (len - 1))));
	r_addr += len;
      }
    #if GASNET_DEBUG
      sreq->args.rdma.loc_addr = sr_desc->sg_list[0].addr;
      sreq->args.rdma.rem_addr = sr_desc->wr.rdma.remote_addr;
      sreq->args.rdma.length   = (uint32_t)(r_addr - sr_desc->wr.rdma.remote_addr);
    #endif
      break;

    case IBV_WR_RDMA_READ:
      GASNETI_TRACE_PRINTF(D,("%s op=GET rkey=0x%08x\n", type, (unsigned int)sr_desc->wr.rdma.rkey));
      for (int j = 0; j < sr_desc->num_sge; ++j) {
        uintptr_t l_addr = sr_desc->sg_list[j].addr;
        size_t    len    = sr_desc->sg_list[j].length;
	unsigned  lkey   = sr_desc->sg_list[j].lkey;
        GASNETI_TRACE_PRINTF(D,("  %i: lkey=0x%08x len=%"PRIuPTR" local=[%p-%p] remote=[%p-%p]\n",
			        j, lkey, (uintptr_t)len,
				(void *)l_addr, (void *)(l_addr + (len - 1)),
				(void *)r_addr, (void *)(r_addr + (len - 1))));
	r_addr += len;
      }
    #if GASNET_DEBUG
      sreq->args.rdma.loc_addr = sr_desc->sg_list[0].addr;
      sreq->args.rdma.rem_addr = sr_desc->wr.rdma.remote_addr;
      sreq->args.rdma.length   = r_addr - sr_desc->wr.rdma.remote_addr;
    #endif
      break;

    default:
      gasneti_fatalerror("Invalid operation %d for %s\n", sr_desc->opcode, type);
    }

    /* check for reasonable message sizes
     * With SEND 0-bytes triggers a Mellanox bug
     * With RDMA ops, 0-bytes makes no sense.
     */
    #if GASNET_DEBUG
    {
      u_int32_t	sum = 0;

      for (i = 0; i < sr_desc->num_sge; ++i) {
        sum += sr_desc->sg_list[i].length;
        gasneti_assert(GASNETC_ALLOW_0BYTE_MSG ||
                       (sr_desc->sg_list[i].length != 0));
        gasneti_assert(sr_desc->sg_list[i].length <= gasnetc_max_msg_sz);
        gasneti_assert(sr_desc->sg_list[i].length <= sum); /* check for overflow of 'sum' */
      }

      gasneti_assert(sum <= gasnetc_max_msg_sz);
    }
    #endif
  }
}
#else /* DEBUG || TRACE */
  #define gasnetc_snd_validate(a,b,c,d)	do {} while (0)
#endif /* DEBUG || TRACE */


GASNETI_COLD GASNETI_NORETURN
static void gasnetc_snd_post_fail(int rc, int is_inline) {
  if (GASNETC_IS_EXITING()) {
    /* disconnected by another thread */
    gasnetc_exit(0);
  } else {
    /* unexpected error */
    GASNETC_IBV_CHECK(rc, is_inline ? "while posting an inline send work request"
                                    : "while posting a send work request");
  }
  gasneti_fatalerror("unreachable");
}
GASNETI_NORETURNP(gasnetc_snd_post_fail)

static void
gasnetc_snd_post_inner(gasnetc_cep_t * const cep, struct ibv_send_wr *sr_desc, int is_inline GASNETI_THREAD_FARG)
{
  // Loop until space is available for 1 new entry on the CQ.
  // If we hold the last one then threads sending to ANY node will stall.
  // So this is the last resource to acquire
  GASNETI_SPIN_UNTIL_TRACE(gasnetc_sema_trydown(cep->snd_cq_sema_p),
                           C, POST_SR_STALL_CQ, gasnetc_poll_snd());

  // Post the operation
  struct ibv_send_wr *bad_wr;
  int rc = ibv_post_send(cep->qp_handle, sr_desc, &bad_wr);
  if_pf (rc) gasnetc_snd_post_fail(rc, is_inline);
}

void gasnetc_snd_post_common(gasnetc_sreq_t *sreq, struct ibv_send_wr *sr_desc, int is_inline GASNETI_THREAD_FARG) {
  gasnetc_cep_t * const cep = sreq->cep;

  /* Must be bound to a qp by now */
  gasneti_assert(cep != NULL );

  gasneti_assert(sreq->opcode != GASNETC_OP_FREE);
  gasneti_assert(sreq->opcode != GASNETC_OP_INVALID);

  /* A valid callback will never take NULL as its data */
  gasneti_assert(sreq->comp.cb == NULL || sreq->comp.data != NULL);

  // setup some remaining fields
  const enum ibv_send_flags inline_flag = is_inline ? IBV_SEND_INLINE
                                                    : (enum ibv_send_flags)0;
  const enum ibv_send_flags signal_flag = GASNETC_USE_SEND_SIGNALLED ? IBV_SEND_SIGNALED
                                                                     : (enum ibv_send_flags)0;
  sr_desc->send_flags = inline_flag | signal_flag;
  sr_desc->wr_id = (uintptr_t)sreq;
#if GASNETC_IBV_XRC_OFED
  sr_desc->qp_type.xrc.remote_srqn = cep->xrc_remote_srq_num; /* Even if unused */
#elif GASNETC_IBV_XRC_MLNX
  sr_desc->xrc_remote_srq_num = cep->xrc_remote_srq_num; /* Even if unused */
#endif
  sr_desc->next = NULL;

  /* Trace and debug */
  if (is_inline) {
    GASNETC_STAT_EVENT(POST_INLINE_SR);
    gasnetc_snd_validate(sreq, sr_desc, 1, "POST_INLINE_SR");
  } else {
    GASNETC_STAT_EVENT_VAL(POST_SR, sr_desc->num_sge);
    gasnetc_snd_validate(sreq, sr_desc, 1, "POST_SR");
  }

#if GASNETC_HAVE_FENCED_PUTS
  // When GASNET_USE_FENCED_PUTS is enabled, we must post both the Put and an
  // Atomic such that the conduit-level remote completion callback for the Put
  // will not execute until the ibv-level CQE for the Atomic.
  GASNETC_DECL_SR_DESC(amo_sr_desc, 1);
  if (sreq->opcode & gasnetc_op_needs_fence_mask) {
    gasnetc_sreq_t *amo_sreq = gasnetc_get_sreq(GASNETC_OP_FENCE GASNETI_THREAD_PASS);
    amo_sreq->cep = cep;
    amo_sreq->fence_sreq = sreq;

    // Setup fetch-and-add using dummy auxseg locations reserved for this purpose
    amo_sr_desc->wr_id = (uintptr_t)amo_sreq;
    amo_sr_desc->opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;
    amo_sr_desc->wr.atomic.compare_add = 0;
    amo_sr_desc->wr.atomic.remote_addr = GASNETC_FENCE_REM_ADDR(cep);
    amo_sr_desc->wr.atomic.rkey = cep->hca->aux_rkeys[gasnetc_epid2node(cep->epid)];
    amo_sr_desc->num_sge = 1;
    amo_sr_desc->sg_list[0].length = sizeof(uint64_t);
    amo_sr_desc->sg_list[0].addr = GASNETC_FENCE_LOC_ADDR(cep);
    amo_sr_desc->sg_list[0].lkey = cep->hca->aux_reg.handle->lkey;
  #if GASNETC_IBV_XRC_OFED
    amo_sr_desc->qp_type.xrc.remote_srqn = sr_desc->qp_type.xrc.remote_srqn;
  #elif GASNETC_IBV_XRC_MLNX
    amo_sr_desc->xrc_remote_srq_num = sr_desc->xrc_remote_srq_num;
  #endif

    // Assume we can obtain resources to make a single ibv_post_send() call,
    // generating a single CQE for the Atomic (suppressing the one for the Put)
    amo_sr_desc->send_flags = IBV_SEND_SIGNALED;
    amo_sr_desc->next = NULL;
    sr_desc->send_flags = inline_flag; // Strips IBV_SEND_SIGNALED
    sr_desc->next = amo_sr_desc;

    // Try at most twice (w/ a CQ poll between) to obtain a second CQ slot
    // Spinning indefinitely while holding one slot could deadlock if
    // multiple threads in a PAR build are all doing the same.
    // Even in a SEQ or PARSYNC build, there is an advantage to posting the
    // Put without unnecessary delay.
    if_pf (!gasnetc_sema_trydown(GASNETC_CEP_SQ_SEMA(cep)) &&
           (gasnetc_snd_reap(1), !gasnetc_sema_trydown(GASNETC_CEP_SQ_SEMA(cep)))) {
      // Since we failed to get a second SQ slot we split the two post operations
      GASNETC_STAT_EVENT(POST_SR_SPLIT);
      // Move the remote completion callback from the Put to the Atomic
      amo_sreq->opcode = GASNETC_OP_ATOMIC;
      amo_sreq->comp.cb   = sreq->comp.cb;
      amo_sreq->comp.data = sreq->comp.data;
      sreq->comp.cb = NULL;
      // Post only the Put, releasing a SQ slot for eventual reclamation
      sr_desc->next = NULL;
      sr_desc->send_flags = inline_flag | IBV_SEND_SIGNALED;
      gasnetc_snd_post_inner(cep, sr_desc, is_inline GASNETI_THREAD_PASS);
      // Ensure the post call on the normal code path will post the Atomic
      sr_desc = amo_sr_desc;
      is_inline = 0;
      // Now we spin to obtain a SQ slot for just the Atomic operation
      GASNETI_SPIN_UNTIL_TRACE(gasnetc_sema_trydown(GASNETC_CEP_SQ_SEMA(cep)),
                               C, POST_SR_STALL_SQ2, gasnetc_snd_reap(1));
    }
  }
#endif

  /* Post it */
  gasnetc_snd_post_inner(cep, sr_desc, is_inline GASNETI_THREAD_PASS);
}
#define gasnetc_snd_post(x,y)		gasnetc_snd_post_common(x,y,0 GASNETI_THREAD_PASS)
#define gasnetc_snd_post_inline(x,y)	gasnetc_snd_post_common(x,y,1 GASNETI_THREAD_PASS)

#if GASNETC_USE_RCV_THREAD
static void gasnetc_rcv_thread(struct ibv_wc *comp_p, void *arg)
{
  gasnetc_hca_t * const hca = (gasnetc_hca_t *)arg;
  gasnetc_rbuf_t ** const spare_p = &hca->rcv_thread_priv;

#if GASNETI_THREADINFO_OPT
  if_pf (! hca->rcv_threadinfo) hca->rcv_threadinfo = _gasneti_mythread_slow();
  GASNET_POST_THREADINFO(hca->rcv_threadinfo);
#endif

  gasneti_assert(gasnetc_use_rcv_thread);

  gasneti_assert((comp_p->opcode == IBV_WC_RECV) ||
                 (comp_p->status != IBV_WC_SUCCESS));

  if_pf (comp_p->status != IBV_WC_SUCCESS) {
    gasnetc_dump_cqs(comp_p, hca, 0);
    gasneti_fatalerror("aborting on reap of failed AM recv");
  }
  #if GASNETC_DYNAMIC_CONNECT && !GASNETC_USE_CONN_THREAD
  else if_pf (comp_p->wr_id & 1) {
    gasnetc_conn_rcv_wc(comp_p);
  }
  #endif
  else {
    gasnetc_rcv_am(comp_p, spare_p GASNETI_THREAD_PASS);
    GASNETC_STAT_EVENT_VAL(RCV_REAP, 1);
  #if !GASNETC_PIN_SEGMENT
    /* Handler might have queued work for firehose */
    firehose_poll();
  #endif 
  #if GASNETC_SERIALIZE_POLL_CQ
    // In exclusive mode it is not safe to run progress functions, because
    // AM Request injection within a progress function cannot poll for credits.
    // TODO: revisit if/when non-communicating progress functions are separated.
    if (! gasnetc_rcv_thread_poll_exclusive)
  #endif 
      GASNETI_PROGRESSFNS_RUN();
  }
}
#endif /* GASNETC_USE_RCV_THREAD */

#if GASNETC_USE_SND_THREAD
static void gasnetc_snd_thread(struct ibv_wc *comp_p, void *arg)
{
  gasnetc_hca_t * const hca = (gasnetc_hca_t *)arg;

  gasneti_assert(gasnetc_use_snd_thread);

  gasneti_assert((comp_p->opcode == IBV_WC_SEND) ||
                 (comp_p->opcode == IBV_WC_RDMA_WRITE) ||
                 (comp_p->opcode == IBV_WC_RDMA_READ) ||
             //  (comp_p->opcode == IBV_WC_COMP_SWAP) ||
                 (comp_p->opcode == IBV_WC_FETCH_ADD) ||
                 (comp_p->status != IBV_WC_SUCCESS));

  if_pf (comp_p->status != IBV_WC_SUCCESS) {
    gasnetc_dump_cqs(comp_p, hca, 0);
    gasneti_fatalerror("aborting on reap of failed snd");
  }
  else {
    gasnetc_snd_reap_one(comp_p, hca GASNETC_COLLECT_ONE);
    GASNETC_STAT_EVENT(SND_REAP_THR);
  }
}
#endif /* GASNETC_USE_SND_THREAD */

#if GASNETC_PIN_SEGMENT
/*
 * ###############################################################
 * Static helper functions for RDMA when the segment is pre-pinned
 * ###############################################################
 */

/* Assemble and post a bounce-buffer PUT or GET */
GASNETI_INLINE(gasnetc_bounce_common)
void gasnetc_bounce_common(
                gasnetc_EP_t ep, gasnetc_epid_t epid,
                int rem_epidx,
                struct ibv_send_wr *sr_desc,
                size_t len,
                gasnetc_sreq_t *sreq,
                enum ibv_wr_opcode op
                GASNETI_THREAD_FARG)
{
  sr_desc->opcode      = op;
  sr_desc->num_sge     = 1;
  sr_desc->sg_list[0].addr = (uintptr_t)sreq->bb_buff;
  sr_desc->sg_list[0].length = len;

  gasnetc_cep_t *cep = gasnetc_bind_cep(ep, epid, sreq);
  sr_desc->wr.rdma.rkey = gasnetc_seg_rkey(cep, rem_epidx);
  sr_desc->sg_list[0].lkey = GASNETC_SND_LKEY(cep);

  gasnetc_snd_post(sreq, sr_desc);
  sr_desc->wr.rdma.remote_addr += len;
}

// Assemble and all-but-post a zero-copy PUT or GET using either the seg_lkey,
// ODP, or firehose to obtain the lkeys.  All cases delay the bind to a qp until the
// total xfer len is known.
// Returns bytes transferred
GASNETI_INLINE(gasnetc_zerocp_common)
size_t gasnetc_zerocp_common(
                gasnetc_EP_t ep, gasnetc_epid_t epid,
                int rem_epidx,
                struct ibv_send_wr *sr_desc,
                size_t len,
                gasnetc_sreq_t *sreq,
                enum ibv_wr_opcode op
                GASNETI_THREAD_FARG)
{
  uintptr_t loc_addr = sr_desc->sg_list[0].addr;
  gasnetc_cep_t *cep;

  sr_desc->opcode = op;

  // Limit how much we can xfer
  // Note that op is a manifest constant in all callers
  //
  // We don't want stripe_sz=N to split a N+1 byte xfer into N-byte and 1-byte xfers.
  // So, sizes between "stripe_sz" and "stripe_split" are spilt into two equal chunks.
  // Currently stripe_split = MIN(2*stripe_sz, gasnetc_max_msg_sz), which results in
  // the logic only ever creating chunks of size in [stripe_sz/2, stripe_sz].
  switch (op) {
    case IBV_WR_RDMA_WRITE:
      if (len > gasnetc_put_stripe_sz) {
        len = (len > gasnetc_put_stripe_split) ? gasnetc_put_stripe_sz : (len >> 1);
      }
      break;
    case IBV_WR_RDMA_READ:
      if (len > gasnetc_get_stripe_sz) {
        len = (len > gasnetc_get_stripe_split) ? gasnetc_get_stripe_sz : (len >> 1);
      }
      break;
    default:
      gasneti_unreachable_error(("Invalid 'op' in gasnetc_zerocp_common: %d",(int)op));
  }

  size_t sent = 0;

  // TODO-EX:
  //     This will be replaced by general multi-registration support later
  const int loc_auxseg = gasneti_in_local_auxsegment((gasneti_EP_t)ep, (void*)loc_addr, len);

  if_pf (gasnetc_in_bound_segment(ep, loc_addr, len) || loc_auxseg) {
    // loc_addr is in aux- or client-segment
    cep = gasnetc_bind_cep(ep, epid, sreq);
    sr_desc->sg_list[0].lkey = gasnetc_seg_lkey(ep, cep, loc_auxseg);
    gasneti_assert_uint(sr_desc->sg_list[0].addr ,==, loc_addr);
    sr_desc->sg_list[0].length = len;
    sr_desc->num_sge = 1;
    sreq->fh_count = 0;
    sent = len;
#if GASNETC_IBV_ODP
  } else if (gasnetc_use_odp) {
    // TODO-EX: older implicit ODP emulation had 128MB limit.  May need to chunk here.
    cep = gasnetc_bind_cep(ep, epid, sreq);
    sr_desc->sg_list[0].lkey = cep->hca->implicit_odp.lkey;
    gasneti_assert_uint(sr_desc->sg_list[0].addr ,==, loc_addr);
    sr_desc->sg_list[0].length = len;
    sr_desc->num_sge = 1;
    sreq->fh_count = 0;
    sent = len;
#endif
  } else {
    const firehose_request_t *fh_loc = gasnetc_fh_aligned_local_pin(loc_addr, len);
    if_pf (! fh_loc) {
      return 0; // Attempt to xfer un-pinnable memory such as due to bug 3338
    }
    int seg;
    size_t remain = len;
    for (seg = 0; fh_loc != NULL; ++seg) {
      const size_t count = MIN(remain, (fh_loc->addr + fh_loc->len - loc_addr));
      sreq->fh_ptr[seg] = fh_loc;
      sreq->fh_count = seg + 1;
      sr_desc->sg_list[seg].addr = loc_addr;
      sr_desc->sg_list[seg].length = count;
      loc_addr += count;
      remain -= count;
      if (!remain || seg == (GASNETC_SND_SG-1)) {
	break; /* End of xfer or sg list */
      }

      /* We hold a local firehose already, we can only 'try' or risk deadlock */
      fh_loc = gasnetc_fh_try_local_pin(loc_addr, 1);
    }
    gasneti_assert_int(sreq->fh_count ,>, 0);
    sr_desc->num_sge = sreq->fh_count;
    cep = gasnetc_bind_cep(ep, epid, sreq);
    for (seg = 0; seg < sr_desc->num_sge; ++seg) {
      /* Xlate to actual lkeys */
      sr_desc->sg_list[seg].lkey = GASNETC_FH_LKEY(cep, sreq->fh_ptr[seg]);
    }
    sent = len - remain;
  }

  sr_desc->wr.rdma.rkey = gasnetc_seg_rkey(cep, rem_epidx);

  gasneti_assert(sent > 0);
  return sent;
}

/* Helper for rdma puts: inline send case */
GASNETI_INLINE(gasnetc_do_put_inline)
void gasnetc_do_put_inline(
                                  gasnetc_EP_t ep, const gasnetc_epid_t epid,
                                  int rem_epidx,
                                  struct ibv_send_wr *sr_desc,
                                  size_t nbytes,
                                  gasnetc_atomic_val_t *remote_cnt, gasnetc_cb_t remote_cb
				  GASNETI_THREAD_FARG) {
  gasnetc_cep_t *cep;
  gasnetc_sreq_t *sreq;

  GASNETI_TRACE_EVENT_VAL(C, RDMA_PUT_INLINE, nbytes);

  gasneti_assert(nbytes != 0);
  gasneti_assert(nbytes <= gasnetc_inline_limit);
  gasneti_assert(gasneti_i_segment_kind_is_host(ep->_segment));

  sreq = gasnetc_get_sreq(GASNETC_OP_PUT_INLINE GASNETI_THREAD_PASS);
  sreq->fh_count = 0;
  if (remote_cnt) {
    ++(*remote_cnt);
    sreq->comp.data = remote_cnt;
    sreq->comp.cb = remote_cb;
  }

  sr_desc->opcode      = IBV_WR_RDMA_WRITE;
  sr_desc->num_sge     = 1;
  sr_desc->sg_list[0].length = nbytes;

  cep = gasnetc_bind_cep(ep, epid, sreq);
  sr_desc->wr.rdma.rkey = gasnetc_seg_rkey(cep, rem_epidx);

  gasnetc_snd_post_inline(sreq, sr_desc);
  sr_desc->wr.rdma.remote_addr += nbytes;
  sr_desc->sg_list[0].addr += nbytes;
}
      
/* Helper for rdma puts: bounce buffer case */
GASNETI_INLINE(gasnetc_do_put_bounce)
void gasnetc_do_put_bounce(
                                  gasnetc_EP_t ep, const gasnetc_epid_t epid,
                                  int rem_epidx,
                                  struct ibv_send_wr *sr_desc,
                                  size_t nbytes,
                                  gasnetc_atomic_val_t *remote_cnt, gasnetc_cb_t remote_cb
				  GASNETI_THREAD_FARG) {
  uintptr_t src = sr_desc->sg_list[0].addr;
  GASNETI_TRACE_EVENT_VAL(C, RDMA_PUT_BOUNCE, nbytes);

  const int is_long_payload = gasnetc_epid2qpi(epid); // non-zero qpi identifies Long payloads
  const gasnetc_sreq_opcode_t sreq_op = is_long_payload ? GASNETC_OP_LONG_BOUNCE
                                                        : GASNETC_OP_PUT_BOUNCE;

  gasneti_assert(nbytes != 0);
  gasneti_assert(gasneti_i_segment_kind_is_host(ep->_segment));

  do {
    gasnetc_sreq_t * const sreq = gasnetc_get_sreq(sreq_op GASNETI_THREAD_PASS);
    const size_t count = MIN(GASNETC_BUFSZ, nbytes);

    sreq->bb_buff = gasnetc_get_bbuf(1 GASNETI_THREAD_PASS);
    GASNETI_MEMCPY(sreq->bb_buff, (void *)src, count);
    if (remote_cnt) {
      ++(*remote_cnt);
      sreq->comp.data = remote_cnt;
      sreq->comp.cb = remote_cb;
    }

    gasnetc_bounce_common(ep, epid, rem_epidx, sr_desc, count, sreq, IBV_WR_RDMA_WRITE GASNETI_THREAD_PASS);

    src += count;
    nbytes -= count;
  } while (nbytes);
  sr_desc->sg_list[0].addr = src;
}

// Helper for rdma puts: zero copy case
// Returns count of unsent bytes, if any
GASNETI_INLINE(gasnetc_do_put_zerocp)
size_t gasnetc_do_put_zerocp(
                                  gasnetc_EP_t ep, const gasnetc_epid_t epid,
                                  int rem_epidx,
                                  struct ibv_send_wr *sr_desc,
                                  size_t nbytes,
                                  gasnetc_atomic_val_t *cnt, gasnetc_cb_t cb
                                  GASNETI_THREAD_FARG)
{
  GASNETI_TRACE_EVENT_VAL(C, RDMA_PUT_ZEROCP, nbytes);

  const int is_long_payload = gasnetc_epid2qpi(epid); // non-zero qpi identified Long payloads
  const gasnetc_sreq_opcode_t sreq_op = is_long_payload ? GASNETC_OP_LONG_ZEROCP
                                                        : GASNETC_OP_PUT_ZEROCP;

  gasneti_assert(nbytes != 0);

  // loop over max-length xfers
  do {
    gasnetc_sreq_t * const sreq = gasnetc_get_sreq(sreq_op GASNETI_THREAD_PASS);
    size_t count = gasnetc_zerocp_common(ep, epid, rem_epidx, sr_desc, nbytes, sreq,
                                         IBV_WR_RDMA_WRITE GASNETI_THREAD_PASS);
    if_pf (!count) {
      // Failed to register memory, such as for read-only memory (bug 3338)
      // Return non-zero (tells caller to fall-back to bounce-buffers)
      GASNETI_TRACE_EVENT_VAL(C, RDMA_PUT_READONLY, nbytes);
      sreq->opcode = GASNETC_OP_FREE;
      return nbytes;
    }

    gasneti_assert(count <= nbytes);
    nbytes -= count;

    if (cnt) {
      ++(*cnt);
      sreq->comp.data = cnt;
      sreq->comp.cb = cb;
    }

    gasnetc_snd_post(sreq, sr_desc);
    sr_desc->wr.rdma.remote_addr += count;
    sr_desc->sg_list[0].addr += count;
  } while (nbytes);

  return 0;
}

/* Helper for rdma gets: bounce buffer case */
GASNETI_INLINE(gasnetc_do_get_bounce)
void gasnetc_do_get_bounce(
                                  gasnetc_EP_t ep, const gasnetc_epid_t epid,
                                  int rem_epidx,
                                  struct ibv_send_wr *sr_desc,
                                  size_t nbytes,
                                  gasnetc_atomic_val_t *remote_cnt, gasnetc_cb_t remote_cb
				  GASNETI_THREAD_FARG) {
#if GASNETC_FH_OPTIONAL /* Only reachable if Firehose has been disabled */
  uintptr_t dst = sr_desc->sg_list[0].addr;
  GASNETI_TRACE_EVENT_VAL(C, RDMA_GET_BOUNCE, nbytes);

  gasneti_assert(nbytes != 0);
  gasneti_assert(remote_cnt != NULL);
  gasneti_assert(gasneti_i_segment_kind_is_host(ep->_segment));

  do {
    gasnetc_sreq_t * const sreq = gasnetc_get_sreq(GASNETC_OP_GET_BOUNCE GASNETI_THREAD_PASS);
    const size_t count = MIN(GASNETC_BUFSZ, nbytes);

    nbytes -= count;
    if (nbytes) ++(*remote_cnt); // Do NOT advance prior to the last injection

    sreq->bb_addr  = (void *)dst;
    sreq->bb_len   = count;
    sreq->bb_buff  = gasnetc_get_bbuf(1 GASNETI_THREAD_PASS);
    sreq->comp.cb = remote_cb;
    sreq->comp.data = remote_cnt;

    gasnetc_bounce_common(ep, epid, rem_epidx, sr_desc, count, sreq, IBV_WR_RDMA_READ GASNETI_THREAD_PASS);

    dst += count;
  } while (nbytes);
  sr_desc->sg_list[0].addr = dst;
#else
  gasneti_fatalerror("unreachble call to gasnetc_do_get_bounce()");
#endif
}

/* Helper for rdma gets: zero copy case */
GASNETI_INLINE(gasnetc_do_get_zerocp)
void gasnetc_do_get_zerocp(     
                                  gasnetc_EP_t ep, const gasnetc_epid_t epid,
                                  int rem_epidx,
                                  struct ibv_send_wr *sr_desc,
                                  size_t nbytes,
                                  gasnetc_atomic_val_t *remote_cnt, gasnetc_cb_t remote_cb
                                  GASNETI_THREAD_FARG)
{
  GASNETI_TRACE_EVENT_VAL(C, RDMA_GET_ZEROCP, nbytes);

  gasneti_assert(nbytes != 0);
  gasneti_assert(remote_cnt != NULL);

  // loop over max-length xfers
  do {
    gasnetc_sreq_t * const sreq = gasnetc_get_sreq(GASNETC_OP_GET_ZEROCP GASNETI_THREAD_PASS);
    size_t count = gasnetc_zerocp_common(ep, epid, rem_epidx, sr_desc, nbytes, sreq,
                                         IBV_WR_RDMA_READ GASNETI_THREAD_PASS);
    if_pf (!count) {
      // TODO: idealy we could retry memory registration to tolerate transient read-only
      // status as may occur with some tools which play games with protections to get signals.
      gasneti_fatalerror("Attempt to GET into non-writable memory at %p\n",
                         (void*)(uintptr_t)sr_desc->sg_list[0].addr);
    }
    gasneti_assert(count <= nbytes);

    nbytes -= count;
    if (nbytes) ++(*remote_cnt); // Do NOT advance prior to the last injection

    sreq->comp.cb = remote_cb;
    sreq->comp.data = remote_cnt;

    gasnetc_snd_post(sreq, sr_desc);
    sr_desc->wr.rdma.remote_addr += count;
    sr_desc->sg_list[0].addr += count;
  } while (nbytes);
}

#else /* !GASNETC_PIN_SEGMENT */
/*
 * ###################################################################
 * Static helper functions for RDMA when the segment is NOT pre-pinned
 * ###################################################################
 */
GASNETI_INLINE(gasnetc_fh_put_inline)
void gasnetc_fh_put_inline(gasnetc_sreq_t *sreq GASNETI_THREAD_FARG) {
  GASNETC_DECL_SR_DESC(sr_desc, 1);
  const firehose_request_t * const fh_rem = sreq->fh_ptr[0];
  size_t len = sreq->fh_len;
  gasnetc_atomic_val_t *lc;
  gasnetc_cb_t lc_cb;
  gasnetc_cep_t *cep;

  gasneti_assert(fh_rem != NULL);
  gasneti_assert(sreq->fh_rem_addr >= fh_rem->addr);
  gasneti_assert(sreq->fh_rem_addr + (len - 1) <= fh_rem->addr + (fh_rem->len - 1));

  sr_desc->opcode      = IBV_WR_RDMA_WRITE;
  sr_desc->num_sge     = 1;
  sr_desc->wr.rdma.remote_addr = sreq->fh_rem_addr;
  sr_desc->sg_list[0].addr = sreq->fh_loc_addr;
  sr_desc->sg_list[0].length = len;

  lc_cb = sreq->fh_lc_cb;
  lc = sreq->fh_lc;

  cep = gasnetc_bind_cep(sreq->fh_ep, sreq->epid, sreq);
  sr_desc->wr.rdma.rkey = GASNETC_FH_RKEY(cep, fh_rem);

  gasnetc_snd_post_inline(sreq, sr_desc);

  if_pf (lc_cb) lc_cb(lc); /* locally complete */
}

GASNETI_INLINE(gasnetc_fh_put_bounce)
void gasnetc_fh_put_bounce(gasnetc_sreq_t *orig_sreq GASNETI_THREAD_FARG) {
  GASNETC_DECL_SR_DESC(sr_desc, 1);
  const firehose_request_t * const fh_rem = orig_sreq->fh_ptr[0];
  gasnetc_epid_t epid = orig_sreq->epid;
  size_t nbytes = orig_sreq->fh_len;
  gasnetc_EP_t ep = orig_sreq->fh_ep;
  gasnetc_cep_t *cep;
  uintptr_t src = orig_sreq->fh_loc_addr;
  uintptr_t dst = orig_sreq->fh_rem_addr;

  const gasnetc_sreq_opcode_t sreq_op = orig_sreq->opcode;

  gasneti_assert(nbytes != 0);
  gasneti_assert(orig_sreq->fh_rem_addr >= fh_rem->addr);
  gasneti_assert(orig_sreq->fh_rem_addr + (nbytes - 1) <= fh_rem->addr + (fh_rem->len - 1));

  /* Use full bounce buffers until just one buffer worth of data remains */
  while (nbytes > GASNETC_BUFSZ) {
    gasnetc_sreq_t * const sreq = gasnetc_get_sreq(sreq_op GASNETI_THREAD_PASS);
    sreq->fh_bbuf = gasnetc_get_bbuf(1 GASNETI_THREAD_PASS);
    GASNETI_MEMCPY(sreq->fh_bbuf, (void *)src, GASNETC_BUFSZ);
    sreq->fh_count = 0;

    sr_desc->opcode      = IBV_WR_RDMA_WRITE;
    sr_desc->num_sge     = 1;
    sr_desc->wr.rdma.remote_addr = dst;
    sr_desc->sg_list[0].addr = (uintptr_t)sreq->fh_bbuf;
    sr_desc->sg_list[0].length  = GASNETC_BUFSZ;

    cep = gasnetc_bind_cep(ep, epid, sreq);
    sr_desc->wr.rdma.rkey = GASNETC_FH_RKEY(cep, fh_rem);
    sr_desc->sg_list[0].lkey = GASNETC_SND_LKEY(cep);

    /* Send all ops on same qp to get point-to-point ordering for proper fh_release() */
    epid = sreq->epid;

    gasnetc_snd_post(sreq, sr_desc);

    src += GASNETC_BUFSZ;
    dst += GASNETC_BUFSZ;
    nbytes -= GASNETC_BUFSZ;
  }

  /* Send out the last buffer w/ the original resource */
  gasneti_assert(nbytes <= GASNETC_BUFSZ);

  orig_sreq->fh_bbuf = gasnetc_get_bbuf(1 GASNETI_THREAD_PASS);
  GASNETI_MEMCPY(orig_sreq->fh_bbuf, (void *)src, nbytes);
  if_pf (orig_sreq->fh_lc_cb) orig_sreq->fh_lc_cb(orig_sreq->fh_lc); /* locally complete */

  sr_desc->opcode      = IBV_WR_RDMA_WRITE;
  sr_desc->num_sge     = 1;
  sr_desc->wr.rdma.remote_addr = dst;
  sr_desc->sg_list[0].addr = (uintptr_t)orig_sreq->fh_bbuf;
  sr_desc->sg_list[0].length  = nbytes;

  cep = gasnetc_bind_cep(ep, epid, orig_sreq);
  sr_desc->wr.rdma.rkey = GASNETC_FH_RKEY(cep, fh_rem);
  sr_desc->sg_list[0].lkey = GASNETC_SND_LKEY(cep);

  gasnetc_snd_post(orig_sreq, sr_desc);
}

GASNETI_INLINE(gasnetc_fh_post)
void gasnetc_fh_post(gasnetc_sreq_t *sreq, enum ibv_wr_opcode op GASNETI_THREAD_FARG) {
  GASNETC_DECL_SR_DESC(sr_desc, GASNETC_SND_SG);
  struct ibv_sge *sg_entry;
  gasnetc_cep_t *cep;
  uintptr_t loc_addr;
  size_t remain;
  int i;

  gasneti_assert_int(sreq->fh_count ,>=, 2);
  gasneti_assert_int(sreq->fh_count ,<=, GASNETC_MAX_FH);
  gasneti_assert(sreq->fh_ptr[0] != NULL);
  gasneti_assert(sreq->fh_ptr[1] != NULL);

  sr_desc->opcode = op;
  sr_desc->wr.rdma.remote_addr = sreq->fh_rem_addr;
  sr_desc->num_sge = sreq->fh_count - 1;

  remain = sreq->fh_len;
  loc_addr = sreq->fh_loc_addr;
  sg_entry = sr_desc->sg_list;

  cep = gasnetc_bind_cep(sreq->fh_ep, sreq->epid, sreq);
  sr_desc->wr.rdma.rkey = GASNETC_FH_RKEY(cep, sreq->fh_ptr[0]);

  for (i = 1; i < sreq->fh_count; ++i) {
    const firehose_request_t *fh_req = sreq->fh_ptr[i];
    uintptr_t next = fh_req->addr + fh_req->len;
    size_t nbytes = MIN(remain, (next - loc_addr));

    gasneti_assert(loc_addr < next);
    gasneti_assert(remain > 0);
    gasneti_assert(nbytes > 0);

    sg_entry->addr = loc_addr;
    sg_entry->length = nbytes;
    sg_entry->lkey = GASNETC_FH_LKEY(cep, fh_req);

    ++sg_entry;
    remain -= nbytes;
    loc_addr += nbytes;
  }
  gasneti_assert(remain == 0);

  gasnetc_snd_post(sreq, sr_desc);
}

static void gasnetc_fh_do_put(gasnetc_sreq_t *sreq GASNETI_THREAD_FARG) {
  gasnetc_counter_t * const am_oust = sreq->fh_oust;

  switch (sreq->opcode) {
    case GASNETC_OP_PUT_INMOVE:
      /* All done in the AM.  Complete the sreq here since snd_reap will never see it. */
      gasneti_assert(sreq->fh_len == 0);
      if (sreq->comp.cb != NULL) {
        sreq->comp.cb(sreq->comp.data);
      }
      gasneti_assert_int(sreq->fh_count ,>, 0);
      firehose_release(sreq->fh_ptr, sreq->fh_count);
      sreq->opcode = GASNETC_OP_FREE;
      break;

    case GASNETC_OP_PUT_INLINE:
      gasneti_assert(sreq->fh_len > 0);
      GASNETI_TRACE_EVENT_VAL(C, RDMA_PUT_INLINE, sreq->fh_len);
      gasnetc_fh_put_inline(sreq GASNETI_THREAD_PASS);
      break;

    case GASNETC_OP_PUT_BOUNCE:
    case GASNETC_OP_LONG_BOUNCE:
      gasneti_assert(sreq->fh_len > 0);
      GASNETI_TRACE_EVENT_VAL(C, RDMA_PUT_BOUNCE, sreq->fh_len);
      gasnetc_fh_put_bounce(sreq GASNETI_THREAD_PASS);
      break;

    case GASNETC_OP_PUT_ZEROCP:
    case GASNETC_OP_LONG_ZEROCP:
      gasneti_assert(sreq->fh_len > 0);
      GASNETI_TRACE_EVENT_VAL(C, RDMA_PUT_ZEROCP, sreq->fh_len);
      gasnetc_fh_post(sreq, IBV_WR_RDMA_WRITE GASNETI_THREAD_PASS);
      break;

    default: gasneti_unreachable_error(("Invalid opcode in gasnetc_fh_do_put sreq: 0x%x",(int)sreq->opcode));
  }

  gasnetc_counter_dec_if_pf(am_oust);
}

#define gasnetc_sreq_is_ready(sreq) \
  gasnetc_atomic_decrement_and_test(&((sreq)->fh_ready), GASNETI_ATOMIC_REL|GASNETI_ATOMIC_ACQ)

static void gasnetc_fh_put_cb(void *context, const firehose_request_t *fh_rem, int allLocalHit) {
  gasnetc_sreq_t *sreq = context;

  gasneti_assert(fh_rem != NULL);
  sreq->fh_ptr[0] = fh_rem;

  if (gasnetc_sreq_is_ready(sreq)) {
    GASNET_BEGIN_FUNCTION(); // TODO: THREAD_FARG for firehose callbacks?
    gasnetc_fh_do_put(sreq GASNETI_THREAD_PASS);
  }
}

static void gasnetc_fh_do_get(gasnetc_sreq_t *sreq GASNETI_THREAD_FARG) {
  GASNETI_TRACE_EVENT_VAL(C, RDMA_GET_ZEROCP, sreq->fh_len);
  gasnetc_fh_post(sreq, IBV_WR_RDMA_READ GASNETI_THREAD_PASS);
}

static void gasnetc_fh_get_cb(void *context, const firehose_request_t *fh_rem, int allLocalHit) {
  gasnetc_sreq_t *sreq = context;

  sreq->fh_ptr[0] = fh_rem;

  if (gasnetc_sreq_is_ready(sreq)) {
    GASNET_BEGIN_FUNCTION(); // TODO: THREAD_FARG for firehose callbacks?
    gasnetc_fh_do_get(sreq GASNETI_THREAD_PASS);
  }

  gasneti_assert(sreq->fh_oust == NULL);
}

GASNETI_INLINE(gasnetc_get_local_fh)
size_t gasnetc_get_local_fh(gasnetc_sreq_t *sreq, uintptr_t loc_addr, size_t len) {
  size_t remain;
  int i;

  gasneti_assert(len != 0);

  for (i = 1, remain = len; (remain && (i < GASNETC_MAX_FH)); ++i) {
    const firehose_request_t *fh_loc = gasnetc_fh_try_local_pin(loc_addr, 1);
    if (!fh_loc) {
      break;
    } else {
      size_t nbytes = MIN(remain, (fh_loc->addr + fh_loc->len - loc_addr));
      sreq->fh_ptr[i] = fh_loc;
      remain -= nbytes;
      loc_addr += nbytes;
    }
  }
  if (i > 1) {
    sreq->fh_count = i;
    len -= remain;
  } else {
    // TODO-EX: ODP support for segment everything?
    const firehose_request_t *fh_loc = gasnetc_fh_aligned_local_pin(loc_addr, len);
    if_pt (fh_loc) {
      len = MIN(remain, (fh_loc->addr + fh_loc->len - loc_addr));
      sreq->fh_ptr[1] = fh_loc;
      sreq->fh_count = 2;
    } else {
      // Attempt to xfer un-pinnable memory such as due to bug 3338
      GASNETI_TRACE_EVENT_VAL(C, RDMA_PUT_READONLY, len);
      len = 0;
      sreq->fh_count = 1; // remote only
    }
  }

  return len;
}

static size_t gasnetc_fh_put_args_fn(void * context, firehose_remotecallback_args_t *args) {
    gasnetc_sreq_t *sreq = context;
    const size_t len = MIN(gasnetc_putinmove_limit, sreq->fh_len);

    args->addr = (void *)(sreq->fh_rem_addr);
    sreq->fh_putinmove = args->len = len;
    GASNETI_MEMCPY(args->data, (void *)(sreq->fh_loc_addr), len);

    return gasneti_offsetof(firehose_remotecallback_args_t, data[len]);
}

GASNETI_INLINE(gasnetc_fh_put_helper)
size_t gasnetc_fh_put_helper(
                gasnetc_EP_t ep, gasnetc_epid_t epid,
                gasnetc_sreq_t *sreq,
                gasnetc_atomic_val_t *local_cnt,
                gasnetc_atomic_val_t *remote_cnt,
                uintptr_t loc_addr,
                uintptr_t rem_addr,
                size_t len
                GASNETI_THREAD_FARG) {
  const gex_Rank_t node = gasnetc_epid2node(epid);
  const int is_long_payload = gasnetc_epid2qpi(epid); // non-zero qpi identifies Long payloads
  const firehose_request_t *fh_rem;
  size_t putinmove = sreq->fh_putinmove = 0;

  sreq->fh_rem_addr = rem_addr;
  sreq->fh_loc_addr = loc_addr;
  sreq->fh_ep = ep;

  /* See how much (if any) is already pinned.  A call to firehose_partial_remote_pin()
   * might acquire a firehose for a region starting above rem_addr.  By instead calling
   * firehose_try_remote_pin() with len==1, we get a *contiguous* firehose if available.
   * We count on the implementation of firehose region giving out the largest region
   * that covers our request.
   */
  fh_rem = firehose_try_remote_pin(node, rem_addr, 1, 0, NULL);

  if_pt (fh_rem != NULL) {
    /* HIT in remote firehose table - some initial part of the region is pinned */
    sreq->fh_ptr[0] = fh_rem;
    gasneti_assert(rem_addr >= fh_rem->addr);
    gasneti_assert(rem_addr <= (fh_rem->addr + fh_rem->len - 1));
    len = sreq->fh_len = MIN(len, (fh_rem->addr + fh_rem->len - rem_addr));
    sreq->fh_oust = NULL; /* No asynchrony on a HIT */
  } else {
    /* MISS: Some initial part (or all) of the region is unpinned */
    uint32_t flags = 0;
    firehose_remotecallback_args_fn_t args_fn = NULL;
    gasnetc_atomic_set(&sreq->fh_ready, 2, 0);
    len = sreq->fh_len = gasnetc_fh_aligned_len(rem_addr, len);
    if (len <= gasnetc_putinmove_limit_adjusted) {
      /* Put-in-move optimization used only if the entire xfer can be
       * piggybacked, or if the remainder fits in an inline.
       */
      flags = FIREHOSE_FLAG_ENABLE_REMOTE_CALLBACK;
      args_fn = &gasnetc_fh_put_args_fn;
    }
    (void)firehose_remote_pin(node, rem_addr, len, flags, NULL,
			      args_fn, &gasnetc_fh_put_cb, sreq);
    putinmove = sreq->fh_putinmove;
    if (putinmove) {
      GASNETI_TRACE_EVENT_VAL(C, RDMA_PUT_IN_MOVE, putinmove);
    }
    gasnetc_counter_inc_if(sreq->fh_oust);
  }

  /* If the original request can be completed entirely w/o use of
   * local firehoses then do so.  We don't worry about anything
   * done by the put-in-move optimization, under the assumption that
   * the original request len is representative of future requests.
   */
  int is_nonbulk = (sreq->fh_lc_cb == gasnetc_cb_counter); // GEX_EVENT_NOW
  if ((len <= gasnetc_inline_limit) ||
	(is_nonbulk && (len <= gasnetc_nonbulk_bounce_limit))) {
    sreq->fh_count = 1; /* Just the remote one */
  } else {
    size_t new_len = gasnetc_get_local_fh(sreq, loc_addr, len);
    if_pf (!new_len) {
      // Failed to register memory, such as for read-only memory (bug 3338)
      // So, use bounce buffers
      sreq->opcode = is_long_payload ? GASNETC_OP_LONG_BOUNCE : GASNETC_OP_PUT_BOUNCE;
      if_pf (fh_rem == NULL) { /* Memory will be copied asynchronously */
	if (local_cnt) ++(*local_cnt);
      } else { /* Memory will be copied synchronously before return */
	sreq->fh_lc_cb = NULL;
      }
      if (remote_cnt != NULL) {
	++(*remote_cnt);
      }
      goto ready_check; // skip the normal protocol options
    }
    len = new_len;
  }

  if_pf (len <= putinmove) {
    /* AM is carrying at least as much as we could pin locally */
    len = putinmove;
    sreq->fh_len = 0;
    sreq->opcode = GASNETC_OP_PUT_INMOVE;
    if (remote_cnt != NULL) {
	++(*remote_cnt);
    }
  } else {
    /* Adjust sreq for len (which may have been reduced for local alignment)
     * and for any data piggybacked on the AM (if any).
     */
    size_t nbytes = len - putinmove; 

    sreq->fh_len = nbytes;
    sreq->fh_rem_addr += putinmove;
    sreq->fh_loc_addr += putinmove;

    if (nbytes <= gasnetc_inline_limit) {
      /* Inline when small enough */
      sreq->opcode = GASNETC_OP_PUT_INLINE;
      if_pf (fh_rem == NULL) { /* Memory will be copied asynchronously */
	if (local_cnt) ++(*local_cnt);
      } else { /* Memory will be copied synchronously before return */
	sreq->fh_lc_cb = NULL;
      }
      if (remote_cnt != NULL) {
	++(*remote_cnt);
      }
    } else if (is_nonbulk && (nbytes <= gasnetc_nonbulk_bounce_limit)) {
      /* Bounce buffer use for non-bulk puts (upto a limit) */
      sreq->opcode = is_long_payload ? GASNETC_OP_LONG_BOUNCE : GASNETC_OP_PUT_BOUNCE;
      if_pf (fh_rem == NULL) { /* Memory will be copied asynchronously */
	++(*local_cnt);
      } else { /* Memory will be copied synchronously before return */
	sreq->fh_lc_cb = NULL;
      }
      if (remote_cnt != NULL) {
	++(*remote_cnt);
      }
    } else {
      /* Use the local firehose(s) obtained earlier */
      sreq->opcode = is_long_payload ? GASNETC_OP_LONG_ZEROCP : GASNETC_OP_PUT_ZEROCP;
      /* The init or the sync (or neither) might wait on completion, but never both */
      if (local_cnt != NULL) {
	++(*local_cnt);
	sreq->comp.data = local_cnt;
	sreq->comp.cb = sreq->fh_lc_cb;
      } else if (remote_cnt != NULL) {
	++(*remote_cnt);
      }
    }
  }
  gasneti_assert(sreq->opcode != GASNETC_OP_INVALID);

ready_check:
  if ((fh_rem != NULL) || gasnetc_sreq_is_ready(sreq)) {
    gasnetc_fh_do_put(sreq GASNETI_THREAD_PASS);
  }

  gasneti_assert(len >= putinmove);
  gasneti_assert(len > 0);
  return len;
}

GASNETI_INLINE(gasnetc_fh_get_helper)
size_t gasnetc_fh_get_helper(gasnetc_EP_t ep, gasnetc_epid_t epid,
                             gasnetc_sreq_t *sreq,
                             uintptr_t loc_addr, uintptr_t rem_addr,
                             size_t len, gasnetc_atomic_val_t *remote_cnt
                             GASNETI_THREAD_FARG) {
  const size_t orig_len = len;
  const gex_Rank_t node = gasnetc_epid2node(epid);
  const firehose_request_t *fh_rem;

  sreq->fh_rem_addr = rem_addr;
  sreq->fh_loc_addr = loc_addr;
  sreq->fh_ep = ep;

  /* See how much (if any) is already pinned.  A call to firehose_partial_remote_pin()
   * might acquire a firehose for a region starting above rem_addr.  By instead calling
   * firehose_try_remote_pin() with len==1, we get a *contiguous* firehose if available.
   * We count on the implementation of firehose region giving out the largest region
   * that covers our request.
   */
  fh_rem = firehose_try_remote_pin(node, rem_addr, 1, 0, NULL);

  if_pt (fh_rem != NULL) {
    /* HIT in remote firehose table - some initial part of the region is pinned */
    sreq->fh_ptr[0] = fh_rem;
    gasneti_assert(rem_addr >= fh_rem->addr);
    gasneti_assert(rem_addr <= (fh_rem->addr + fh_rem->len - 1));
    len = MIN(len, (fh_rem->addr + fh_rem->len - rem_addr));
  } else {
    /* MISS: Some initial part (or all) of the region is unpinned */
    gasnetc_atomic_set(&sreq->fh_ready, 2, 0);
    len = gasnetc_fh_aligned_len(rem_addr, len);
    (void)firehose_remote_pin(node, rem_addr, len, 0, NULL,
			      NULL, &gasnetc_fh_get_cb, sreq);
  }

  len = sreq->fh_len = gasnetc_get_local_fh(sreq, loc_addr, len);
  if_pf (!len) {
    // TODO: idealy we could retry memory registration to tolerate transient read-only
    // status as may occur with some tools which play games with protections to get signals.
    gasneti_fatalerror("Attempt to GET into non-writable memory at %p\n", (void *)loc_addr);
  }

  if (len != orig_len) ++(*remote_cnt); // Do NOT advance prior to the last injection

  if ((fh_rem != NULL) || gasnetc_sreq_is_ready(sreq)) {
    gasnetc_fh_do_get(sreq GASNETI_THREAD_PASS);
  }

  gasneti_assert(len > 0);
  return len;
}
#endif

GASNETI_INLINE(idiv_round_up)
int idiv_round_up(int numerator, int denominator) {
  return (numerator + denominator - 1) / denominator;
}

/* ------------------------------------------------------------------------------------ *
 *  Externally visible functions                                                        *
 * ------------------------------------------------------------------------------------ */

/*
 * Check/compute limits before allocating anything
 */
extern int gasnetc_sndrcv_limits(void) {
  gasnetc_hca_t		*hca;
  int 			h;

  { // How many threads do we expect may execute handlers concurrently?
    int rcv_thread = (gasnetc_use_rcv_thread ? 1 : 0); // AM recv thread
#if GASNET_PAR
    // Guess based on (cpus / procs), rounded up.
    int cpus = gasneti_cpu_count();
    int procs = gasneti_myhost.node_count;
    int client_threads = (cpus + procs - 1) / procs;
#else
    // At most 1 client thread *in GASNet*
    int client_threads = 1;
#endif
    int threads = rcv_thread + client_threads;
    gasnetc_rbuf_spares = MAX(1,gasneti_getenv_int_withdefault("GASNET_RBUF_SPARES", threads, 0));
  }

  // Count normal qps to be placed on each HCA
  // XXX: this logic depends on the current gasnetc_select_port() logic;
  // in particular on the simple node-independent repetition of ports.
  for (int i = 0; i < gasnetc_num_qps; ++i) {
      hca = &gasnetc_hca[gasnetc_port_tbl[i % gasnetc_num_ports].hca_index];
      hca->qps += 1;
      hca->max_qps += gasneti_nodes;
  }

  /* Ops outstanding per peer and total: */
  if (gasnetc_op_oust_limit == 0) { /* 0 = automatic limit computation */
    gasnetc_op_oust_per_qp = INT_MAX;
    GASNETC_FOR_ALL_HCA_INDEX(h) {
      gasneti_atomic_val_t tmp = MIN(GASNETI_ATOMIC_MAX, gasnetc_hca[h].hca_cap.max_cqe);
      gasnetc_op_oust_per_qp = MIN(gasnetc_op_oust_per_qp, (tmp / gasnetc_hca[h].qps));
    }
  } else {
    gasnetc_op_oust_per_qp = idiv_round_up(MIN(GASNETI_ATOMIC_MAX, gasnetc_op_oust_limit), gasnetc_num_qps);
    GASNETC_FOR_ALL_HCA(hca) {
      int tmp = hca->qps * gasnetc_op_oust_per_qp;
      if (tmp > hca->hca_cap.max_cqe) {
        GASNETI_RETURN_ERRR(RESOURCE, "GASNET_NETWORKDEPTH_{PP,TOTAL} exceed HCA capabilities");
      }
    }
  }
  gasnetc_op_oust_pp = idiv_round_up(gasnetc_op_oust_pp, gasnetc_num_qps);
  gasnetc_op_oust_per_qp = MIN(gasnetc_op_oust_per_qp, gasnetc_op_oust_pp*gasneti_nodes);
  gasnetc_op_oust_limit = gasnetc_num_qps * gasnetc_op_oust_per_qp;
  GASNETI_TRACE_PRINTF(I, ("Final/effective GASNET_NETWORKDEPTH_TOTAL = %d", gasnetc_op_oust_limit));

  /* AM recv buffer allocation.
   * There are 3 roles a rcv buffer might fill (counts per HCA):
   * (1) (gasnetc_num_hcas * gasnetc_rbuf_spares) used in recv path to post before processing
   * (2) (gasnetc_am_oust_pp * hca->max_qps) used to catch Requests
   * (3) (gasnetc_am_oust_pp * hca->max_qps) used to catch Replies
   * However distribution over QPs and SRQ may each reduce the second two.
   */
  gasnetc_am_oust_pp = idiv_round_up(gasnetc_am_oust_pp, gasnetc_num_qps);
  gasnetc_am_rqst_per_qp = gasnetc_am_oust_pp * (gasneti_nodes - 1);

  // Compute gasnetc_am_oust_pp (and report GASNET_AM_CREDITS_PP)
  GASNETC_FOR_ALL_HCA(hca) {
    int tmp = hca->hca_cap.max_cqe - gasnetc_rbuf_spares;
    tmp = idiv_round_up(tmp, 2 * hca->qps); // Remainder to be split between Request and Reply, spread over the qps
    gasnetc_am_rqst_per_qp = MIN(gasnetc_am_rqst_per_qp, tmp);
  }
  gasnetc_am_oust_pp = idiv_round_up(gasnetc_am_rqst_per_qp, MAX(1, (gasneti_nodes - 1)));
  GASNETI_TRACE_PRINTF(I, ("Final/effective GASNET_AM_CREDITS_PP = %d", gasnetc_am_oust_pp * gasnetc_num_qps));

  // Compute gasnetc_am_oust_limit (and report GASNET_AM_CREDITS_TOTAL)
  if (gasnetc_am_oust_limit == 0) { // automatic limit computations.
    gasnetc_am_oust_limit = MIN(gasnetc_op_oust_limit, gasnetc_num_qps * gasnetc_am_rqst_per_qp);
  } else {
    gasnetc_am_oust_limit = MIN(gasnetc_am_oust_limit, gasnetc_op_oust_limit);
    if (gasnetc_am_oust_limit > gasnetc_num_qps * gasnetc_am_rqst_per_qp) {
      GASNETI_RETURN_ERRR(RESOURCE, "GASNET_AM_CREDITS_{PP,TOTAL} exceed HCA capabilities");
    }
  }
  GASNETI_TRACE_PRINTF(I, ("Final/effective GASNET_AM_CREDITS_TOTAL = %d", gasnetc_am_oust_limit));

  // Ensure credit coallescing can't deadlock a Request (bug 1418)
  gasnetc_am_credits_slack = MIN(gasnetc_am_credits_slack, gasnetc_am_oust_pp - 1);
  gasnetc_am_credits_slack = MIN(gasnetc_am_credits_slack, 256);
  GASNETI_TRACE_PRINTF(I, ("Final/effective GASNET_AM_CREDITS_SLACK = %d", gasnetc_am_credits_slack));

  if (gasnetc_bbuf_limit == 0) { /* 0 = automatic limit computation */
    /* We effectively count local AMs against gasnetc_op_oust_limit for simplicity,
     * but only expect one in-flight per thread anyway. */
    gasnetc_bbuf_limit = gasnetc_op_oust_limit;
  } else {
    gasnetc_bbuf_limit = MIN(gasnetc_bbuf_limit, gasnetc_op_oust_limit);
  }
  /* SRQ may raise this.  So, report is deferred. */

  const int rcv_spares = gasnetc_num_hcas * gasnetc_rbuf_spares;
#if GASNETC_IBV_SRQ
  if (gasnetc_use_srq) {
    unsigned int srq_wr_per_qp = (gasnetc_rbuf_limit - rcv_spares) / gasnetc_num_qps;
    int orig = gasnetc_rbuf_limit;
    int tmp;

    /* Ensure each path has some reasonable miniumum.
     * Since this is not scaled w/ nodes it could safely be much larger than this.
     */
    const int min_wr_per_qp = 2;
    if (srq_wr_per_qp && (srq_wr_per_qp < min_wr_per_qp)) {
      srq_wr_per_qp = min_wr_per_qp;
      gasneti_console_message(
              "WARNING","Requested GASNET_RBUF_COUNT %d increased to %d",
              orig, gasnetc_num_qps * srq_wr_per_qp + rcv_spares);
    }

    /* Check against HCA limits */
    GASNETC_FOR_ALL_HCA(hca) {
      unsigned int tmp = hca->hca_cap.max_srq_wr / hca->qps;
      if (!srq_wr_per_qp || (tmp < srq_wr_per_qp)) {
        srq_wr_per_qp = tmp;
      }
    }
    gasnetc_rbuf_limit = gasnetc_num_qps * srq_wr_per_qp + rcv_spares;

    /* Warn only if reduced relative to an explicit  non-zero value */
    if (gasnetc_rbuf_set && orig && (gasnetc_rbuf_limit < orig)) {
      gasneti_console_message(
              "WARNING","Requested GASNET_RBUF_COUNT %d reduced by HCA's max_srq_wr to %d",
              orig, gasnetc_rbuf_limit);
    }

    /* As per README:
       GASNET_USE_SRQ < 0: Use SRQ only if memory savings would result
       TODO: The "rbuf_spares" may be unused when SRQ is active?
     */
    tmp = MIN(gasnetc_am_rqst_per_qp, srq_wr_per_qp/2);  // Half of non-spares
    gasneti_assert(gasnetc_rbuf_limit != 0);
    GASNETI_TRACE_PRINTF(I, ("Final/effective GASNET_RBUF_COUNT = %d (SRQ limit: %d, w/o SRQ: %d)",
                             2 * tmp * gasnetc_num_qps + rcv_spares,
                             gasnetc_rbuf_limit,
                             2 * gasnetc_am_rqst_per_qp * gasnetc_num_qps + rcv_spares));
    if ((gasnetc_use_srq < 0) && (tmp == gasnetc_am_rqst_per_qp)) {
      GASNETI_TRACE_PRINTF(I, ("SRQ disabled because GASNET_USE_SRQ = -1 and no buffer savings would result"));
      gasnetc_use_srq = 0;
    } else {
      GASNETI_TRACE_PRINTF(I, ("SRQ enabled"));
      gasnetc_am_rqst_per_qp = tmp;
      gasnetc_use_srq = 1;
      gasnetc_am_credits_slack = 0;
      /* Need to ensure some BBUFs avail even if max number of AM Requests are all blocked */
      gasnetc_bbuf_limit = MAX(gasnetc_bbuf_limit, MIN(64, gasnetc_op_oust_limit) + gasnetc_am_oust_limit);
    }
  } else {
    GASNETI_TRACE_PRINTF(I, ("Final/effective GASNET_RBUF_COUNT = %d",
                             2 * gasnetc_am_rqst_per_qp * gasnetc_num_qps + rcv_spares));
    GASNETI_TRACE_PRINTF(I, ("SRQ disabled"));
    gasnetc_use_srq = 0;
  }
  /* gasnetc_use_srq is just 0 or 1 from here on */
#else
  GASNETI_TRACE_PRINTF(I, ("Final/effective GASNET_RBUF_COUNT = %d",
                           2 * gasnetc_am_rqst_per_qp * gasnetc_num_qps + rcv_spares));
#endif
  GASNETI_TRACE_PRINTF(I, ("Final/effective GASNET_BBUF_COUNT = %d", gasnetc_bbuf_limit));
  gasnetc_am_credits_slack_orig = gasnetc_am_credits_slack;

  gasnetc_alloc_qps = gasnetc_num_qps; /* Default w/o SRQ or XRC */
#if GASNETC_IBV_SRQ
  if (gasnetc_use_srq) {
    gasnetc_alloc_qps = 2 * gasnetc_num_qps;
    GASNETC_FOR_ALL_HCA(hca) {
      hca->max_qps *= 2;
    }
  }
 #if GASNETC_IBV_XRC
  else if (gasnetc_use_xrc) {
    /* No SRQ means no XRC either */
    gasnetc_use_xrc = 0;
  }
  GASNETI_TRACE_PRINTF(I, ("XRC %sabled", gasnetc_use_xrc ? "en" : "dis"));
 #endif
#endif

  /* sanity/bounds checks */
  GASNETC_FOR_ALL_HCA(hca) {
    const unsigned int max_qp = hca->hca_cap.max_qp;
    const unsigned int max_qp_wr = hca->hca_cap.max_qp_wr;

    if_pf (hca->max_qps > max_qp) {
      GASNETI_RETURN_ERRR(RESOURCE, "job size exceeds HCA capabilities");
    }
    if_pf (gasnetc_am_oust_pp * 2 > max_qp_wr) {
      GASNETI_RETURN_ERRR(RESOURCE, "GASNET_AM_CREDITS_PP exceeds HCA capabilities");
    }
    if_pf (gasnetc_op_oust_pp > max_qp_wr) {
      GASNETI_RETURN_ERRR(RESOURCE, "GASNET_NETWORKDEPTH_PP exceeds HCA capabilities");
    }
  }

  return GASNET_OK;
}

#if GASNETC_IBV_SRQ
static struct ibv_srq * gasnetc_create_srq(gasnetc_hca_t *hca, const int max_wr) {
  struct ibv_srq *result = NULL;
  #if GASNETC_IBV_XRC_OFED
    if (gasnetc_use_xrc) {
      struct ibv_srq_init_attr_ex attr;
      memset(&attr, 0, sizeof(struct ibv_srq_init_attr_ex));
      attr.attr.max_wr = max_wr;
      attr.attr.max_sge = 1;
      attr.comp_mask = IBV_SRQ_INIT_ATTR_TYPE | IBV_SRQ_INIT_ATTR_XRCD |
                       IBV_SRQ_INIT_ATTR_CQ | IBV_SRQ_INIT_ATTR_PD;
      attr.srq_type = IBV_SRQT_XRC;
      attr.xrcd = hca->xrc_domain;
      attr.cq = hca->rcv_cq;
      attr.pd = hca->pd;
      result = ibv_create_srq_ex(hca->handle, &attr);
    } else
  #elif GASNETC_IBV_XRC_MLNX
    if (gasnetc_use_xrc) {
      struct ibv_srq_init_attr attr;
      memset(&attr, 0, sizeof(attr));
      attr.attr.max_wr = max_wr;
      attr.attr.max_sge = 1;
      result = ibv_create_xrc_srq(hca->pd, hca->xrc_domain, hca->rcv_cq, &attr);
    } else
  #endif
    {
      struct ibv_srq_init_attr attr;
      memset(&attr, 0, sizeof(attr));
      attr.attr.max_wr = max_wr;
      attr.attr.max_sge = 1;
      result = ibv_create_srq(hca->pd, &attr);
    }
    return result;
}
#endif

extern int gasnetc_sndrcv_init(gasnetc_EP_t ep) {
  gasnetc_hca_t		*hca;
  int	act_size;
  int			vstat;
  gasnetc_buffer_t	*buf;
  gasnetc_rbuf_t	*rbuf;
  int 			padded_size, h, i;
  size_t		size;
  int			ud_rcvs = 0;

#if GASNETC_DYNAMIC_CONNECT
  /* Default to handling 4 + 2*lg(remote_nodes) incomming UD requests and 4 outgoing */
  gasnetc_ud_rcvs = 1;
  while ((1 << gasnetc_ud_rcvs) < (int)gasneti_nodes) {
    ++gasnetc_ud_rcvs;
  }
  gasnetc_ud_rcvs = 4 + 2 * gasnetc_ud_rcvs;
  gasnetc_ud_snds = 4;
  gasnetc_ud_rcvs = gasneti_getenv_int_withdefault("GASNET_CONNECT_RCVS", gasnetc_ud_rcvs, 0);
  gasnetc_ud_snds = gasneti_getenv_int_withdefault("GASNET_CONNECT_SNDS", gasnetc_ud_snds, 0);
  GASNETI_TRACE_PRINTF(I, ("Buffers for dynamic connections: rcv=%d snd=%d",
                            gasnetc_ud_rcvs, gasnetc_ud_snds));
  #if !GASNETC_USE_CONN_THREAD
  ud_rcvs = gasnetc_ud_rcvs;
  #endif
#endif

  /*
   * setup RCV resources
   */

  /* create one RCV CQ per HCA */
  GASNETC_FOR_ALL_HCA(hca) {
    const int rcv_count = 2 * gasnetc_am_rqst_per_qp * hca->qps + gasnetc_rbuf_spares;
    const int cqe_count = rcv_count + (!hca->hca_index ? ud_rcvs : 0);
    gasnetc_progress_thread_t *rcv_thread = NULL;
  #if GASNETC_USE_RCV_THREAD
    if (gasnetc_use_rcv_thread) rcv_thread = &hca->rcv_thread;
  #endif
    vstat = gasnetc_create_cq(hca->handle, cqe_count, &hca->rcv_cq, &act_size, rcv_thread);
    GASNETC_IBV_CHECK(vstat, "from gasnetc_create_cq(rcv_cq)");
    GASNETI_TRACE_PRINTF(I, ("Recv CQ length: requested=%d actual=%d", (int)cqe_count, (int)act_size));
    gasneti_assert(act_size >= cqe_count);
    /* We don't set rcv_count = act_size here, as that could nearly double the memory allocated below */

    if (gasneti_nodes > 1) {
      /* Allocated pinned memory for receive buffers */
      size = GASNETI_PAGE_ALIGNUP(rcv_count * sizeof(gasnetc_buffer_t));
      buf = gasnetc_mmap(size);
      if_pf (buf == GASNETC_MMAP_FAILED) {
        buf = NULL;
      } else {
        vstat = gasnetc_pin(hca, buf, size, IBV_ACCESS_LOCAL_WRITE, &hca->rcv_reg);
        if (vstat != 0) {
	  gasnetc_munmap(buf, size);
          buf = NULL;
        }
      }
      if_pf (buf == NULL) {
        GASNETI_RETURN_ERRR(RESOURCE, "Unable to allocate pinned memory for AM recv buffers");
      }

#if GASNETC_IBV_SRQ
      /* create two shared recv queues per HCA */
      if (gasnetc_use_srq) {
        const int rbuf_count = hca->qps * gasnetc_am_rqst_per_qp;

        hca->rqst_srq = gasnetc_create_srq(hca, rbuf_count);
        GASNETC_IBV_CHECK_PTR(hca->rqst_srq, "from gasnetc_create_srq(Request)");

        hca->repl_srq = gasnetc_create_srq(hca, rbuf_count);
        GASNETC_IBV_CHECK_PTR(hca->repl_srq, "from gasnetc_create_srq(Reply)");

        gasnetc_sema_init(&hca->am_sema, rbuf_count, rbuf_count);
      }
#endif
  
      /* Allocated normal memory for receive descriptors (rbuf's) */
      padded_size = GASNETI_ALIGNUP(sizeof(gasnetc_rbuf_t), GASNETI_CACHE_LINE_BYTES);
      if (NULL == hca->rbufs) {
        hca->rbufs = gasneti_malloc_aligned(GASNETI_CACHE_LINE_BYTES, rcv_count*padded_size);
        gasneti_leak_aligned(hca->rbufs);
      }
  
      /* Initialize the rbuf's */
      gasnetc_lifo_init(&hca->rbuf_freelist);
      rbuf = hca->rbufs;
      for (i = 0; i < rcv_count; ++i) {
        rbuf->rr_desc.num_sge    = 1;
        rbuf->rr_desc.sg_list    = &rbuf->rr_sg;
        rbuf->rr_desc.wr_id      = (uintptr_t)rbuf;	/* CQE will point back to this request */
        rbuf->rr_desc.next       = NULL;
        rbuf->rr_sg.length       = GASNETC_BUFSZ;
        rbuf->rr_sg.addr         = (uintptr_t)&buf[i];
        rbuf->rr_ep              = ep;
        gasnetc_lifo_push(&hca->rbuf_freelist, rbuf);
  
        rbuf = (gasnetc_rbuf_t *)((uintptr_t)rbuf + padded_size);
      }
#if GASNETC_USE_RCV_THREAD
      if (gasnetc_use_rcv_thread) {
        hca->rcv_thread_priv = gasnetc_lifo_pop(&hca->rbuf_freelist);
        gasneti_assert(hca->rcv_thread_priv != NULL);
      }
#endif
#if GASNETC_SERIALIZE_POLL_CQ
      gasnetc_atomic_set(&hca->poll_cq_semas.snd,0,0);
      gasnetc_atomic_set(&hca->poll_cq_semas.rcv,0,0);
#endif
    }
  }

  /*
   * setup SND resources
   */

  /* create the SND CQ and associated semaphores */
  if (NULL == gasnetc_cq_semas) {
    gasnetc_cq_semas = (gasnetc_sema_t *)
	  gasneti_malloc_aligned(GASNETI_CACHE_LINE_BYTES, gasnetc_num_hcas*sizeof(gasnetc_sema_t));
    gasneti_leak_aligned(gasnetc_cq_semas);
  }
  gasnetc_op_oust_per_qp = MAX(1, gasnetc_op_oust_per_qp); /* Avoid error in single-node case */
  GASNETC_FOR_ALL_HCA(hca) {
    const int rqst_count = gasnetc_use_srq ? gasnetc_am_rqst_per_qp : 0;
    const int cqe_count = hca->qps * (gasnetc_op_oust_per_qp + rqst_count);
    gasnetc_progress_thread_t *snd_thread = NULL;
  #if GASNETC_USE_SND_THREAD
    if (gasnetc_use_snd_thread) snd_thread = &hca->snd_thread;
  #endif
    vstat = gasnetc_create_cq(hca->handle, cqe_count, &hca->snd_cq, &act_size, snd_thread);
    GASNETC_IBV_CHECK(vstat, "from gasnetc_create_cq(snd_cq)");
    GASNETI_TRACE_PRINTF(I, ("Send CQ length: requested=%d actual=%d", (int)cqe_count, (int)act_size));
    gasneti_assert(act_size >= cqe_count);
    /* We use actual size here, since the memory has been allocated anyway */
    gasnetc_sema_init(&gasnetc_cq_semas[hca->hca_index], act_size, act_size);
    hca->snd_cq_sema_p = &gasnetc_cq_semas[hca->hca_index];
  }

  /* Allocated pinned memory for AMs and bounce buffers
   * TODO: Can/should we *USE* any extra allocated due to rounding-up? */
 if (gasnetc_bbuf_limit) {
  gasneti_assert((gasnetc_use_srq == 0) || (gasnetc_use_srq == 1)); // SRQ requires one extra
  size = GASNETI_PAGE_ALIGNUP((gasnetc_use_srq + gasnetc_bbuf_limit) * sizeof(gasnetc_buffer_t));
  buf = gasnetc_mmap(size);
  if_pf (buf == GASNETC_MMAP_FAILED) {
    buf = NULL;
  } else {
    GASNETC_FOR_ALL_HCA_INDEX(h) {
      vstat = gasnetc_pin(&gasnetc_hca[h], buf, size,
		          IBV_ACCESS_LOCAL_WRITE, &gasnetc_hca[h].snd_reg);
      if (vstat != 0) {
	for (h -= 1; h >= 0; --h) {
	  gasnetc_unpin(&gasnetc_hca[h], &gasnetc_hca[h].snd_reg);
	}
        gasnetc_munmap(buf, size);
        buf = NULL;
	break;
      }
    }
  }
  if_pf (buf == NULL) {
      GASNETI_RETURN_ERRR(RESOURCE, "Unable to allocate pinned memory for AM/bounce buffers");
  }
  gasnetc_lifo_init(&gasnetc_bbuf_freelist);
  for (i = 0; i < gasnetc_bbuf_limit; ++i) {
    gasnetc_lifo_push(&gasnetc_bbuf_freelist, buf);
    ++buf;
  }
#if GASNETC_IBV_SRQ
  if (gasnetc_use_srq) {
    gasnetc_atomic_ptr_set(&gasnetc_spare_reply_bbuf, (uintptr_t)buf, 0);
  } else {
    // Should (already) be initialized non-NULL to prevent buffer-freeing path from setting it
    gasneti_assert(gasnetc_atomic_ptr_read(&gasnetc_spare_reply_bbuf, 0));
  }
#endif
 }

#if GASNETC_PIN_SEGMENT
  // Allocate rkey tables for primodial segments
  // TODO: move primordial rkeys to shared (PSHM) storage
  GASNETC_FOR_ALL_HCA(hca) {
    hca->rkeys = gasneti_calloc(gasneti_nodes, sizeof(uint32_t));
    gasneti_leak(hca->rkeys);
  }
#endif

#if GASNETC_HAVE_FENCED_PUTS
  // Speed critical path checks
  gasnetc_op_needs_fence_mask = gasnetc_use_fenced_puts ?  GASNETC_OP_NEEDS_FENCE : 0;
#endif

  /* Init thread-local data */
  gasnetc_per_thread_setup();

  return GASNET_OK;
}

extern void gasnetc_sndrcv_init_peer(gex_Rank_t node, gasnetc_cep_t *cep) {
  const int first = !cep->hca->num_qps;
  for (int i = 0; i < gasnetc_alloc_qps; ++i, ++cep) {
    gasnetc_hca_t *hca = cep->hca;
    cep->epid = gasnetc_epid(node, i);
    cep->snd_cq_sema_p = hca->snd_cq_sema_p;

  #if GASNETC_IB_MAX_HCAS > 1
    /* "Cache" the local keys associated w/ this cep */
    if (gasneti_nodes > 1) {
      cep->rcv_lkey = hca->rcv_reg.handle->lkey;
    }
    cep->snd_lkey = hca->snd_reg.handle->lkey;
  #endif

    cep->rbuf_freelist = &hca->rbuf_freelist;

    // Prepost to SRQ for only one peer on each HCA
    if (gasnetc_use_srq && first) {
      for (int j = 0; j < gasnetc_am_rqst_per_qp; ++j) {
        gasnetc_rcv_post(cep, gasnetc_lifo_pop(cep->rbuf_freelist));
      }
    }

    if (!GASNETI_NBRHD_JOBRANK_IS_LOCAL(node)) { // AM resources never used w/i NBRHD
      if (!gasnetc_use_srq) {
        for (int j = 0; j < 2 * gasnetc_am_oust_pp; ++j) {
          // Prepost one rcv buffer for each possible incomming Request or Reply
          gasnetc_rcv_post(cep, gasnetc_lifo_pop(cep->rbuf_freelist));
        }
      }

      /* Setup semaphores/counters */
      /* sq_sema now set when QP is created */
      if (gasnetc_use_srq && (i < gasnetc_num_qps)) {
        gasnetc_sema_init(&cep->am_rem, 0, 0);
      } else {
        gasnetc_sema_init(&cep->am_rem, gasnetc_am_oust_pp, gasnetc_am_oust_pp);
      }
    } else {
      /* Should never use AM resources for loopback or same supernode */
      /* XXX: is this now unreachable with new connect code? */
      gasnetc_sema_init(&cep->am_rem, 0, 0);
      gasnetc_atomic_set(&cep->am_flow.credit, 0, 0);
    }

    hca->num_qps++;
    gasneti_assert_uint(hca->num_qps ,<=, hca->max_qps);
  }
}

extern void gasnetc_sndrcv_init_inline(void) {
  gasnetc_am_inline_limit_sndrcv = MIN(gasnetc_inline_limit, sizeof(gasnetc_am_tmp_buf_t));
#if !GASNETC_PIN_SEGMENT
  gasnetc_putinmove_limit_adjusted = gasnetc_putinmove_limit
	  				? (gasnetc_putinmove_limit + gasnetc_inline_limit)
					: 0;
#endif
}

extern void gasnetc_sndrcv_attach_peer(gex_Rank_t node, gasnetc_cep_t *cep) {
#if GASNETC_PIN_SEGMENT
  for (int i = 0; i < gasnetc_alloc_qps; ++i, ++cep) {
    gasnetc_hca_t *hca = cep->hca;
    gasneti_assert(hca->rkeys);
    cep->rkey = hca->rkeys[node];
  }
#else
  /* Nothing currently needed */
#endif
}

#if GASNETC_IBV_SHUTDOWN
static void
gasnetc_unpin_unmap(gasnetc_hca_t *hca, gasnetc_memreg_t *reg) {
  if (reg->len) {
    gasnetc_unpin(hca, reg);
    gasnetc_unmap(reg);
  }
}

void gasnetc_sys_flush_reph(gex_Token_t token, gex_AM_Arg_t credits) {
  gasnetc_cep_t *cep = ((gasnetc_rbuf_t *)token)->cep;

  gasneti_assert(! gasnetc_use_srq); /* SRQ prohibits credit coallescing */
  gasneti_assert(0 != credits); /* Should not have sent otherwise */

  /* May reduce trafic by sending banked credits back sooner: */
  gasnetc_am_credits_slack = 0;

  if (--credits) { /* Since this is a Reply, one credit has already been posted */
    gasnetc_sema_up_n(&cep->am_rem, credits);
  }
}

static int gasnetc_close_recvd[16]; /* Note 16-bit GASNET_MAXNODES */

void gasnetc_sys_close_reqh(gex_Token_t token) {
  gex_Rank_t peer = gasnetc_msgsource(token);
  int distance, shift;

  distance = (peer > gasneti_mynode) ? peer - gasneti_mynode
                                     : peer + (gasneti_nodes - gasneti_mynode);
  gasneti_assert(GASNETI_POWEROFTWO(distance));

  shift = 0;
  while ((1<<shift) != distance) ++shift;

  gasneti_assert(!gasnetc_close_recvd[shift]);
  gasnetc_close_recvd[shift] = 1;

  if (! GASNETI_NBRHD_JOBRANK_IS_LOCAL(peer)) {
    gasnetc_rbuf_t *rbuf = (gasnetc_rbuf_t *)token;
    gasneti_assert(rbuf->rbuf_needReply);
    rbuf->rbuf_needReply = 0; /* we are terminating flow control */
  }
}

extern void
gasnetc_sndrcv_quiesce(void) {
  GASNET_BEGIN_FUNCTION(); // OK - not a critical-path
  gasnetc_hca_t *hca;

  gasnetc_EP_t ep = gasnetc_ep0;  // TODO-EX: multi-EP suport

  /* suspend credit coallescing (if any) and return any banked credits */
  if (! gasnetc_use_srq) {
    gex_Rank_t i;

    gasnetc_rbuf_t rbuf;
  #if GASNETI_THREADINFO_OPT
    rbuf.rbuf_threadinfo = GASNETI_MYTHREAD;
  #endif

    gasnetc_am_credits_slack = 0;
    for (i = 0; i < gasneti_nodes; ++i) { /* Stagger to avoid hot-spotting */
      gex_Rank_t node = (i < gasneti_nodes - gasneti_mynode)
                               ? (gasneti_mynode + i)
                               : (gasneti_mynode - (gasneti_nodes - i));
      gasnetc_cep_t *cep = GASNETC_NODE2CEP(ep, node);
      int qpi;
      if (GASNETI_NBRHD_JOBRANK_IS_LOCAL(node) || !cep) continue;
      for (qpi = 0; qpi < gasnetc_alloc_qps; ++qpi, ++cep) {
        int cr = gasnetc_atomic_swap(&cep->am_flow.credit, 0, 0);
        if (!cr) continue;
        /* Since the banked credits count remote recv buffers we can send a "Phantom Reply".
         * This avoids soliciting a potentially unnecessary Reply.
         */
        int fake_hidx = gasneti_handleridx(gasnetc_sys_close_reqh);
        rbuf.cep = cep;
        rbuf.rbuf_needReply = 1;
      #if GASNET_DEBUG
        rbuf.rbuf_handlerRunning = 1;
      #endif
        rbuf.rbuf_flags = GASNETC_MSG_GENFLAGS(1, gasneti_Short, 0, fake_hidx, node);
        rbuf.rr_ep = ep;
        gasnetc_ReplySysShort((gex_Token_t)&rbuf, NULL, gasneti_handleridx(gasnetc_sys_flush_reph), 1, cr);
      }
    }
  }

  /* drain in-flight AMs by allocating all of the AM credits */
  {
    gex_Rank_t node;
    for (node = 0; node < gasneti_nodes; ++node) {
      gasnetc_cep_t *cep = GASNETC_NODE2CEP(ep, node);
      int qpi_offset = gasnetc_use_srq ? gasnetc_num_qps : 0;
      int qpi;
      if (GASNETI_NBRHD_JOBRANK_IS_LOCAL(node) || !cep) continue;
      for (qpi = qpi_offset, cep += qpi_offset; qpi < gasnetc_alloc_qps; ++qpi, ++cep) {
        int remain = gasnetc_am_oust_pp;
        gasnetc_sema_t *sema = &cep->am_rem;
        GASNETI_SPIN_WHILE((remain -= gasnetc_sema_trydown_partial(sema, remain)),
                           gasnetc_poll_both());
      }
    }
  }

  { /* Dissemination barrier via special AM Request hander which does NOT return credits */
    unsigned int shift, distance;
    for (shift = 0, distance = 1; distance < gasneti_nodes; ++shift, distance *= 2) {
      gex_Rank_t peer = (distance <= gasneti_mynode) ? gasneti_mynode - distance
                                                        : gasneti_mynode + (gasneti_nodes - distance);
      if (GASNETI_NBRHD_JOBRANK_IS_LOCAL(peer)) {
        gex_AM_RequestShort0(gasneti_THUNK_TM, peer, gasneti_handleridx(gasnetc_sys_close_reqh), 0);
      } else {
        static gasnetc_counter_t dummy = GASNETC_COUNTER_INITIALIZER; /* So PFs don't run */
        const int qp_offset = gasnetc_use_srq ? gasnetc_num_qps : 0;
        gasnetc_cep_t *cep = gasnetc_get_cep(ep, peer) + qp_offset;
        gasnetc_sema_up(&cep->am_rem);
        /* OK if some other AM Request gets in this gap; we'll block for the reply. */
        gasnetc_RequestSysShort(cep->epid, &dummy, gasneti_handleridx(gasnetc_sys_close_reqh), 0);
      }
      GASNETI_SPIN_UNTIL(gasnetc_close_recvd[shift], gasnetc_poll_both());
      gasnetc_close_recvd[shift] = 0;
    }
  }

  /* complete ALL send/put/get by allocating all of the CQ slots */
  GASNETC_FOR_ALL_HCA(hca) {
    int remain = hca->snd_cq->cqe;
    gasnetc_sema_t *sema = hca->snd_cq_sema_p;
    GASNETI_SPIN_WHILE((remain -= gasnetc_sema_trydown_partial(sema, remain)),
                       gasnetc_poll_both());
  }

  /* Resume credit coallescing (in any) */
  gasnetc_am_credits_slack = gasnetc_am_credits_slack_orig;
}

extern int gasnetc_sndrcv_shutdown(void) {
  gasnetc_hca_t *hca;
  int rc;

  GASNETC_FOR_ALL_HCA(hca) {
  #if GASNETC_IBV_SRQ
    if (gasnetc_use_srq && (gasneti_nodes > 1)) {
      rc = ibv_destroy_srq(hca->rqst_srq);
      GASNETC_IBV_CHECK(rc, "from ibv_destroy_srq(request)");
      rc = ibv_destroy_srq(hca->repl_srq);
      GASNETC_IBV_CHECK(rc, "from ibv_destroy_srq(reply)");
    }
  #endif

    rc = ibv_destroy_cq(hca->rcv_cq);
    GASNETC_IBV_CHECK(rc, "from ibv_destroy_cq(rcv_cq)");
    rc = ibv_destroy_cq(hca->snd_cq);
    GASNETC_IBV_CHECK(rc, "from ibv_destroy_cq(snd_cq)");

  #if GASNETC_USE_RCV_THREAD
    if (gasnetc_use_rcv_thread) {
      rc = ibv_destroy_comp_channel(hca->rcv_thread.compl);
      GASNETC_IBV_CHECK(rc, "from ibv_destroy_comp_chanel(rcv_thread)");
    }
  #endif

  #if GASNETC_USE_SND_THREAD
    if (gasnetc_use_snd_thread) {
      rc = ibv_destroy_comp_channel(hca->snd_thread.compl);
      GASNETC_IBV_CHECK(rc, "from ibv_destroy_comp_chanel(snd_thread)");
    }
  #endif

    gasnetc_unpin_unmap(hca, &hca->snd_reg);
    gasnetc_unpin_unmap(hca, &hca->rcv_reg);
  }

  return GASNET_OK;
}
#endif

#if GASNETC_USE_RCV_THREAD || GASNETC_USE_SND_THREAD
int gasnetc_query_progress_threads(
            gex_Client_t                     e_client,
            unsigned int                    *count_p,
            const gex_ProgressThreadInfo_t **info_p,
            gex_Flags_t                      flags)
{
  // Leverage conduit-independent version for common error checks
  int result = gasneti_query_progress_threads(e_client,count_p,info_p,flags);
  if (result) return (result);

  *count_p = gasnetc_progress_thread_count;
  *info_p = gasnetc_progress_thread_count ? gasnetc_progress_thread_info : NULL;

  return GASNET_OK;
}

extern void gasnetc_sndrcv_start_thread(gex_Flags_t init_flags) {
  int defer = !!(init_flags & GEX_FLAG_DEFER_THREADS);
  #if GASNETC_USE_RCV_THREAD
  if (gasnetc_use_rcv_thread) {
    int rcv_max_rate = gasneti_getenv_int_withdefault("GASNET_RCV_THREAD_RATE", 0, 0);
    gasnetc_hca_t *hca;

    GASNETC_FOR_ALL_HCA(hca) {
      /* spawn the RCV thread */
      hca->rcv_thread.fn = gasnetc_rcv_thread;
      hca->rcv_thread.fn_arg = hca;
      if (rcv_max_rate > 0) {
        hca->rcv_thread.thread_rate.ns = ((uint64_t)1E9) / rcv_max_rate;
      }
      hca->rcv_thread.keep_alive.ns = gasneti_getenv_int_withdefault("GASNET_RCV_THREAD_IDLE", 0, 0);
    #if GASNETC_SERIALIZE_POLL_CQ
      gasneti_assert(!gasnetc_rcv_thread_poll_exclusive ||
                     !gasnetc_rcv_thread_poll_serialize); // mutually exclusive
      if (gasnetc_rcv_thread_poll_exclusive) {
        hca->rcv_thread.exclusive_poll = &hca->poll_cq_semas.rcv;
      #if (GASNETC_IB_MAX_HCAS > 1)
        // Remove thread contention in the AMPoll path.
        // Note that this cannot safetly be done sooner, because AM-based comms
        // are used in startup logic prior to spawning this thread.
        gasnetc_rcv_poll_multi_hcas = 0;
      #endif
      } else if (gasnetc_rcv_thread_poll_serialize) {
        hca->rcv_thread.serialize_poll = &hca->poll_cq_semas.rcv;
      }
    #endif
    #if GASNETI_THREADINFO_OPT
      hca->rcv_threadinfo = NULL;
    #endif
      if (defer) {
        gex_ProgressThreadInfo_t *p = &gasnetc_progress_thread_info[gasnetc_progress_thread_count++];
        gasneti_assert_uint(gasnetc_progress_thread_count, <=, GASNETC_MAX_PROGRESS_THREADS);
        p->gex_device_list = hca->hca_id;
        p->gex_thread_roles = GEX_THREAD_ROLE_RCV;
        p->gex_progress_fn = &gasnetc_progress_thread;
        p->gex_progress_arg = &hca->rcv_thread;
      } else {
        gasnetc_spawn_progress_thread(&hca->rcv_thread);
      }
    }
  }
  #endif
  #if GASNETC_USE_SND_THREAD
  if (gasnetc_use_snd_thread) {
    int snd_max_rate = gasneti_getenv_int_withdefault("GASNET_SND_THREAD_RATE", 0, 0);
    gasnetc_hca_t *hca;

    GASNETC_FOR_ALL_HCA(hca) {
      /* spawn the SND thread */
      hca->snd_thread.fn = gasnetc_snd_thread;
      hca->snd_thread.fn_arg = hca;
      if (snd_max_rate > 0) {
        hca->snd_thread.thread_rate.ns = ((uint64_t)1E9) / snd_max_rate;
      }
      hca->snd_thread.keep_alive.ns = gasneti_getenv_int_withdefault("GASNET_SND_THREAD_IDLE", 0, 0);
    #if GASNETC_SERIALIZE_POLL_CQ
      gasneti_assert(!gasnetc_snd_thread_poll_exclusive ||
                     !gasnetc_snd_thread_poll_serialize); // mutually exclusive
      if (gasnetc_snd_thread_poll_exclusive) {
        hca->snd_thread.exclusive_poll = &hca->poll_cq_semas.snd;
      #if (GASNETC_IB_MAX_HCAS > 1)
        // Remove thread contention in the AMPoll path.
        // Note that this cannot safetly be done sooner, due to the
        // communication in startup logic prior to spawning this thread.
        gasnetc_snd_poll_multi_hcas = 0;
      #endif
      } else if (gasnetc_snd_thread_poll_serialize) {
        hca->snd_thread.serialize_poll = &hca->poll_cq_semas.snd;
      }
    #endif
      if (defer) {
        gex_ProgressThreadInfo_t *p = &gasnetc_progress_thread_info[gasnetc_progress_thread_count++];
        gasneti_assert_uint(gasnetc_progress_thread_count, <=, GASNETC_MAX_PROGRESS_THREADS);
        p->gex_device_list = hca->hca_id;
        p->gex_thread_roles = GEX_THREAD_ROLE_SND;
        p->gex_progress_fn = &gasnetc_progress_thread;
        p->gex_progress_arg = &hca->snd_thread;
      } else {
        gasnetc_spawn_progress_thread(&hca->snd_thread);
      }
    }
  }
  #endif
}

extern void gasnetc_sndrcv_stop_thread(int block) {
  #if GASNETC_USE_RCV_THREAD
  if (gasnetc_use_rcv_thread) {
    gasnetc_hca_t *hca;

    GASNETC_FOR_ALL_HCA(hca) {
      /* stop the RCV thread if we have started it */
      if (hca->rcv_thread.fn == gasnetc_rcv_thread) {
        gasnetc_stop_progress_thread(&hca->rcv_thread, block);
      }
    }
  }
  #endif

  #if GASNETC_USE_SND_THREAD
  if (gasnetc_use_snd_thread) {
    gasnetc_hca_t *hca;

    GASNETC_FOR_ALL_HCA(hca) {
      /* stop the SND thread if we have started it */
      if (hca->snd_thread.fn == gasnetc_snd_thread) {
        gasnetc_stop_progress_thread(&hca->snd_thread, block);
      }
    }
  }
  #endif
}
#endif

/* Just gasnetc_AMPoll w/o CHECKATTACH when !handler_context */
extern void gasnetc_sndrcv_poll(int handler_context) {
  GASNET_BEGIN_FUNCTION(); // OK - this is used only in init/exit paths
  gasnetc_do_poll(!handler_context, 1 GASNETI_THREAD_PASS);
}

extern void gasnetc_counter_wait_aux(gasnetc_counter_t *counter, int handler_context GASNETI_THREAD_FARG)
{
  const gasnetc_atomic_val_t initiated = (counter->initiated & GASNETI_ATOMIC_MAX);
  gasnetc_atomic_t * const completed = &counter->completed;

  // caller has checked that (initiated != completed)
  // so the spin loops below are "DOUNTIL"
  if (handler_context) {
    // must not poll rcv queue in hander context
    GASNETI_SPIN_DOUNTIL((initiated == gasnetc_atomic_read(completed, 0)),
                         gasnetc_poll_snd());
  } else {
    GASNETI_SPIN_DOUNTIL((initiated == gasnetc_atomic_read(completed, 0)),
                         { gasnetc_poll_both(); GASNETI_PROGRESSFNS_RUN(); });
  }
}

#if GASNETC_PIN_SEGMENT
/*
 * ############################################
 * RDMA ops used when the segment is pre-pinned
 * ############################################
 */

/* Perform an RDMA put
 *
 * Uses inline if possible, bounce buffers if "small enough" and the caller is planning to wait
 * for local completion.  Otherwise zero-copy is used (with firehose if the source is not pre-pinned).
 * If firehose is disabled, then bounce buffers are used for unpinned sources.
 */
// TODO-EX:
//   Curently we may need to pre-increment (*local_cnt) and (*remote_cnt) in
//   order to prevent the counters from becoming balanced (as observed in the
//   corresponding callback) before the final ibv-level operation has been
//   injected.  That requires a coresponding atomic-add (via the completion
//   callback) of the completed counter at the end.  Ideally we would be able
//   to manage all the increments of the initiated counters (as done in
//   gasnetc_rdma_get()) such that premature counter balance would be
//   impossible.  HOWEVER, attempts to do so w/o significant rewrites have
//   failed so far.  The main issues are:
//     + One or both of the counters might not advance at all
//     + Identifying the *last* increment of local_cnt or remote_cnt
//   For certain cases we may be able to resolve both of those issues, notably
//   the cases in which only one of local_cnt or remote_cnt is non-NULL.
//
//   Currently we entirely avoid the "bias" of the counter(s) only for the
//   case of the inline put.
extern int gasnetc_rdma_put(
                gex_TM_t tm, gex_Rank_t rank,
                void *src_ptr, void *dst_ptr,
                size_t nbytes,
                gex_Flags_t flags,
                gasnetc_atomic_val_t *local_cnt,
                gasnetc_cb_t local_cb,
                gasnetc_atomic_val_t *remote_cnt,
                gasnetc_cb_t remote_cb
                GASNETI_THREAD_FARG)
{
  gasnetc_EP_t ep = (gasnetc_EP_t) gasneti_e_tm_to_i_ep(tm);
  GASNETC_DECL_SR_DESC(sr_desc, GASNETC_SND_SG);

  // TODO-EX:
  //     This will be replaced by general multi-registration support later
  const gex_EP_Location_t loc = gasneti_e_tm_rank_to_location(tm, rank, 0);
  const gex_Rank_t jobrank = loc.gex_rank;
  const int rem_epidx = gasneti_in_auxsegment(jobrank, dst_ptr, nbytes) ? -1 : loc.gex_ep_index;

  // To reach here legally, at least one of three things must be true:
  //  1. local ep is bound to non-host memory
  //  2. remote ep is neither primordial nor aux-seg
  //  3. remote jobrank is not in-nbrhd
  // Otherwise, PSHM should be used.
  gasneti_assert(!gasneti_i_segment_kind_is_host(ep->_segment) ||
                 (rem_epidx > 0) ||
                 !GASNETI_NBRHD_JOBRANK_IS_LOCAL(jobrank));

  // Local "device memory" can never use inline or bounce buffers
  // TODO: maybe some devices classes can in the future?
  const int device_mem = !gasneti_i_segment_kind_is_host(ep->_segment);

  gasneti_assert(nbytes != 0);
  
  sr_desc->wr.rdma.remote_addr = (uintptr_t)dst_ptr;
  sr_desc_sg_lst[0].addr = (uintptr_t)src_ptr;

  /* Use a short-cut for sends that are short enough
   *
   * Note that we do this based only on the size, without checking whether
   * the caller cares about local completion, or whether zero-copy is possible.
   */
  if ((nbytes <= gasnetc_inline_limit) && !device_mem)
  {
    gasnetc_do_put_inline(ep, jobrank, rem_epidx, sr_desc, nbytes, remote_cnt, remote_cb GASNETI_THREAD_PASS);
    return 0;
  }

  // May need to do a bit of extra work to prevent premature counter balance
  const int bias_remote_cnt = (remote_cb == gasnetc_cb_eop_put);
  if (bias_remote_cnt) ++(*remote_cnt);

  // Distinct cases below for host memory, depending on whether LC matters or not
  // TODO-EX: this may suggest 2 distinct functions are in order?
  if (device_mem) {
    gasneti_assert(gasnetc_in_bound_segment(ep, (uintptr_t)src_ptr, nbytes));
    const int bias_local_cnt = (local_cb == gasnetc_cb_eop_alc);
    if (bias_local_cnt) ++(*local_cnt);

    // IB reports only a single completion, which is for RC.
    // PutBlocking and Put{NB,NBI} with GEX_EVENT_DEFER use *only* an RC counter, so we
    // must report IB-level completion there.
    // Otherwise, we report to the LC counter which also blocks RC (either at injection for
    // GEX_EVENT_NOW, or else at Test/Wait on the event due to the root/leaf relationship).
    gasnetc_atomic_val_t *cnt = local_cnt ? local_cnt : remote_cnt;
    gasnetc_cb_t           cb = local_cnt ? local_cb  : remote_cb;
    gasneti_assert(cb); // Currently never "fire-and-forget" to device memory

    size_t unsent = gasnetc_do_put_zerocp(ep, jobrank, rem_epidx, sr_desc, nbytes,
                                          cnt, cb GASNETI_THREAD_PASS);
    gasneti_assert_uint(unsent ,==, 0);
    if (bias_local_cnt) local_cb(local_cnt);
  } else if (local_cb) {
    const int bias_local_cnt  = (local_cb  == gasnetc_cb_eop_alc);
    if (bias_local_cnt) ++(*local_cnt);

    // Because IB lacks native indication of local completion (LC), the only ways to
    // detect LC are to wait for RC, or use bounce buffers to achieve synchronous LC.
    // So, use bounce buffers for a non-bulk put if "not too large".
    // Also use bounce buffers if (firehose disabled AND src is in neither the client
    // nor aux segment) OR zero copy fails such as for read-only memory (bug 3338).
    size_t to_xfer = nbytes;
    int is_nonbulk = (local_cb == gasnetc_cb_counter); // GEX_EVENT_NOW
    if ((is_nonbulk && (nbytes <= gasnetc_nonbulk_bounce_limit)) ||
        (!GASNETC_USE_FIREHOSE &&
         !gasnetc_in_bound_segment(ep, (uintptr_t)src_ptr, nbytes) &&
         !gasneti_in_local_auxsegment((gasneti_EP_t)ep, src_ptr, nbytes)) ||
        ((to_xfer = gasnetc_do_put_zerocp(ep, jobrank, rem_epidx, sr_desc, nbytes,
                                         local_cnt, local_cb GASNETI_THREAD_PASS)))) {
      gasnetc_do_put_bounce(ep, jobrank, rem_epidx, sr_desc, to_xfer,
                            remote_cnt, remote_cb GASNETI_THREAD_PASS);
    }

    if (bias_local_cnt) local_cb(local_cnt);
  } else {
    // Use bounce buffers if (firehose disabled AND src is in neither the client
    // nor aux segment) OR zero copy fails such as for read-only memory (bug 3338).
    size_t to_xfer = nbytes;
    if ((!GASNETC_USE_FIREHOSE &&
         !gasnetc_in_bound_segment(ep, (uintptr_t)src_ptr, nbytes) &&
         !gasneti_in_local_auxsegment((gasneti_EP_t)ep, src_ptr, nbytes)) ||
        ((to_xfer = gasnetc_do_put_zerocp(ep, jobrank, rem_epidx, sr_desc, nbytes,
                                          remote_cnt, remote_cb GASNETI_THREAD_PASS)))) {
      gasnetc_do_put_bounce(ep, jobrank, rem_epidx, sr_desc, to_xfer,
                            remote_cnt, remote_cb GASNETI_THREAD_PASS);
    }
  }

  if (bias_remote_cnt) remote_cb(remote_cnt);

  return 0;
}

// Put specialized for needs of AM Long payload
// * caller needs to control the qpi (via cep)
// * always has local callbacks (never GEX_EVENT_DEFER)
// * never has remote callbacks
// * assumed never small enough for inline send (would be packed instead)
extern int gasnetc_rdma_long_put(
                gasnetc_EP_t ep, gasnetc_cep_t *cep,
                void *src_ptr, void *dst_ptr,
                size_t nbytes,
                gex_Flags_t flags,
                gasnetc_atomic_val_t *local_cnt,
                gasnetc_cb_t local_cb
                GASNETI_THREAD_FARG)
{
  gasnetc_epid_t epid = cep->epid;
  GASNETC_DECL_SR_DESC(sr_desc, GASNETC_SND_SG);

  // TODO-EX:
  //     This will be replaced by general multi-registration support later
  // TODO: this lacks support for non-primordial EP since we are missing the index information
  const int rem_epidx = gasneti_in_auxsegment(gasnetc_epid2node(epid), dst_ptr, nbytes) ? -1 : 0;

  gasneti_assert(nbytes != 0);
  
  sr_desc->wr.rdma.remote_addr = (uintptr_t)dst_ptr;
  sr_desc_sg_lst[0].addr = (uintptr_t)src_ptr;

  // May need to do a bit of extra work to prevent premature counter balance
  const int bias_local_cnt  = (local_cb  == gasnetc_cb_eop_alc);
  if (bias_local_cnt) ++(*local_cnt);

  // Because IB lacks native indication of local completion (LC), the only ways to
  // detect LC are to wait for RC, or use bounce buffers to achieve synchronous LC.
  // So, use bounce buffers for if "not too large".
  // Also use bounce buffers if (firehose disabled AND src is in neither the client
  // nor aux segment) OR zero copy fails such as for read-only memory (bug 3338).
  size_t to_xfer = nbytes;
  int is_nonbulk = (local_cb == gasnetc_cb_counter); // GEX_EVENT_NOW
  if ((is_nonbulk && (nbytes <= gasnetc_nonbulk_bounce_limit)) ||
      (!GASNETC_USE_FIREHOSE &&
       !gasnetc_in_bound_segment(ep, (uintptr_t)src_ptr, nbytes) &&
       !gasneti_in_local_auxsegment((gasneti_EP_t)ep, src_ptr, nbytes)) ||
      ((to_xfer = gasnetc_do_put_zerocp(ep, epid, rem_epidx, sr_desc, nbytes,
                                        local_cnt, local_cb GASNETI_THREAD_PASS)))) {
    gasnetc_do_put_bounce(ep, epid, rem_epidx, sr_desc, to_xfer,
                          NULL, NULL GASNETI_THREAD_PASS);
  }

  if (bias_local_cnt) local_cb(local_cnt);

  return 0;
}

/* Perform an RDMA get
 *
 * Uses zero-copy (with firehose if the destination is not pre-pinned).
 * If firehose is disabled, then bounce buffers are used for unpinned destinations.
 *
 * This function takes care to keep its increments of (*remote_cnt) an *extra*
 * step ahead of the ibv-level injections.  This means it is not possible for
 * the initiated and completed counters to balance before to the last injection.
 */
extern int gasnetc_rdma_get(
                gex_TM_t tm, gex_Rank_t rank,
                void *src_ptr, void *dst_ptr,
                size_t nbytes,
                gex_Flags_t flags,
                gasnetc_atomic_val_t *remote_cnt,
                gasnetc_cb_t remote_cb
                GASNETI_THREAD_FARG)
{
  gasnetc_EP_t ep = (gasnetc_EP_t) gasneti_e_tm_to_i_ep(tm);
  GASNETC_DECL_SR_DESC(sr_desc, GASNETC_SND_SG);

  // TODO-EX:
  //     This will be replaced by general multi-registration support later
  const int loc_auxseg = gasneti_in_local_auxsegment((gasneti_EP_t)ep, dst_ptr, nbytes);
  const gex_EP_Location_t loc = gasneti_e_tm_rank_to_location(tm, rank, 0);
  const gex_Rank_t jobrank = loc.gex_rank;
  const int rem_epidx = gasneti_in_auxsegment(jobrank, src_ptr, nbytes) ? -1 : loc.gex_ep_index;

  // To reach here legally, at least one of three things must be true:
  //  1. local ep is bound to non-host memory
  //  2. remote ep is neither primordial nor aux-seg
  //  3. remote jobrank is not in-nbrhd
  // Otherwise, PSHM should be used.
  gasneti_assert(!gasneti_i_segment_kind_is_host(ep->_segment) ||
                 (rem_epidx > 0) ||
                 !GASNETI_NBRHD_JOBRANK_IS_LOCAL(jobrank));

  gasneti_assert(nbytes != 0);
  gasneti_assert(remote_cnt != NULL);

  ++(*remote_cnt);

  sr_desc->wr.rdma.remote_addr = (uintptr_t)src_ptr;
  sr_desc_sg_lst[0].addr = (uintptr_t)dst_ptr;

  if (!GASNETC_USE_FIREHOSE &&
      !gasnetc_in_bound_segment(ep, (uintptr_t)dst_ptr, nbytes) &&
      !gasneti_in_local_auxsegment((gasneti_EP_t)ep, dst_ptr, nbytes)) {
    /* Firehose disabled.  Use bounce buffers since dst_ptr is out-of-segment */
    gasnetc_do_get_bounce(ep, jobrank, rem_epidx, sr_desc, nbytes, remote_cnt, remote_cb GASNETI_THREAD_PASS);
  } else {
    gasnetc_do_get_zerocp(ep, jobrank, rem_epidx, sr_desc, nbytes, remote_cnt, remote_cb GASNETI_THREAD_PASS);
  }

  return 0;
}
#else
/*
 * ###########################################
 * RDMA ops when the segment is NOT pre-pinned
 * ###########################################
 */
/* RDMA put */
// TODO-EX: see comment prior to gasnetc_rdma_put() regarding counters
extern int gasnetc_rdma_put(
                gex_TM_t tm, gex_Rank_t rank,
                void *src_ptr, void *dst_ptr,
                size_t nbytes,
                gex_Flags_t flags,
                gasnetc_atomic_val_t *local_cnt,
                gasnetc_cb_t local_cb,
                gasnetc_atomic_val_t *remote_cnt,
                gasnetc_cb_t remote_cb
                GASNETI_THREAD_FARG)
{
  gex_Rank_t jobrank = gasneti_e_tm_rank_to_jobrank(tm, rank);
  gasnetc_EP_t ep = (gasnetc_EP_t) gasneti_e_tm_to_i_ep(tm);

  uintptr_t src = (uintptr_t)src_ptr;
  uintptr_t dst = (uintptr_t)dst_ptr;

  gasneti_assert(nbytes != 0);

  // May need to do a bit of extra work to prevent premature counter balance
  const int bias_local_cnt  = (local_cb  == gasnetc_cb_eop_alc);
  if (bias_local_cnt) ++(*local_cnt);
  const int bias_remote_cnt = (remote_cb == gasnetc_cb_eop_put);
  if (bias_remote_cnt) ++(*remote_cnt);

  do {
    gasnetc_sreq_t * const sreq = gasnetc_get_sreq(GASNETC_OP_INVALID GASNETI_THREAD_PASS);
    size_t count;

    sreq->epid = jobrank;
 
    sreq->comp.cb = remote_cb;
    sreq->comp.data = remote_cnt;
    sreq->fh_lc = local_cnt;
    sreq->fh_lc_cb = local_cb;
    sreq->fh_oust = NULL;

    count = gasnetc_fh_put_helper(ep, jobrank, sreq, local_cnt, remote_cnt, src, dst, nbytes GASNETI_THREAD_PASS);

    src += count;
    dst += count;
    nbytes -= count;
  } while (nbytes);

  // Fix the bias, if any, we introduced above
  if (bias_local_cnt)  local_cb(local_cnt);
  if (bias_remote_cnt) remote_cb(remote_cnt);

  gasnetc_poll_rcv(); /* Progress may depend on firehose AM Reply */
  return 0;
}

// Put specialized for needs of AM Long payload
// * caller needs to control the qpi (via cep)
// * always has local callbacks (never GEX_EVENT_DEFER)
// * never has remote callbacks
// * must block for firehose movement (if any) to compelte
extern int gasnetc_rdma_long_put(
                gasnetc_EP_t ep, gasnetc_cep_t *cep,
                void *src_ptr, void *dst_ptr,
                size_t nbytes,
                gex_Flags_t flags,
                gasnetc_atomic_val_t *local_cnt,
                gasnetc_cb_t local_cb
                GASNETI_THREAD_FARG)
{
  gasnetc_epid_t epid = cep->epid;
  uintptr_t src = (uintptr_t)src_ptr;
  uintptr_t dst = (uintptr_t)dst_ptr;

  gasneti_assert(nbytes != 0);

  // May need to do a bit of extra work to prevent premature counter balance
  const int bias_local_cnt  = (local_cb  == gasnetc_cb_eop_alc);
  if (bias_local_cnt) ++(*local_cnt);

  // May need to block for firehose misses to be resolved
  gasnetc_counter_t am_oust = GASNETC_COUNTER_INITIALIZER;

  do {
    gasnetc_sreq_t * const sreq = gasnetc_get_sreq(GASNETC_OP_INVALID GASNETI_THREAD_PASS);
    size_t count;

    sreq->epid = epid;
 
    sreq->comp.cb = NULL;
    sreq->comp.data = NULL;
    sreq->fh_lc = local_cnt;
    sreq->fh_lc_cb = local_cb;
    sreq->fh_oust = &am_oust;

    count = gasnetc_fh_put_helper(ep, epid, sreq, local_cnt, NULL, src, dst, nbytes GASNETI_THREAD_PASS);

    src += count;
    dst += count;
    nbytes -= count;
  } while (nbytes);

  // Fix the bias, if any, we introduced above
  if (bias_local_cnt)  local_cb(local_cnt);

  // Stall for outstanding firehose misses
  // TODO: could eliminate stall when gasnetc_use_rcv_thread by using fh callback to inject header
  gasnetc_counter_wait(&am_oust, 0 GASNETI_THREAD_PASS);

  return 0;
}

/* Perform an RDMA get
 *
 * This function takes care to keep its increments of (*remote_cnt) an *extra*
 * step ahead of the ibv-level injections.  This means it is not possible for
 * the initiated and completed counters to balance before to the last injection.
 * So, a caller does not need to take any additional "insurance".
 */
extern int gasnetc_rdma_get(
                gex_TM_t tm, gex_Rank_t rank,
                void *src_ptr, void *dst_ptr,
                size_t nbytes,
                gex_Flags_t flags,
                gasnetc_atomic_val_t *remote_cnt,
                gasnetc_cb_t remote_cb
                GASNETI_THREAD_FARG)
{
  gex_Rank_t jobrank = gasneti_e_tm_rank_to_jobrank(tm, rank);
  gasnetc_EP_t ep = (gasnetc_EP_t) gasneti_e_tm_to_i_ep(tm);

  uintptr_t src = (uintptr_t)src_ptr;
  uintptr_t dst = (uintptr_t)dst_ptr;

  gasneti_assert(nbytes != 0);
  gasneti_assert(remote_cnt != NULL);

  ++(*remote_cnt);

  do {
    gasnetc_sreq_t * const sreq = gasnetc_get_sreq(GASNETC_OP_GET_ZEROCP GASNETI_THREAD_PASS);
    size_t count;

    sreq->epid = jobrank;
 
    sreq->comp.cb = remote_cb;
    sreq->comp.data = remote_cnt;

    count = gasnetc_fh_get_helper(ep, jobrank, sreq, dst, src, nbytes, remote_cnt GASNETI_THREAD_PASS);

    src += count;
    dst += count;
    nbytes -= count;
  } while (nbytes);

  gasnetc_poll_rcv(); /* Progress may depend on firehose AM Reply */
  return 0;
}
#endif

