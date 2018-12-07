/*
 * libwebsockets-test-server - libwebsockets test implementation
 *
 * Copyright (C) 2010-2016 Andy Green <andy@warmcat.com>
 *
 * This file is made available under the Creative Commons CC0 1.0
 * Universal Public Domain Dedication.
 *
 * The person who associated a work with this deed has dedicated
 * the work to the public domain by waiving all of his or her rights
 * to the work worldwide under copyright law, including all related
 * and neighboring rights, to the extent allowed by law. You can copy,
 * modify, distribute and perform the work, even for commercial purposes,
 * all without asking permission.
 *
 * The test apps are intended to be adapted for use in your code, which
 * may be proprietary.  So unlike the library itself, they are licensed
 * Public Domain.
 */

#if defined(_WIN32) && defined(EXTERNAL_POLL)
#define WINVER 0x0600
#define _WIN32_WINNT 0x0600
#define poll(fdArray, fds, timeout)  WSAPoll((LPWSAPOLLFD)(fdArray), (ULONG)(fds), (INT)(timeout))
#endif

#include "lws_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <pthread.h>
#include <libwebsockets.h>

#ifdef _WIN32
#include <io.h>
#include "gettimeofday.h"
#else
#include <syslog.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#ifndef __func__
#define __func__ __FUNCTION__
#endif

#define MAX_ECHO_PAYLOAD 1024

struct per_session_data__echo {
	size_t rx, tx;
	unsigned char buf[LWS_PRE + MAX_ECHO_PAYLOAD];
	unsigned int len;
	unsigned int index;
	int final;
	int continuation;
	int binary;
};

static int versa, state;

int close_testing;
int max_poll_elements;
int debug_level = 7;

#ifdef EXTERNAL_POLL
struct lws_pollfd *pollfds;
int *fd_lookup;
int count_pollfds;
#endif
volatile int force_exit = 0;
struct lws_context *context;

/*
 * This mutex lock protects code that changes or relies on wsi list outside of
 * the service thread.  The service thread will acquire it when changing the
 * wsi list and other threads should acquire it while dereferencing wsis or
 * calling apis like lws_callback_on_writable_all_protocol() which
 * use the wsi list and wsis from a different thread context.
 */
pthread_mutex_t lock_established_conns;

/* http server gets files from this path */
#define LOCAL_RESOURCE_PATH "/usr/local/share/libwebsockets-test-server"
char *resource_path = LOCAL_RESOURCE_PATH;

/*
 * multithreaded version - protect wsi lifecycle changes in the library
 * these are called from protocol 0 callbacks
 */

void test_server_lock(int care)
{
	if (care)
		pthread_mutex_lock(&lock_established_conns);
}
void test_server_unlock(int care)
{
	if (care)
		pthread_mutex_unlock(&lock_established_conns);
}

/*
 * This demo server shows how to use libwebsockets for one or more
 * websocket protocols in the same server
 *
 * It defines the following websocket protocols:
 *
 *  dumb-increment-protocol:  once the socket is opened, an incrementing
 *				ascii string is sent down it every 50ms.
 *				If you send "reset\n" on the websocket, then
 *				the incrementing number is reset to 0.
 *
 *  lws-mirror-protocol: copies any received packet to every connection also
 *				using this protocol, including the sender
 */

enum demo_protocols {
	/* always first */
	PROTOCOL_LWS_ECHO,

	/* always last */
	DEMO_PROTOCOL_COUNT
};

static int
callback_echo(struct lws *wsi, enum lws_callback_reasons reason, void *user,
	      void *in, size_t len)
{
	struct per_session_data__echo *pss =
			(struct per_session_data__echo *)user;
	int n;

	switch (reason) {

#ifndef LWS_NO_SERVER

	case LWS_CALLBACK_SERVER_WRITEABLE:
do_tx:

		n = LWS_WRITE_CONTINUATION;
		if (!pss->continuation) {
			if (pss->binary)
				n = LWS_WRITE_BINARY;
			else
				n = LWS_WRITE_TEXT;
			pss->continuation = 1;
		}
		if (!pss->final)
			n |= LWS_WRITE_NO_FIN;
		lwsl_info("+++ test-echo: writing %d, with final %d\n",
			  pss->len, pss->final);

		pss->tx += pss->len;
		n = lws_write(wsi, &pss->buf[LWS_PRE], pss->len, n);
		//n = lws_write(wsi, &pss->buf[LWS_PRE], pss->len, (lws_write_protocol)n);
		if (n < 0) {
			lwsl_err("ERROR %d writing to socket, hanging up\n", n);
			return 1;
		}
		if (n < (int)pss->len) {
			lwsl_err("Partial write\n");
			return -1;
		}
		pss->len = -1;
		if (pss->final)
			pss->continuation = 0;
		lws_rx_flow_control(wsi, 1);
		break;

	case LWS_CALLBACK_RECEIVE:
do_rx:
		pss->final = lws_is_final_fragment(wsi);
		pss->binary = lws_frame_is_binary(wsi);
		lwsl_info("+++ test-echo: RX len %d final %d, pss->len=%d\n",
			  len, pss->final, (int)pss->len);

		memcpy(&pss->buf[LWS_PRE], in, len);
		//assert((int)pss->len == -1);
		pss->len = (unsigned int)len;
		pss->rx += len;

		lws_rx_flow_control(wsi, 0);
		lws_callback_on_writable(wsi);
		break;
#endif

#ifndef LWS_NO_CLIENT
	/* when the callback is used for client operations --> */

	case LWS_CALLBACK_CLOSED:
	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		lwsl_debug("closed\n");
		state = 0;
		break;

	case LWS_CALLBACK_CLIENT_ESTABLISHED:
		lwsl_debug("Client has connected\n");
		pss->index = 0;
		pss->len = -1;
		state = 2;
		//lws_callback_on_writable_all_protocol(context, &protocols[0]);
		lws_callback_on_writable(wsi);
		break;

	case LWS_CALLBACK_CLIENT_RECEIVE:
#ifndef LWS_NO_SERVER
		if (versa)
			goto do_rx;
#endif
		lwsl_notice("Client RX: %s", (char *)in);
		//lws_callback_on_writable_all_protocol(context, &protocols[0]);
		lws_callback_on_writable(wsi);
		break;

	case LWS_CALLBACK_CLIENT_WRITEABLE:
#ifndef LWS_NO_SERVER
		if (versa) {
			if (pss->len != (unsigned int)-1)
				goto do_tx;
			break;
		}
#endif
		/* we will send our packet... */
		char str[100];
		scanf("%s", str);
		pss->len = sprintf((char *)&pss->buf[LWS_PRE], "%s\n", str);
		//pss->len = sprintf((char *)&pss->buf[LWS_PRE],
		//		   "hello from libwebsockets-test-echo client pid %d index %d\n",
		//		   getpid(), pss->index++);
		lwsl_notice("Client TX: %s", &pss->buf[LWS_PRE]);
		n = lws_write(wsi, &pss->buf[LWS_PRE], pss->len, LWS_WRITE_TEXT);
		if (n < 0) {
			lwsl_err("ERROR %d writing to socket, hanging up\n", n);
			return -1;
		}
		if (n < (int)pss->len) {
			lwsl_err("Partial write\n");
			return -1;
		}
		break;
#endif
	case LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED:
		/* reject everything else except permessage-deflate */
		if (strcmp((const char *)in, "permessage-deflate"))
			return 1;
		break;

	default:
		break;
	}

	return 0;
}


/* list of supported protocols and callbacks */

static struct lws_protocols protocols[] = {
	/* first protocol must always be HTTP handler */

	{
		"chat",		/* name - can be overriden with -e */
		callback_echo,
		sizeof(struct per_session_data__echo),	/* per_session_data_size */
		MAX_ECHO_PAYLOAD,
	},
	{ NULL, NULL, 0, 0 } /* terminator */
};

void *thread_echo(void *threadid)
{
	while (!force_exit) {
		/*
		 * this lock means wsi in the active list cannot
		 * disappear underneath us, because the code to add and remove
		 * them is protected by the same lock
		 */
		pthread_mutex_lock(&lock_established_conns);
		lws_callback_on_writable_all_protocol(context,
				&protocols[PROTOCOL_LWS_ECHO]);
		pthread_mutex_unlock(&lock_established_conns);
		usleep(100000);
	}

	pthread_exit(NULL);
}

void *thread_service(void *threadid)
{
	while (lws_service_tsi(context, 1000, (int)(long)threadid) >= 0 && !force_exit)
		;

	pthread_exit(NULL);
}

void sighandler(int sig)
{
	force_exit = 1;
	lws_cancel_service(context);
}

static const struct lws_extension exts[] = {
	{
		"permessage-deflate",
		lws_extension_callback_pm_deflate,
		"permessage-deflate; client_no_context_takeover; client_max_window_bits"
	},
	{
		"deflate-frame",
		lws_extension_callback_pm_deflate,
		"deflate_frame"
	},
	{ NULL, NULL, NULL /* terminator */ }
};

static struct option options[] = {
	{ "help",	no_argument,		NULL, 'h' },
	{ "debug",	required_argument,	NULL, 'd' },
	{ "port",	required_argument,	NULL, 'p' },
	{ "ssl",	no_argument,		NULL, 's' },
	{ "allow-non-ssl",	no_argument,	NULL, 'a' },
	{ "interface",  required_argument,	NULL, 'i' },
	{ "closetest",  no_argument,		NULL, 'c' },
	{ "libev",  no_argument,		NULL, 'e' },
	{ "threads",  required_argument,	NULL, 'j' },
#ifndef LWS_NO_DAEMONIZE
	{ "daemonize", 	no_argument,		NULL, 'D' },
#endif
	{ "resource_path", required_argument,	NULL, 'r' },
	{ NULL, 0, 0, 0 }
};

int main(int argc, char **argv)
{
	struct lws_context_creation_info info;
	char interface_name[128] = "";
	const char *iface = NULL;
	pthread_t pthread_echo, pthread_service[32];
	char cert_path[1024];
	char key_path[1024];
 	int threads = 1;
	int use_ssl = 0;
	void *retval;
	int opts = 0;
	int n = 0;
#ifndef _WIN32
	int syslog_options = LOG_PID | LOG_PERROR;
#endif
#ifndef LWS_NO_DAEMONIZE
 	int daemonize = 0;
#endif

	/*
	 * take care to zero down the info struct, he contains random garbaage
	 * from the stack otherwise
	 */
	memset(&info, 0, sizeof info);
	info.port = 7681;

	pthread_mutex_init(&lock_established_conns, NULL);

	while (n >= 0) {
		n = getopt_long(argc, argv, "eci:hsap:d:Dr:j:", options, NULL);
		if (n < 0)
			continue;
		switch (n) {
		case 'j':
			threads = atoi(optarg);
			if (threads > ARRAY_SIZE(pthread_service)) {
				lwsl_err("Max threads %d\n",
					 ARRAY_SIZE(pthread_service));
				return 1;
			}
			break;
		case 'e':
			opts |= LWS_SERVER_OPTION_LIBEV;
			break;
#ifndef LWS_NO_DAEMONIZE
		case 'D':
			daemonize = 1;
			#ifndef _WIN32
			syslog_options &= ~LOG_PERROR;
			#endif
			break;
#endif
		case 'd':
			debug_level = atoi(optarg);
			break;
		case 's':
			use_ssl = 1;
			break;
		case 'a':
			opts |= LWS_SERVER_OPTION_ALLOW_NON_SSL_ON_SSL_PORT;
			break;
		case 'p':
			info.port = atoi(optarg);
			break;
		case 'i':
			strncpy(interface_name, optarg, sizeof interface_name);
			interface_name[(sizeof interface_name) - 1] = '\0';
			iface = interface_name;
			break;
		case 'c':
			close_testing = 1;
			fprintf(stderr, " Close testing mode -- closes on "
					   "client after 50 dumb increments"
					   "and suppresses lws_mirror spam\n");
			break;
		case 'r':
			resource_path = optarg;
			printf("Setting resource path to \"%s\"\n", resource_path);
			break;
		case 'h':
			fprintf(stderr, "Usage: test-server "
					"[--port=<p>] [--ssl] "
					"[-d <log bitfield>] "
					"[--resource_path <path>]\n");
			exit(1);
		}
	}

#if !defined(LWS_NO_DAEMONIZE) && !defined(WIN32)
	/*
	 * normally lock path would be /var/lock/lwsts or similar, to
	 * simplify getting started without having to take care about
	 * permissions or running as root, set to /tmp/.lwsts-lock
	 */
	if (daemonize && lws_daemonize("/tmp/.lwsts-lock")) {
		fprintf(stderr, "Failed to daemonize\n");
		return 1;
	}
#endif

	signal(SIGINT, sighandler);

#ifndef _WIN32
	/* we will only try to log things according to our debug_level */
	setlogmask(LOG_UPTO (LOG_DEBUG));
	openlog("lwsts", syslog_options, LOG_DAEMON);
#endif

	/* tell the library what debug level to emit and to send it to syslog */
	lws_set_log_level(debug_level, lwsl_emit_syslog);
	lwsl_notice("libwebsockets test server pthreads - license LGPL2.1+SLE\n");
	lwsl_notice("(C) Copyright 2010-2016 Andy Green <andy@warmcat.com>\n");

	printf("Using resource path \"%s\"\n", resource_path);
#ifdef EXTERNAL_POLL
	max_poll_elements = getdtablesize();
	pollfds = malloc(max_poll_elements * sizeof (struct lws_pollfd));
	fd_lookup = malloc(max_poll_elements * sizeof (int));
	if (pollfds == NULL || fd_lookup == NULL) {
		lwsl_err("Out of memory pollfds=%d\n", max_poll_elements);
		return -1;
	}
#endif

	info.iface = iface;
	info.protocols = protocols;
	info.extensions = exts;

	info.ssl_cert_filepath = NULL;
	info.ssl_private_key_filepath = NULL;

	if (use_ssl) {
		if (strlen(resource_path) > sizeof(cert_path) - 32) {
			lwsl_err("resource path too long\n");
			return -1;
		}
		sprintf(cert_path, "%s/libwebsockets-test-server.pem",
			resource_path);
		if (strlen(resource_path) > sizeof(key_path) - 32) {
			lwsl_err("resource path too long\n");
			return -1;
		}
		sprintf(key_path, "%s/libwebsockets-test-server.key.pem",
			resource_path);

		info.ssl_cert_filepath = cert_path;
		info.ssl_private_key_filepath = key_path;
	}
	info.gid = -1;
	info.uid = -1;
	info.options = opts;
	info.count_threads = threads;
	info.extensions = exts;
	info.max_http_header_pool = 4;

	context = lws_create_context(&info);
	if (context == NULL) {
		lwsl_err("libwebsocket init failed\n");
		return -1;
	}

	/* start the dumb increment thread */

	//n = pthread_create(&pthread_echo, NULL, thread_echo, 0);
	//if (n) {
	//	lwsl_err("Unable to create dumb thread\n");
	//	goto done;
	//}

	/*
	 * notice the actual number of threads may be capped by the library,
	 * so use lws_get_count_threads() to get the actual amount of threads
	 * initialized.
	 */

	for (n = 0; n < lws_get_count_threads(context); n++)
		if (pthread_create(&pthread_service[n], NULL, thread_service,
				   (void *)(long)n))
			lwsl_err("Failed to start service thread\n");

	/* wait for all the service threads to exit */

	while ((--n) >= 0)
		pthread_join(pthread_service[n], &retval);

	/* wait for pthread_echo to exit */
	pthread_join(pthread_echo, &retval);

done:
	lws_context_destroy(context);
	pthread_mutex_destroy(&lock_established_conns);

	lwsl_notice("libwebsockets-test-server exited cleanly\n");

#ifndef _WIN32
	closelog();
#endif

	return 0;
}
