/**
 * @file agent.c
 * @author David Kupka <xkupka01@stud.fit.vutbr.cz>
 * @brief NETCONF agent. Starts as ssh subsystem, performs handshake and passes
 * messages between server and client.
 *
 * Copyright (c) 2011, CESNET, z.s.p.o.
 * All rights reserved.
 *
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the CESNET, z.s.p.o. nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include <libxml/tree.h>

#include <libnetconf_xml.h>

#include "common.c"
#include "comm.h"

/* Define libnetconf submodules necessary for the NETCONF agent */
#define NC_INIT_AGENT NC_INIT_NOTIF

volatile int done = 0;

typedef int model_t;

struct ntf_thread_config {
	struct nc_session *session;
	nc_rpc *subscribe_rpc;
};

static void* notification_thread(void* arg)
{
	struct ntf_thread_config *config = (struct ntf_thread_config*)arg;

	ncntf_dispatch_send(config->session, config->subscribe_rpc);
	nc_rpc_free(config->subscribe_rpc);
	free(config);

	return (NULL);
}

/*!
 * \brief Signal handler
 *
 * Handles received UNIX signals and sets value to control main loop
 *
 * \param sig 	signal number
 */
void signal_handler(int sig)
{
	clb_print(NC_VERB_VERBOSE, "Signal received.");

	fprintf(stderr, "Signal %d received.\n", sig);

	switch (sig) {
	case SIGINT:
	case SIGTERM:
	case SIGQUIT:
	case SIGABRT:
	case SIGKILL:
		if (done == 0) {
			/* first attempt */
			done = 1;
		} else {
			/* second attempt */
			clb_print(NC_VERB_ERROR, "Hey! I need some time to stop, be patient next time!");
			exit(EXIT_FAILURE);
		}
		break;
	default:
		clb_print(NC_VERB_ERROR, "exiting on signal.");
		exit(EXIT_FAILURE);
		break;
	}
}

static struct nc_cpblts* get_server_capabilities(conn_t* conn)
{
	struct nc_cpblts* srv_cpblts;
	char **cpblts_list = NULL;
	int i;

	if ((cpblts_list = comm_get_srv_cpblts(conn)) == NULL) {
		clb_print(NC_VERB_ERROR, "Cannot get server capabilities!");
		return (NULL);
	}

	/* Fill server capabilities structure */
	srv_cpblts = nc_cpblts_new((const char* const*)cpblts_list);

	/* cleanup */
	for (i = 0; cpblts_list != NULL && cpblts_list[i] != NULL; i++) {
		free(cpblts_list[i]);
	}
	free(cpblts_list);

	return srv_cpblts;
}

int process_message(struct nc_session *session, conn_t *conn, const nc_rpc *rpc)
{
	nc_reply * reply = NULL;
	struct nc_err * err;
	pthread_t thread;
	struct ntf_thread_config * ntf_config;
	xmlNodePtr op;
	char * sid;

	if (rpc == NULL) {
		nc_verb_error("Invalid RPC to process.");
		return (EXIT_FAILURE);
	}

	/* close-session message */
	switch (nc_rpc_get_op(rpc)) {
	case NC_OP_CLOSESESSION:
		if (comm_close(conn) != EXIT_SUCCESS) {
			err = nc_err_new(NC_ERR_OP_FAILED);
			reply = nc_reply_error(err);
		} else {
			reply = nc_reply_ok();
		}
		done = 1;
		break;
	case NC_OP_KILLSESSION:
		if ((op = ncxml_rpc_get_op_content(rpc)) == NULL || op->name == NULL ||
				xmlStrEqual(op->name, BAD_CAST "kill-session") == 0) {
			clb_print(NC_VERB_ERROR, "Corrupted RPC message.");
			reply = nc_reply_error(nc_err_new(NC_ERR_OP_FAILED));
			goto send_reply;
		}
		if (op->children == NULL || xmlStrEqual(op->children->name, BAD_CAST "session-id") == 0) {
			clb_print(NC_VERB_ERROR, "No session id found.");
			err = nc_err_new(NC_ERR_MISSING_ELEM);
			nc_err_set(err, NC_ERR_PARAM_INFO_BADELEM, "session-id");
			reply = nc_reply_error(err);
			goto send_reply;
		}
		sid = (char *)xmlNodeGetContent(op->children);
		reply = comm_kill_session(conn, sid);
		free(sid);
		break;
	case NC_OP_CREATESUBSCRIPTION:
		/* create-subscription message */
		if (nc_cpblts_enabled(session, "urn:ietf:params:netconf:capability:notification:1.0") == 0) {
			reply = nc_reply_error(nc_err_new(NC_ERR_OP_NOT_SUPPORTED));
			goto send_reply;
		}

		/* check if notifications are allowed on this session */
		if (nc_session_notif_allowed(session) == 0) {
			clb_print(NC_VERB_ERROR, "Notification subscription is not allowed on this session.");
			err = nc_err_new(NC_ERR_OP_FAILED);
			nc_err_set(err, NC_ERR_PARAM_TYPE, "protocol");
			nc_err_set(err, NC_ERR_PARAM_MSG, "Another notification subscription is currently active on this session.");
			reply = nc_reply_error(err);
			goto send_reply;
		}

		reply = ncntf_subscription_check(rpc);
		if (nc_reply_get_type(reply) != NC_REPLY_OK) {
			goto send_reply;
		}

		if ((ntf_config = malloc(sizeof(struct ntf_thread_config))) == NULL) {
			clb_print(NC_VERB_ERROR, "Memory allocation failed.");
			err = nc_err_new(NC_ERR_OP_FAILED);
			nc_err_set(err, NC_ERR_PARAM_MSG, "Memory allocation failed.");
			reply = nc_reply_error(err);
			err = NULL;
			goto send_reply;
		}
		ntf_config->session = (struct nc_session*)session;
		ntf_config->subscribe_rpc = nc_rpc_dup(rpc);

		/* perform notification sending */
		if ((pthread_create(&thread, NULL, notification_thread, ntf_config)) != 0) {
			nc_reply_free(reply);
			err = nc_err_new(NC_ERR_OP_FAILED);
			nc_err_set(err, NC_ERR_PARAM_MSG, "Creating thread for sending Notifications failed.");
			reply = nc_reply_error(err);
			err = NULL;
			goto send_reply;
		}
		pthread_detach(thread);
		break;
	default:
		/* other messages */
		reply = comm_operation(conn, rpc);
		break;
	}

send_reply:
	nc_session_send_reply(session, rpc, reply);
	nc_reply_free(reply);
	return EXIT_SUCCESS;
}

#ifdef ENABLE_TLS
char *get_tls_username(void)
{
#ifndef PATCHED_STUNNEL
	char *subj, *cn, *aux;
	int len;

	/* try to get information from environment variable commonly provided by stunnel(1) */
	subj = getenv("SSL_CLIENT_DN");
	if (!subj) {
		/* we are not able to get correct username */
		clb_print(NC_VERB_ERROR, "Missing \'SSL_CLIENT_DN\' enviornment variable, unable to get username.");
		return (NULL);
	}
	/* parse subject to get CN */
	cn = strstr(subj, "CN=");
	if (!cn) {
		clb_print(NC_VERB_ERROR, "Client certificate does not include commonName, unable to get username.");
		return (NULL);
	}
	cn = cn + 3;
	/* detect if the CN is followed by another item */
	aux = strchr(cn, '/');
	/* get the length of the CN value */
	if (aux != NULL) {
		len = aux - cn;
	} else {
		len = strlen(cn);
	}
	/* store (only) the CN value into the resulting string */
	aux = malloc(len * sizeof(char));
	strncpy(aux, cn, len);
	aux[len] = '\0';

	return (aux);
#else
	/*
	 * we are running with patched stunnel(1) which provides much more info
	 * from the client certificate
	 */
#endif
}

#endif /* ENABLE_TLS */

int main()
{
	conn_t *con;
	struct nc_session * netconf_con;
	nc_rpc * rpc = NULL;
	struct nc_cpblts * capabilities = NULL;
	int ret;
	NC_MSG_TYPE rpc_type;
	int timeout = 500; /* ms, poll timeout */
	struct pollfd fds;
	struct sigaction action;

#ifdef ENABLE_TLS
	struct passwd *pw;
	char* username;
#endif

	/* set signal handler */
	sigfillset(&action.sa_mask);
	action.sa_handler = signal_handler;
	action.sa_flags = 0;
	sigaction(SIGINT, &action, NULL );
	sigaction(SIGQUIT, &action, NULL );
	sigaction(SIGABRT, &action, NULL );
	sigaction(SIGTERM, &action, NULL );
	sigaction(SIGKILL, &action, NULL );

#ifdef DEBUG
	nc_verbosity(NC_VERB_DEBUG);
#endif
	openlog("netopeer-agent", LOG_PID, LOG_DAEMON);
	nc_callback_print(clb_print);

	/* initialize library */
	if (nc_init(NC_INIT_AGENT) < 0) {
		clb_print(NC_VERB_ERROR, "Library initialization failed");
		return EXIT_FAILURE;
	}

	/* connect to server */
	if ((con = comm_connect()) == NULL) {
		clb_print(NC_VERB_ERROR, "Cannot connect to Netopeer server.");
		return EXIT_FAILURE;
	}
	clb_print(NC_VERB_VERBOSE, "Connected with Netopeer server");

	/* get server capabilities */
	if ((capabilities = get_server_capabilities(con)) == NULL) {
		clb_print(NC_VERB_ERROR, "Cannot get server capabilities.");
		return EXIT_FAILURE;
	}

#ifdef ENABLE_TLS
	/*
	 * Are we running with the TLS transport? If yes, the TLS server should
	 * provide SSL_CLIENT_DN environment variable for us.
	 */
	if (getenv("SSL_CLIENT_DN")) {
		/* try to get client certificate from stunnel */
		username = get_tls_username();

		/* accept client session and handle capabilities */
		netconf_con = nc_session_accept_username(capabilities, username);
		nc_cpblts_free(capabilities);

		/* switch user if possible/needed */
		/*
		 * OpenSSH (sshd) does this automatically, but TLS server (stunnel) does not,
		 * so in case of SSH transport, we already have different UID, in case of
		 * TLS transport, we are going to try to switch UID if we can
		 */
		if (getuid() == 0) {
			/* we are going to drop privileges forever */
			pw = getpwnam(username);
			if (pw) {
				setuid(pw->pw_uid);
			}

			/*
			 * if this part fails, we still can continue as user 0 - username is
			 * stored in the NETCONF session information and all NETCONF actions
			 * are (should be) taken according to this value.
			 */
		}

		free(username);
	} else {
#else
	{
#endif
		/* there is probably SSH transport */
		netconf_con = nc_session_accept(capabilities);
		nc_cpblts_free(capabilities);
	}
	if (netconf_con == NULL) {
		clb_print(NC_VERB_ERROR, "Failed to connect agent.");
		return EXIT_FAILURE;
	}

	/* monitor this session and build statistics */
	nc_session_monitor(netconf_con);

	if (comm_session_info(con, netconf_con)) {
		clb_print(NC_VERB_ERROR, "Failed to comunicate with server.");
		return EXIT_FAILURE;
	}

	clb_print(NC_VERB_VERBOSE, "Handshake finished");

	fds.fd = nc_session_get_eventfd(netconf_con);
	fds.events = POLLIN;

	while (!done) {
		ret = poll(&fds, 1, timeout);
		if (ret < 0 && errno != EINTR) { /* poll error */
			clb_print(NC_VERB_ERROR, "poll failed.");
			goto cleanup;
		} else if (ret == 0) { /* timeout */
			continue;
		} else if (ret > 0) { /* event occured */
			if (fds.revents & POLLHUP) { /* client hung up */
				clb_print(NC_VERB_VERBOSE, "Connection closed by client");
				comm_close(con);
				goto cleanup;
			} else if (fds.revents & POLLERR) { /* I/O error */
				clb_print(NC_VERB_ERROR, "I/O error.");
				goto cleanup;
			} else if (fds.revents & POLLIN) { /* data ready */
				/* read data from input */
				rpc_type = nc_session_recv_rpc(netconf_con, -1, &rpc);
				if (rpc_type != NC_MSG_RPC) {
					switch (rpc_type) {
					case NC_MSG_NONE:
						/* the request was already processed by libnetconf or no message available */
						/* continue in main while loop */
						break;
					case NC_MSG_UNKNOWN:
						if (nc_session_get_status(netconf_con) != NC_SESSION_STATUS_WORKING) {
							/* something really bad happened, and communication is not possible anymore */
							clb_print(NC_VERB_ERROR, "Failed to receive clinets message");
							goto cleanup;
						}
						/* continue in main while loop */
						break;
					default:
						/* continue in main while loop */
						break;
					}
				} else {
					clb_print(NC_VERB_VERBOSE, "Processing client message");
					if (process_message(netconf_con, con, rpc) != EXIT_SUCCESS) {
						clb_print(NC_VERB_WARNING, "Message processing failed");
					}
					nc_rpc_free(rpc);
					rpc = NULL;
				}
			}
		}
	}

cleanup:
	nc_rpc_free(rpc);
	nc_session_free(netconf_con);
	nc_close(0);

	return (EXIT_SUCCESS);
}

