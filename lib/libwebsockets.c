/*
 * libwebsockets - small server side websockets and web server implementation
 * 
 * Copyright (C) 2010 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#include "private-libwebsockets.h"

#ifdef LWS_OPENSSL_SUPPORT
SSL_CTX *ssl_ctx;
int use_ssl;
#endif


extern int 
libwebsocket_read(struct libwebsocket *wsi, unsigned char * buf, size_t len);


/* document the generic callback (it's a fake prototype under this) */
/**
 * callback() - User server actions
 * @wsi:	Opaque websocket instance pointer
 * @reason:	The reason for the call
 * @user:	Pointer to per-session user data allocated by library
 * @in:		Pointer used for some callback reasons
 * @len:	Length set for some callback reasons
 * 
 * 	This callback is the way the user controls what is served.  All the
 * 	protocol detail is hidden and handled by the library.
 * 
 * 	For each connection / session there is user data allocated that is
 * 	pointed to by "user".  You set the size of this user data area when
 * 	the library is initialized with libwebsocket_create_server.
 * 
 * 	You get an opportunity to initialize user data when called back with
 * 	LWS_CALLBACK_ESTABLISHED reason.
 * 
 * 	LWS_CALLBACK_ESTABLISHED:  after successful websocket handshake
 * 
 * 	LWS_CALLBACK_CLOSED: when the websocket session ends
 *
 * 	LWS_CALLBACK_BROADCAST: signal to send to client (you would use
 * 				libwebsocket_write() taking care about the
 * 				special buffer requirements
 * 	LWS_CALLBACK_RECEIVE: data has appeared for the server, it can be
 *				found at *in and is len bytes long
 *
 *  	LWS_CALLBACK_HTTP: an http request has come from a client that is not
 * 				asking to upgrade the connection to a websocket
 * 				one.  This is a chance to serve http content,
 * 				for example, to send a script to the client
 * 				which will then open the websockets connection.
 * 				@in points to the URI path requested and 
 * 				libwebsockets_serve_http_file() makes it very
 * 				simple to send back a file to the client.
 */
extern int callback(struct libwebsocket * wsi,
			 enum libwebsocket_callback_reasons reason, void * user,
							  void *in, size_t len);


void 
libwebsocket_close_and_free_session(struct libwebsocket *wsi)
{
	int n;

	if ((unsigned long)wsi < LWS_MAX_PROTOCOLS)
		return;

	n = wsi->state;

	wsi->state = WSI_STATE_DEAD_SOCKET;

	if (wsi->protocol->callback && n == WSI_STATE_ESTABLISHED)
		wsi->protocol->callback(wsi, LWS_CALLBACK_CLOSED,
						      wsi->user_space, NULL, 0);

	for (n = 0; n < WSI_TOKEN_COUNT; n++)
		if (wsi->utf8_token[n].token)
			free(wsi->utf8_token[n].token);

//	fprintf(stderr, "closing fd=%d\n", wsi->sock);

#ifdef LWS_OPENSSL_SUPPORT
	if (use_ssl) {
		n = SSL_get_fd(wsi->ssl);
		SSL_shutdown(wsi->ssl);
		close(n);
		SSL_free(wsi->ssl);
	} else {
#endif
		shutdown(wsi->sock, SHUT_RDWR);
		close(wsi->sock);
#ifdef LWS_OPENSSL_SUPPORT
	}
#endif
	if (wsi->user_space)
		free(wsi->user_space);

	free(wsi);
}

static int
libwebsocket_poll_connections(struct libwebsocket_context *this)
{
	unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING + MAX_BROADCAST_PAYLOAD +
						  LWS_SEND_BUFFER_POST_PADDING];
	int client;
	int n;
	size_t len;

	/* check for activity on client sockets */
	
	for (client = this->count_protocols + 1; client < this->fds_count;
								     client++) {
		
		/* handle session socket closed */
		
		if (this->fds[client].revents & (POLLERR | POLLHUP)) {
			
			debug("Session Socket %d %p (fd=%d) dead\n",
				  client, this->wsi[client], this->fds[client]);

			libwebsocket_close_and_free_session(this->wsi[client]);
			goto nuke_this;
		}
		
		/* any incoming data ready? */

		if (!(this->fds[client].revents & POLLIN))
			continue;

		/* broadcast? */

		if ((unsigned long)this->wsi[client] < LWS_MAX_PROTOCOLS) {

			len = read(this->fds[client].fd,
				   buf + LWS_SEND_BUFFER_PRE_PADDING,
				   MAX_BROADCAST_PAYLOAD);
			if (len < 0) {
				fprintf(stderr, "Error receiving broadcast payload\n");
				continue;
			}

			/* broadcast it to all guys with this protocol index */

			for (n = this->count_protocols + 1;
						     n < this->fds_count; n++) {

				if ((unsigned long)this->wsi[n] <
							      LWS_MAX_PROTOCOLS)
					continue;

				/*
				 * never broadcast to non-established
				 * connection
				 */

				if (this->wsi[n]->state != WSI_STATE_ESTABLISHED)
					continue;

				/*
				 * only broadcast to connections using
				 * the requested protocol
				 */

				if (this->wsi[n]->protocol->protocol_index !=
					       (unsigned long)this->wsi[client])
					continue;

				this->wsi[n]->protocol-> callback(this->wsi[n],
						 LWS_CALLBACK_BROADCAST, 
						 this->wsi[n]->user_space,
						 buf + LWS_SEND_BUFFER_PRE_PADDING, len);
			}

			continue;
		}

#ifdef LWS_OPENSSL_SUPPORT
		if (this->use_ssl)
			n = SSL_read(this->wsi[client]->ssl, buf, sizeof buf);
		else
#endif
			n = recv(this->fds[client].fd, buf, sizeof buf, 0);

		if (n < 0) {
			fprintf(stderr, "Socket read returned %d\n", n);
			continue;
		}
		if (!n) {
//			fprintf(stderr, "POLLIN with 0 len waiting\n");
				libwebsocket_close_and_free_session(
							     this->wsi[client]);
			goto nuke_this;
		}


		/* service incoming data */

		if (libwebsocket_read(this->wsi[client], buf, n) >= 0)
			continue;
		
		/*
		 * it closed and nuked wsi[client], so remove the
		 * socket handle and wsi from our service list
		 */
nuke_this:

		debug("nuking wsi %p, fsd_count = %d\n",
					this->wsi[client], this->fds_count - 1);

		this->fds_count--;
		for (n = client; n < this->fds_count; n++) {
			this->fds[n] = this->fds[n + 1];
			this->wsi[n] = this->wsi[n + 1];
		}
		break;
	}

	return 0;
}



/**
 * libwebsocket_create_server() - Create the listening websockets server
 * @port:	Port to listen on
 * @protocols:	Array of structures listing supported protocols and a protocol-
 * 		specific callback for each one.  The list is ended with an
 * 		entry that has a NULL callback pointer.
 * 	        It's not const because we write the owning_server member
 * @ssl_cert_filepath:	If libwebsockets was compiled to use ssl, and you want
 * 			to listen using SSL, set to the filepath to fetch the
 * 			server cert from, otherwise NULL for unencrypted
 * @ssl_private_key_filepath: filepath to private key if wanting SSL mode,
 * 			else ignored
 * @gid:	group id to change to after setting listen socket, or -1.
 * @uid:	user id to change to after setting listen socket, or -1.
 * 
 * 	This function creates the listening socket and takes care
 * 	of all initialization in one step.
 *
 * 	After initialization, it forks a thread that will sits in a service loop
 *	and returns to the caller.  The actual service actions are performed by
 * 	user code in a per-protocol callback from the appropriate one selected
 *	by the client from the list in @protocols.
 * 
 * 	The protocol callback functions are called for a handful of events
 * 	including http requests coming in, websocket connections becoming
 * 	established, and data arriving; it's also called periodically to allow
 * 	async transmission.
 *
 * 	HTTP requests are sent always to the FIRST protocol in @protocol, since
 * 	at that time websocket protocol has not been negotiated.  Other
 * 	protocols after the first one never see any HTTP callack activity.
 * 
 * 	The server created is a simple http server by default; part of the
 * 	websocket standard is upgrading this http connection to a websocket one.
 * 
 * 	This allows the same server to provide files like scripts and favicon /
 * 	images or whatever over http and dynamic data over websockets all in
 * 	one place; they're all handled in the user callback.
 */

int libwebsocket_create_server(int port,
			       struct libwebsocket_protocols *protocols,
			       const char * ssl_cert_filepath,
			       const char * ssl_private_key_filepath,
			       int gid, int uid)
{
	int n;
	int client;
	int sockfd;
	int fd;
	unsigned int clilen;
	struct sockaddr_in serv_addr, cli_addr;
	int opt = 1;
	struct libwebsocket_context * this = NULL;
	unsigned int slen;

#ifdef LWS_OPENSSL_SUPPORT
	SSL_METHOD *method;
	char ssl_err_buf[512];

	use_ssl = ssl_cert_filepath != NULL && ssl_private_key_filepath != NULL;
	if (use_ssl)
		fprintf(stderr, " Compiled with SSL support, using it\n");
	else
		fprintf(stderr, " Compiled with SSL support, not using it\n");

#else
	if (ssl_cert_filepath != NULL && ssl_private_key_filepath != NULL) {
		fprintf(stderr, " Not compiled for OpenSSl support!\n");
		return -1;
	}
	fprintf(stderr, " Compiled without SSL support, serving unencrypted\n");
#endif

#ifdef LWS_OPENSSL_SUPPORT
	if (use_ssl) {
		SSL_library_init();

		OpenSSL_add_all_algorithms();
		SSL_load_error_strings();

			// Firefox insists on SSLv23 not SSLv3
			// Konq disables SSLv2 by default now, SSLv23 works

		method = (SSL_METHOD *)SSLv23_server_method();
		if (!method) {
			fprintf(stderr, "problem creating ssl method: %s\n",
				ERR_error_string(ERR_get_error(), ssl_err_buf));
			return -1;
		}
		ssl_ctx = SSL_CTX_new(method);	/* create context */
		if (!ssl_ctx) {
			printf("problem creating ssl context: %s\n",
				ERR_error_string(ERR_get_error(), ssl_err_buf));
			return -1;
		}
		/* set the local certificate from CertFile */
		n = SSL_CTX_use_certificate_file(ssl_ctx,
					ssl_cert_filepath, SSL_FILETYPE_PEM);
		if (n != 1) {
			fprintf(stderr, "problem getting cert '%s': %s\n",
				ssl_cert_filepath,
				ERR_error_string(ERR_get_error(), ssl_err_buf));
			return -1;
		}
		/* set the private key from KeyFile */
		if (SSL_CTX_use_PrivateKey_file(ssl_ctx,
						ssl_private_key_filepath,
						SSL_FILETYPE_PEM) != 1) {
			fprintf(stderr, "ssl problem getting key '%s': %s\n",
						ssl_private_key_filepath,
				ERR_error_string(ERR_get_error(), ssl_err_buf));
			return (-1);
		}
		/* verify private key */
		if (!SSL_CTX_check_private_key(ssl_ctx)) {
			fprintf(stderr, "Private SSL key doesn't match cert\n");
			return (-1);
		}

		/* SSL is happy and has a cert it's content with */
	}
#endif

	this = malloc(sizeof (struct libwebsocket_context));

	/* set up our external listening socket we serve on */
  
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		fprintf(stderr, "ERROR opening socket");
		return -1;
	}
	
	/* allow us to restart even if old sockets in TIME_WAIT */
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	bzero((char *) &serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(port);

	n = bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
	if (n < 0) {
              fprintf(stderr, "ERROR on binding to port %d (%d %d)\n", port, n,
									 errno);
              return -1;
        }

	/* drop any root privs for this process */

	if (gid != -1)
		if (setgid(gid))
			fprintf(stderr, "setgid: %s\n", strerror(errno));
	if (uid != -1)
		if (setuid(uid))
			fprintf(stderr, "setuid: %s\n", strerror(errno));

 	/*
	 * prepare the poll() fd array... it's like this
	 *
	 * [0] = external listening socket
	 * [1 .. this->count_protocols] = per-protocol broadcast sockets
	 * [this->count_protocols + 1 ... this->fds_count-1] = connection skts
	 */

	this->fds_count = 1;
	this->fds[0].fd = sockfd;
	this->fds[0].events = POLLIN;
	this->count_protocols = 0;
#ifdef LWS_OPENSSL_SUPPORT
	this->use_ssl = use_ssl;
#endif

	listen(sockfd, 5);
	fprintf(stderr, " Listening on port %d\n", port);

	/* set up our internal broadcast trigger sockets per-protocol */

	for (; protocols[this->count_protocols].callback;
						      this->count_protocols++) {
		protocols[this->count_protocols].owning_server = this;
		protocols[this->count_protocols].protocol_index =
							  this->count_protocols;

		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0) {
			fprintf(stderr, "ERROR opening socket");
			return -1;
		}
		
		/* allow us to restart even if old sockets in TIME_WAIT */
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

		bzero((char *) &serv_addr, sizeof(serv_addr));
		serv_addr.sin_family = AF_INET;
		serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
		serv_addr.sin_port = 0; /* pick the port for us */

		n = bind(fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
		if (n < 0) {
		      fprintf(stderr, "ERROR on binding to port %d (%d %d)\n",
								port, n, errno);
		      return -1;
		}

		slen = sizeof cli_addr;
		n = getsockname(fd, (struct sockaddr *)&cli_addr, &slen);
		if (n < 0) {
			fprintf(stderr, "getsockname failed\n");
			return -1;
		}
		protocols[this->count_protocols].broadcast_socket_port =
						       ntohs(cli_addr.sin_port);
		listen(fd, 5);

		debug("  Protocol %s broadcast socket %d\n",
				protocols[this->count_protocols].name,
						      ntohs(cli_addr.sin_port));

		this->fds[this->fds_count].fd = fd;
		this->fds[this->fds_count].events = POLLIN;
		/* wsi only exists for connections, not broadcast listener */
		this->wsi[this->fds_count] = NULL;
		this->fds_count++;
	}


	/*
	 * We will enter out poll and service loop now, just before that
	 * fork and return to caller for the main thread of execution
	 */

	n = fork();
	if (n < 0) {
		fprintf(stderr, "Failed to fork websocket poll loop\n");
		return -1;
	}
	if (n) {
		/* original process context */

		/*
		 * before we return to caller, we set up per-protocol
		 * broadcast sockets connected to the server ready to use
		 */

		/* give server fork a chance to start up */
		sleep(1);

		for (client = 1; client < this->count_protocols + 1; client++) {
			fd = socket(AF_INET, SOCK_STREAM, 0);
			if (fd < 0) {
				fprintf(stderr,"Unable to create socket\n");
				return -1;
			}
			cli_addr.sin_family = AF_INET;
			cli_addr.sin_port = htons(
				   protocols[client - 1].broadcast_socket_port);
			cli_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
			n = connect(fd, (struct sockaddr *)&cli_addr,
							       sizeof cli_addr);
			if (n < 0) {
				fprintf(stderr, "Unable to connect to "
						"broadcast socket %d, %s\n",
						client, strerror(errno));
				return -1;
			}

			protocols[client - 1].broadcast_socket_user_fd = fd;
		}

		fprintf(stderr, "libwebsocket poll process forked\n");
		
		return 0;
	}

	/* we want a SIGHUP when our parent goes down */
	prctl(PR_SET_PDEATHSIG, SIGHUP);

	/* in this forked process, sit and service websocket connections */
    
	while (1) {

		n = poll(this->fds, this->fds_count, 1000);

		if (n < 0 || this->fds[0].revents & (POLLERR | POLLHUP)) {
			fprintf(stderr, "Listen Socket dead\n");
			goto fatal;
		}
		if (n == 0) /* poll timeout */
			continue;

		/* handle accept on listening socket? */

		for (client = 0; client < this->count_protocols + 1; client++) {

			if (!this->fds[client].revents & POLLIN)
				continue;

			/* listen socket got an unencrypted connection... */

			clilen = sizeof(cli_addr);
			fd  = accept(this->fds[client].fd,
				     (struct sockaddr *)&cli_addr, &clilen);
			if (fd < 0) {
				fprintf(stderr, "ERROR on accept");
				continue;
			}

			if (this->fds_count >= MAX_CLIENTS) {
				fprintf(stderr, "too busy");
				close(fd);
				continue;
			}

			if (client) {
				/*
				 * accepting a connection to broadcast socket
				 * set wsi to be protocol index not pointer
				 */

				this->wsi[this->fds_count] =
				      (struct libwebsocket *)(long)(client - 1);

				goto fill_in_fds;
			}

			/* accepting connection to main listener */

			this->wsi[this->fds_count] =
					    malloc(sizeof(struct libwebsocket));
			if (!this->wsi[this->fds_count])
				return -1;

	#ifdef LWS_OPENSSL_SUPPORT
			if (this->use_ssl) {

				this->wsi[this->fds_count]->ssl =
							       SSL_new(ssl_ctx);
				if (this->wsi[this->fds_count]->ssl == NULL) {
					fprintf(stderr, "SSL_new failed: %s\n",
					    ERR_error_string(SSL_get_error(
					    this->wsi[this->fds_count]->ssl, 0),
									 NULL));
					free(this->wsi[this->fds_count]);
					continue;
				}

				SSL_set_fd(this->wsi[this->fds_count]->ssl, fd);

				n = SSL_accept(this->wsi[this->fds_count]->ssl);
				if (n != 1) {
					/*
					 * browsers seem to probe with various
					 * ssl params which fail then retry
					 * and succeed
					 */
					debug("SSL_accept failed skt %u: %s\n",
					      fd,
					      ERR_error_string(SSL_get_error(
					      this->wsi[this->fds_count]->ssl,
								     n), NULL));
					SSL_free(
					       this->wsi[this->fds_count]->ssl);
					free(this->wsi[this->fds_count]);
					continue;
				}
				debug("accepted new SSL conn  "
				      "port %u on fd=%d SSL ver %s\n",
					ntohs(cli_addr.sin_port), fd,
					  SSL_get_version(this->wsi[
							this->fds_count]->ssl));
				
			} else
	#endif
				debug("accepted new conn  port %u on fd=%d\n",
						  ntohs(cli_addr.sin_port), fd);
				
			/* intialize the instance struct */

			this->wsi[this->fds_count]->sock = fd;
			this->wsi[this->fds_count]->state = WSI_STATE_HTTP;
			this->wsi[this->fds_count]->name_buffer_pos = 0;

			for (n = 0; n < WSI_TOKEN_COUNT; n++) {
				this->wsi[this->fds_count]->
						     utf8_token[n].token = NULL;
				this->wsi[this->fds_count]->
						    utf8_token[n].token_len = 0;
			}

			/*
			 * these can only be set once the protocol is known
			 * we set an unestablished connection's protocol pointer
			 * to the start of the supported list, so it can look
			 * for matching ones during the handshake
			 */
			this->wsi[this->fds_count]->protocol = protocols;
			this->wsi[this->fds_count]->user_space = NULL;

			/*
			 * Default protocol is 76
			 * After 76, there's a header specified to inform which
			 * draft the client wants, when that's seen we modify
			 * the individual connection's spec revision accordingly
			 */
			this->wsi[this->fds_count]->ietf_spec_revision = 76;

fill_in_fds:

			/*
			 * make sure NO events are seen yet on this new socket
			 * (otherwise we inherit old fds[client].revents from
			 * previous socket there and die mysteriously! )
			 */
			this->fds[this->fds_count].revents = 0;

			this->fds[this->fds_count].events = POLLIN;
			this->fds[this->fds_count++].fd = fd;

		}


		/* service anything incoming on websocket connection */

		libwebsocket_poll_connections(this);
	}
	
fatal:

	/* close listening skt and per-protocol broadcast sockets */
	for (client = 0; client < this->fds_count; client++)
		close(this->fds[0].fd);

#ifdef LWS_OPENSSL_SUPPORT
	SSL_CTX_free(ssl_ctx);
#endif
	kill(0, SIGTERM);

	if (this)
		free(this);
	
	return 0;
}

/**
 * libwebsockets_get_protocol() - Returns a protocol pointer from a websocket
 * 				  connection.
 * @wsi:	pointer to struct websocket you want to know the protocol of
 *
 * 
 * 	This is useful to get the protocol to broadcast back to from inside
 * the callback.
 */

const struct libwebsocket_protocols *
libwebsockets_get_protocol(struct libwebsocket *wsi)
{
	return wsi->protocol;
}

/**
 * libwebsockets_broadcast() - Sends a buffer to rthe callback for all active
 * 				  connections of the given protocol.
 * @protocol:	pointer to the protocol you will broadcast to all members of
 * @buf:  buffer containing the data to be broadcase.  NOTE: this has to be
 * 		allocated with LWS_SEND_BUFFER_PRE_PADDING valid bytes before
 * 		the pointer and LWS_SEND_BUFFER_POST_PADDING afterwards in the
 * 		case you are calling this function from callback context.
 * @len:	length of payload data in buf, starting from buf.
 * 
 * 	This function allows bulk sending of a packet to every connection using
 * the given protocol.  It does not send the data directly; instead it calls
 * the callback with a reason type of LWS_CALLBACK_BROADCAST.  If the callback
 * wants to actually send the data for that connection, the callback itself
 * should call libwebsocket_write().
 *
 * libwebsockets_broadcast() can be called from another fork context without
 * having to take any care about data visibility between the processes, it'll
 * "just work".
 */


int
libwebsockets_broadcast(const struct libwebsocket_protocols * protocol,
						 unsigned char *buf, size_t len)
{
	struct libwebsocket_context * this = protocol->owning_server;
	int n;

	if (!protocol->broadcast_socket_user_fd) {
		/*
		 * we are being called from poll thread context
		 * eg, from a callback.  In that case don't use sockets for
		 * broadcast IPC (since we can't open a socket connection to
		 * a socket listening on our own thread) but directly do the
		 * send action.
		 *
		 * Locking is not needed because we are by definition being
		 * called in the poll thread context and are serialized.
		 */

		for (n = this->count_protocols + 1; n < this->fds_count; n++) {

			if ((unsigned long)this->wsi[n] < LWS_MAX_PROTOCOLS)
				continue;

			/* never broadcast to non-established connection */

			if (this->wsi[n]->state != WSI_STATE_ESTABLISHED)
				continue;

			/* only broadcast to guys using requested protocol */

			if (this->wsi[n]->protocol != protocol)
				continue;

			this->wsi[n]->protocol-> callback(this->wsi[n],
					 LWS_CALLBACK_BROADCAST, 
					 this->wsi[n]->user_space,
					 buf, len);
		}

		return 0;
	}

	n = send(protocol->broadcast_socket_user_fd, buf, len, 0);

	return n;
}
