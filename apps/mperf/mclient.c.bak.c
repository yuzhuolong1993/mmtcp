#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/queue.h>
#include <assert.h>
#include <stdbool.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>
#include "cpu.h"
#include "rss.h"
#include "http_parsing.h"
#include "debug.h"

//#define MAX_CPUS 		16

#define MAX_URL_LEN 		128
#define MAX_FILE_LEN 		128
#define HTTP_HEADER_LEN 	1024

#define IP_RANGE 		1
#define MAX_IP_STR_LEN 		16

#define BUF_SIZE 		(32 * 1024)

#define CALC_MD5SUM 		FALSE

#define TIMEVAL_TO_MSEC(t)      ((t.tv_sec * 1000) + (t.tv_usec / 1000))
#define TIMEVAL_TO_USEC(t)      ((t.tv_sec * 1000000) + (t.tv_usec))
#define TS_GT(a,b)              ((int64_t)((a)-(b)) > 0)

#ifndef TRUE
#define TRUE			(1)
#endif

#ifndef FALSE
#define FALSE			(0)
#endif

#define CONCURRENCY		1
#define BUF_LEN 		8192
#define MAX_FLOW_NUM 		(10000)
#define MAX_EVENTS 		(30000)

#define DEBUG(fmt, args...)	fprintf(stderr, "[DEBUG] " fmt "\n", ## args)
#define ERROR(fmt, args...)	fprintf(stderr, fmt "\n", ## args)
#define SAMPLE(fmt, args...)	fprintf(stdout, fmt "\n", ## args)

#define SEND_MODE 		1
#define WAIT_MODE		2

static pthread_t app_thread[MAX_CPUS];
static char *conf_file = NULL;
static int backlog = -1;
static int port_number = 9000;
/*----------------------------------------------------------------------------*/
struct thread_context
{
	int core;
	mctx_t mctx;
};
struct wget_vars
{
	int request_sent;

	char response[HTTP_HEADER_LEN];
	int resp_len;
	int headerset;
	uint32_t header_len;
	uint64_t file_len;
	uint64_t recv;
	uint64_t write;

	struct timeval t_start;
	struct timeval t_end;
	
	int fd;
};
/*----------------------------------------------------------------------------*/
void
SignalHandler(int signum)
{
	ERROR("Received SIGINT");
	exit(-1);
}
/*----------------------------------------------------------------------------*/
void
print_usage(int mode)
{
	if (mode == SEND_MODE || mode == 0) {
		ERROR("(client initiates)   usage: ./client send [ip] [port] [length (seconds)]");
	}
	if (mode == WAIT_MODE || mode == 0) {
		ERROR("(server initiates)   usage: ./client wait [length (seconds)]");
	}
}
/*----------------------------------------------------------------------------*/
thread_context_t 
CreateContext(int core)
{
	thread_context_t ctx;

	ctx = (thread_context_t)calloc(1, sizeof(struct thread_context));
	if (!ctx) {
		perror("malloc");
		TRACE_ERROR("Failed to allocate memory for thread context.\n");
		return NULL;
	}
	ctx->core = core;

	ctx->mctx = mtcp_create_context(core);
	if (!ctx->mctx) {
		TRACE_ERROR("Failed to create mtcp context.\n");
		free(ctx);
		return NULL;
	}

	return ctx;
}
/*----------------------------------------------------------------------------*/
static inline int 
CreateConnection(thread_context_t ctx)
{
	mctx_t mctx = ctx->mctx;
	struct mtcp_epoll_event ev;
	struct sockaddr_in addr;
	int sockid;
	int ret;

	sockid = mtcp_socket(mctx, AF_INET, SOCK_STREAM, 0);
	if (sockid < 0) {
		TRACE_INFO("Failed to create socket!\n");
		return -1;
	}
	memset(&ctx->wvars[sockid], 0, sizeof(struct wget_vars));
	ret = mtcp_setsock_nonblock(mctx, sockid);
	if (ret < 0) {
		TRACE_ERROR("Failed to set socket in nonblocking mode.\n");
		#ifdef RTE_LIBRTE_PDUMP
		/* uninitialize packet capture framework */
		rte_pdump_uninit();
#endif
		exit(-1);
	}

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = daddr;
	addr.sin_port = dport;
	
	ret = mtcp_connect(mctx, sockid, 
			(struct sockaddr *)&addr, sizeof(struct sockaddr_in));
	if (ret < 0) {
		if (errno != EINPROGRESS) {
			perror("mtcp_connect");
			mtcp_close(mctx, sockid);
			return -1;
		}
	}

	ctx->started++;
	ctx->pending++;
	ctx->stat.connects++;

	ev.events = MTCP_EPOLLOUT;
	ev.data.sockid = sockid;
	mtcp_epoll_ctl(mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, sockid, &ev);

	return sockid;
}
/*----------------------------------------------------------------------------*/
static int
SendData(struct thread_context *ctx, int sockid, struct client_vars *sv)
{
	int ret;
	int sent;

	if (sv->done) {
		return 0;
	}

	while (1) {
		ret = mtcp_write(ctx->mctx, sockid,  
				fcache[sv->fidx].file + sv->total_sent, len);
		
	}
}
/*----------------------------------------------------------------------------*/
void *
RunWgetMain(void *arg)
{
	thread_context_t ctx;
	mctx_t mctx;
	int core;

	core = *(int *)arg;
	mtcp_core_affinitize(core);
	ctx = CreateContext(core);
	if (!ctx) {
		return NULL;
	}
	mctx = ctx->mctx;
	g_stat[core] = &ctx->stat;
	srand(time(NULL));

	mtcp_init_rss(mctx, saddr, IP_RANGE, daddr, dport);

	n = flows[core];
	if (n == 0) {
		TRACE_DBG("Application thread %d finished.\n", core);
		pthread_exit(NULL);
		return NULL;
	}

	ctx->target = n;

	daddr_in.s_addr = daddr;
	fprintf(stderr, "Thread %d handles %d flows. connecting to %s:%u\n", 
			core, n, inet_ntoa(daddr_in), ntohs(dport));

	/* Initialization */
	maxevent = max_fds * 3;
	ep = mtcp_epoll_create(mctx, maxevents);
	if (ep < 0) {
		TRACE_ERROR("Failed to create epoll struct!n");
#ifdef RTE_LIBRTE_PDUMP
		/* uninitialize packet capture framework */
		rte_pdump_uninit();
#endif
		exit(EXIT_FAILURE);
	}

	events = (struct mtcp_epoll_event *)
			calloc(maxevents, sizeof(struct mtcp_epoll_event));
	if (!events) {
		TRACE_ERROR("Failed to allocate events!\n");
#ifdef RTE_LIBRTE_PDUMP
		/* uninitialize packet capture framework */
		rte_pdump_uninit();
#endif
		exit(EXIT_FAILURE);
	}

	ctx->ep = ep;

	wvars = (struct client_vars *)calloc(max_fds, sizeof(struct client_vars));
	if (!wvars) {
		TRACE_ERROR("Failed to create wget variables!\n");
#ifdef RTE_LIBRTE_PDUMP
		/* uninitialize packet capture framework */
		rte_pdump_uninit();
#endif
		exit(EXIT_FAILURE);
	}

	ctx->wvars = wvars;
	ctx->started = ctx->done = ctx->pending = 0;
	ctx->errors = ctx->incompletes = 0;

	gettimeofday(&cur_tv, NULL);
	prev_tv = cur_tv;

	// while (!done[core]) {
	while (1) {
		gettimeofday(&cur_tv, NULL);

		if (core == 0 && cur_tv.tv_sec > prev_tv.tv_sec) {
		  	PrintStats();
			prev_tv = cur_tv;
		}

		while (ctx->pending < concurrency && ctx->started < ctx->target) {
			if (CreateConnection(ctx) < 0) {
				done[core] = TRUE;
				break;
			}
		}

		nevents = mtcp_epoll_wait(mctx, ep, events, maxevents, -1);
		ctx->stat.waits++;

		if (nevents < 0) {
			if (errno != EINTR) {
				TRACE_ERROR("mtcp_epoll_wait failed! ret: %d\n", nevents);
			}
			done[core] = TRUE;
			break;
		} else {
			ctx->stat.events += nevents;
		}

		if (nevents < 0) {
			fprintf(stderr, "mtcp_epoll_wait ERROR!\n");
			done[core] = TRUE;
			break;
		} else {
			ctx->stat.events += nevents;
		}

		for (i=0; i<nevents; i++) {
			if (events[i].events & MTCP_EPOLLERR) {

			} else if (events[i].events & MTCP_EPOLLIN) {
				fprintf(stderr, "No EPOLLIN sent, but there is EPOLLIN.\n");
			} else if (events[i].events == MTCP_EPOLLOUT) {
				struct client_vars *wv = &wvars[events[i].data.sockid];
				if (!wv->request_sent) {
					SendData(ctx, events[i].data.sockid, wv);
				} else {
					//TRACE_DBG("Request already sent.\n");
				}
			} else {
				assert(0);
			}
		}
	}
}

/*----------------------------------------------------------------------------*/
int
main(int argc, char **argv) 
{
	int ret;
	int process_cpu;
	struct mtcp_conf mcfg;
	while (-1 != (o = getopt(argc, argv, "N:c:o:n:f:"))) {
		switch (o) {
			case 'p':
				port_number = mystrtol(optarg, 10);
				break;
			case 'b':
				backlog = mystrtol(optarg, 10);
				break;
			case 'f':
				conf_file = optarg;
				break;
		}
	}

	if (total_flow < core_limit) {
		core_limit = total_flows;
	}
	if (total_concurrency > 0) 
		concurrency = total_concurrency / core_limit;

	/* set the max number of fds 3x larger than concurrency */
	max_fds = concurrency * 3;

#ifdef RTE_LIBRTE_PDUMP
	/* initialize packet capture framework */
	rte_pdump_init(NULL);
#endif
	if (conf_file == NULL) {
		TRACE_ERROR("mTCP configuration file is not set!\n");
#ifdef RTE_LIBRTE_PDUMP
		/* uninitialize packet capture framework */
		rte_pdump_uninit();
#endif
		exit(EXIT_FAILURE);
	}

	ret = mtcp_init(conf_file);
	if (ret) {
		TRACE_ERROR("Failed to initialize mtcp.\n");
#ifdef RTE_LIBRTE_PDUMP
		/* uninitialize packet capture framework */
		rte_pdump_uninit();
#endif
		exit(EXIT_FAILURE);
	}

	mtcp_getconf(&mcfg);
	mcfg.max_concurrency = max_fds;
	mcfg.max_num_buffers = max_fds;
	mtcp_setconf(&mcfg);

	mtcp_register_signal(SIGINT, SignalHandler);

	flow_per_thread = total_flows / core_limit;
	flow_remainder_cnt = total_flows % core_limit;

	for (i=0; i<1; i++) {
		cores[i] = i;
		done[i] = FALSE;
		flows[i] = flow_per_thread;
		if (flow_remainder_cnt-- > 0)
			flows[i]++;

		if (flows[i] == 0)
			continue;

		if (pthread_create(&app_thread[i], 
					NULL, RunWgetMain, (void *)&cores[i])) {
			perror("pthread_create");
			TRACE_ERROR("Failed to create client thread.\n");
#ifdef RTE_LIBRTE_PDUMP
			/* uninitialize packet capture framework */
			rte_pdump_uninit();
#endif
			exit(-1);
		}
	}
	for (i=0; i<1; i++) {
		pthread_join(app_thread[i], NULL);
		TRACE_INFO("Client thread %d joined.\n", i);
	}
	mtcp_destroy();
	return 0;
}
