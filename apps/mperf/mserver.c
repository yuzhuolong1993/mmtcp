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
/*----------------------------------------------------------------------------*/
static pthread_t app_thread[MAX_CPUS];
static char *conf_file = NULL;
static int backlog = -1;
static int port_number = 9000;

struct server_vars {
	char request[HTTP_HEADER_LEN];
	int recv_len;
	int request_len;
	long int total_read, total_sent;
	uint8_t done;
	uint8_t rspheader_sent;
	uint8_t keep_alive;

	int fidx;						// file cache index
	char fname[NAME_LIMIT];				// file name
	long int fsize;					// file size
};

struct thread_context
{
	int core;
	mctx_t mctx;
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
HandleReadEvent(struct thread_context *ctx, int sockid, struct server_vars *sv)
{
	int rd;

	rd = 1;
	while (rd > 0) {
		rd = mtcp_read(mctx, sockid, buf, BUF_SIZE);
		if (rd <= 0)
			break;
		ctx->stat.reads += rd;

		TRACE_APP("Socket %d: mtcp_read ret: %d, total_recv: %lu, "
				"header_set: %d, header_len: %u, file_len: %lu\n", 
				sockid, rd, wv->recv + rd, 
				wv->headerset, wv->header_len, wv->file_len);

		// pbuf = buf;
		wv->recv += rd;
	}
	return 0;
}
/*----------------------------------------------------------------------------*/
void *
RunServerThread(void *arg)
{
	int core = *(int *)arg;
	struct thread_context *ctx;
	struct mtcp_epoll_event *events;
	mctx_t mctx;
	int ep;

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

	while (!done[core]) {
	// while (1) {
		nevents = mtcp_epoll_wait(mctx, ep, events, MAX_EVENTS, -1);
		if (nevents < 0) {
			// if (errno)
			fprintf(stderr, "mtcp_epoll_wait ERROR!\n");
			break;
		}
		do_accept = FALSE;
		for (i=0; i<nevents; i++) {
			if (events[i].data.sockid == listener) {
				/* if the event is for the listener, accept connection */
				do_accept = TRUE;
			}
			else if (events[i].events & MTCP_EPOLLERR) {

			}
			else if (events[i].events & MTCP_EPOLLIN) {
				ret = HandleReadEvent(ctx, events[i].data.sockid, 
						&ctx->svars[events[i].data.sockid]);
				if (ret == 0) {

				}
				else if (ret < 0) {

				}
			}
			else if (events[i].events & MTCP_EPOLLOUT) {
				fprintf(stderr, "No EPOLLOUT sent, but there is EPOLLOUT.\n");
			}
			else {
				assert(0);
			}
		}
		if (do_accept) {
			while (1) {
				ret = AcceptConnection(ctx, listener);
				if (ret < 0)
					break;
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
	closedir(dir);
	return 0;
}
