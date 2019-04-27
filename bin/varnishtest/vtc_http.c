/*-
 * Copyright (c) 2008-2019 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#include <sys/socket.h>

#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "vtc.h"
#include "vtc_http.h"

#include "vct.h"
#include "vfil.h"
#include "vgz.h"
#include "vnum.h"
#include "vrnd.h"
#include "vtcp.h"
#include "hpack.h"

extern const struct cmds http_cmds[];

/* SECTION: client-server client/server
 *
 * Client and server threads are fake HTTP entities used to test your Varnish
 * and VCL. They take any number of arguments, and the one that are not
 * recognized, assuming they don't start with '-', are treated as
 * specifications, laying out the actions to undertake::
 *
 *         client cNAME [...]
 *         server sNAME [...]
 *
 * Clients and server are identified by a string that's the first argument,
 * clients' names start with 'c' and servers' names start with 's'.
 *
 * As the client and server commands share a good deal of arguments and
 * specification actions, they are grouped in this single section, specific
 * items will be explicitly marked as such.
 *
 * SECTION: client-server.macros Macros and automatic behaviour
 *
 * To make things easier in the general case, clients will connect by default
 * to the first Varnish server declared and the -vcl+backend switch of the
 * ``varnish`` command will add all the declared servers as backends.
 *
 * Be careful though, servers will by default listen to the 127.0.0.1 IP and
 * will pick a random port, and publish 3 macros: sNAME_addr, sNAME_port and
 * sNAME_sock, but only once they are started.
 * For 'varnish -vcl+backend' to create the vcl with the correct values, the
 * server must be started first.
 *
 * SECTION: client-server.args Arguments
 *
 * \-start
 *        Start the thread in background, processing the last given
 *        specification.
 *
 * \-wait
 *        Block until the thread finishes.
 *
 * \-run (client only)
 *        Equivalent to "-start -wait".
 *
 * \-repeat NUMBER
 *        Instead of processing the specification only once, do it NUMBER times.
 *
 * \-keepalive
 *        For repeat, do not open new connections but rather run all
 *        iterations in the same connection
 *
 * \-break (server only)
 *        Stop the server.
 *
 * \-listen STRING (server only)
 *        Dictate the listening socket for the server. STRING is of the form
 *        "IP PORT", or "/PATH/TO/SOCKET" for a Unix domain socket. In the
 *        latter case, the path must begin with '/', and the server must be
 *        able to create it.
 *
 * \-connect STRING (client only)
 *        Indicate the server to connect to. STRING is also of the form
 *        "IP PORT", or "/PATH/TO/SOCKET". As with "server -listen", a
 *        Unix domain socket is recognized when STRING begins with a '/'.
 *
 * \-dispatch (server only, s0 only)
 *        Normally, to keep things simple, server threads only handle one
 *        connection at a time, but the -dispatch switch allows to accept
 *        any number of connection and handle them following the given spec.
 *
 *        However, -dispatch is only allowed for the server name "s0".
 *
 * \-proxy1 STRING (client only)
 *        Use the PROXY protocol version 1 for this connection. STRING
 *        is of the form "CLIENTIP:PORT SERVERIP:PORT".
 *
 * \-proxy2 STRING (client only)
 *        Use the PROXY protocol version 2 for this connection. STRING
 *        is of the form "CLIENTIP:PORT SERVERIP:PORT".
 *
 * SECTION: client-server.spec Specification
 *
 * It's a string, either double-quoted "like this", but most of the time
 * enclosed in curly brackets, allowing multilining. Write a command per line in
 * it, empty line are ignored, and long line can be wrapped by using a
 * backslash. For example::
 *
 *     client c1 {
 *         txreq -url /foo \
 *               -hdr "bar: baz"
 *
 *         rxresp
 *     } -run
 */

#define ONLY_CLIENT(hp, av)						\
	do {								\
		if (hp->h2)						\
			vtc_fatal(hp->vl,				\
			    "\"%s\" only possible before H/2 upgrade",	\
					av[0]);				\
		if (hp->sfd != NULL)					\
			vtc_fatal(hp->vl,				\
			    "\"%s\" only possible in client", av[0]);	\
	} while (0)

#define ONLY_SERVER(hp, av)						\
	do {								\
		if (hp->h2)						\
			vtc_fatal(hp->vl,				\
			    "\"%s\" only possible before H/2 upgrade",	\
					av[0]);				\
		if (hp->sfd == NULL)					\
			vtc_fatal(hp->vl,				\
			    "\"%s\" only possible in server", av[0]);	\
	} while (0)


/* XXX: we may want to vary this */
static const char * const nl = "\r\n";

/**********************************************************************
 * Generate a synthetic body
 */

char *
synth_body(const char *len, int rnd)
{
	int i, j, k, l;
	char *b;


	AN(len);
	i = strtoul(len, NULL, 0);
	assert(i > 0);
	b = malloc(i + 1L);
	AN(b);
	l = k = '!';
	for (j = 0; j < i; j++) {
		if ((j % 64) == 63) {
			b[j] = '\n';
			k++;
			if (k == '~')
				k = '!';
			l = k;
		} else if (rnd) {
			b[j] = (VRND_RandomTestable() % 95) + ' ';
		} else {
			b[j] = (char)l;
			if (++l == '~')
				l = '!';
		}
	}
	b[i - 1] = '\n';
	b[i] = '\0';
	return (b);
}

/**********************************************************************
 * Finish and write the vsb to the fd
 */

static void
http_write(const struct http *hp, int lvl, const char *pfx)
{
	ssize_t l;

	AZ(VSB_finish(hp->vsb));
	vtc_dump(hp->vl, lvl, pfx, VSB_data(hp->vsb), VSB_len(hp->vsb));
	l = write(hp->fd, VSB_data(hp->vsb), VSB_len(hp->vsb));
	if (l != VSB_len(hp->vsb))
		vtc_log(hp->vl, hp->fatal, "Write failed: (%zd vs %zd) %s",
		    l, VSB_len(hp->vsb), strerror(errno));
}

/**********************************************************************
 * find header
 */

static char *
http_find_header(char * const *hh, const char *hdr)
{
	int n, l;
	char *r;

	l = strlen(hdr);

	for (n = 3; hh[n] != NULL; n++) {
		if (strncasecmp(hdr, hh[n], l) || hh[n][l] != ':')
			continue;
		for (r = hh[n] + l + 1; vct_issp(*r); r++)
			continue;
		return (r);
	}
	return (NULL);
}

/**********************************************************************
 * count header
 */

static int
http_count_header(char * const *hh, const char *hdr)
{
	int n, l, r = 0;

	l = strlen(hdr);

	for (n = 3; hh[n] != NULL; n++) {
		if (strncasecmp(hdr, hh[n], l) || hh[n][l] != ':')
			continue;
		r++;
	}
	return (r);
}

/* SECTION: client-server.spec.expect
 *
 * expect STRING1 OP STRING2
 *         Test if "STRING1 OP STRING2" is true, and if not, fails the test.
 *         OP can be ==, <, <=, >, >= when STRING1 and STRING2 represent numbers
 *         in which case it's an order operator. If STRING1 and STRING2 are
 *         meant as strings OP is a matching operator, either == (exact match)
 *         or ~ (regex match).
 *
 *         varnishtet will first try to resolve STRING1 and STRING2 by looking
 *         if they have special meanings, in which case, the resolved value is
 *         use for the test. Note that this value can be a string representing a
 *         number, allowing for tests such as::
 *
 *                 expect req.http.x-num > 2
 *
 *         Here's the list of recognized strings, most should be obvious as they
 *         either match VCL logic, or the txreq/txresp options:
 *
 *         - remote.ip
 *         - remote.port
 *         - remote.path
 *         - req.method
 *         - req.url
 *         - req.proto
 *         - resp.proto
 *         - resp.status
 *         - resp.reason
 *         - resp.chunklen
 *         - req.bodylen
 *         - req.body
 *         - resp.bodylen
 *         - resp.body
 *         - req.http.NAME
 *         - resp.http.NAME
 */

static const char *
cmd_var_resolve(struct http *hp, char *spec)
{
	char **hh, *hdr;
	if (!strcmp(spec, "remote.ip"))
		return (hp->rem_ip);
	if (!strcmp(spec, "remote.port"))
		return (hp->rem_port);
	if (!strcmp(spec, "remote.path"))
		return (hp->rem_path);
	if (!strcmp(spec, "req.method"))
		return (hp->req[0]);
	if (!strcmp(spec, "req.url"))
		return (hp->req[1]);
	if (!strcmp(spec, "req.proto"))
		return (hp->req[2]);
	if (!strcmp(spec, "resp.proto"))
		return (hp->resp[0]);
	if (!strcmp(spec, "resp.status"))
		return (hp->resp[1]);
	if (!strcmp(spec, "resp.reason"))
		return (hp->resp[2]);
	if (!strcmp(spec, "resp.chunklen"))
		return (hp->chunklen);
	if (!strcmp(spec, "req.bodylen"))
		return (hp->bodylen);
	if (!strcmp(spec, "req.body"))
		return (hp->body != NULL ? hp->body : spec);
	if (!strcmp(spec, "resp.bodylen"))
		return (hp->bodylen);
	if (!strcmp(spec, "resp.body"))
		return (hp->body != NULL ? hp->body : spec);
	if (!strncmp(spec, "req.http.", 9)) {
		hh = hp->req;
		hdr = spec + 9;
	} else if (!strncmp(spec, "resp.http.", 10)) {
		hh = hp->resp;
		hdr = spec + 10;
	} else if (!strcmp(spec, "h2.state")) {
		if (hp->h2)
			return ("true");
		else
			return ("false");
	} else
		return (spec);
	hdr = http_find_header(hh, hdr);
	return (hdr);
}

static void
cmd_http_expect(CMD_ARGS)
{
	struct http *hp;
	const char *lhs;
	char *cmp;
	const char *rhs;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	AZ(strcmp(av[0], "expect"));
	av++;

	AN(av[0]);
	AN(av[1]);
	AN(av[2]);
	AZ(av[3]);
	lhs = cmd_var_resolve(hp, av[0]);
	cmp = av[1];
	rhs = cmd_var_resolve(hp, av[2]);

	vtc_expect(vl, av[0], lhs, cmp, av[2], rhs);
}

static void
cmd_http_expect_pattern(CMD_ARGS)
{
	char *p;
	struct http *hp;
	char t = '0';

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	AZ(strcmp(av[0], "expect_pattern"));
	av++;
	AZ(av[0]);
	for (p = hp->body; *p != '\0'; p++) {
		if (*p != t)
			vtc_fatal(hp->vl,
			    "EXPECT PATTERN FAIL @%zd should 0x%02x is 0x%02x",
			    p - hp->body, t, *p);
		t += 1;
		t &= ~0x08;
	}
	vtc_log(hp->vl, 4, "EXPECT PATTERN SUCCESS");
}

/**********************************************************************
 * Split a HTTP protocol header
 */

static void
http_splitheader(struct http *hp, int req)
{
	char *p, *q, **hh;
	int n;
	char buf[20];

	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);
	if (req) {
		memset(hp->req, 0, sizeof hp->req);
		hh = hp->req;
	} else {
		memset(hp->resp, 0, sizeof hp->resp);
		hh = hp->resp;
	}

	n = 0;
	p = hp->rx_b;
	if (*p == '\0') {
		vtc_log(hp->vl, 4, "No headers");
		return;
	}

	/* REQ/PROTO */
	while (vct_islws(*p))
		p++;
	hh[n++] = p;
	while (!vct_islws(*p))
		p++;
	AZ(vct_iscrlf(p));
	*p++ = '\0';

	/* URL/STATUS */
	while (vct_issp(*p))		/* XXX: H space only */
		p++;
	AZ(vct_iscrlf(p));
	hh[n++] = p;
	while (!vct_islws(*p))
		p++;
	if (vct_iscrlf(p)) {
		hh[n++] = NULL;
		q = p;
		p += vct_skipcrlf(p);
		*q = '\0';
	} else {
		*p++ = '\0';
		/* PROTO/MSG */
		while (vct_issp(*p))		/* XXX: H space only */
			p++;
		hh[n++] = p;
		while (!vct_iscrlf(p))
			p++;
		q = p;
		p += vct_skipcrlf(p);
		*q = '\0';
	}
	assert(n == 3);

	while (*p != '\0') {
		assert(n < MAX_HDR);
		if (vct_iscrlf(p))
			break;
		hh[n++] = p++;
		while (*p != '\0' && !vct_iscrlf(p))
			p++;
		q = p;
		p += vct_skipcrlf(p);
		*q = '\0';
	}
	p += vct_skipcrlf(p);
	assert(*p == '\0');

	for (n = 0; n < 3 || hh[n] != NULL; n++) {
		bprintf(buf, "http[%2d] ", n);
		vtc_dump(hp->vl, 4, buf, hh[n], -1);
	}
}


/**********************************************************************
 * Receive another character
 */

static int
http_rxchar(struct http *hp, int n, int eof)
{
	int i;
	struct pollfd pfd[1];

	while (n > 0) {
		pfd[0].fd = hp->fd;
		pfd[0].events = POLLIN;
		pfd[0].revents = 0;
		i = poll(pfd, 1, hp->timeout);
		if (i < 0 && errno == EINTR)
			continue;
		if (i == 0) {
			vtc_log(hp->vl, hp->fatal,
			    "HTTP rx timeout (fd:%d %u ms)",
			    hp->fd, hp->timeout);
			continue;
		}
		if (i < 0) {
			vtc_log(hp->vl, hp->fatal,
			    "HTTP rx failed (fd:%d poll: %s)",
			    hp->fd, strerror(errno));
			continue;
		}
		assert(i > 0);
		assert(hp->rx_p + n < hp->rx_e);
		i = read(hp->fd, hp->rx_p, n);
		if (!(pfd[0].revents & POLLIN))
			vtc_log(hp->vl, 4,
			    "HTTP rx poll (fd:%d revents: %x n=%d, i=%d)",
			    hp->fd, pfd[0].revents, n, i);
		if (i == 0 && eof)
			return (i);
		if (i == 0) {
			vtc_log(hp->vl, hp->fatal,
			    "HTTP rx EOF (fd:%d read: %s) %d",
			    hp->fd, strerror(errno), n);
			return (-1);
		}
		if (i < 0) {
			vtc_log(hp->vl, hp->fatal,
			    "HTTP rx failed (fd:%d read: %s)",
			    hp->fd, strerror(errno));
			return (-1);
		}
		hp->rx_p += i;
		*hp->rx_p = '\0';
		n -= i;
	}
	return (1);
}

static int
http_rxchunk(struct http *hp)
{
	char *q, *old;
	int i;

	old = hp->rx_p;
	do {
		if (http_rxchar(hp, 1, 0) < 0)
			return (-1);
	} while (hp->rx_p[-1] != '\n');
	vtc_dump(hp->vl, 4, "len", old, -1);
	i = strtoul(old, &q, 16);
	bprintf(hp->chunklen, "%d", i);
	if ((q == old) || (q == hp->rx_p) || (*q != '\0' && !vct_islws(*q))) {
		vtc_log(hp->vl, hp->fatal, "Chunklen fail (%02x @ %td)",
		    (*q & 0xff), q - old);
		return (-1);
	}
	assert(*q == '\0' || vct_islws(*q));
	hp->rx_p = old;
	if (i > 0) {
		if (http_rxchar(hp, i, 0) < 0)
			return (-1);
		vtc_dump(hp->vl, 4, "chunk", old, i);
	}
	old = hp->rx_p;
	if (http_rxchar(hp, 2, 0) < 0)
		return (-1);
	if (!vct_iscrlf(old)) {
		vtc_log(hp->vl, hp->fatal, "Chunklen without CRLF");
		return (-1);
	}
	hp->rx_p = old;
	*hp->rx_p = '\0';
	return (i);
}

/**********************************************************************
 * Swallow a HTTP message body
 *
 * max: 0 is all
 */

static void
http_swallow_body(struct http *hp, char * const *hh, int body, int max)
{
	const char *p, *q;
	int i, l, ll;

	l = hp->rx_p - hp->body;

	p = http_find_header(hh, "transfer-encoding");
	q = http_find_header(hh, "content-length");
	if (p != NULL && !strcasecmp(p, "chunked")) {
		if (q != NULL) {
			vtc_log(hp->vl, hp->fatal, "Both C-E: Chunked and C-L");
			return;
		}
		ll = 0;
		while (http_rxchunk(hp) > 0) {
			ll = (hp->rx_p - hp->body) - l;
			if (max && ll >= max)
				break;
		}
		p = "chunked";
	} else if (q != NULL) {
		ll = strtoul(q, NULL, 10);
		if (max && ll > l + max)
			ll = max;
		else
			ll -= l;
		i = http_rxchar(hp, ll, 0);
		if (i < 0)
			return;
		p = "c-l";
	} else if (body) {
		ll = 0;
		do  {
			i = http_rxchar(hp, 1, 1);
			if (i < 0)
				return;
			ll += i;
			if (max && ll >= max)
				break;
		} while (i > 0);
		p = "eof";
	} else {
		p = "none";
		ll = l = 0;
	}
	vtc_dump(hp->vl, 4, p, hp->body + l, ll);
	l += ll;
	hp->bodyl = l;
	bprintf(hp->bodylen, "%d", l);
}

/**********************************************************************
 * Receive a HTTP protocol header
 */

static void
http_rxhdr(struct http *hp)
{
	int i, s = 0;
	char *p;
	ssize_t l;

	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);
	hp->rx_p = hp->rx_b;
	*hp->rx_p = '\0';
	hp->body = NULL;
	bprintf(hp->bodylen, "%s", "<undef>");
	while (1) {
		p = hp->rx_p;
		i = http_rxchar(hp, 1, 1);
		if (i < 1)
			break;
		if (s == 0 && *p == '\r')
			s = 1;
		else if ((s == 0 || s == 1) && *p == '\n')
			s = 2;
		else if (s == 2 && *p == '\r')
			s = 3;
		else if ((s == 2 || s == 3) && *p == '\n')
			break;
		else
			s = 0;
	}
	l = hp->rx_p - hp->rx_b;
	vtc_dump(hp->vl, 4, "rxhdr", hp->rx_b, l);
	vtc_log(hp->vl, 4, "rxhdrlen = %zd", l);
	hp->body = hp->rx_p;
}

/* SECTION: client-server.spec.rxresp
 *
 * rxresp [-no_obj] (client only)
 *         Receive and parse a response's headers and body. If -no_obj is
 *         present, only get the headers.
 */

static void
cmd_http_rxresp(CMD_ARGS)
{
	struct http *hp;
	int has_obj = 1;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	ONLY_CLIENT(hp, av);
	AZ(strcmp(av[0], "rxresp"));
	av++;

	for (; *av != NULL; av++)
		if (!strcmp(*av, "-no_obj"))
			has_obj = 0;
		else
			vtc_fatal(hp->vl,
			    "Unknown http rxresp spec: %s\n", *av);
	http_rxhdr(hp);
	http_splitheader(hp, 0);
	if (http_count_header(hp->resp, "Content-Length") > 1)
		vtc_fatal(hp->vl,
		    "Multiple Content-Length headers.\n");
	if (!has_obj)
		return;
	if (!hp->resp[0] || !hp->resp[1])
		return;
	if (hp->head_method)
		return;
	if (!strcmp(hp->resp[1], "200"))
		http_swallow_body(hp, hp->resp, 1, 0);
	else
		http_swallow_body(hp, hp->resp, 0, 0);
	vtc_log(hp->vl, 4, "bodylen = %s", hp->bodylen);
}

/* SECTION: client-server.spec.rxresphdrs
 *
 * rxresphdrs (client only)
 *         Receive and parse a response's headers.
 */

static void
cmd_http_rxresphdrs(CMD_ARGS)
{
	struct http *hp;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	ONLY_CLIENT(hp, av);
	AZ(strcmp(av[0], "rxresphdrs"));
	av++;

	for (; *av != NULL; av++)
		vtc_fatal(hp->vl, "Unknown http rxresp spec: %s\n", *av);
	http_rxhdr(hp);
	http_splitheader(hp, 0);
	if (http_count_header(hp->resp, "Content-Length") > 1)
		vtc_fatal(hp->vl,
		    "Multiple Content-Length headers.\n");
}

/**********************************************************************
 * Ungzip rx'ed body
 */

#define OVERHEAD 64L

static void
cmd_http_gunzip(CMD_ARGS)
{
	int i;
	z_stream vz;
	struct http *hp;
	char *p;
	unsigned l;

	(void)av;
	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);

	memset(&vz, 0, sizeof vz);

	AN(hp->body);
	if (hp->body[0] != (char)0x1f || hp->body[1] != (char)0x8b)
		vtc_log(hp->vl, hp->fatal,
		    "Gunzip error: Body lacks gzip magics");
	vz.next_in = TRUST_ME(hp->body);
	vz.avail_in = hp->bodyl;

	l = hp->bodyl * 10;
	p = calloc(1, l);
	AN(p);

	vz.next_out = TRUST_ME(p);
	vz.avail_out = l;

	assert(Z_OK == inflateInit2(&vz, 31));
	i = inflate(&vz, Z_FINISH);
	assert(vz.total_out < l);
	hp->bodyl = vz.total_out;
	memcpy(hp->body, p, hp->bodyl);
	free(p);
	vtc_log(hp->vl, 3, "new bodylen %u", hp->bodyl);
	vtc_dump(hp->vl, 4, "body", hp->body, hp->bodyl);
	bprintf(hp->bodylen, "%u", hp->bodyl);
#ifdef VGZ_EXTENSIONS
	vtc_log(hp->vl, 4, "startbit = %ju %ju/%ju",
	    (uintmax_t)vz.start_bit,
	    (uintmax_t)vz.start_bit >> 3, (uintmax_t)vz.start_bit & 7);
	vtc_log(hp->vl, 4, "lastbit = %ju %ju/%ju",
	    (uintmax_t)vz.last_bit,
	    (uintmax_t)vz.last_bit >> 3, (uintmax_t)vz.last_bit & 7);
	vtc_log(hp->vl, 4, "stopbit = %ju %ju/%ju",
	    (uintmax_t)vz.stop_bit,
	    (uintmax_t)vz.stop_bit >> 3, (uintmax_t)vz.stop_bit & 7);
#endif
	if (i != Z_STREAM_END)
		vtc_log(hp->vl, hp->fatal,
		    "Gunzip error = %d (%s) in:%jd out:%jd",
		    i, vz.msg, (intmax_t)vz.total_in, (intmax_t)vz.total_out);
	assert(Z_OK == inflateEnd(&vz));
	hp->body[hp->bodyl] = '\0';
}

/**********************************************************************
 * Create a gzip'ed body
 */

static void
gzip_body(const struct http *hp, const char *txt, char **body, int *bodylen)
{
	int l;
	z_stream vz;
#ifdef VGZ_EXTENSIONS
	int i;
#endif

	memset(&vz, 0, sizeof vz);

	l = strlen(txt);
	*body = calloc(1, l + OVERHEAD);
	AN(*body);

	vz.next_in = TRUST_ME(txt);
	vz.avail_in = l;

	vz.next_out = TRUST_ME(*body);
	vz.avail_out = l + OVERHEAD;

	assert(Z_OK == deflateInit2(&vz,
	    hp->gziplevel, Z_DEFLATED, 31, 9, Z_DEFAULT_STRATEGY));
	assert(Z_STREAM_END == deflate(&vz, Z_FINISH));
	*bodylen = vz.total_out;
#ifdef VGZ_EXTENSIONS
	i = vz.stop_bit & 7;
	if (hp->gzipresidual >= 0 && hp->gzipresidual != i)
		vtc_log(hp->vl, hp->fatal,
		    "Wrong gzip residual got %d wanted %d",
		    i, hp->gzipresidual);
	vtc_log(hp->vl, 4, "startbit = %ju %ju/%ju",
	    (uintmax_t)vz.start_bit,
	    (uintmax_t)vz.start_bit >> 3, (uintmax_t)vz.start_bit & 7);
	vtc_log(hp->vl, 4, "lastbit = %ju %ju/%ju",
	    (uintmax_t)vz.last_bit,
	    (uintmax_t)vz.last_bit >> 3, (uintmax_t)vz.last_bit & 7);
	vtc_log(hp->vl, 4, "stopbit = %ju %ju/%ju",
	    (uintmax_t)vz.stop_bit,
	    (uintmax_t)vz.stop_bit >> 3, (uintmax_t)vz.stop_bit & 7);
	assert(Z_OK == deflateEnd(&vz));
#endif
}

/**********************************************************************
 * Handle common arguments of a transmited request or response
 */

static char* const *
http_tx_parse_args(char * const *av, struct vtclog *vl, struct http *hp,
    char *body, unsigned nohost)
{
	int bodylen = 0;
	char *b, *c;
	char *nullbody;
	char *m;
	ssize_t len;
	int nolen = 0;
	int l;

	(void)vl;
	nullbody = body;

	for (; *av != NULL; av++) {
		if (!strcmp(*av, "-nolen")) {
			nolen = 1;
		} else if (!strcmp(*av, "-nohost")) {
			nohost = 1;
		} else if (!strcmp(*av, "-hdr")) {
			if (!strncasecmp(av[1], "Host:", 5))
				nohost = 1;
			VSB_printf(hp->vsb, "%s%s", av[1], nl);
			av++;
		} else if (!strcmp(*av, "-hdrlen")) {
			VSB_printf(hp->vsb, "%s: ", av[1]);
			l = atoi(av[2]);
			while (l-- > 0)
				VSB_putc(hp->vsb, '0' + (l % 10));
			VSB_printf(hp->vsb, "%s", nl);
			av+=2;
		} else
			break;
	}
	for (; *av != NULL; av++) {
		if (!strcmp(*av, "-body")) {
			assert(body == nullbody);
			REPLACE(body, av[1]);

			AN(body);
			av++;
			bodylen = strlen(body);
			for (b = body; *b != '\0'; b++) {
				if (*b == '\\' && b[1] == '0') {
					*b = '\0';
					for (c = b+1; *c != '\0'; c++) {
						*c = c[1];
					}
					b++;
					bodylen--;
				}
			}
		} else if (!strcmp(*av, "-bodyfrom")) {
			assert(body == nullbody);
			free(body);
			body = VFIL_readfile(NULL, av[1], &len);
			AN(body);
			assert(len < INT_MAX);
			bodylen = len;
			av++;
		} else if (!strcmp(*av, "-bodylen")) {
			assert(body == nullbody);
			free(body);
			body = synth_body(av[1], 0);
			bodylen = strlen(body);
			av++;
		} else if (!strcmp(*av, "-gzipresidual")) {
			hp->gzipresidual = strtoul(av[1], NULL, 0);
			av++;
		} else if (!strcmp(*av, "-gziplevel")) {
			hp->gziplevel = strtoul(av[1], NULL, 0);
			av++;
		} else if (!strcmp(*av, "-gziplen")) {
			assert(body == nullbody);
			free(body);
			b = synth_body(av[1], 1);
			gzip_body(hp, b, &body, &bodylen);
			free(b);
			VSB_printf(hp->vsb, "Content-Encoding: gzip%s", nl);
			// vtc_hexdump(hp->vl, 4, "gzip", (void*)body, bodylen);
			av++;
		} else if (!strcmp(*av, "-gzipbody")) {
			assert(body == nullbody);
			free(body);
			gzip_body(hp, av[1], &body, &bodylen);
			VSB_printf(hp->vsb, "Content-Encoding: gzip%s", nl);
			// vtc_hexdump(hp->vl, 4, "gzip", (void*)body, bodylen);
			av++;
		} else
			break;
	}
	if (!nohost) {
		m = macro_get("localhost", NULL);
		AN(m);
		VSB_printf(hp->vsb, "Host: %s%s", m, nl);
		free(m);
	}
	if (body != NULL && !nolen)
		VSB_printf(hp->vsb, "Content-Length: %d%s", bodylen, nl);
	VSB_cat(hp->vsb, nl);
	if (body != NULL) {
		VSB_bcat(hp->vsb, body, bodylen);
		free(body);
	}
	return (av);
}

/* SECTION: client-server.spec.txreq
 *
 * txreq|txresp [...]
 *         Send a minimal request or response, but overload it if necessary.
 *
 *         txreq is client-specific and txresp is server-specific.
 *
 *         The only thing different between a request and a response, apart
 *         from who can send them is that the first line (request line vs
 *         status line), so all the options are prety much the same.
 *
 *         \-method STRING (txreq only)
 *                 What method to use (default: "GET").
 *
 *         \-req STRING (txreq only)
 *                 Alias for -method.
 *
 *         \-url STRING (txreq only)
 *                 What location to use (default "/").
 *
 *         \-proto STRING
 *                 What protocol use in the status line.
 *                 (default: "HTTP/1.1").
 *
 *         \-status NUMBER (txresp only)
 *                 What status code to return (default 200).
 *
 *         \-reason STRING (txresp only)
 *                 What message to put in the status line (default: "OK").
 *
 *         These three switches can appear in any order but must come before the
 *         following ones.
 *
 *         \-nohost
 *                 Don't include a Host header in the request.
 *
 *         \-nolen
 *                 Don't include a Content-Length header.
 *
 *         \-hdr STRING
 *                 Add STRING as a header, it must follow this format:
 *                 "name: value". It can be called multiple times.
 *
 *         \-hdrlen STRING NUMBER
 *                 Add STRING as a header with NUMBER bytes of content.
 *
 *         You can then use the arguments related to the body:
 *
 *         \-body STRING
 *                 Input STRING as body.
 *
 *         \-bodyfrom FILE
 *                 Same as -body but content is read from FILE.
 *
 *         \-bodylen NUMBER
 *                 Generate and input a body that is NUMBER bytes-long.
 *
 *         \-gziplevel NUMBER
 *		   Set the gzip level (call it before any of the other gzip
 *		   switches).
 *
 *         \-gzipresidual NUMBER
 *                 Add extra gzip bits. You should never need it.
 *
 *         \-gzipbody STRING
 *                 Zip STRING and send it as body.
 *
 *         \-gziplen NUMBER
 *                 Combine -body and -gzipbody: create a body of length NUMBER,
 *                 zip it and send as body.
 */

/**********************************************************************
 * Transmit a response
 */

static void
cmd_http_txresp(CMD_ARGS)
{
	struct http *hp;
	const char *proto = "HTTP/1.1";
	const char *status = "200";
	const char *reason = "OK";
	char* body = NULL;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	ONLY_SERVER(hp, av);
	AZ(strcmp(av[0], "txresp"));
	av++;

	VSB_clear(hp->vsb);

	for (; *av != NULL; av++) {
		if (!strcmp(*av, "-proto")) {
			proto = av[1];
			av++;
		} else if (!strcmp(*av, "-status")) {
			status = av[1];
			av++;
		} else if (!strcmp(*av, "-reason")) {
			reason = av[1];
			av++;
			continue;
		} else
			break;
	}

	VSB_printf(hp->vsb, "%s %s %s%s", proto, status, reason, nl);

	/* send a "Content-Length: 0" header unless something else happens */
	REPLACE(body, "");

	av = http_tx_parse_args(av, vl, hp, body, 1);
	if (*av != NULL)
		vtc_fatal(hp->vl, "Unknown http txresp spec: %s\n", *av);

	http_write(hp, 4, "txresp");
}

static void
cmd_http_upgrade(CMD_ARGS)
{
	char *h;
	struct http *hp;

	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	ONLY_SERVER(hp, av);
	AN(hp->sfd);

	h = http_find_header(hp->req, "Upgrade");
	if (!h || strcmp(h, "h2c"))
		vtc_fatal(vl, "Req misses \"Upgrade: h2c\" header");

	h = http_find_header(hp->req, "Connection");
	if (!h || strcmp(h, "Upgrade, HTTP2-Settings"))
		vtc_fatal(vl, "Req misses \"Connection: "
			"Upgrade, HTTP2-Settings\" header");

	h = http_find_header(hp->req, "HTTP2-Settings");
	if (!h)
		vtc_fatal(vl, "Req misses \"HTTP2-Settings\" header");


	parse_string("txresp -status 101 "
				"-hdr \"Connection: Upgrade\" "
				"-hdr \"Upgrade: h2c\"\n", cmd, hp, vl);

	b64_settings(hp, h);

	parse_string("rxpri\n"
			"stream 0 {\n"
			"txsettings\n"
			"rxsettings\n"
			"txsettings -ack\n"
			"rxsettings\n"
			"expect settings.ack == true\n"
			"} -start\n", cmd, hp, vl);
}

/**********************************************************************
 * Receive a request
 */

/* SECTION: client-server.spec.rxreq
 *
 * rxreq (server only)
 *         Receive and parse a request's headers and body.
 */
static void
cmd_http_rxreq(CMD_ARGS)
{
	struct http *hp;

	(void)cmd;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	ONLY_SERVER(hp, av);
	AZ(strcmp(av[0], "rxreq"));
	av++;

	for (; *av != NULL; av++)
		vtc_fatal(vl, "Unknown http rxreq spec: %s\n", *av);
	http_rxhdr(hp);
	http_splitheader(hp, 1);
	if (http_count_header(hp->req, "Content-Length") > 1)
		vtc_fatal(vl, "Multiple Content-Length headers.\n");
	http_swallow_body(hp, hp->req, 0, 0);
	vtc_log(vl, 4, "bodylen = %s", hp->bodylen);
}

/* SECTION: client-server.spec.rxreqhdrs
 *
 * rxreqhdrs (server only)
 *         Receive and parse a request's headers (but not the body).
 */

static void
cmd_http_rxreqhdrs(CMD_ARGS)
{
	struct http *hp;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	AZ(strcmp(av[0], "rxreqhdrs"));
	av++;

	for (; *av != NULL; av++)
		vtc_fatal(hp->vl, "Unknown http rxreq spec: %s\n", *av);
	http_rxhdr(hp);
	http_splitheader(hp, 1);
	if (http_count_header(hp->req, "Content-Length") > 1)
		vtc_fatal(hp->vl, "Multiple Content-Length headers.\n");
}

/* SECTION: client-server.spec.rxreqbody
 *
 * rxreqbody (server only)
 *         Receive a request's body.
 */

static void
cmd_http_rxreqbody(CMD_ARGS)
{
	struct http *hp;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	ONLY_SERVER(hp, av);
	AZ(strcmp(av[0], "rxreqbody"));
	av++;

	for (; *av != NULL; av++)
		vtc_fatal(hp->vl, "Unknown http rxreq spec: %s\n", *av);
	http_swallow_body(hp, hp->req, 0, 0);
	vtc_log(hp->vl, 4, "bodylen = %s", hp->bodylen);
}

/* SECTION: client-server.spec.rxrespbody
 *
 * rxrespbody (client only)
 *         Receive (part of) a response's body.
 *
 * -max : max length of this receive, 0 for all
 */

static void
cmd_http_rxrespbody(CMD_ARGS)
{
	struct http *hp;
	int max = 0;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	ONLY_CLIENT(hp, av);
	AZ(strcmp(av[0], "rxrespbody"));
	av++;

	for (; *av != NULL; av++)
		if (!strcmp(*av, "-max")) {
			max = atoi(av[1]);
			av++;
		} else
			vtc_fatal(hp->vl,
			    "Unknown http rxrespbody spec: %s\n", *av);

	http_swallow_body(hp, hp->resp, 1, max);
	vtc_log(hp->vl, 4, "bodylen = %s", hp->bodylen);
}

/* SECTION: client-server.spec.rxchunk
 *
 * rxchunk
 *         Receive an HTTP chunk.
 */

static void
cmd_http_rxchunk(CMD_ARGS)
{
	struct http *hp;
	int ll, i;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	ONLY_CLIENT(hp, av);

	i = http_rxchunk(hp);
	if (i == 0) {
		ll = hp->rx_p - hp->body;
		hp->bodyl = ll;
		bprintf(hp->bodylen, "%d", ll);
		vtc_log(hp->vl, 4, "bodylen = %s", hp->bodylen);
	}
}

/**********************************************************************
 * Transmit a request
 */

static void
cmd_http_txreq(CMD_ARGS)
{
	struct http *hp;
	const char *req = "GET";
	const char *url = "/";
	const char *proto = "HTTP/1.1";
	const char *up = NULL;
	unsigned nohost;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	ONLY_CLIENT(hp, av);
	AZ(strcmp(av[0], "txreq"));
	av++;

	VSB_clear(hp->vsb);

	hp->head_method = 0;
	for (; *av != NULL; av++) {
		if (!strcmp(*av, "-url")) {
			url = av[1];
			av++;
		} else if (!strcmp(*av, "-proto")) {
			proto = av[1];
			av++;
		} else if (!strcmp(*av, "-method") ||
		    !strcmp(*av, "-req")) {
			req = av[1];
			hp->head_method = !strcasecmp(av[1], "HEAD") ;
			av++;
		} else if (!hp->sfd && !strcmp(*av, "-up")) {
			up = av[1];
			av++;
		} else
			break;
	}
	VSB_printf(hp->vsb, "%s %s %s%s", req, url, proto, nl);

	if (up)
		VSB_printf(hp->vsb, "Connection: Upgrade, HTTP2-Settings%s"
				"Upgrade: h2c%s"
				"HTTP2-Settings: %s%s", nl, nl, up, nl);

	nohost = strcasecmp(proto, "HTTP/1.1") != 0;
	av = http_tx_parse_args(av, vl, hp, NULL, nohost);
	if (*av != NULL)
		vtc_fatal(hp->vl, "Unknown http txreq spec: %s\n", *av);
	http_write(hp, 4, "txreq");

	if (up) {
		parse_string("rxresp\n"
				"expect resp.status == 101\n"
				"expect resp.http.connection == Upgrade\n"
				"expect resp.http.upgrade == h2c\n"
				"txpri\n", http_cmds, hp, vl);
		b64_settings(hp, up);
		parse_string("stream 0 {\n"
				"txsettings\n"
				"rxsettings\n"
				"txsettings -ack\n"
				"rxsettings\n"
				"expect settings.ack == true"
			     "} -start\n", http_cmds, hp, vl);
	}
}

/* SECTION: client-server.spec.recv
 *
 * recv NUMBER
 *         Read NUMBER bytes from the connection.
 */

static void
cmd_http_recv(CMD_ARGS)
{
	struct http *hp;
	int i, n;
	char u[32];

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	AN(av[1]);
	AZ(av[2]);
	n = strtoul(av[1], NULL, 0);
	while (n > 0) {
		i = read(hp->fd, u, n > 32 ? 32 : n);
		if (i > 0)
			vtc_dump(hp->vl, 4, "recv", u, i);
		else
			vtc_log(hp->vl, hp->fatal, "recv() got %d (%s)", i,
			    strerror(errno));
		n -= i;
	}
}

/* SECTION: client-server.spec.send
 *
 * send STRING
 *         Push STRING on the connection.
 */

static void
cmd_http_send(CMD_ARGS)
{
	struct http *hp;
	int i;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	AN(av[1]);
	AZ(av[2]);
	vtc_dump(hp->vl, 4, "send", av[1], -1);
	i = write(hp->fd, av[1], strlen(av[1]));
	if (i != strlen(av[1]))
		vtc_log(hp->vl, hp->fatal, "Write error in http_send(): %s",
		    strerror(errno));
}

/* SECTION: client-server.spec.send_n
 *
 * send_n NUMBER STRING
 *         Write STRING on the socket NUMBER times.
 */

static void
cmd_http_send_n(CMD_ARGS)
{
	struct http *hp;
	int i, n, l;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	AN(av[1]);
	AN(av[2]);
	AZ(av[3]);
	n = strtoul(av[1], NULL, 0);
		vtc_dump(hp->vl, 4, "send_n", av[2], -1);
	l = strlen(av[2]);
	while (n--) {
		i = write(hp->fd, av[2], l);
		if (i != l)
			vtc_log(hp->vl, hp->fatal,
			    "Write error in http_send(): %s",
			    strerror(errno));
	}
}

/* SECTION: client-server.spec.send_urgent
 *
 * send_urgent STRING
 *         Send string as TCP OOB urgent data. You will never need this.
 */

static void
cmd_http_send_urgent(CMD_ARGS)
{
	struct http *hp;
	int i;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	AN(av[1]);
	AZ(av[2]);
	vtc_dump(hp->vl, 4, "send_urgent", av[1], -1);
	i = send(hp->fd, av[1], strlen(av[1]), MSG_OOB);
	if (i != strlen(av[1]))
		vtc_log(hp->vl, hp->fatal,
		    "Write error in http_send_urgent(): %s", strerror(errno));
}

/* SECTION: client-server.spec.sendhex
 *
 * sendhex STRING
 *         Send bytes as described by STRING. STRING should consist of hex pairs
 *         possibly separated by whitespace or newlines. For example:
 *         "0F EE a5    3df2".
 */

static void
cmd_http_sendhex(CMD_ARGS)
{
	struct vsb *vsb;
	struct http *hp;
	int j;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	AN(av[1]);
	AZ(av[2]);
	vsb = vtc_hex_to_bin(hp->vl, av[1]);
	assert(VSB_len(vsb) >= 0);
	vtc_hexdump(hp->vl, 4, "sendhex", VSB_data(vsb), VSB_len(vsb));
	j = write(hp->fd, VSB_data(vsb), VSB_len(vsb));
	assert(j == VSB_len(vsb));
	VSB_destroy(&vsb);
}

/* SECTION: client-server.spec.chunked
 *
 * chunked STRING
 *         Send STRING as chunked encoding.
 */

static void
cmd_http_chunked(CMD_ARGS)
{
	struct http *hp;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	AN(av[1]);
	AZ(av[2]);
	VSB_clear(hp->vsb);
	VSB_printf(hp->vsb, "%jx%s%s%s",
	    (uintmax_t)strlen(av[1]), nl, av[1], nl);
	http_write(hp, 4, "chunked");
}

/* SECTION: client-server.spec.chunkedlen
 *
 * chunkedlen NUMBER
 *         Do as ``chunked`` except that the string will be generated
 *         for you, with a length of NUMBER characters.
 */

static void
cmd_http_chunkedlen(CMD_ARGS)
{
	unsigned len;
	unsigned u, v;
	char buf[16384];
	struct http *hp;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	AN(av[1]);
	AZ(av[2]);
	VSB_clear(hp->vsb);

	len = atoi(av[1]);

	if (len == 0) {
		VSB_printf(hp->vsb, "0%s%s", nl, nl);
	} else {
		for (u = 0; u < sizeof buf; u++)
			buf[u] = (u & 7) + '0';

		VSB_printf(hp->vsb, "%x%s", len, nl);
		for (u = 0; u < len; u += v) {
			v = len - u;
			if (v > sizeof buf)
				v = sizeof buf;
			VSB_bcat(hp->vsb, buf, v);
		}
		VSB_printf(hp->vsb, "%s", nl);
	}
	http_write(hp, 4, "chunked");
}


/* SECTION: client-server.spec.timeout
 *
 * timeout NUMBER
 *         Set the TCP timeout for this entity.
 */

static void
cmd_http_timeout(CMD_ARGS)
{
	struct http *hp;
	double d;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	AN(av[1]);
	AZ(av[2]);
	d = VNUM(av[1]);
	if (isnan(d))
		vtc_fatal(vl, "timeout is not a number (%s)", av[1]);
	hp->timeout = (int)(d * 1000.0);
}

/* SECTION: client-server.spec.expect_close
 *
 * expect_close
 *	Reads from the connection, expecting nothing to read but an EOF.
 */
static void
cmd_http_expect_close(CMD_ARGS)
{
	struct http *hp;
	struct pollfd fds[1];
	char c;
	int i;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	AZ(av[1]);

	vtc_log(vl, 4, "Expecting close (fd = %d)", hp->fd);
	if (hp->h2)
		stop_h2(hp);
	while (1) {
		fds[0].fd = hp->fd;
		fds[0].events = POLLIN;
		fds[0].revents = 0;
		i = poll(fds, 1, hp->timeout);
		if (i < 0 && errno == EINTR)
			continue;
		if (i == 0)
			vtc_log(vl, hp->fatal, "Expected close: timeout");
		if (i != 1 || !(fds[0].revents & (POLLIN|POLLERR|POLLHUP)))
			vtc_log(vl, hp->fatal,
			    "Expected close: poll = %d, revents = 0x%x",
			    i, fds[0].revents);
		i = read(hp->fd, &c, 1);
		if (VTCP_Check(i))
			break;
		if (i == 1 && vct_islws(c))
			continue;
		vtc_log(vl, hp->fatal,
		    "Expecting close: read = %d, c = 0x%02x", i, c);
	}
	vtc_log(vl, 4, "fd=%d EOF, as expected", hp->fd);
}

/* SECTION: client-server.spec.close
 *
 * close (server only)
 *	Close the connection. Note that if operating in HTTP/2 mode no
 *	extra (GOAWAY) frame is sent, it's simply a TCP close.
 */
static void
cmd_http_close(CMD_ARGS)
{
	struct http *hp;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	ONLY_SERVER(hp, av);
	AZ(av[1]);
	assert(hp->sfd != NULL);
	assert(*hp->sfd >= 0);
	if (hp->h2)
		stop_h2(hp);
	VTCP_close(&hp->fd);
	vtc_log(vl, 4, "Closed");
}

/* SECTION: client-server.spec.accept
 *
 * accept (server only)
 *	Close the current connection, if any, and accept a new one. Note
 *	that this new connection is HTTP/1.x.
 */
static void
cmd_http_accept(CMD_ARGS)
{
	struct http *hp;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	ONLY_SERVER(hp, av);
	AZ(av[1]);
	assert(hp->sfd != NULL);
	assert(*hp->sfd >= 0);
	if (hp->h2)
		stop_h2(hp);
	if (hp->fd >= 0)
		VTCP_close(&hp->fd);
	vtc_log(vl, 4, "Accepting");
	hp->fd = accept(*hp->sfd, NULL, NULL);
	if (hp->fd < 0)
		vtc_log(vl, hp->fatal, "Accepted failed: %s", strerror(errno));
	vtc_log(vl, 3, "Accepted socket fd is %d", hp->fd);
}

/* SECTION: client-server.spec.loop
 *
 * loop NUMBER STRING
 *         Process STRING as a specification, NUMBER times.
 */

static void
cmd_http_loop(CMD_ARGS)
{
	struct http *hp;
	unsigned n, m;

	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	AN(av[1]);
	AN(av[2]);
	AZ(av[3]);
	n = strtoul(av[1], NULL, 0);
	for (m = 1 ; m <= n; m++) {
		vtc_log(vl, 4, "Loop #%u", m);
		parse_string(av[2], cmd, hp, vl);
	}
}

/* SECTION: client-server.spec.fatal
 *
 * fatal|non_fatal
 *         Control whether a failure of this entity should stop the test.
 */

static void
cmd_http_fatal(CMD_ARGS)
{
	struct http *hp;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);

	AZ(av[1]);
	if (!strcmp(av[0], "fatal"))
		hp->fatal = 0;
	else if (!strcmp(av[0], "non_fatal"))
		hp->fatal = -1;
	else
		vtc_fatal(vl, "XXX: fatal %s", cmd->name);
}

#define cmd_http_non_fatal cmd_http_fatal

static const char PREFACE[24] = {
	0x50, 0x52, 0x49, 0x20, 0x2a, 0x20, 0x48, 0x54,
	0x54, 0x50, 0x2f, 0x32, 0x2e, 0x30, 0x0d, 0x0a,
	0x0d, 0x0a, 0x53, 0x4d, 0x0d, 0x0a, 0x0d, 0x0a
};

/* SECTION: client-server.spec.txpri
 *
 * txpri (client only)
 *	Send an HTTP/2 preface ("PRI * HTTP/2.0\\r\\n\\r\\nSM\\r\\n\\r\\n")
 *	and set client to HTTP/2.
 */
static void
cmd_http_txpri(CMD_ARGS)
{
	size_t l;
	struct http *hp;
	(void)cmd;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	ONLY_CLIENT(hp, av);

	vtc_dump(hp->vl, 4, "txpri", PREFACE, sizeof(PREFACE));
	/* Dribble out the preface */
	l = write(hp->fd, PREFACE, 18);
	if (l != 18)
		vtc_log(vl, hp->fatal, "Write failed: (%zd vs %zd) %s",
		    l, sizeof(PREFACE), strerror(errno));
	usleep(10000);
	l = write(hp->fd, PREFACE + 18, sizeof(PREFACE) - 18);
	if (l != sizeof(PREFACE) - 18)
		vtc_log(vl, hp->fatal, "Write failed: (%zd vs %zd) %s",
		    l, sizeof(PREFACE), strerror(errno));

	start_h2(hp);
	AN(hp->h2);
}

/* SECTION: client-server.spec.rxpri
 *
 * rxpri (server only)
 *	Receive a preface. If valid set the server to HTTP/2, abort
 *	otherwise.
 */
static void
cmd_http_rxpri(CMD_ARGS)
{
	struct http *hp;
	(void)cmd;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	ONLY_SERVER(hp, av);

	hp->rx_p = hp->rx_b;
	if (!http_rxchar(hp, sizeof(PREFACE), 0))
		vtc_fatal(vl, "Couldn't retrieve connection preface");
	if (memcmp(hp->rx_b, PREFACE, sizeof(PREFACE)))
		vtc_fatal(vl, "Received invalid preface\n");
	start_h2(hp);
	AN(hp->h2);
}

/* SECTION: client-server.spec.settings
 *
 * settings -dectbl INT
 *	Force internal HTTP/2 settings to certain values. Currently only
 *	support setting the decoding table size.
 */
static void
cmd_http_settings(CMD_ARGS)
{
	uint32_t n;
	char *p;
	struct http *hp;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	(void)cmd;

	if (!hp->h2)
		vtc_fatal(hp->vl, "Only possible in H/2 mode");

	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);

	for (; *av != NULL; av++) {
		if (!strcmp(*av, "-dectbl")) {
			n = strtoul(av[1], &p, 0);
			if (*p != '\0')
				vtc_fatal(hp->vl, "-dectbl takes an integer as "
				    "argument (found %s)", av[1]);
			assert(HPK_ResizeTbl(hp->decctx, n) != hpk_err);
			av++;
		} else
			vtc_fatal(vl, "Unknown settings spec: %s\n", *av);
	}
}

static void
cmd_http_stream(CMD_ARGS)
{
	struct http *hp;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	if (!hp->h2) {
		vtc_log(hp->vl, 4, "Not in H/2 mode, do what's needed");
		if (hp->sfd)
			parse_string("rxpri", http_cmds, hp, vl);
		else
			parse_string("txpri", http_cmds, hp, vl);
		parse_string("stream 0 {\n"
				"txsettings\n"
				"rxsettings\n"
				"txsettings -ack\n"
				"rxsettings\n"
				"expect settings.ack == true"
			     "} -run\n", http_cmds, hp, vl);
	}
	cmd_stream(av, hp, cmd, vl);
}

/* SECTION: client-server.spec.write_body
 *
 * write_body STRING
 *	Write the body of a request or a response to a file. By using the
 *	shell command, higher-level checks on the body can be performed
 *	(eg. XML, JSON, ...) provided that such checks can be delegated
 *	to an external program.
 */
static void
cmd_http_write_body(CMD_ARGS)
{
	struct http *hp;

	(void)cmd;
	(void)vl;
	CAST_OBJ_NOTNULL(hp, priv, HTTP_MAGIC);
	AN(av[0]);
	AN(av[1]);
	AZ(av[2]);
	AZ(strcmp(av[0], "write_body"));
	if (VFIL_writefile(NULL, av[1], hp->body, hp->bodyl) != 0)
		vtc_fatal(hp->vl, "failed to write body: %s (%d)",
		    strerror(errno), errno);
}

/**********************************************************************
 * Execute HTTP specifications
 */

const struct cmds http_cmds[] = {
#define CMD_HTTP(n) { #n, cmd_http_##n },
	/* session */
	CMD_HTTP(accept)
	CMD_HTTP(close)
	CMD_HTTP(recv)
	CMD_HTTP(send)
	CMD_HTTP(send_n)
	CMD_HTTP(send_urgent)
	CMD_HTTP(sendhex)
	CMD_HTTP(timeout)

	/* spec */
	CMD_HTTP(fatal)
	CMD_HTTP(loop)
	CMD_HTTP(non_fatal)

	/* body */
	CMD_HTTP(gunzip)
	CMD_HTTP(write_body)

	/* HTTP/1.x */
	CMD_HTTP(chunked)
	CMD_HTTP(chunkedlen)
	CMD_HTTP(rxchunk)

	/* HTTP/2 */
	CMD_HTTP(stream)
	CMD_HTTP(settings)

	/* client */
	CMD_HTTP(rxresp)
	CMD_HTTP(rxrespbody)
	CMD_HTTP(rxresphdrs)
	CMD_HTTP(txpri)
	CMD_HTTP(txreq)

	/* server */
	CMD_HTTP(rxpri)
	CMD_HTTP(rxreq)
	CMD_HTTP(rxreqbody)
	CMD_HTTP(rxreqhdrs)
	CMD_HTTP(txresp)
	CMD_HTTP(upgrade)

	/* expect */
	CMD_HTTP(expect)
	CMD_HTTP(expect_close)
	CMD_HTTP(expect_pattern)
#undef CMD_HTTP
	{ NULL, NULL }
};

static void
http_process_cleanup(void *arg)
{
	struct http *hp = arg;

	if (hp->h2)
		stop_h2(hp);
	VSB_destroy(&hp->vsb);
	free(hp->rx_b);
	free(hp->rem_ip);
	free(hp->rem_port);
	free(hp->rem_path);
	FREE_OBJ(hp);
}

int
http_process(struct vtclog *vl, const char *spec, int sock, int *sfd,
    const char *addr, int rcvbuf)
{
	struct http *hp;
	int retval, oldbuf;
	socklen_t intlen = sizeof(int);

	(void)sfd;
	ALLOC_OBJ(hp, HTTP_MAGIC);
	AN(hp);
	hp->fd = sock;
	hp->timeout = vtc_maxdur * 1000 / 2;

	if (rcvbuf) {
		// XXX setsockopt() too late on SunOS
		// https://github.com/varnishcache/varnish-cache/pull/2980#issuecomment-486214661
		hp->rcvbuf = rcvbuf;

		oldbuf = 0;
		AZ(getsockopt(hp->fd, SOL_SOCKET, SO_RCVBUF, &oldbuf, &intlen));
		AZ(setsockopt(hp->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, intlen));
		AZ(getsockopt(hp->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &intlen));

		vtc_log(vl, 3, "-rcvbuf fd=%d old=%d new=%d actual=%d",
		    hp->fd, oldbuf, hp->rcvbuf, rcvbuf);
	}

	hp->nrxbuf = 2048*1024;
	hp->rx_b = malloc(hp->nrxbuf);
	AN(hp->rx_b);
	hp->rx_e = hp->rx_b + hp->nrxbuf;
	hp->rx_p = hp->rx_b;
	*hp->rx_p = '\0';

	hp->vsb = VSB_new_auto();
	AN(hp->vsb);

	hp->sfd = sfd;

	hp->rem_ip = malloc(VTCP_ADDRBUFSIZE);
	AN(hp->rem_ip);

	hp->rem_port = malloc(VTCP_PORTBUFSIZE);
	AN(hp->rem_port);

	hp->vl = vl;
	hp->gziplevel = 0;
	hp->gzipresidual = -1;

	if (*addr != '/') {
		VTCP_hisname(sock, hp->rem_ip, VTCP_ADDRBUFSIZE, hp->rem_port,
			     VTCP_PORTBUFSIZE);
		hp->rem_path = NULL;
	} else {
		strcpy(hp->rem_ip, "0.0.0.0");
		strcpy(hp->rem_port, "0");
		hp->rem_path = strdup(addr);
	}
	pthread_cleanup_push(http_process_cleanup, hp);
	parse_string(spec, http_cmds, hp, vl);
	retval = hp->fd;
	pthread_cleanup_pop(0);
	http_process_cleanup(hp);
	return (retval);
}

/**********************************************************************
 * Magic test routine
 *
 * This function brute-forces some short strings through gzip(9) to
 * find candidates for all possible 8 bit positions of the stopbit.
 *
 * Here is some good short output strings:
 *
 *	0 184 <e04c8d0fd604c>
 *	1 257 <1ea86e6cf31bf4ec3d7a86>
 *	2 106 <10>
 *	3 163 <a5e2e2e1c2e2>
 *	4 180 <71c5d18ec5d5d1>
 *	5 189 <39886d28a6d2988>
 *	6 118 <80000>
 *	7 151 <386811868>
 *
 */

#if 0
void xxx(void);

void
xxx(void)
{
	z_stream vz;
	int n;
	char ibuf[200];
	char obuf[200];
	int fl[8];
	int i, j;

	for (n = 0; n < 8; n++)
		fl[n] = 9999;

	memset(&vz, 0, sizeof vz);

	for (n = 0;  n < 999999999; n++) {
		*ibuf = 0;
		for (j = 0; j < 7; j++) {
			snprintf(strchr(ibuf, 0), 5, "%x",
			    (unsigned)VRND_RandomTestable() & 0xffff);
			vz.next_in = TRUST_ME(ibuf);
			vz.avail_in = strlen(ibuf);
			vz.next_out = TRUST_ME(obuf);
			vz.avail_out = sizeof obuf;
			assert(Z_OK == deflateInit2(&vz,
			    9, Z_DEFLATED, 31, 9, Z_DEFAULT_STRATEGY));
			assert(Z_STREAM_END == deflate(&vz, Z_FINISH));
			i = vz.stop_bit & 7;
			if (fl[i] > strlen(ibuf)) {
				printf("%d %jd <%s>\n", i, vz.stop_bit, ibuf);
				fl[i] = strlen(ibuf);
			}
			assert(Z_OK == deflateEnd(&vz));
		}
	}

	printf("FOO\n");
}
#endif
