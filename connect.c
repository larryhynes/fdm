/* $Id$ */

/*
 * Copyright (c) 2006 Nicholas Marriott <nicm@users.sourceforge.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "fdm.h"

#ifndef	NO_PROXY
int	httpproxy(struct server *, struct proxy *, struct io *, char **);
int	socks5proxy(struct server *, struct proxy *, struct io *, char **);
int	getport(char *);
SSL    *makessl(int, int, char **);

char *
sslerror(const char *fn)
{
	char	*cause;
	
	xasprintf(&cause,
	    "%s: %s", fn, ERR_error_string(ERR_get_error(), NULL));
	return (cause);
}

char *
sslerror2(int n, const char *fn)
{
	char	*cause;
	int	 saved_errno;

	saved_errno = errno;
	switch (n) {
	case SSL_ERROR_ZERO_RETURN:
		errno = ECONNRESET;
		/* FALLTHROUGH */
	case SSL_ERROR_SYSCALL:
		xasprintf(&cause, "%s: %s", fn, strerror(saved_errno));
		return (cause);
	case SSL_ERROR_WANT_CONNECT:
		xasprintf(&cause, "%s: want connect", fn);
		return (cause);
	case SSL_ERROR_WANT_ACCEPT:
		xasprintf(&cause, "%s: want accept", fn);
		return (cause);
	case SSL_ERROR_WANT_READ:
		xasprintf(&cause, "%s: want read", fn);
		return (cause);
	case SSL_ERROR_WANT_WRITE:
		xasprintf(&cause, "%s: want write", fn);
		return (cause);
	}

	xasprintf(&cause,
	    "%s: %d: %s", fn, n, ERR_error_string(ERR_get_error(), NULL));
	return (cause);
}

struct proxy *
getproxy(const char *xurl)
{
	struct proxy		*pr = NULL;
	char			*ptr, *end, *saved, *url;
	struct {
		const char	*proto;
		enum proxytype	 type;
		int		 ssl;
		const char	*port;
	} *proxyent, proxylist[] = {
		{ "http://",    PROXY_HTTP,   0, "http" },
		{ "https://",   PROXY_HTTPS,  1, "https" },
		{ "socks://",   PROXY_SOCKS5, 0, "socks" },
		{ "socks5://",  PROXY_SOCKS5, 0, "socks" },
		{ NULL,	        0,	      0, NULL }
	};

	/* copy the url so we can mangle it */
	saved = url = xstrdup(xurl);

	/* find proxy */
	for (proxyent = proxylist; proxyent->proto != NULL; proxyent++) {
		if (strncmp(url, proxyent->proto, strlen(proxyent->proto)) == 0)
			break;
	}
	if (proxyent->proto == NULL)
		goto error;
	url += strlen(proxyent->proto);

	pr = xcalloc(1, sizeof *pr);
	pr->type = proxyent->type;
	pr->server.ssl = proxyent->ssl;
	pr->server.port = xstrdup(proxyent->port);

	/* strip trailing '/' characters */
	ptr = url + strlen(url) - 1;
	while (ptr >= url && *ptr == '/')
		*ptr-- = '\0';
	if (*url == '\0')
		goto error;

	/* look for a user/pass */
	if ((end = strchr(url, '@')) != NULL) {
		ptr = strchr(url, ':');
		if (ptr == NULL || ptr >= end)
			goto error;

		*ptr++ = '\0';
		pr->user = xstrdup(url);
		*end++ = '\0';
		pr->pass = xstrdup(ptr);
		if (*pr->user == '\0' || *pr->pass == '\0')
			goto error;

		url = end;
	}

	/* extract port if available */
	if ((ptr = strchr(url, ':')) != NULL) {
		xfree(pr->server.port);
		pr->server.port = NULL;

		*ptr++ = '\0';
		if (*ptr == '\0')
			goto error;
		pr->server.port = xstrdup(ptr);
	}

	/* and fill in the host */
	if (*url == '\0')
		goto error;
	pr->server.host = xstrdup(url);

	xfree(saved);
	return (pr);

error:
	if (pr != NULL) {
		if (pr->user != NULL)
			xfree(pr->user);
		if (pr->pass != NULL)
			xfree(pr->pass);

		if (pr->server.port != NULL)
			xfree(pr->server.port);
		if (pr->server.host != NULL)
			xfree(pr->server.host);

		xfree(pr);
	}

	xfree(saved);
	return (NULL);
}

struct io *
connectproxy(struct server *srv, struct proxy *pr, const char *eol, int timeout,
    char **cause)
{
	struct io	*io;

	if (pr == NULL)
		return (connectio(srv, eol, timeout, cause));

	io = connectio(&pr->server, IO_CRLF, timeout, cause);
	if (io == NULL)
		return (NULL);

	switch (pr->type) {
	case PROXY_HTTP:
		if (httpproxy(srv, pr, io, cause) != 0)
			goto error;
		break;
	case PROXY_SOCKS5:
		if (socks5proxy(srv, pr, io, cause) != 0)
			goto error;
		break;
	default:
		fatalx("unknown proxy type");
	}

	/* if the original request was for SSL, initiate it now */
	if (srv->ssl && (io->ssl = makessl(io->fd, srv->verify, cause)) == NULL)
		goto error;

	io->eol = eol;
	return (io);

error:
	io_close(io);
	io_free(io);
	return (NULL);
}

int
getport(char *port)
{
	struct servent	*sv;
	int	         n;
	const char	*errstr;

	sv = getservbyname(port, NULL);
	if (sv == NULL) {
		n = strtonum(port, 1, UINT16_MAX, &errstr);
		if (errstr != NULL) {
			endservent();
			return (-1);
		}
	} else
		n = ntohs(sv->s_port);
	endservent();

	return (n);
}

int
socks5proxy(struct server *srv, struct proxy *pr, struct io *io, char **cause)
{
	int	port, auth;
	char	buf[1024], *ptr;
	size_t	len;

	if ((port = getport(srv->port)) < 0) {
		xasprintf(cause, "bad port: %s", srv->port);
		return (1);
	}

	/* method selection */
	auth = pr->user != NULL && pr->pass != NULL;
	buf[0] = 5;
	buf[1] = auth ? 2 : 1;
	buf[2] = 0;	/* 0 = no auth */
	buf[3] = 2;	/* 2 = user/pass auth */
	io_write(io, buf, auth ? 4 : 3);
	if (io_wait(io, 2, cause) != 0)
		return (1);
	io_read2(io, buf, 2);
	if (buf[0] != 5) {
		xasprintf(cause, "bad protocol version: %d", buf[0]);
		return (1);
	}
	if ((buf[1] != 0 && buf[1] != 2) || (auth == 0 && buf[1] == 2)) {
		xasprintf(cause, "unexpected method: %d", buf[1]);
		return (1);
	}

	/* user/pass negotiation */
	if (buf[1] == 2) {
		ptr = buf;
		*ptr++ = 5;
		len = strlen(pr->user);
		if (len > 255) {
			xasprintf(cause, "user too long");
			return (1);
		}
		*ptr++ = len;
		memcpy(ptr, pr->user, len);
		ptr += len;
		len = strlen(pr->pass);
		if (len > 255) {
			xasprintf(cause, "pass too long");
			return (1);
		}
		*ptr++ = len;
		memcpy(ptr, pr->pass, len);
		ptr += len;
		io_write(io, buf, ptr - buf);

		if (io_wait(io, 2, cause) != 0)
			return (1);
		io_read2(io, buf, 2);
		if (buf[0] != 5) {
			xasprintf(cause, "bad protocol version: %d", buf[0]);
			return (1);
		}
		if (buf[1] != 0) {
			xasprintf(cause, "authentication failed");
			return (1);
		}
	}

	/* connect request */
	ptr = buf;
	*ptr++ = 5;
	*ptr++ = 1; /* 1 = connect */
	*ptr++ = 0; /* reserved */
	*ptr++ = 3; /* 3 = domain name */
	len = strlen(srv->host);
	if (len > 255) {
		xasprintf(cause, "host too long");
		return (1);
	}
	*ptr++ = len;
	memcpy(ptr, srv->host, len);
	ptr += len;
	*ptr++ = (port >> 8) & 0xff;
	*ptr++ = port & 0xff;
	io_write(io, buf, ptr - buf);

	/* connect response */
	if (io_wait(io, 5, cause) != 0)
		return (1);
	io_read2(io, buf, 5);
	if (buf[0] != 5) {
		xasprintf(cause, "bad protocol version: %d", buf[0]);
		return (1);
	}
	switch (buf[1]) {
	case 0:
		break;
	case 1:
		xasprintf(cause, "%d: server failure", buf[1]);
		return (1);
	case 2:
		xasprintf(cause, "%d: connection not permitted", buf[1]);
		return (1);
	case 3:
		xasprintf(cause, "%d: network unreachable", buf[1]);
		return (1);
	case 4:
		xasprintf(cause, "%d: host unreachable", buf[1]);
		return (1);
	case 5:
		xasprintf(cause, "%d: connection refused", buf[1]);
		return (1);
	case 6:
		xasprintf(cause, "%d: TTL expired", buf[1]);
		return (1);
	case 7:
		xasprintf(cause, "%d: command not supported", buf[1]);
		return (1);
	case 8:
		xasprintf(cause, "%d: address type not supported", buf[1]);
		return (1);
	default:
		xasprintf(cause, "%d: unknown failure", buf[1]);
		return (1);
	}

	/* flush the rest */
	switch (buf[3]) {
	case 1: /* IPv4 */
		len = 5;
		break;
	case 3: /* IPv6 */
		len = 17;
		break;
	case 4: /* host */
		len = buf[4] + 2;
		break;
	default:
		xasprintf(cause, "unknown address type: %d", buf[3]);
		return (1);
	}
	if (io_wait(io, len, cause) != 0)
		return (1);
	io_read2(io, buf, len);

	return (0);
}

int
httpproxy(struct server *srv, struct proxy *pr, struct io *io, char **cause)
{
	char	*line;
	int	 port, header;

	if (pr->user != NULL || pr->pass != NULL) {
		xasprintf(cause, "HTTP proxy authentication is not supported");
		return (1);
	}

	if ((port = getport(srv->port)) < 0) {
		xasprintf(cause, "bad port: %s", srv->port);
		return (1);
	}

	io_writeline(io, "CONNECT %s:%d HTTP/1.1", srv->host, port);
	io_writeline(io, NULL);

	header = 0;
	for (;;) {
		if (io_pollline(io, &line, cause) != 1)
			return (1);

		if (header == 0) {
			if (strlen(line) < 12 ||
			    strncmp(line, "HTTP/", 5) != 0 ||
			    strncmp(line + 8, " 200", 4) != 0) {
				xfree(line);
				xasprintf(cause, "unexpected data: %s", line);
				return (1);
			}
			header = 1;
		} else {
			if (*line == '\0')
				return (0);
		}

		xfree(line);
	}
}
#endif /* NO_PROXY */

SSL *
makessl(int fd, int verify, char **cause)
{
	SSL_CTX		*ctx;
	SSL		*ssl;
	X509		*cert = NULL;
	char		 buf[256];
	int	 	 n, mode;
	long		 r;

	ctx = SSL_CTX_new(SSLv23_client_method());
	SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

	ssl = SSL_new(ctx);
	if (ssl == NULL) {
		*cause = sslerror("SSL_new");
		goto error;
	}

	if (SSL_set_fd(ssl, fd) != 1) {
		*cause = sslerror("SSL_set_fd");
		goto error;
	}

	/*
	 * Switch the socket to blocking mode to be sure we have the received
	 * the certificate. XXX This means our timeout is ignored.
	 */
	if ((mode = fcntl(fd, F_GETFL)) == -1)
		fatal("fcntl");
	if (fcntl(fd, F_SETFL, mode & ~O_NONBLOCK) == -1)
		fatal("fcntl");

	SSL_set_connect_state(ssl);
	if ((n = SSL_connect(ssl)) < 1) {
		*cause = sslerror2(SSL_get_error(ssl, n), "SSL_connect");
		goto error;
	}

	/* reset to non-blocking mode */
	if (fcntl(fd, F_SETFL, mode & ~O_NONBLOCK) == -1)
		fatal("fcntl");

	if ((cert = SSL_get_peer_certificate(ssl)) != NULL) {
		X509_NAME_oneline(X509_get_subject_name(cert), buf, sizeof buf);
		log_debug2("cert subject: %s", buf);
		X509_NAME_oneline(X509_get_issuer_name(cert), buf, sizeof buf);
		log_debug2("cert issuer: %s", buf);

		/* verify certificate if necessary */
		if (verify && ((r = SSL_get_verify_result(ssl)) != X509_V_OK)) {
			xasprintf(cause, "certificate verification failed: %s",
			    X509_verify_cert_error_string(r));
			goto error;
		}
	} else if (verify) {
		/* no certificate, error if we wanted to verify it */
		xasprintf(cause, 
		    "certificate verification failed: no certificate");
		goto error;
	}

	return (ssl);

error:
	if (cert != NULL)
		X509_free(cert);
	SSL_CTX_free(ctx);
	if (ssl != NULL)
		SSL_free(ssl);
	return (NULL);
}

struct io *
connectio(struct server *srv, const char *eol, int timeout, char **cause)
{
	int		 fd = -1, error = 0, mode;
	struct addrinfo	 hints;
	struct addrinfo	*ai;
	const char	*fn = NULL;
	SSL		*ssl;

	if (srv->ai == NULL) {
		memset(&hints, 0, sizeof hints);
		hints.ai_family = PF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		error = getaddrinfo(srv->host, srv->port, &hints, &srv->ai);
		if (error != 0) {
			*cause = xstrdup(gai_strerror(error));
			return (NULL);
		}
	}

	for (ai = srv->ai; ai != NULL; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0) {
			fn = "socket";
			continue;
		}
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
			error = errno;
			close(fd);
			errno = error;
			fd = -1;
			fn = "connect";
			continue;
		}
		break;
	}

	if (fd < 0) {
		xasprintf(cause, "%s: %s", fn, strerror(errno));
		return (NULL);
	}
	if (!srv->ssl)
		return (io_create(fd, NULL, eol, timeout));

       /*
	* Set non-blocking. Normally io_create does this, but it must be
        * done before SSL_set_fd otherwise the SSL won't be non-blocking for
        * SSL_connect.
        */
	if ((mode = fcntl(fd, F_GETFL)) == -1)
		fatal("fcntl");
	if (fcntl(fd, F_SETFL, mode | O_NONBLOCK) == -1)
		fatal("fcntl");
	
	if ((ssl = makessl(fd, srv->verify, cause)) == NULL) {
		close(fd);
		return (NULL);
	}
	return (io_create(fd, ssl, eol, timeout));
}
