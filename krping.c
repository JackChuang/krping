/*
 * Copyright (c) 2005 Ammasso, Inc. All rights reserved.
 * Copyright (c) 2006-2009 Open Grid Computing, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/* Jack's usage:
 *   comment:
 *   sudo insmod rdma_krping.ko debug=1
 *   sudo rmmod rdma_krping
 *
 *   server:
 *   sudo echo "server,addr=192.168.69.127,port=9999,verbose" > /proc/krping
 *   client:
 *   sudo echo "client,addr=192.168.69.127,port=9999,verbose" > /proc/krping
 *
 *   perf testing:
 *  sudo echo "server,addr=192.168.69.127,port=9999,verbose,size=8388607" > /proc/krping 
 *
 *  sudo echo "client,addr=192.168.69.127,port=9999,verbose,size=8388607" > /proc/krping 
 *
 *
 *   4096 8192 16384 32768 65536 131072 262144 524288 1048576 2097152 4194304 8388608
 *   Add ",size=%d" in "..."
 *
 */
#include <linux/version.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/parser.h>
#include <linux/proc_fs.h>
#include <linux/inet.h>
#include <linux/list.h>
#include <linux/in.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/time.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>

#include <asm/atomic.h>
#include <asm/pci.h>

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "getopt.h"

#define PFX "krping: "

static int debug;
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level (0=none, 1=all)");
#define DEBUG_LOG if (debug) printk

MODULE_AUTHOR("Steve Wise");
MODULE_DESCRIPTION("RDMA ping server");
MODULE_LICENSE("Dual BSD/GPL");

static const struct krping_option krping_opts[] = {
	{"count", OPT_INT, 'C'},
	{"size", OPT_INT, 'S'},
	{"addr", OPT_STRING, 'a'},
	{"addr6", OPT_STRING, 'A'},
	{"port", OPT_INT, 'p'},
	{"verbose", OPT_NOPARAM, 'v'},
	{"validate", OPT_NOPARAM, 'V'},
	{"server", OPT_NOPARAM, 's'},
	{"client", OPT_NOPARAM, 'c'},
	{"server_inv", OPT_NOPARAM, 'I'},
 	{"wlat", OPT_NOPARAM, 'l'},
 	{"rlat", OPT_NOPARAM, 'L'},
 	{"bw", OPT_NOPARAM, 'B'},
 	{"duplex", OPT_NOPARAM, 'd'},
 	{"txdepth", OPT_INT, 'T'},
 	{"poll", OPT_NOPARAM, 'P'},
 	{"local_dma_lkey", OPT_NOPARAM, 'Z'},
 	{"read_inv", OPT_NOPARAM, 'R'},
 	{"fr", OPT_NOPARAM, 'f'},
	{NULL, 0, 0}
};

struct krping_stats {
	unsigned long long send_bytes;
	unsigned long long send_msgs;
	unsigned long long recv_bytes;
	unsigned long long recv_msgs;
	unsigned long long write_bytes;
	unsigned long long write_msgs;
	unsigned long long read_bytes;
	unsigned long long read_msgs;
};

#define htonll(x) cpu_to_be64((x))
#define ntohll(x) cpu_to_be64((x))

static DEFINE_MUTEX(krping_mutex);

/*
 * List of running krping threads.
 */
static LIST_HEAD(krping_cbs);

static struct proc_dir_entry *krping_proc;

/*
 * Invoke like this, one on each side, using the server's address on
 * the RDMA device (iw%d):
 *
 * /bin/echo server,port=9999,addr=192.168.69.142,validate > /proc/krping  
 * /bin/echo client,port=9999,addr=192.168.69.142,validate > /proc/krping  
 * /bin/echo client,port=9999,addr6=2001:db8:0:f101::1,validate > /proc/krping
 *
 * krping "ping/pong" loop:
 * 	client sends source rkey/addr/len
 *	    server receives source rkey/add/len
 *	    server rdma reads "ping" data from source
 * 	    server sends "go ahead" on rdma read completion
 *	client sends sink rkey/addr/len
 * 	    server receives sink rkey/addr/len
 * 	    server rdma writes "pong" data to sink
 * 	    server sends "go ahead" on rdma write completion
 * 	<repeat loop>
 */

/*
 * These states are used to signal events between the completion handler
 * and the main client or server thread.
 *
 * Once CONNECTED, they cycle through RDMA_READ_ADV, RDMA_WRITE_ADV, 
 * and RDMA_WRITE_COMPLETE for each ping.
 */
enum test_state {
	IDLE = 1,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,
	RDMA_READ_ADV,
	RDMA_READ_COMPLETE,
	RDMA_WRITE_ADV,
	RDMA_WRITE_COMPLETE,
	ERROR
};

struct krping_rdma_info {
	uint64_t buf;
	uint32_t rkey;
	uint32_t size;
};

/*
 * Default max buffer size for IO...
 */
//#define RPING_BUFSIZE 128*1024
#define RPING_BUFSIZE 8*1024*1024 //Jack
#define RPING_SQ_DEPTH 64

/*
 * Control block struct.
 */
struct krping_cb {
	int server;			/* 0 iff client */
	struct ib_cq *cq;
	struct ib_pd *pd;
	struct ib_qp *qp;

	struct ib_mr *dma_mr;

	struct ib_fast_reg_page_list *page_list;
	int page_list_len;
	struct ib_reg_wr reg_mr_wr;
	struct ib_send_wr invalidate_wr;
	struct ib_mr *reg_mr;
	int server_invalidate;
	int read_inv;
	u8 key;

	struct ib_recv_wr rq_wr;	/* recv work request record */
	struct ib_sge recv_sgl;		/* recv single SGE */
	struct krping_rdma_info recv_buf;/* malloc'd buffer */
	u64 recv_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(recv_mapping)

	struct ib_send_wr sq_wr;	/* send work requrest record */
	struct ib_sge send_sgl;
	struct krping_rdma_info send_buf;/* single send buf */
	u64 send_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(send_mapping)

	struct ib_rdma_wr rdma_sq_wr;	/* rdma work request record */
	struct ib_sge rdma_sgl;		/* rdma single SGE */
	char *rdma_buf;			/* used as rdma sink */
	u64  rdma_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(rdma_mapping)
	struct ib_mr *rdma_mr;

	uint32_t remote_rkey;		/* remote guys RKEY */
	uint64_t remote_addr;		/* remote guys TO */
	uint32_t remote_len;		/* remote guys LEN */

	char *start_buf;		/* rdma read src */
	u64  start_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(start_mapping)
	struct ib_mr *start_mr;

	enum test_state state;		/* used for cond/signalling */
	wait_queue_head_t sem;
	struct krping_stats stats;

	uint16_t port;			/* dst port in NBO */
	u8 addr[16];			/* dst addr in NBO */
	char *addr_str;			/* dst addr string */
	uint8_t addr_type;		/* ADDR_FAMILY - IPv4/V6 */
	int verbose;			/* verbose logging */
	int count;			/* ping count */
	int size;			/* ping data size */
	int validate;			/* validate ping data */
	int wlat;			/* run wlat test */
	int rlat;			/* run rlat test */
	int bw;				/* run bw test */
	int duplex;			/* run bw full duplex test */
	int poll;			/* poll or block for rlat test */
	int txdepth;			/* SQ depth */
	int local_dma_lkey;		/* use 0 for lkey */
	int frtest;			/* reg test */

	/* CM stuff */
	struct rdma_cm_id *cm_id;	/* connection on client side,*/
					/* listener on server side. */
	struct rdma_cm_id *child_cm_id;	/* connection on server side */
	struct list_head list;	
};

static int krping_cma_event_handler(struct rdma_cm_id *cma_id,
				   struct rdma_cm_event *event)
{
	int ret;
	struct krping_cb *cb = cma_id->context;
    DEBUG_LOG("external>>>>>>%s();\n", __func__);

	DEBUG_LOG("cma_event type %d cma_id %p (%s)\n", event->event, cma_id,
		  (cma_id == cb->cm_id) ? "parent" : "child");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			printk(KERN_ERR PFX "rdma_resolve_route error %d\n", 
			       ret);
			wake_up_interruptible(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		cb->state = ROUTE_RESOLVED;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		cb->state = CONNECT_REQUEST;
		cb->child_cm_id = cma_id;
		DEBUG_LOG("child cma %p\n", cb->child_cm_id);
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		DEBUG_LOG("ESTABLISHED\n");
		//if (!cb->server) { // JackM
			cb->state = CONNECTED;
		//}
        DEBUG_LOG("%s(): cb->state=%d, CONNECTED=%d\n", __func__, cb->state, CONNECTED);
		wake_up(&cb->sem);
		//wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printk(KERN_ERR PFX "cma event %d, error %d\n", event->event,
		       event->status);
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
        DEBUG_LOG("%s(): cb->state=%d, CONNECTED=%d\n", __func__, cb->state, CONNECTED);
		printk(KERN_ERR PFX "DISCONNECT EVENT...\n");
		cb->state = ERROR;
		wake_up_interruptible(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		printk(KERN_ERR PFX "cma detected device removal!!!!\n");
		break;

	default:
		printk(KERN_ERR PFX "oof bad type!\n");
		wake_up_interruptible(&cb->sem);
		break;
	}
	return 0;
}

static int server_recv(struct krping_cb *cb, struct ib_wc *wc)
{
	if (wc->byte_len != sizeof(cb->recv_buf)) {
		printk(KERN_ERR PFX "Received bogus data, size %d\n", 
		       wc->byte_len);
		return -1;
	}

	cb->remote_rkey = ntohl(cb->recv_buf.rkey);
	cb->remote_addr = ntohll(cb->recv_buf.buf);
	cb->remote_len  = ntohl(cb->recv_buf.size);
	DEBUG_LOG("\t>>>Received rkey %d addr %llx len %d from peer\n",
		  cb->remote_rkey, (unsigned long long)cb->remote_addr, 
		  cb->remote_len);

	if (cb->state <= CONNECTED || cb->state == RDMA_WRITE_COMPLETE)
		cb->state = RDMA_READ_ADV;
	else
		cb->state = RDMA_WRITE_ADV;

	return 0;
}

static int client_recv(struct krping_cb *cb, struct ib_wc *wc)
{
	if (wc->byte_len != sizeof(cb->recv_buf)) {
		printk(KERN_ERR PFX "Received bogus data, size %d\n", 
		       wc->byte_len);
		return -1;
	}

	if (cb->state == RDMA_READ_ADV)
		cb->state = RDMA_WRITE_ADV;
	else
		cb->state = RDMA_WRITE_COMPLETE;

	return 0;
}

//static void to remotekrping_cq_event_handler(struct ib_cq *cq, void *ctx)
static void krping_cq_event_handler(struct ib_cq *cq, void *ctx)
{
	struct krping_cb *cb = ctx;
	struct ib_wc wc;
	struct ib_recv_wr *bad_wr;
	int ret;

	DEBUG_LOG("\nexternal------>%s();\n", __func__);

	BUG_ON(cb->cq != cq);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "cq completion in ERROR state\n");
		return;
	}
	if (cb->frtest) {
		printk(KERN_ERR PFX "cq completion event in frtest!\n");
		return;
	}
	if (!cb->wlat && !cb->rlat && !cb->bw) {
		ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
    }
	//while ((ret = ib_poll_cq(cb->cq, 1, &wc)) == 1) { //fail
	while ((ret = ib_poll_cq(cb->cq, 1, &wc)) > 0) { //fail
        /**
         * IBV_WC_LOC_PROT_ERR (4) - 
         * This event is generated when a user attempts to access an address outside of the registered memory
         * region. For example, this may happen if the Lkey does not match the address in the WR.
         * 
         * IBV_WC_LOC_PROT_ERR (4) - Local Protection Error: the locally posted Work Request’s buffers in the 
         * scatter/gather list does not reference a Memory Region that is valid for the requested operation.
         * 
         * 
         **/
		if (wc.status) { // !=IBV_WC_SUCCESS
			if (wc.status == IB_WC_WR_FLUSH_ERR) {
				DEBUG_LOG("cq flushed\n");
				continue;
			} else {
				printk(KERN_ERR PFX "cq completion failed with "
				       "wr_id %Lx status %d opcode %d vender_err %x\n",
					wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
				goto error;
			}
		}

		switch (wc.opcode) {
		case IB_WC_SEND:
			DEBUG_LOG("----- SEND COMPLETION -----\n");
			cb->stats.send_bytes += cb->send_sgl.length;
			cb->stats.send_msgs++;
			break;

		case IB_WC_RDMA_WRITE:
			DEBUG_LOG("----- RDMA WRITE COMPLETION ----- (good)\n");
			cb->stats.write_bytes += cb->rdma_sq_wr.wr.sg_list->length;
			cb->stats.write_msgs++;
			cb->state = RDMA_WRITE_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RDMA_READ:
			DEBUG_LOG("----- RDMA READ COMPLETION ----- (good)\n");
			cb->stats.read_bytes += cb->rdma_sq_wr.wr.sg_list->length;
			cb->stats.read_msgs++;
			cb->state = RDMA_READ_COMPLETE;
			wake_up_interruptible(&cb->sem);
			break;

		case IB_WC_RECV:
			DEBUG_LOG("----- RECV COMPLETION -----\n");
			cb->stats.recv_bytes += sizeof(cb->recv_buf);
			cb->stats.recv_msgs++;
			if (cb->wlat || cb->rlat || cb->bw)
				ret = server_recv(cb, &wc);
			else
				ret = cb->server ? server_recv(cb, &wc) :
						   client_recv(cb, &wc);
			if (ret) {
				printk(KERN_ERR PFX "recv wc error: %d\n", ret);
				goto error;
			}

	        DEBUG_LOG("ib_post_recv (something completed) <check recv buf> \n");
			ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
			if (ret) {
				printk(KERN_ERR PFX "post recv error: %d\n", 
				       ret);
				goto error;
			}
			wake_up_interruptible(&cb->sem);
			break;

		default:
			printk(KERN_ERR PFX
			       "%s:%d Unexpected opcode %d, Shutting down\n",
			       __func__, __LINE__, wc.opcode);
			goto error;
		}
	}
	if (ret) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		goto error;
	}
	return;
error:
	cb->state = ERROR;
	wake_up_interruptible(&cb->sem);
}

static int krping_accept(struct krping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;
	DEBUG_LOG("\n->%s();\n", __func__);
	DEBUG_LOG("\taccepting client connection request......\n");

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	ret = rdma_accept(cb->child_cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR PFX "rdma_accept error: %d\n", ret);
		return ret;
	}

	if (!cb->wlat && !cb->rlat && !cb->bw) {
        DEBUG_LOG("%s(): wating for a signal......\n", __func__);
        //wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
		wait_event(cb->sem, cb->state >= CONNECTED);
        DEBUG_LOG("%s(): got the signal !!!! cb->state=%d\n", __func__, cb->state);
		if (cb->state == ERROR) {
			printk(KERN_ERR PFX "wait for CONNECTED state %d\n", 
				cb->state);
			return -1;
		}
	}	
    DEBUG_LOG("\n");
	return 0;
}

static void krping_setup_wr(struct krping_cb *cb) // set up sgl, used for rdma
{
	cb->recv_sgl.addr = cb->recv_dma_addr; // addr
	//cb->recv_sgl.addr = cb->recv_buf.buf; // wrong: canno use kernel addr
	cb->recv_sgl.length = sizeof cb->recv_buf;  //sizeof cb->recv_buf(16)
                                                //sizeof cb->recv_buf.buf(8)
	printk("sizeof cb->recv_buf=%d, sizeof cb->recv_buf.buf=%d\n", sizeof cb->recv_buf, sizeof cb->recv_buf.buf);
    cb->recv_sgl.lkey = cb->qp->device->local_dma_lkey; // wrong (0)
    cb->rq_wr.sg_list = &cb->recv_sgl;
	cb->rq_wr.num_sge = 1;

	cb->send_sgl.addr = cb->send_dma_addr; // addr
	//cb->send_sgl.addr = cb->send_buf.buf; // wrong: cannot use kernel addr
	cb->send_sgl.length = sizeof cb->send_buf;
	cb->send_sgl.lkey = cb->qp->device->local_dma_lkey; // wrong (0)

    //3
    cb->recv_sgl.lkey = cb->pd->local_dma_lkey; // correct
	cb->send_sgl.lkey = cb->pd->local_dma_lkey; // correct
    //4
    //cb->recv_sgl.lkey = cb->reg_mr->lkey; // never tried. TODO: try
	//cb->send_sgl.lkey = cb->reg_mr->lkey; // never tried. TODO: try

	DEBUG_LOG("@@@ <addr>\n");
	DEBUG_LOG("@@@ 2 cb->recv_sgl.addr = %p\n", cb->recv_sgl.addr); // this is not local_recv_buffer // it's exhanged local addr to remote
	DEBUG_LOG("@@@ 2 cb->recv_dma_addr = %p\n", cb->recv_dma_addr); // v      addr (O) (mapped to each other)
	DEBUG_LOG("@@@ 2 cb->send_buf.buf = %p\n", cb->send_buf.buf);   // kernel addr (X) (mapped to each other)
	DEBUG_LOG("@@@ 2 cb->send_sgl.addr = %p\n", cb->send_sgl.addr);
	DEBUG_LOG("@@@ 2 cb->send_dma_addr = %p\n", cb->send_dma_addr); // v      addr
	DEBUG_LOG("@@@ 2 cb->recv_buf.buf = %p\n", cb->recv_buf.buf);   // kernel addr

	DEBUG_LOG("@@@ <lkey>\n");
	DEBUG_LOG("@@@ 2 cb->qp->device->local_dma_lkey = %d\n", cb->qp->device->local_dma_lkey);    //0
	DEBUG_LOG("@@@ 3lkey=%d from ../mad.c (ctx->pd->local_dma_lkey)\n", cb->pd->local_dma_lkey); //4450 dynamic
	DEBUG_LOG("@@@ 4lkey=%d from client\/server example(cb->mr->lkey)\n", cb->reg_mr->lkey);     //4463 dynamic

	cb->sq_wr.opcode = IB_WR_SEND; // normal send / recv
	cb->sq_wr.send_flags = IB_SEND_SIGNALED;
	cb->sq_wr.sg_list = &cb->send_sgl; // sge
	cb->sq_wr.num_sge = 1;

	if (cb->server || cb->wlat || cb->rlat || cb->bw) { //(only server setup rdma_sq_wr since only server does RDMA)
        DEBUG_LOG("only server setup rdma_sq_wr\n");
		cb->rdma_sgl.addr = cb->rdma_dma_addr;
		
        cb->rdma_sq_wr.wr.sg_list = &cb->rdma_sgl;
		cb->rdma_sq_wr.wr.send_flags = IB_SEND_SIGNALED;
		cb->rdma_sq_wr.wr.num_sge = 1;
	}

	/* 
	 * A chain of 2 WRs, INVALDATE_MR + REG_MR.
	 * both unsignaled.  The client uses them to reregister
	 * the rdma buffers with a new key each iteration.
	 */
    // Jack: check can we fix the key
	cb->reg_mr_wr.wr.opcode = IB_WR_REG_MR;     //(legacy:fastreg)
	cb->reg_mr_wr.mr = cb->reg_mr;

	cb->invalidate_wr.next = &cb->reg_mr_wr.wr;
	cb->invalidate_wr.opcode = IB_WR_LOCAL_INV;
}

static int krping_setup_buffers(struct krping_cb *cb) // init all buffers < 1.pd->cq->qp 2.[mr] 3.xxx >
{
	int ret;
	DEBUG_LOG("\n->%s();\n", __func__);

	DEBUG_LOG(PFX "krping_setup_buffers called on cb %p\n", cb);

	cb->recv_dma_addr = dma_map_single(cb->pd->device->dma_device,  // for remote access  (mapping together)
				   &cb->recv_buf,                                   // for local access (mapping together)
				   sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);        // cb->recv_dma
	pci_unmap_addr_set(cb, recv_mapping, cb->recv_dma_addr);
	cb->send_dma_addr = dma_map_single(cb->pd->device->dma_device,  // send 
					   &cb->send_buf, sizeof(cb->send_buf),         // use send buffer
					   DMA_BIDIRECTIONAL);                          // cb->send_dma
	pci_unmap_addr_set(cb, send_mapping, cb->send_dma_addr);

	cb->rdma_buf = kmalloc(cb->size, GFP_KERNEL);           // alloc rdma buffer (the only allocated buf)
	if (!cb->rdma_buf) {
		DEBUG_LOG(PFX "rdma_buf malloc failed\n");
		ret = -ENOMEM;
		goto bail;
	}
	cb->rdma_dma_addr = dma_map_single(cb->pd->device->dma_device,  // 
			       cb->rdma_buf, cb->size,                          // userdma buffer 
			       DMA_BIDIRECTIONAL);                              // 
	pci_unmap_addr_set(cb, rdma_mapping, cb->rdma_dma_addr);
	
    DEBUG_LOG("@@@ 1 cb->rdma_buf = dma_map_single( cb->rdma_buf )\n");
    DEBUG_LOG("@@@ 1 cb->rdma_buf = 0x%p \t a ????? for local access (mapping together)\n", cb->rdma_buf);
    DEBUG_LOG("@@@ 1 cb->rdma_dma_addr = 0x%p a kernel vaddr for remote access  (mapping together)\n", cb->rdma_dma_addr);
	
    cb->page_list_len = (((cb->size - 1) & PAGE_MASK) + PAGE_SIZE)
				>> PAGE_SHIFT;
	cb->reg_mr = ib_alloc_mr(cb->pd,  IB_MR_TYPE_MEM_REG, // fill up lkey and rkey
				 cb->page_list_len);
	if (IS_ERR(cb->reg_mr)) {
		ret = PTR_ERR(cb->reg_mr);
		DEBUG_LOG(PFX "recv_buf reg_mr failed %d\n", ret);
		goto bail;
	}

    //cb->send_sgl.lkey = cb->reg_mr->lkey; // Jack: doesn't work
    //cb->recv_sgl.lkey = cb->reg_mr->lkey; // Jack: doesn't work
	DEBUG_LOG("@@@ reg rkey %d page_list_len %u\n",
	                cb->reg_mr->rkey, cb->page_list_len);

	DEBUG_LOG("@@@ 1 Jack cb->reg_mr->lkey %d from mr \n", cb->reg_mr->lkey);
	DEBUG_LOG("@@@ 1 Jack cb->send_sgl.lkey %d from mr \n", cb->send_sgl.lkey);
	DEBUG_LOG("@@@ 1 Jack cb->recv_sgl.lkey %d from mr \n", cb->recv_sgl.lkey);
	DEBUG_LOG("@@@ 1 correct lkey=%d (ref: ./drivers/infiniband/core/mad.c ) \
            (ctx->pd->local_dma_lkey)\n", cb->pd->local_dma_lkey);     //4450 dynamic

    /*
        // these are all NULL.
        cb->dma_mr->lkey, cb->dma_mr->rkey
        cb->rdma_mr->lkey, cb->rdma_mr->rkey
        cb->start_mr->lkey, cb->start_mr->rkey
    */

	if (!cb->server || cb->wlat || cb->rlat || cb->bw) { // only client generates rdma address for server
        DEBUG_LOG("only server setup start_buf start_dma_addr\n");
		cb->start_buf = kmalloc(cb->size, GFP_KERNEL);
		if (!cb->start_buf) {
			DEBUG_LOG(PFX "start_buf malloc failed\n");
			ret = -ENOMEM;
			goto bail;
		}

		cb->start_dma_addr = dma_map_single(cb->pd->device->dma_device, 
						   cb->start_buf, cb->size, // only client generates rdma address for server
						   DMA_BIDIRECTIONAL);
	    DEBUG_LOG("@@@ cb->start_dma_addr = 0x%lx Jack (only client)\n", cb->start_dma_addr);
		pci_unmap_addr_set(cb, start_mapping, cb->start_dma_addr);
	    DEBUG_LOG("@@@ cb->start_dma_addr = 0x%lx Jack (only client)\n", cb->start_dma_addr);
	}

	krping_setup_wr(cb); //(only server setup rdma_sq_wr since only server issue rdma operations)
	DEBUG_LOG(PFX "allocated & registered buffers...\n");
	DEBUG_LOG("\n\n");
	return 0;
bail:
	if (cb->reg_mr && !IS_ERR(cb->reg_mr))
		ib_dereg_mr(cb->reg_mr);
	if (cb->rdma_mr && !IS_ERR(cb->rdma_mr))
		ib_dereg_mr(cb->rdma_mr);
	if (cb->dma_mr && !IS_ERR(cb->dma_mr))
		ib_dereg_mr(cb->dma_mr);
	if (cb->rdma_buf)
		kfree(cb->rdma_buf);
	if (cb->start_buf)
		kfree(cb->start_buf);
	return ret;
}

static void krping_free_buffers(struct krping_cb *cb)
{
	DEBUG_LOG("krping_free_buffers called on cb %p\n", cb);
	
	if (cb->dma_mr)
		ib_dereg_mr(cb->dma_mr);
	if (cb->rdma_mr)
		ib_dereg_mr(cb->rdma_mr);
	if (cb->start_mr)
		ib_dereg_mr(cb->start_mr);
	if (cb->reg_mr)
		ib_dereg_mr(cb->reg_mr);

	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, recv_mapping),
			 sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, send_mapping),
			 sizeof(cb->send_buf), DMA_BIDIRECTIONAL);
	dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, rdma_mapping),
			 cb->size, DMA_BIDIRECTIONAL);
	kfree(cb->rdma_buf);
	if (cb->start_buf) {
		dma_unmap_single(cb->pd->device->dma_device,
			 pci_unmap_addr(cb, start_mapping),
			 cb->size, DMA_BIDIRECTIONAL);
		kfree(cb->start_buf);
	}
}

static int krping_create_qp(struct krping_cb *cb)
{
	struct ib_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = cb->txdepth;
	init_attr.cap.max_recv_wr = 2;
	
	/* For flush_qp() */
	init_attr.cap.max_send_wr++;
	init_attr.cap.max_recv_wr++;

	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;

	if (cb->server) {
		ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->child_cm_id->qp;
	} else {
		ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->cm_id->qp;
	}

	return ret;
}

static void krping_free_qp(struct krping_cb *cb)
{
	ib_destroy_qp(cb->qp);
	ib_destroy_cq(cb->cq);
	ib_dealloc_pd(cb->pd);
}

static int krping_setup_qp(struct krping_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;
	struct ib_cq_init_attr attr = {0};
	
    DEBUG_LOG("\n->%s();\n", __func__);

	//cb->pd = ib_alloc_pd(cm_id->device, 0);
	cb->pd = ib_alloc_pd(cm_id->device);    
	if (IS_ERR(cb->pd)) {
		printk(KERN_ERR PFX "ib_alloc_pd failed\n");
		return PTR_ERR(cb->pd);
	}
	DEBUG_LOG("created pd %p\n", cb->pd);

	attr.cqe = cb->txdepth * 2;
	attr.comp_vector = 0;
	cb->cq = ib_create_cq(cm_id->device, krping_cq_event_handler, NULL,
			      cb, &attr);
	if (IS_ERR(cb->cq)) {
		printk(KERN_ERR PFX "ib_create_cq failed\n");
		ret = PTR_ERR(cb->cq);
		goto err1;
	}
	DEBUG_LOG("created cq %p task\n", cb->cq);

	if (!cb->wlat && !cb->rlat && !cb->bw && !cb->frtest) {
		ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
		if (ret) {
			printk(KERN_ERR PFX "ib_create_cq failed\n");
			goto err2;
		}
	}

	ret = krping_create_qp(cb);
	if (ret) {
		printk(KERN_ERR PFX "krping_create_qp failed: %d\n", ret);
		goto err2;
	}
	DEBUG_LOG("created qp %p\n", cb->qp);
	return 0;
err2:
	ib_destroy_cq(cb->cq);
err1:
	ib_dealloc_pd(cb->pd);
	return ret;
}

/*
 * return the (possibly rebound) rkey for the rdma buffer.
 * REG mode: invalidate and rebind via reg wr.
 * other modes: just return the mr rkey.
 */
static u32 krping_rdma_rkey(struct krping_cb *cb, u64 buf, int post_inv)
{
	u32 rkey;
	struct ib_send_wr *bad_wr;
	int ret;
	struct scatterlist sg = {0};

	cb->invalidate_wr.ex.invalidate_rkey = cb->reg_mr->rkey;

	/*
	 * Update the reg key.
	 */
	ib_update_fast_reg_key(cb->reg_mr, ++cb->key);
	cb->reg_mr_wr.key = cb->reg_mr->rkey;

	/*
	 * Update the reg WR with new buf info.
	 */
	if (buf == (u64)cb->start_dma_addr)
		cb->reg_mr_wr.access = IB_ACCESS_REMOTE_READ;
	else
		cb->reg_mr_wr.access = IB_ACCESS_REMOTE_WRITE | IB_ACCESS_LOCAL_WRITE;
	sg_dma_address(&sg) = buf;      // rdma_buf = rdma_buf 
	sg_dma_len(&sg) = cb->size;

	//ret = ib_map_mr_sg(cb->reg_mr, &sg, 1, NULL, PAGE_SIZE);
	ret = ib_map_mr_sg(cb->reg_mr, &sg, 1, PAGE_SIZE);  // snyc ib_dma_sync_single_for_cpu/dev
	BUG_ON(ret <= 0 || ret > cb->page_list_len);

	DEBUG_LOG(PFX "post_inv = %d, reg_mr new rkey %d pgsz %u len %u"
		" iova_start %llx\n",
		post_inv,
		cb->reg_mr_wr.key,
		cb->reg_mr->page_size,
		cb->reg_mr->length,
		cb->reg_mr->iova);

	DEBUG_LOG("ib_post_send>>>>\n");
	if (post_inv)
		ret = ib_post_send(cb->qp, &cb->invalidate_wr, &bad_wr);
	else
		ret = ib_post_send(cb->qp, &cb->reg_mr_wr.wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "post send error %d\n", ret);
		cb->state = ERROR;
	}
	rkey = cb->reg_mr->rkey; //
	return rkey;
}

static void krping_format_send(struct krping_cb *cb, u64 buf)
{
	struct krping_rdma_info *info = &cb->send_buf; // update send_buf
	u32 rkey;

	/*
	 * Client side will do reg or mw bind before
	 * advertising the rdma buffer.  Server side
	 * sends have no data.
	 */
	if (!cb->server || cb->wlat || cb->rlat || cb->bw) { // only client
		rkey = krping_rdma_rkey(cb, buf, !cb->server_invalidate);
		info->buf = htonll(buf);            // update. hton: host to net order
		info->rkey = htonl(rkey);           // update
		info->size = htonl(cb->size);       // update
		DEBUG_LOG("RDMA addr %llx rkey %d len %d\n",
			  (unsigned long long)buf, rkey, cb->size);
	}
}

static void krping_test_server(struct krping_cb *cb)
{
	struct ib_send_wr *bad_wr, inv;
    //struct ib_recv_wr *bad_wr2;
	int ret;

	while (1) {
	    // Jack
        //DEBUG_LOG("ib_post_recv<<<<\n");
	    //ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr2);
	    //if (ret) {
		//    printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
		//    break;
	    //}

            /** When using kernel space verbs, before and after RDMA operations it is recommended to call ib_dma_sync*(OFED API) on buffers because of CPU cache. **/
            /*
            struct scatterlist sg = {0};
            //sg_dma_address(&sg) = buf;      // rdma_buf = rdma_buf 
            //sg_dma_address(&sg) = cb->rdma_dma_addr;      // rdma_buf = rdma_buf 
            sg_dma_address(&sg) = cb->start_dma_addr;      // rdma_buf = rdma_buf 
            sg_dma_len(&sg) = cb->size;
            ret = ib_map_mr_sg(cb->reg_mr, &sg, 1, PAGE_SIZE);  // snyc ib_dma_sync_single_for_cpu/dev 
            */

		/* Wait for client's Start STAG/TO/Len */
		wait_event_interruptible(cb->sem, cb->state >= RDMA_READ_ADV);
		//wait_event(cb->sem, cb->state >= RDMA_READ_ADV);
        if (cb->state != RDMA_READ_ADV) {
			printk(KERN_ERR PFX "wait for RDMA_READ_ADV state %d\n",
				cb->state);
			break;
		}

		DEBUG_LOG("----- SERVER RECEIVED SINK (3things) ADV -----\n");
		
        
        
        DEBUG_LOG("----- cb->size=%d -----\n", cb->size);
        //TODO: place a timer here ( rdtll) TODO: remove all DEBUG msg 
        //TODO: check SIZE- - 128k really!? 
// example:
// unsigned long ts_start, ts_end;
// rdtscll(ts_start);
// (...measuring...)
// rdtscll(ts_end);
        
        
        unsigned long ts_start, ts_compose, ts_post, ts_end;
        rdtscll(ts_start);
        // 
        //vmalloc(4k); rdma_dma_addr // payload or receiveing buf (ithink depends 1st or 2nd) TODO: check 2nd's buf. if diff GOOD
        // 
        
// time1 : compose msg info
		cb->rdma_sq_wr.rkey = cb->remote_rkey;              // updated from remote
		cb->rdma_sq_wr.remote_addr = cb->remote_addr;       // updated from remote
		cb->rdma_sq_wr.wr.sg_list->length = cb->remote_len; // updated from remote

		cb->rdma_sgl.lkey = krping_rdma_rkey(cb, cb->rdma_dma_addr, !cb->read_inv); // Jack: payload or receiveing buf 
		cb->rdma_sq_wr.wr.next = NULL;

		/* Issue RDMA Read. */
		if (cb->read_inv)
			cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ_WITH_INV;
		else {

			cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ;
			/*  
			 * Immediately follow the read with a 
			 * fenced LOCAL_INV.
			 */
			cb->rdma_sq_wr.wr.next = &inv;
			memset(&inv, 0, sizeof inv);
			inv.opcode = IB_WR_LOCAL_INV;
			inv.ex.invalidate_rkey = cb->reg_mr->rkey;
			inv.send_flags = IB_SEND_FENCE;
		}

	//DEBUG_LOG("ib_post_send>>>>\n");
// time 2: send
        rdtscll(ts_compose);
		ret = ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}
		cb->rdma_sq_wr.wr.next = NULL;
// time 3: send done
        rdtscll(ts_post);
	//DEBUG_LOG("----- SERVER POSTED RDMA READ REQ AND WAITING FOR READ COMPLETE -----\n");

		/* Wait for read completion */
		wait_event_interruptible(cb->sem, 
					 cb->state >= RDMA_READ_COMPLETE);
// time 4: read(task) done
        rdtscll(ts_end);
		if (cb->state != RDMA_READ_COMPLETE) {
			printk(KERN_ERR PFX 
			       "wait for RDMA_READ_COMPLETE state %d\n",
			       cb->state);
			break;
		}
        
        //DEBUG_LOG("----- ts_start=%lu, ts_compose=%lu, ts_post=%lu, ts_end=%lu  ----\n",
        //                                     ts_start, ts_compose, ts_post, ts_end);
        DEBUG_LOG("----- compose time=%lu, post time=%lu, end time=%lu  ----\n",
                                    ts_compose-ts_start, ts_post-ts_start, ts_end-ts_start);

		DEBUG_LOG("----- SERVER RECEIVED READ COMPLETE  ----\n");
		DEBUG_LOG("\n\n\n");

		/* Display data in recv buf */
		if (cb->verbose)
			printk(KERN_INFO PFX "server ping data: %s\n", 
				cb->rdma_buf);

		/* Tell client to continue */
		if (cb->server && cb->server_invalidate) {
			cb->sq_wr.ex.invalidate_rkey = cb->remote_rkey;
			cb->sq_wr.opcode = IB_WR_SEND_WITH_INV;
			DEBUG_LOG("send-w-inv rkey %d\n", cb->remote_rkey);
		} 
	    DEBUG_LOG("ib_post_send>>>>\n");
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}
		DEBUG_LOG("----- SERVER POSTED GO AHEAD -----\n");

		/* Wait for client's RDMA STAG/TO/Len */
		wait_event_interruptible(cb->sem, cb->state >= RDMA_WRITE_ADV);
		if (cb->state != RDMA_WRITE_ADV) {
			printk(KERN_ERR PFX 
			       "wait for RDMA_WRITE_ADV state %d\n",
			       cb->state);
			break;
		}
		DEBUG_LOG("----- SERVER RECEIVED SINK (3things) ADV -----\n");

		/* RDMA Write echo data */
		cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_WRITE;  //WRITE
		cb->rdma_sq_wr.rkey = cb->remote_rkey;
		cb->rdma_sq_wr.remote_addr = cb->remote_addr;
		cb->rdma_sq_wr.wr.sg_list->length = strlen(cb->rdma_buf) + 1;
		if (cb->local_dma_lkey)
			cb->rdma_sgl.lkey = cb->qp->device->local_dma_lkey;
		else 
			cb->rdma_sgl.lkey = krping_rdma_rkey(cb, cb->rdma_dma_addr, 0);
			
		DEBUG_LOG("rdma write from lkey %d laddr %llx len %d\n",
			  cb->rdma_sq_wr.wr.sg_list->lkey,
			  (unsigned long long)cb->rdma_sq_wr.wr.sg_list->addr,
			  cb->rdma_sq_wr.wr.sg_list->length);

	    DEBUG_LOG("ib_post_send>>>>\n");
		ret = ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}

		/* Wait for completion */
		ret = wait_event_interruptible(cb->sem, cb->state >= 
							 RDMA_WRITE_COMPLETE);
		if (cb->state != RDMA_WRITE_COMPLETE) {
			printk(KERN_ERR PFX 
			       "wait for RDMA_WRITE_COMPLETE state %d\n",
			       cb->state);
			break;
		}
		DEBUG_LOG("server rdma write complete \n");

		cb->state = CONNECTED;

		/* Tell client to begin again */
		if (cb->server && cb->server_invalidate) {
			cb->sq_wr.ex.invalidate_rkey = cb->remote_rkey;
			cb->sq_wr.opcode = IB_WR_SEND_WITH_INV;
			DEBUG_LOG("send-w-inv rkey %d\n", cb->remote_rkey);
		} 
	    DEBUG_LOG("ib_post_send>>>>\n");
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}
		DEBUG_LOG("----- SERVER POSTED GO AHEAD -----\n");
        
        
        //Jack: experiment TODO: new a size
        // cb->size *=2; <<<< new a size
	}
}

static void rlat_test(struct krping_cb *cb)
{
	int scnt;
	int iters = cb->count;
	struct timeval start_tv, stop_tv;
	int ret;
	struct ib_wc wc;
	struct ib_send_wr *bad_wr;
	int ne;

	scnt = 0;
	cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_READ;
	cb->rdma_sq_wr.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr.wr.sg_list->length = cb->size;

	do_gettimeofday(&start_tv);
	if (!cb->poll) {
		cb->state = RDMA_READ_ADV;
		ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
	}
	while (scnt < iters) {

		cb->state = RDMA_READ_ADV;
		ret = ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX  
				"Couldn't post send: ret=%d scnt %d\n",
				ret, scnt);
			return;
		}

		do {
			if (!cb->poll) {
				wait_event_interruptible(cb->sem, 
					cb->state != RDMA_READ_ADV);
				if (cb->state == RDMA_READ_COMPLETE) {
					ne = 1;
					ib_req_notify_cq(cb->cq, 
						IB_CQ_NEXT_COMP);
				} else {
					ne = -1;
				}
			} else
				ne = ib_poll_cq(cb->cq, 1, &wc);
			if (cb->state == ERROR) {
				printk(KERN_ERR PFX 
					"state == ERROR...bailing scnt %d\n", 
					scnt);
				return;
			}
		} while (ne == 0);

		if (ne < 0) {
			printk(KERN_ERR PFX "poll CQ failed %d\n", ne);
			return;
		}
		if (cb->poll && wc.status != IB_WC_SUCCESS) {
			printk(KERN_ERR PFX "Completion wth error at %s:\n",
				cb->server ? "server" : "client");
			printk(KERN_ERR PFX "Failed status %d: wr_id %d\n",
				wc.status, (int) wc.wr_id);
			return;
		}
		++scnt;
	}
	do_gettimeofday(&stop_tv);

        if (stop_tv.tv_usec < start_tv.tv_usec) {
                stop_tv.tv_usec += 1000000;
                stop_tv.tv_sec  -= 1;
        }

	printk(KERN_ERR PFX "delta sec %lu delta usec %lu iter %d size %d\n",
		stop_tv.tv_sec - start_tv.tv_sec, 
		stop_tv.tv_usec - start_tv.tv_usec,
		scnt, cb->size);
}

static void wlat_test(struct krping_cb *cb)
{
	int ccnt, scnt, rcnt;
	int iters=cb->count;
	volatile char *poll_buf = (char *) cb->start_buf;
	char *buf = (char *)cb->rdma_buf;
	struct timeval start_tv, stop_tv;
	cycles_t *post_cycles_start, *post_cycles_stop;
	cycles_t *poll_cycles_start, *poll_cycles_stop;
	cycles_t *last_poll_cycles_start;
	cycles_t sum_poll = 0, sum_post = 0, sum_last_poll = 0;
	int i;
	int cycle_iters = 1000;

	ccnt = 0;
	scnt = 0;
	rcnt = 0;

	post_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!post_cycles_start) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	post_cycles_stop = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!post_cycles_stop) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	poll_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!poll_cycles_start) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	poll_cycles_stop = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!poll_cycles_stop) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	last_poll_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), 
		GFP_KERNEL);
	if (!last_poll_cycles_start) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_WRITE;
	cb->rdma_sq_wr.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr.wr.sg_list->length = cb->size;

	if (cycle_iters > iters)
		cycle_iters = iters;
	do_gettimeofday(&start_tv);
	while (scnt < iters || ccnt < iters || rcnt < iters) {

		/* Wait till buffer changes. */
		if (rcnt < iters && !(scnt < 1 && !cb->server)) {
			++rcnt;
			while (*poll_buf != (char)rcnt) {
				if (cb->state == ERROR) {
					printk(KERN_ERR PFX 
						"state = ERROR, bailing\n");
					return;
				}
			}
		}

		if (scnt < iters) {
			struct ib_send_wr *bad_wr;

			*buf = (char)scnt+1;
			if (scnt < cycle_iters)
				post_cycles_start[scnt] = get_cycles();
			if (ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr)) {
				printk(KERN_ERR PFX  
					"Couldn't post send: scnt=%d\n",
					scnt);
				return;
			}
			if (scnt < cycle_iters)
				post_cycles_stop[scnt] = get_cycles();
			scnt++;
		}

		if (ccnt < iters) {
			struct ib_wc wc;
			int ne;

			if (ccnt < cycle_iters)
				poll_cycles_start[ccnt] = get_cycles();
			do {
				if (ccnt < cycle_iters)
					last_poll_cycles_start[ccnt] = 
						get_cycles();
				ne = ib_poll_cq(cb->cq, 1, &wc);
			} while (ne == 0);
			if (ccnt < cycle_iters)
				poll_cycles_stop[ccnt] = get_cycles();
			++ccnt;

			if (ne < 0) {
				printk(KERN_ERR PFX "poll CQ failed %d\n", ne);
				return;
			}
			if (wc.status != IB_WC_SUCCESS) {
				printk(KERN_ERR PFX 
					"Completion wth error at %s:\n",
					cb->server ? "server" : "client");
				printk(KERN_ERR PFX 
					"Failed status %d: wr_id %d\n",
					wc.status, (int) wc.wr_id);
				printk(KERN_ERR PFX 
					"scnt=%d, rcnt=%d, ccnt=%d\n",
					scnt, rcnt, ccnt);
				return;
			}
		}
	}
	do_gettimeofday(&stop_tv);

        if (stop_tv.tv_usec < start_tv.tv_usec) {
                stop_tv.tv_usec += 1000000;
                stop_tv.tv_sec  -= 1;
        }

	for (i=0; i < cycle_iters; i++) {
		sum_post += post_cycles_stop[i] - post_cycles_start[i];
		sum_poll += poll_cycles_stop[i] - poll_cycles_start[i];
		sum_last_poll += poll_cycles_stop[i]-last_poll_cycles_start[i];
	}
	printk(KERN_ERR PFX 
		"delta sec %lu delta usec %lu iter %d size %d cycle_iters %d"
		" sum_post %llu sum_poll %llu sum_last_poll %llu\n",
		stop_tv.tv_sec - start_tv.tv_sec, 
		stop_tv.tv_usec - start_tv.tv_usec,
		scnt, cb->size, cycle_iters, 
		(unsigned long long)sum_post, (unsigned long long)sum_poll, 
		(unsigned long long)sum_last_poll);
	kfree(post_cycles_start);
	kfree(post_cycles_stop);
	kfree(poll_cycles_start);
	kfree(poll_cycles_stop);
	kfree(last_poll_cycles_start);
}

static void bw_test(struct krping_cb *cb)
{
	int ccnt, scnt, rcnt;
	int iters=cb->count;
	struct timeval start_tv, stop_tv;
	cycles_t *post_cycles_start, *post_cycles_stop;
	cycles_t *poll_cycles_start, *poll_cycles_stop;
	cycles_t *last_poll_cycles_start;
	cycles_t sum_poll = 0, sum_post = 0, sum_last_poll = 0;
	int i;
	int cycle_iters = 1000;

	ccnt = 0;
	scnt = 0;
	rcnt = 0;

	post_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!post_cycles_start) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	post_cycles_stop = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!post_cycles_stop) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	poll_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!poll_cycles_start) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	poll_cycles_stop = kmalloc(cycle_iters * sizeof(cycles_t), GFP_KERNEL);
	if (!poll_cycles_stop) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	last_poll_cycles_start = kmalloc(cycle_iters * sizeof(cycles_t), 
		GFP_KERNEL);
	if (!last_poll_cycles_start) {
		printk(KERN_ERR PFX "%s kmalloc failed\n", __FUNCTION__);
		return;
	}
	cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_WRITE;
	cb->rdma_sq_wr.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr.wr.sg_list->length = cb->size;

	if (cycle_iters > iters)
		cycle_iters = iters;
	do_gettimeofday(&start_tv);
	while (scnt < iters || ccnt < iters) {

		while (scnt < iters && scnt - ccnt < cb->txdepth) {
			struct ib_send_wr *bad_wr;

			if (scnt < cycle_iters)
				post_cycles_start[scnt] = get_cycles();
			if (ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr)) {
				printk(KERN_ERR PFX  
					"Couldn't post send: scnt=%d\n",
					scnt);
				return;
			}
			if (scnt < cycle_iters)
				post_cycles_stop[scnt] = get_cycles();
			++scnt;
		}

		if (ccnt < iters) {
			int ne;
			struct ib_wc wc;

			if (ccnt < cycle_iters)
				poll_cycles_start[ccnt] = get_cycles();
			do {
				if (ccnt < cycle_iters)
					last_poll_cycles_start[ccnt] = 
						get_cycles();
				ne = ib_poll_cq(cb->cq, 1, &wc);
			} while (ne == 0);
			if (ccnt < cycle_iters)
				poll_cycles_stop[ccnt] = get_cycles();
			ccnt += 1;

			if (ne < 0) {
				printk(KERN_ERR PFX "poll CQ failed %d\n", ne);
				return;
			}
			if (wc.status != IB_WC_SUCCESS) {
				printk(KERN_ERR PFX 
					"Completion wth error at %s:\n",
					cb->server ? "server" : "client");
				printk(KERN_ERR PFX 
					"Failed status %d: wr_id %d\n",
					wc.status, (int) wc.wr_id);
				return;
			}
		}
	}
	do_gettimeofday(&stop_tv);

        if (stop_tv.tv_usec < start_tv.tv_usec) {
                stop_tv.tv_usec += 1000000;
                stop_tv.tv_sec  -= 1;
        }

	for (i=0; i < cycle_iters; i++) {
		sum_post += post_cycles_stop[i] - post_cycles_start[i];
		sum_poll += poll_cycles_stop[i] - poll_cycles_start[i];
		sum_last_poll += poll_cycles_stop[i]-last_poll_cycles_start[i];
	}
	printk(KERN_ERR PFX 
		"delta sec %lu delta usec %lu iter %d size %d cycle_iters %d"
		" sum_post %llu sum_poll %llu sum_last_poll %llu\n",
		stop_tv.tv_sec - start_tv.tv_sec, 
		stop_tv.tv_usec - start_tv.tv_usec,
		scnt, cb->size, cycle_iters, 
		(unsigned long long)sum_post, (unsigned long long)sum_poll, 
		(unsigned long long)sum_last_poll);
	kfree(post_cycles_start);
	kfree(post_cycles_stop);
	kfree(poll_cycles_start);
	kfree(poll_cycles_stop);
	kfree(last_poll_cycles_start);
}

static void krping_rlat_test_server(struct krping_cb *cb)
{
	struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	/* Spin waiting for client's Start STAG/TO/Len */
	while (cb->state < RDMA_READ_ADV) {
		krping_cq_event_handler(cb->cq, cb);
	}

	/* Send STAG/TO/Len to client */
	krping_format_send(cb, cb->start_dma_addr);
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status) {
		printk(KERN_ERR PFX "send completiong error %d\n", wc.status);
		return;
	}

	wait_event_interruptible(cb->sem, cb->state == ERROR);
}

static void krping_wlat_test_server(struct krping_cb *cb)
{
	struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	/* Spin waiting for client's Start STAG/TO/Len */
	while (cb->state < RDMA_READ_ADV) {
		krping_cq_event_handler(cb->cq, cb);
	}

	/* Send STAG/TO/Len to client */
	krping_format_send(cb, cb->start_dma_addr);
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status) {
		printk(KERN_ERR PFX "send completiong error %d\n", wc.status);
		return;
	}

	wlat_test(cb);
	wait_event_interruptible(cb->sem, cb->state == ERROR);
}

static void krping_bw_test_server(struct krping_cb *cb)
{
	struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	/* Spin waiting for client's Start STAG/TO/Len */
	while (cb->state < RDMA_READ_ADV) {
		krping_cq_event_handler(cb->cq, cb);
	}

	/* Send STAG/TO/Len to client */
	krping_format_send(cb, cb->start_dma_addr);
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status) {
		printk(KERN_ERR PFX "send completiong error %d\n", wc.status);
		return;
	}

	if (cb->duplex)
		bw_test(cb);
	wait_event_interruptible(cb->sem, cb->state == ERROR);
}

static int reg_supported(struct ib_device *dev)
{
	u64 needed_flags = IB_DEVICE_MEM_MGT_EXTENSIONS |
			   IB_DEVICE_LOCAL_DMA_LKEY;
    struct ib_device_attr device_attr; 
    int ret;
    ret = ib_query_device(dev, &device_attr);
    //DEBUG_LOG("%s(): return %d\n", __func__, ret);
    //DEBUG_LOG("%s(): needed_flags %llx\n", __func__, needed_flags);
    //DEBUG_LOG("%s(): device_attr.device_cap_flag %llx\n", __func__, device_attr.device_cap_flags);
    DEBUG_LOG("%s(): IB_DEVICE_MEM_MGT_EXTENSIONS %llx\n", __func__, IB_DEVICE_MEM_MGT_EXTENSIONS);
    DEBUG_LOG("%s(): IB_DEVICE_LOCAL_DMA_LKEY %llx\n", __func__, IB_DEVICE_LOCAL_DMA_LKEY);
    DEBUG_LOG("%s(): (device_attr.device_cap_flags & needed_flags) %llx\n", __func__, (device_attr.device_cap_flags & needed_flags));
	
    if ((device_attr.device_cap_flags & needed_flags) != needed_flags) {
		printk(KERN_ERR PFX 
			"Fastreg not supported - device_cap_flags 0x%llx\n",
			(u64)device_attr.device_cap_flags);
        //return 0;
		return 1;
	}
	DEBUG_LOG("Fastreg/local_dma_lkey supported - device_cap_flags 0x%llx\n",
		(u64)device_attr.device_cap_flags);

    /*
	u64 needed_flags = IB_DEVICE_MEM_MGT_EXTENSIONS |
			   IB_DEVICE_LOCAL_DMA_LKEY;

	if ((dev->attrs.device_cap_flags & needed_flags) != needed_flags) {
	//if ((dev->ib_device_attr.device_cap_flags & needed_flags) != needed_flags) {
		printk(KERN_ERR PFX 
			"Fastreg not supported - device_cap_flags 0x%llx\n",
			(u64)dev->attrs.device_cap_flags);
			//(u64)dev->ib_device_attr.device_cap_flags);
		return 0;
	}
	//DEBUG_LOG("Fastreg/local_dma_lkey supported - device_cap_flags 0x%llx\n",
	DEBUG_LOG("Fastreg/local_dma_lkey supported - device_cap_flags 0x%llx\n",
		(u64)dev->attrs.device_cap_flags);
		//(u64)dev->ib_device_attr.device_cap_flags);
    */
	//DEBUG_LOG("Fastreg/local_dma_lkey supported - device_cap_flags ??? (Jack: skip this check)\n");
	return 1;
}

static void fill_sockaddr(struct sockaddr_storage *sin, struct krping_cb *cb)
{
	memset(sin, 0, sizeof(*sin));

	if (cb->addr_type == AF_INET) {
		struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
		sin4->sin_family = AF_INET;
		memcpy((void *)&sin4->sin_addr.s_addr, cb->addr, 4);
		sin4->sin_port = cb->port;
	} else if (cb->addr_type == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sin;
		sin6->sin6_family = AF_INET6;
		memcpy((void *)&sin6->sin6_addr, cb->addr, 16);
		sin6->sin6_port = cb->port;
	}
}

static int krping_bind_server(struct krping_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;


	fill_sockaddr(&sin, cb);

	ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *)&sin);
	if (ret) {
		printk(KERN_ERR PFX "rdma_bind_addr error %d\n", ret);
		return ret;
	}
	DEBUG_LOG("rdma_bind_addr successful\n");

	DEBUG_LOG("rdma_listen\n");
	ret = rdma_listen(cb->cm_id, 3);
	if (ret) {
		printk(KERN_ERR PFX "rdma_listen failed: %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= CONNECT_REQUEST);
	if (cb->state != CONNECT_REQUEST) {
		printk(KERN_ERR PFX "wait for CONNECT_REQUEST state %d\n",
			cb->state);
		return -1;
	}

	if (!reg_supported(cb->child_cm_id->device)) //Jack
		return -EINVAL;

	return 0;
}

static void krping_run_server(struct krping_cb *cb)
{
	struct ib_recv_wr *bad_wr;
	int ret;

	ret = krping_bind_server(cb);
	if (ret)
		return;

	ret = krping_setup_qp(cb, cb->child_cm_id);
	if (ret) {
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		goto err0;
	}

	ret = krping_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR PFX "krping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	DEBUG_LOG("ib_post_recv<<<<\n");
	ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
		goto err2;
	}

	ret = krping_accept(cb);
	if (ret) {
		printk(KERN_ERR PFX "connect error %d\n", ret);
		goto err2;
	}

	if (cb->wlat) {
        DEBUG_LOG("TEST server: krping_wlat_test_server()\n");
		krping_wlat_test_server(cb);
    }
	else if (cb->rlat) {
        DEBUG_LOG("TEST server: krping_rlat_test_server()\n");
		krping_rlat_test_server(cb);
    }
	else if (cb->bw) {
        DEBUG_LOG("TEST server: krping_bw_test_server()\n");
		krping_bw_test_server(cb);
    }
	else {
        DEBUG_LOG("\n\n\n\n\n\n\nTEST server: krping_test_server()\n");
		krping_test_server(cb);
    }
	rdma_disconnect(cb->child_cm_id);
err2:
	krping_free_buffers(cb);
err1:
	krping_free_qp(cb);
err0:
	rdma_destroy_id(cb->child_cm_id);
}

static void krping_test_client(struct krping_cb *cb)
{
	int ping, start, cc, i, ret;
	struct ib_send_wr *bad_wr;
	unsigned char c;

	start = 65;
	for (ping = 0; !cb->count || ping < cb->count; ping++) {
		cb->state = RDMA_READ_ADV; // !!!!!!!!!!!
 
		/* Put some ascii text in the buffer. */
		cc = sprintf(cb->start_buf, "rdma-ping-%d: ", ping); // start_buf 
		for (i = cc, c = start; i < cb->size; i++) { // Jack need to change
			cb->start_buf[i] = c;
			c++;
			if (c > 122)
				c = 65;
		}
		start++;
		if (start > 122)
			start = 65;
		cb->start_buf[cb->size - 1] = 0;


	    DEBUG_LOG("\n"); msleep(3000);
	    
        int str_len = strlen(cb->start_buf);
        DEBUG_LOG("strlen()=%d\n", str_len);
        if (str_len/1024)
	        DEBUG_LOG("strlen()=%dK\n", str_len/1024);
        if (str_len/1024/1024)
	        DEBUG_LOG("strlen()=%dM\n", str_len/1024/1024);

        //DEBUG_LOG("%s(): cb->start_dma_addr = 0x%d then upsate rkey\n", cb->start_dma_addr);
        //if (&cb->start_dma_addr!=NULL)
        //    DEBUG_LOG("%s(): cb->start_dma_addr = 0x%llx then upsate rkey\n", cb->start_dma_addr);
        //else
        //    DEBUG_LOG("%s(): cb->start_dma_addr = NULL then upsate rkey\n");
        krping_format_send(cb, cb->start_dma_addr); // (only client) &cb->send_buf = buf(cb->start_dma_addr)  
                                                    // and update rkey (send buf) 
		if (cb->state == ERROR) {
			printk(KERN_ERR PFX "krping_format_send failed\n");
			break;
		}
	    DEBUG_LOG("updated &cb->send_buf.buf = 0x%p\n", &cb->send_buf.buf); //Jack123
	    DEBUG_LOG("updated cb->send_buf.rkey = %d\n", cb->send_buf.rkey);
	    DEBUG_LOG("updated cb->send_buf.size = %d\n", cb->send_buf.size);
        DEBUG_LOG("@@@ (check) cb->start_dma_addr = 0x%llx Jack (only client)\n", cb->start_dma_addr); 

	    DEBUG_LOG("\n\n\n"); msleep(3000);
	    DEBUG_LOG("2. ib_post_send>>>>\n");
    
       
        /** When using kernel space verbs, before and after RDMA operations it is recommended to call ib_dma_sync*(OFED API) on buffers because of CPU cache. **/
            /*
            struct scatterlist sg = {0};
            //sg_dma_address(&sg) = buf;      // rdma_buf = rdma_buf 
            //sg_dma_address(&sg) = cb->rdma_dma_addr;      // rdma_buf = rdma_buf 
            sg_dma_address(&sg) = cb->start_dma_addr;      // rdma_buf = rdma_buf 
            sg_dma_len(&sg) = cb->size;
            ret = ib_map_mr_sg(cb->reg_mr, &sg, 1, PAGE_SIZE);  // snyc ib_dma_sync_single_for_cpu/dev 
            */

        // pd -> cq -> qp.  cb->qp->device->local_dma_lkey
        // qp, ib_send_wr, ib_send_wr
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr); // send
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret);
			break;
		}
       
	    DEBUG_LOG("blocking wait......\n");
        msleep(3000);
	    DEBUG_LOG("\n\n\n");
		/* Wait for server to ACK */
		wait_event_interruptible(cb->sem, cb->state >= RDMA_WRITE_ADV);
		if (cb->state != RDMA_WRITE_ADV) {
			printk(KERN_ERR PFX 
			       "wait for RDMA_WRITE_ADV state %d\n",
			       cb->state);
			break;  // break
		}

        DEBUG_LOG("\n\n\n"); msleep(5000);
		krping_format_send(cb, cb->rdma_dma_addr);
	    DEBUG_LOG("ib_post_send>>>>\n");
		ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			printk(KERN_ERR PFX "post send error %d\n", ret); // error here
			break;
		}

		/* Wait for the server to say the RDMA Write is complete. */
		wait_event_interruptible(cb->sem, 
					 cb->state >= RDMA_WRITE_COMPLETE);
		if (cb->state != RDMA_WRITE_COMPLETE) {
			printk(KERN_ERR PFX 
			       "wait for RDMA_WRITE_COMPLETE state %d\n",
			       cb->state);
			break;
		}

		if (cb->validate)
			if (memcmp(cb->start_buf, cb->rdma_buf, cb->size)) {
				printk(KERN_ERR PFX "data mismatch!\n");
				break;
			}

		if (cb->verbose)
			printk(KERN_INFO PFX "ping data: %s\n", cb->rdma_buf);
#ifdef SLOW_KRPING
		wait_event_interruptible_timeout(cb->sem, cb->state == ERROR, HZ);
#endif
	}
}

static void krping_rlat_test_client(struct krping_cb *cb)
{
	struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	cb->state = RDMA_READ_ADV;

	/* Send STAG/TO/Len to client */
	krping_format_send(cb, cb->start_dma_addr);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "krping_format_send failed\n");
		return;
	}
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status) {
		printk(KERN_ERR PFX "send completion error %d\n", wc.status);
		return;
	}

	/* Spin waiting for server's Start STAG/TO/Len */
	while (cb->state < RDMA_WRITE_ADV) {
		krping_cq_event_handler(cb->cq, cb);
	}

#if 0
{
	int i;
	struct timeval start, stop;
	time_t sec;
	suseconds_t usec;
	unsigned long long elapsed;
	struct ib_wc wc;
	struct ib_send_wr *bad_wr;
	int ne;
	
	cb->rdma_sq_wr.wr.opcode = IB_WR_RDMA_WRITE;
	cb->rdma_sq_wr.rkey = cb->remote_rkey;
	cb->rdma_sq_wr.remote_addr = cb->remote_addr;
	cb->rdma_sq_wr.wr.sg_list->length = 0;
	cb->rdma_sq_wr.wr.num_sge = 0;

	do_gettimeofday(&start);
	for (i=0; i < 100000; i++) {
		if (ib_post_send(cb->qp, &cb->rdma_sq_wr.wr, &bad_wr)) {
			printk(KERN_ERR PFX  "Couldn't post send\n");
			return;
		}
		do {
			ne = ib_poll_cq(cb->cq, 1, &wc);
		} while (ne == 0);
		if (ne < 0) {
			printk(KERN_ERR PFX "poll CQ failed %d\n", ne);
			return;
		}
		if (wc.status != IB_WC_SUCCESS) {
			printk(KERN_ERR PFX "Completion wth error at %s:\n",
				cb->server ? "server" : "client");
			printk(KERN_ERR PFX "Failed status %d: wr_id %d\n",
				wc.status, (int) wc.wr_id);
			return;
		}
	}
	do_gettimeofday(&stop);
	
	if (stop.tv_usec < start.tv_usec) {
		stop.tv_usec += 1000000;
		stop.tv_sec  -= 1;
	}
	sec     = stop.tv_sec - start.tv_sec;
	usec    = stop.tv_usec - start.tv_usec;
	elapsed = sec * 1000000 + usec;
	printk(KERN_ERR PFX "0B-write-lat iters 100000 usec %llu\n", elapsed);
}
#endif

	rlat_test(cb);
}

static void krping_wlat_test_client(struct krping_cb *cb)
{
	struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	cb->state = RDMA_READ_ADV;

	/* Send STAG/TO/Len to client */
	krping_format_send(cb, cb->start_dma_addr);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "krping_format_send failed\n");
		return;
	}
	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status) {
		printk(KERN_ERR PFX "send completion error %d\n", wc.status);
		return;
	}

	/* Spin waiting for server's Start STAG/TO/Len */
	while (cb->state < RDMA_WRITE_ADV) {
		krping_cq_event_handler(cb->cq, cb);
	}

	wlat_test(cb);
}

static void krping_bw_test_client(struct krping_cb *cb)
{
	struct ib_send_wr *bad_wr;
	struct ib_wc wc;
	int ret;

	cb->state = RDMA_READ_ADV;

	/* Send STAG/TO/Len to client */
	krping_format_send(cb, cb->start_dma_addr); // (only client) update rkey (send buf)
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "krping_format_send failed\n");
		return;
	}

	ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "post send error %d\n", ret);
		return;
	}

	/* Spin waiting for send completion */
	while ((ret = ib_poll_cq(cb->cq, 1, &wc) == 0));
	if (ret < 0) {
		printk(KERN_ERR PFX "poll error %d\n", ret);
		return;
	}
	if (wc.status) {
		printk(KERN_ERR PFX "send completion error %d\n", wc.status);
		return;
	}

	/* Spin waiting for server's Start STAG/TO/Len */
	while (cb->state < RDMA_WRITE_ADV) {
		krping_cq_event_handler(cb->cq, cb);
	}

	bw_test(cb);
}

/*
 * Manual qp flush test
 */
static void flush_qp(struct krping_cb *cb)
{
	struct ib_send_wr wr = { 0 }, *bad;
	struct ib_recv_wr recv_wr = { 0 }, *recv_bad;
	struct ib_wc wc;
	int ret;
	int flushed = 0;
	int ccnt = 0;

	rdma_disconnect(cb->cm_id);
	DEBUG_LOG("disconnected!\n");

	wr.opcode = IB_WR_SEND;
	wr.wr_id = 0xdeadbeefcafebabe;
	ret = ib_post_send(cb->qp, &wr, &bad);
	if (ret) {
		printk(KERN_ERR PFX "%s post_send failed ret %d\n", __func__, ret);
		return;
	}

	recv_wr.wr_id = 0xcafebabedeadbeef;
	ret = ib_post_recv(cb->qp, &recv_wr, &recv_bad);
	if (ret) {
		printk(KERN_ERR PFX "%s post_recv failed ret %d\n", __func__, ret);
		return;
	}

	/* poll until the flush WRs complete */
	do {
		ret = ib_poll_cq(cb->cq, 1, &wc);
		if (ret < 0) {
			printk(KERN_ERR PFX "ib_poll_cq failed %d\n", ret);
			return;
		}
		if (ret == 0)
			continue;
		ccnt++;
		if (wc.wr_id == 0xdeadbeefcafebabe ||
		    wc.wr_id == 0xcafebabedeadbeef)
			flushed++;
	} while (flushed != 2);
	DEBUG_LOG("qp_flushed! ccnt %u\n", ccnt);
}

static void krping_fr_test(struct krping_cb *cb)
{
	struct ib_send_wr inv, *bad;
	struct ib_reg_wr fr;
	struct ib_wc wc;
	u8 key = 0;
	struct ib_mr *mr;
	int ret;
	int size = cb->size;
	int plen = (((size - 1) & PAGE_MASK) + PAGE_SIZE) >> PAGE_SHIFT;
	unsigned long start;
	int count = 0;
	int scnt = 0;
	struct scatterlist sg = {0};

	mr = ib_alloc_mr(cb->pd, IB_MR_TYPE_MEM_REG, plen);
	if (IS_ERR(mr)) {
		printk(KERN_ERR PFX "ib_alloc_mr failed %ld\n", PTR_ERR(mr));
		return;
	}

	sg_dma_address(&sg) = 0xcafebabe0000UL;
	sg_dma_len(&sg) = size;
	//ret = ib_map_mr_sg(mr, &sg, 1, NULL, PAGE_SIZE); //offset=NULL
	ret = ib_map_mr_sg(mr, &sg, 1, PAGE_SIZE);
	if (ret <= 0) {
		printk(KERN_ERR PFX "ib_map_mr_sge err %d\n", ret);
		goto err2;
	}

	memset(&fr, 0, sizeof fr);
	fr.wr.opcode = IB_WR_REG_MR;
	fr.access = IB_ACCESS_REMOTE_WRITE | IB_ACCESS_LOCAL_WRITE;
	fr.mr = mr;
	fr.wr.next = &inv;

	memset(&inv, 0, sizeof inv);
	inv.opcode = IB_WR_LOCAL_INV;
	inv.send_flags = IB_SEND_SIGNALED;
	
	DEBUG_LOG("fr_test: stag index %d plen %u size %u depth %u\n", mr->rkey >> 8, plen, cb->size, cb->txdepth);
	start = get_seconds();
	while (!cb->count || count <= cb->count) {
		if (signal_pending(current)) {
			printk(KERN_ERR PFX "signal!\n");
			break;
		}
		if ((get_seconds() - start) >= 9) {
			DEBUG_LOG("fr_test: pausing 1 second! count %u latest size %u plen %u\n", count, size, plen);
			wait_event_interruptible_timeout(cb->sem, cb->state == ERROR, HZ);
			if (cb->state == ERROR)
				break;
			start = get_seconds();
		}	
		while (scnt < (cb->txdepth>>1)) {
			ib_update_fast_reg_key(mr, ++key);
			fr.key = mr->rkey;
			inv.ex.invalidate_rkey = mr->rkey;

			size = prandom_u32() % cb->size;
			if (size == 0)
				size = cb->size;
			sg_dma_len(&sg) = size;
			//ret = ib_map_mr_sg(mr, &sg, 1, NULL, PAGE_SIZE);
			ret = ib_map_mr_sg(mr, &sg, 1, PAGE_SIZE);
			if (ret <= 0) {
				printk(KERN_ERR PFX "ib_map_mr_sge err %d\n", ret);
				goto err2;
			}
			ret = ib_post_send(cb->qp, &fr.wr, &bad);
			if (ret) {
				printk(KERN_ERR PFX "ib_post_send failed %d\n", ret);
				goto err2;	
			}
			scnt++;
		}

		ret = ib_poll_cq(cb->cq, 1, &wc);
		if (ret < 0) {
			printk(KERN_ERR PFX "ib_poll_cq failed %d\n", ret);
			goto err2;	
		}
		if (ret == 1) {
			if (wc.status) {
				printk(KERN_ERR PFX "completion error %u\n", wc.status);
				goto err2;
			}
			count++;
			scnt--;
		}
	}
err2:
	flush_qp(cb);
	DEBUG_LOG("fr_test: done!\n");
	ib_dereg_mr(mr);
}

static int krping_connect_client(struct krping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;
	
    DEBUG_LOG("\n->%s();\n", __func__);

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		printk(KERN_ERR PFX "rdma_connect error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
	if (cb->state == ERROR) {
		printk(KERN_ERR PFX "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	DEBUG_LOG("rdma_connect successful\n");
	return 0;
}

static int krping_bind_client(struct krping_cb *cb)
{
	struct sockaddr_storage sin;
	int ret;

	fill_sockaddr(&sin, cb);

	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *)&sin, 2000);
	if (ret) {
		printk(KERN_ERR PFX "rdma_resolve_addr error %d\n", ret);
		return ret;
	}

	wait_event_interruptible(cb->sem, cb->state >= ROUTE_RESOLVED);
	if (cb->state != ROUTE_RESOLVED) {
		printk(KERN_ERR PFX 
		       "addr/route resolution did not resolve: state %d\n",
		       cb->state);
		return -EINTR;
	}

	if (!reg_supported(cb->cm_id->device)) //Jack
		return -EINVAL;

	DEBUG_LOG("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

static void krping_run_client(struct krping_cb *cb)
{
	struct ib_recv_wr *bad_wr;
	int ret;

	ret = krping_bind_client(cb);
	if (ret)
		return;

	ret = krping_setup_qp(cb, cb->cm_id);
	if (ret) {
		printk(KERN_ERR PFX "setup_qp failed: %d\n", ret);
		return;
	}

	ret = krping_setup_buffers(cb);
	if (ret) {
		printk(KERN_ERR PFX "krping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

    DEBUG_LOG("@@@ (check1) cb->start_dma_addr = 0x%llx Jack (only client)\n", cb->start_dma_addr);

	DEBUG_LOG("ib_post_recv<<<<\n");
	ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		printk(KERN_ERR PFX "ib_post_recv failed: %d\n", ret);
		goto err2;
	}

    DEBUG_LOG("@@@ (check2) cb->start_dma_addr = 0x%llx Jack (only client)\n", cb->start_dma_addr);
	ret = krping_connect_client(cb);
	if (ret) {
		printk(KERN_ERR PFX "connect error %d\n", ret);
		goto err2;
	}

    DEBUG_LOG("@@@ (check3) cb->start_dma_addr = 0x%llx Jack (only client)\n", cb->start_dma_addr);

	if (cb->wlat) {
		DEBUG_LOG("TEST client: krping_wlat_test_client()\n");
		krping_wlat_test_client(cb);
    }
	else if (cb->rlat) {
		DEBUG_LOG("TEST client: krping_rlat_test_client()\n");
		krping_rlat_test_client(cb);
    }
	else if (cb->bw) {
		DEBUG_LOG("TEST client: krping_bw_test_client()\n");
		krping_bw_test_client(cb);
    }
	else if (cb->frtest) {
		DEBUG_LOG("TEST client: krping_fr_test()\n");
		krping_fr_test(cb);
    }
	else {
		DEBUG_LOG("\n\n\n\n\nTEST client: krping_test_client()\n");
		krping_test_client(cb);
    }
	rdma_disconnect(cb->cm_id);
err2:
	krping_free_buffers(cb);
err1:
	krping_free_qp(cb);
}

int krping_doit(char *cmd)
{
	struct krping_cb *cb;
	int op;
	int ret = 0;
	char *optarg;
	unsigned long optint;

	cb = kzalloc(sizeof(*cb), GFP_KERNEL);
	if (!cb)
		return -ENOMEM;

	mutex_lock(&krping_mutex);
	list_add_tail(&cb->list, &krping_cbs);
	mutex_unlock(&krping_mutex);

	cb->server = -1;
	cb->state = IDLE;
	cb->size = 64;
	cb->txdepth = RPING_SQ_DEPTH;
	init_waitqueue_head(&cb->sem);

	while ((op = krping_getopt("krping", &cmd, krping_opts, NULL, &optarg,
			      &optint)) != 0) {
		switch (op) {
		case 'a':
			cb->addr_str = optarg;
			in4_pton(optarg, -1, cb->addr, -1, NULL);
			cb->addr_type = AF_INET;
			printk("ipaddr (%s)\n", optarg);
			break;
		case 'A':
			cb->addr_str = optarg;
			in6_pton(optarg, -1, cb->addr, -1, NULL);
			cb->addr_type = AF_INET6;
			printk("ipv6addr (%s)\n", optarg);
			break;
		case 'p':
			cb->port = htons(optint);
			printk("port %d\n", (int)optint);
			break;
		case 'P':
			cb->poll = 1;
			printk("server\n");
			break;
		case 's':
			cb->server = 1;
			printk("server\n");
			break;
		case 'c':
			cb->server = 0;
			printk("client\n");
			break;
		case 'S':
			cb->size = optint;
			break;
		case 'C':
			cb->count = optint;
			if (cb->count < 0) {
				printk(KERN_ERR PFX "Invalid count %d\n",
					cb->count);
				ret = EINVAL;
			} else
				printk("count %d\n", (int) cb->count);
			break;
		case 'v':
			cb->verbose++;
			printk("verbose\n");
			break;
		case 'V':
			cb->validate++;
			printk("validate data\n");
			break;
		case 'l':
			cb->wlat++;
			break;
		case 'L':
			cb->rlat++;
			break;
		case 'B':
			cb->bw++;
			break;
		case 'd':
			cb->duplex++;
			break;
		case 'I':
			cb->server_invalidate = 0; //Jack default=1
			break;
		case 'T':
			cb->txdepth = optint;
			printk("txdepth %d\n", (int) cb->txdepth);
			break;
		case 'Z':
			cb->local_dma_lkey = 1;
			printk("using local dma lkey\n");
			break;
		case 'R':
			cb->read_inv = 0; // Jack default=1
			printk("using read-with-inv\n");
			break;
		case 'f':
			cb->frtest = 1;
			printk("fast-reg test!\n");
			break;
		default:
			printk(KERN_ERR PFX "unknown opt %s\n", optarg);
			ret = -EINVAL;
			break;
		}
	}
    
    if ((cb->size < 1) ||
        (cb->size > RPING_BUFSIZE)) {
        printk(KERN_ERR PFX "Invalid size %d "
               "(valid range is 1 to %d)\n",
               cb->size, RPING_BUFSIZE);
        ret = EINVAL;
    } else
        printk("size %d\n", (int)optint);
	
    if (ret)
		goto out;

	if (cb->server == -1) {
		printk(KERN_ERR PFX "must be either client or server\n");
		ret = -EINVAL;
		goto out;
	}

	if (cb->server && cb->frtest) {
		printk(KERN_ERR PFX "must be client to run frtest\n");
		ret = -EINVAL;
		goto out;
	}

	if ((cb->frtest + cb->bw + cb->rlat + cb->wlat) > 1) {
		printk(KERN_ERR PFX "Pick only one test: fr, bw, rlat, wlat\n");
		ret = -EINVAL;
		goto out;
	}

	if (cb->wlat || cb->rlat || cb->bw) {
		printk(KERN_ERR PFX "wlat, rlat, and bw tests only support mem_mode MR - which is no longer supported\n");
		ret = -EINVAL;
		goto out;
	}

	cb->cm_id = rdma_create_id(&init_net, krping_cma_event_handler, cb, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(cb->cm_id)) {
		ret = PTR_ERR(cb->cm_id);
		printk(KERN_ERR PFX "rdma_create_id error %d\n", ret);
		goto out;
	}
	DEBUG_LOG("created cm_id %p\n", cb->cm_id);

	if (cb->server)
		krping_run_server(cb);
	else
		krping_run_client(cb);

	DEBUG_LOG("destroy cm_id %p\n", cb->cm_id);
	rdma_destroy_id(cb->cm_id);
out:
	mutex_lock(&krping_mutex);
	list_del(&cb->list);
	mutex_unlock(&krping_mutex);
	kfree(cb);
	return ret;
}

/*
 * Read proc returns stats for each device.
 */
static int krping_read_proc(struct seq_file *seq, void *v)
{
	struct krping_cb *cb;
	int num = 1;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;
	DEBUG_LOG(KERN_INFO PFX "proc read called...\n");
	mutex_lock(&krping_mutex);
	list_for_each_entry(cb, &krping_cbs, list) {
		if (cb->pd) {
			seq_printf(seq,
			     "%d-%s %lld %lld %lld %lld %lld %lld %lld %lld\n",
			     num++, cb->pd->device->name, cb->stats.send_bytes,
			     cb->stats.send_msgs, cb->stats.recv_bytes,
			     cb->stats.recv_msgs, cb->stats.write_bytes,
			     cb->stats.write_msgs,
			     cb->stats.read_bytes,
			     cb->stats.read_msgs);
		} else {
			seq_printf(seq, "%d listen\n", num++);
		}
	}
	mutex_unlock(&krping_mutex);
	module_put(THIS_MODULE);
	return 0;
}

/*
 * Write proc is used to start a ping client or server.
 */
static ssize_t krping_write_proc(struct file * file, const char __user * buffer,
		size_t count, loff_t *ppos)
{
	char *cmd;
	int rc;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	cmd = kmalloc(count, GFP_KERNEL);
	if (cmd == NULL) {
		printk(KERN_ERR PFX "kmalloc failure\n");
		return -ENOMEM;
	}
	if (copy_from_user(cmd, buffer, count)) {
		return -EFAULT;
	}

	/*
	 * remove the \n.
	 */
	cmd[count - 1] = 0;
	DEBUG_LOG(KERN_INFO PFX "proc write |%s|\n", cmd);
	rc = krping_doit(cmd);
	kfree(cmd);
	module_put(THIS_MODULE);
	if (rc)
		return rc;
	else
		return (int) count;
}

static int krping_read_open(struct inode *inode, struct file *file)
{
        return single_open(file, krping_read_proc, inode->i_private);
}

static struct file_operations krping_ops = {
	.owner = THIS_MODULE,
	.open = krping_read_open,
	.read = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
	.write = krping_write_proc,
};

static int __init krping_init(void)
{
	DEBUG_LOG("krping_init\n");
	krping_proc = proc_create("krping", 0666, NULL, &krping_ops);
	if (krping_proc == NULL) {
		printk(KERN_ERR PFX "cannot create /proc/krping\n");
		return -ENOMEM;
	}
	return 0;
}

static void __exit krping_exit(void)
{
	DEBUG_LOG("krping_exit\n");
	remove_proc_entry("krping", NULL);
}

module_init(krping_init);
module_exit(krping_exit);
