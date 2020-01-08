#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <limits.h>

#include <mtcp_api.h>
#include <mtcp_epoll.h>
#define RTE_LIBRTE_PDUMP
#ifdef RTE_LIBRTE_PDUMP
#include <rte_pdump.h>
#endif
#include "cpu.h"
#include "http_parsing.h"
#include "netlib.h"
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
#ifdef MDEBUG
#define DEBUG_PRINT(...) do{ fprintf( stderr, __VA_ARGS__ ); } while( 0 )
#else
#define DEBUG_PRINT(...) do{ } while ( 0 )
#endif
/*----------------------------------------------------------------------------*/
struct server_vars
{
	int recv_len;
	int request_len;
	long int total_read, total_sent;
	uint8_t done;
	uint8_t rspheader_sent;
	uint8_t keep_alive;
	uint64_t recv;
	int fidx;						// file cache index
	long int fsize;					// file size
};
/*----------------------------------------------------------------------------*/
struct thread_context
{
	int core;

	mctx_t mctx;
	int ep;
	struct server_vars *svars;

	int target;
	int started;
	int errors;
	int incompletes;
	int done;
	int pending;

	// struct wget_stat stat;
};
/*----------------------------------------------------------------------------*/
static int num_cores;
static int core_limit;
static pthread_t app_thread[MAX_CPUS];
// static int done[MAX_CPUS];
static char *conf_file = NULL;
static int backlog = -1;
static int port_number = 9000;
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
struct thread_context * 
InitializeServerThread(int core) 
{
	struct thread_context *ctx;

#if HT_SUPPORT
	mtcp_core_affinitize(core + (num_cores / 2));
#else
	mtcp_core_affinitize(core);
#endif

	ctx = (struct thread_context *)calloc(1, sizeof(struct thread_context));
	if (!ctx) {
		TRACE_ERROR("Failed to create thread context!\n");
		return NULL;
	}

	ctx->mctx = mtcp_create_context(core);
	if (!ctx->mctx) {
		TRACE_ERROR("Failed to create mtcp context!\n");
		free(ctx);
		return NULL;
	}

	/* create epoll descriptor */
	ctx->ep = mtcp_epoll_create(ctx->mctx, MAX_EVENTS);
	if (ctx->ep < 0) {
		mtcp_destroy_context(ctx->mctx);
		free(ctx);
		TRACE_ERROR("Failed to create epoll descriptor!\n");
		return NULL;
	}

	/* allocate memory for server variables */
	ctx->svars = (struct server_vars *)
			calloc(MAX_FLOW_NUM, sizeof(struct server_vars));
	if (!ctx->svars) {
		mtcp_close(ctx->mctx, ctx->ep);
		mtcp_destroy_context(ctx->mctx);
		free(ctx);
		TRACE_ERROR("Failed to create server_vars struct!\n");
		return NULL;
	}

	return ctx;
}
/*----------------------------------------------------------------------------*/
int
CreateListeningSocket(struct thread_context *ctx)
{
	int listener;
	struct mtcp_epoll_event ev;
	struct sockaddr_in saddr;
	int ret;

	/* create socket and set it as nonblocking */
	listener = mtcp_socket(ctx->mctx, AF_INET, SOCK_STREAM, 0);
	if (listener < 0) {
		TRACE_ERROR("Failed to create listening socket!\n");
		return -1;
	}
	ret = mtcp_setsock_nonblock(ctx->mctx, listener);
	if (ret < 0) {
		TRACE_ERROR("Failed to set socket in nonblocking mode.\n");
		return -1;
	}

	/* bind to port */
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = INADDR_ANY;
	saddr.sin_port = htons(port_number);
	ret = mtcp_bind(ctx->mctx, listener, 
			(struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
	if (ret < 0) {
		TRACE_ERROR("Failed to bind to the listening socket!\n");
		return -1;
	}

	/* listen (backlog: can be configured) */
	ret = mtcp_listen(ctx->mctx, listener, backlog);
	if (ret < 0) {
		TRACE_ERROR("mtcp_listen() failed!\n");
		return -1;
	}

	/* wait for incoming accept events */
	ev.events = MTCP_EPOLLIN;
	ev.data.sockid = listener;
	mtcp_epoll_ctl(ctx->mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, listener, &ev);

	return listener;
}
/*----------------------------------------------------------------------------*/
void
CleanServerVariable(struct server_vars *sv)
{
	sv->recv_len = 0;
	sv->request_len = 0;
	sv->total_read = 0;
	sv->total_sent = 0;
	sv->done = 0;
	sv->rspheader_sent = 0;
	sv->keep_alive = 0;
}
/*----------------------------------------------------------------------------*/
int 
AcceptConnection(struct thread_context *ctx, int listener)
{
	mctx_t mctx = ctx->mctx;
	struct server_vars *sv;
	struct mtcp_epoll_event ev;
	int c;

	c = mtcp_accept(mctx, listener, NULL, NULL);

	if (c >= 0) {
		if (c >= MAX_FLOW_NUM) {
			TRACE_ERROR("Invalid socket id %d.\n", c);
			return -1;
		}

		sv = &ctx->svars[c];
		CleanServerVariable(sv);
		DEBUG_PRINT("New connection %d accepted.\n", c);
		TRACE_APP("New connection %d accepted.\n", c);
		ev.events = MTCP_EPOLLIN;
		ev.data.sockid = c;
		mtcp_setsock_nonblock(ctx->mctx, c);
		mtcp_epoll_ctl(mctx, ctx->ep, MTCP_EPOLL_CTL_ADD, c, &ev);
		TRACE_APP("Socket %d registered.\n", c);

	} else {
		if (errno != EAGAIN) {
			TRACE_ERROR("mtcp_accept() error %s\n", 
					strerror(errno));
		}
	}

	return c;
}
/*----------------------------------------------------------------------------*/
static int 
HandleRecv(struct thread_context *ctx, int sockid, struct server_vars *sv)
{
	int rd;
	char buf[BUF_LEN];
	mctx_t mctx = ctx->mctx;
	rd = 1;
	while (rd > 0) {
		DEBUG_PRINT("mtcp read start\n");
		rd = mtcp_read(mctx, sockid, buf, BUF_SIZE);
		if (rd <= 0)
			break;
		// ctx->stat.reads += rd;
		if (buf[strlen(buf) - 1] == '\x96')
			break;

		TRACE_APP("Socket %d: mtcp_read ret: %d, total_recv: %lu, "
				"header_set: %d, header_len: %u, file_len: %lu\n", 
				sockid, rd, sv->recv + rd, 
				sv->headerset, sv->header_len, sv->file_len);

		// pbuf = buf;
		sv->recv += rd;
	}
	return 0;
}
/*----------------------------------------------------------------------------*/
void *
RunServerThread(void *arg)
{
	int core = *(int *)arg;
	struct thread_context *ctx;
	mctx_t mctx;
	int listener;
	int ep;
	struct mtcp_epoll_event *events;
	int nevents;
	int i, ret;
	int do_accept;

	ctx = InitializeServerThread(core);
	if (!ctx) {
		TRACE_ERROR("Failed to initialize server thread.\n");
		return NULL;
	}

	mctx = ctx->mctx;
	ep = ctx->ep;

	events = (struct mtcp_epoll_event *)
			calloc(MAX_EVENTS, sizeof(struct mtcp_epoll_event));
	if (!events) {
		TRACE_ERROR("Failed to create event struct!\n");
#ifdef RTE_LIBRTE_PDUMP
		/* uninitialize packet capture framework */
		rte_pdump_uninit();
#endif
		exit(-1);
	}

	listener = CreateListeningSocket(ctx);
	if (listener < 0) {
		TRACE_ERROR("Failed to create listening socket.\n");
#ifdef RTE_LIBRTE_PDUMP
		/* uninitialize packet capture framework */
		rte_pdump_uninit();
#endif
		exit(-1);
	}

	// while (!done[core]) {
	while (1) {
		nevents = mtcp_epoll_wait(mctx, ep, events, MAX_EVENTS, -1);
		DEBUG_PRINT("events:%d\n", nevents);
		if (nevents < 0) {
			// if (errno)
			DEBUG_PRINT("mtcp_epoll_wait ERROR!\n");
			break;
		}
		do_accept = FALSE;
		for (i=0; i<nevents; i++) {
			if (events[i].data.sockid == listener) {
				DEBUG_PRINT("Accept\n");
				/* if the event is for the listener, accept connection */
				do_accept = TRUE;
			}
			else if (events[i].events & MTCP_EPOLLERR) {
				DEBUG_PRINT("EPOLLERR\n");
			}
			else if (events[i].events & MTCP_EPOLLIN) {
				DEBUG_PRINT("EPOLLIN\n");
				ret = HandleRecv(ctx, events[i].data.sockid, 
						&ctx->svars[events[i].data.sockid]);
				if (ret == 0) {

				}
				else if (ret < 0) {

				}
			}
			else if (events[i].events & MTCP_EPOLLOUT) {
				DEBUG_PRINT("EPOLLOUT\n");
				fprintf(stderr, "No EPOLLOUT sent, but there is EPOLLOUT.\n");
			}
			else {
				DEBUG_PRINT("Others\n");
				assert(0);
			}
		}
		if (do_accept) {
			while (1) {
				ret = AcceptConnection(ctx, listener);
				DEBUG_PRINT("accept ret:%d\n", ret);
				if (ret < 0)
					break;
			}
		}
	}
	return NULL;
}
/*----------------------------------------------------------------------------*/
int
main(int argc, char **argv) 
{
	int ret;
	struct mtcp_conf mcfg;
	int cores[MAX_CPUS];
	int process_cpu;
	int i, o;

	num_cores = GetNumCPUs();
	core_limit = num_cores;
	process_cpu = -1;
#ifdef RTE_LIBRTE_PDUMP
	/* initialize packet capture framework */
	rte_pdump_init(NULL);
#endif
	while (-1 != (o = getopt(argc, argv, "N:f:p:c:b:h"))) {
		switch (o) {
			case 'p':
				port_number = mystrtol(optarg, 10);
				break;
			case 'b':
				backlog = mystrtol(optarg, 10);
				break;
			case 'N':
				core_limit = mystrtol(optarg, 10);
				if (core_limit > num_cores) {
					TRACE_CONFIG("CPU limit should be smaller than the "
						     "number of CPUs: %d\n", num_cores);
					return FALSE;
				}
				/** 
				 * it is important that core limit is set 
				 * before mtcp_init() is called. You can
				 * not set core_limit after mtcp_init()
				 */
				mtcp_getconf(&mcfg);
				mcfg.num_cores = core_limit;
				mtcp_setconf(&mcfg);
				break;
			case 'f':
				conf_file = optarg;
				break;
		}
	}
	if (conf_file == NULL) {
		TRACE_CONFIG("You forgot to pass the mTCP startup config file!\n");
#ifdef RTE_LIBRTE_PDUMP
		/* uninitialize packet capture framework */
		rte_pdump_uninit();
#endif
		exit(EXIT_FAILURE);
	}

	ret = mtcp_init(conf_file);
	if (ret) {
		TRACE_CONFIG("Failed to initialize mtcp\n");
#ifdef RTE_LIBRTE_PDUMP
		/* uninitialize packet capture framework */
		rte_pdump_uninit();
#endif
		exit(EXIT_FAILURE);
	}

	mtcp_getconf(&mcfg);
	if (backlog > mcfg.max_concurrency) {
		TRACE_CONFIG("backlog can not be set larger than CONFIG.max_concurrency\n");
		return FALSE;
	}
	if (backlog == -1) {
		backlog = 4096;
	}

	/* register signal handler to mtcp */
	mtcp_register_signal(SIGINT, SignalHandler);

	TRACE_INFO("Application initialization finished.\n");

	for (i=0; i<1; i++) {
		cores[i] = i;
		if (pthread_create(&app_thread[i], 
				   NULL, RunServerThread, (void *)&cores[i])) {
			perror("pthread_create");
			TRACE_CONFIG("Failed to create server thread.\n");
#ifdef RTE_LIBRTE_PDUMP
			/* uninitialize packet capture framework */
			rte_pdump_uninit();
#endif
			exit(EXIT_FAILURE);
		}
	}
	for (i=0; i<1; i++) {
		pthread_join(app_thread[i], NULL);
		if (process_cpu != -1)
			break;
	}

	mtcp_destroy();
	return 0;
}
