/***********************************************************************
*
* sync-pppd.c
*
* An LNS handler which starts pppd attached to a PTY in
* synchronous mode.
*
* Copyright (C) 2002 by Roaring Penguin Software Inc.
*
* This software may be distributed under the terms of the GNU General
* Public License, Version 2, or (at your option) any later version.
*
* LIC: GPL
*
***********************************************************************/

static char const RCSID[] =
"$Id$";

#include "l2tp.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_ether.h>
#include <linux/if_pppol2tp.h>
#include <linux/if_pppox.h>

#define HANDLER_NAME "sync-pppd"

#define DEFAULT_PPPD_PATH "/usr/sbin/pppd"

#define MAX_FDS 256

extern int pty_get(int *mfp, int *sfp);
static int establish_tunnel(l2tp_tunnel *tun);
static void close_tunnel(l2tp_tunnel *tun);
static int tunnel_socket(l2tp_tunnel *tun);
static int establish_session(l2tp_session *ses);
static void close_session(l2tp_session *ses, char const *reason, int may_reestablish);
static void handle_frame(l2tp_session *ses, unsigned char *buf, size_t len);

/* Options for invoking pppd */
#define MAX_OPTS 64
static char *pppd_lns_options[MAX_OPTS+1];
static char *pppd_lac_options[MAX_OPTS+1];
static int num_pppd_lns_options = 0;
static int num_pppd_lac_options = 0;
static int use_unit_option = 0;
static int kernel_mode = 1;
static char *pppd_path = NULL;

#define PUSH_LNS_OPT(x) pppd_lns_options[num_pppd_lns_options++] = (x)
#define PUSH_LAC_OPT(x) pppd_lac_options[num_pppd_lac_options++] = (x)

/* Our call ops */
static l2tp_call_ops my_ops = {
    establish_session,
    close_session,
    handle_frame,
    establish_tunnel,
    close_tunnel,
    tunnel_socket
};

/* Tunnel private info */
struct master {
    EventSelector *es;		/* Event selector */
    l2tp_tunnel *tun;		/* L2TP tunnel we're hooked to */
    int fd;			/* Tunnel UDP socket for event-handler loop */
    EventHandler *event;	/* Event handler */
};

/* The slave process */
struct slave {
    EventSelector *es;		/* Event selector */
    l2tp_session *ses;		/* L2TP session we're hooked to */
    pid_t pid;			/* PID of child PPPD process */
    int fd;			/* File descriptor for event-handler loop */
    EventHandler *event;	/* Event handler */
};

static int handle_lac_opts(EventSelector *es, l2tp_opt_descriptor *desc, char const *value);
static int handle_lns_opts(EventSelector *es, l2tp_opt_descriptor *desc, char const *value);

/* Options */
static l2tp_opt_descriptor my_opts[] = {
    /*  name               type                 addr */
    { "lac-pppd-opts",     OPT_TYPE_CALLFUNC,   (void *) handle_lac_opts},
    { "lns-pppd-opts",     OPT_TYPE_CALLFUNC,   (void *) handle_lns_opts},
    { "set-ppp-if-name",   OPT_TYPE_BOOL,       &use_unit_option},
    { "kernel-mode",       OPT_TYPE_BOOL,       &kernel_mode},
    { "pppd-path",         OPT_TYPE_STRING,     &pppd_path},
    { NULL,                OPT_TYPE_BOOL,       NULL }
};

static int
process_option(EventSelector *es, char const *name, char const *value)
{
    if (!strcmp(name, "*begin*")) return 0;
    if (!strcmp(name, "*end*")) return 0;
    return l2tp_option_set(es, name, value, my_opts);
}

static option_handler my_option_handler = {
    NULL, HANDLER_NAME, process_option
};

static int
handle_lac_opts(EventSelector *es,
		l2tp_opt_descriptor *desc, char const *value)
{
    char word[512];
    while (value && *value) {
	value = l2tp_chomp_word(value, word);
	if (!word[0]) break;
	if (num_pppd_lac_options < MAX_OPTS) {
	    char *x = strdup(word);
	    if (x) PUSH_LAC_OPT(x);
	    pppd_lac_options[num_pppd_lac_options] = NULL;
	} else {
	    break;
	}
    }
    return 0;
}

static int
handle_lns_opts(EventSelector *es,
		l2tp_opt_descriptor *desc, char const *value)
{
    char word[512];
    while (value && *value) {
	value = l2tp_chomp_word(value, word);
	if (!word[0]) break;
	if (num_pppd_lns_options < MAX_OPTS) {
	    char *x = strdup(word);
	    if (x) PUSH_LNS_OPT(x);
	    pppd_lns_options[num_pppd_lns_options] = NULL;
	} else {
	    break;
	}
    }
    return 0;
}

/**********************************************************************
* %FUNCTION: handle_frame
* %ARGUMENTS:
*  ses -- l2tp session
*  buf -- received PPP frame
*  len -- length of frame
* %RETURNS:
*  Nothing
* %DESCRIPTION:
*  Shoots the frame to PPP's pty
***********************************************************************/
static void
handle_frame(l2tp_session *ses,
	     unsigned char *buf,
	     size_t len)
{
    struct slave *sl = ses->private;
    int n;

    if (!sl) return;
    if (kernel_mode) {
	l2tp_set_errmsg("Attempt to write %d bytes to kernel tunnel fd.", len);
	return;
    }

    /* Add framing bytes */
    *--buf = 0x03;
    *--buf = 0xFF;
    len += 2;

    /* TODO: Add error checking */
    if (sl->fd < 0) {
        l2tp_set_errmsg("Attempt to write %d bytes to non existent fd.", len);
    } else n = write(sl->fd, buf, len);
}

/**********************************************************************
* %FUNCTION: close_session
* %ARGUMENTS:
*  ses -- L2TP session
*  reason -- reason why session is closing
* %RETURNS:
*  Nothing
* %DESCRIPTION:
*  Kills pppd.
***********************************************************************/
static void
close_session(l2tp_session *ses, char const *reason, int may_reestablish)
{
    l2tp_tunnel *tunnel = ses->tunnel;
    struct slave *sl = ses->private;
    if (!sl) return;

    /* Detach slave */
    ses->private = NULL;
    sl->ses = NULL;

    kill(sl->pid, SIGTERM);
    if (sl->fd >= 0) close(sl->fd);
    sl->fd = -1;
    if (sl->event) Event_DelHandler(sl->es, sl->event);
    sl->event = NULL;

    /* Re-establish session if desired */
    if (may_reestablish && tunnel->peer->persist && 
        (tunnel->peer->maxfail == 0 || tunnel->peer->fail++ < tunnel->peer->maxfail)) {
        struct timeval t;

        t.tv_sec = tunnel->peer->holdoff;
        t.tv_usec = 0;
        Event_AddTimerHandler(tunnel->es, t, l2tp_tunnel_reestablish, tunnel->peer);
    }
}

/**********************************************************************
* %FUNCTION: slave_exited
* %ARGUMENTS:
*  pid -- PID of exiting slave
*  status -- exit status of slave
*  data -- the slave structure
* %RETURNS:
*  Nothing
* %DESCRIPTION:
*  Handles an exiting slave
***********************************************************************/
static void
slave_exited(pid_t pid, int status, void *data)
{
    l2tp_session *ses;
    struct slave *sl = (struct slave *) data;
    if (!sl) return;

    ses = sl->ses;

    if (sl->fd >= 0) close(sl->fd);
    if (sl->event) Event_DelHandler(sl->es, sl->event);
    sl->fd = -1;
    sl->event = NULL;

    if (ses) {
        l2tp_tunnel *tunnel = ses->tunnel;

        /* Re-establish session if desired */
        if (tunnel->peer->persist) {
            struct timeval t;

            t.tv_sec = tunnel->peer->holdoff;
            t.tv_usec = 0;
            Event_AddTimerHandler(tunnel->es, t, l2tp_tunnel_reestablish, tunnel->peer);
        }
        
	ses->private = NULL;
	l2tp_session_send_CDN(ses, RESULT_GENERAL_REQUEST, 0,
			      "pppd process exited");
    }
    free(sl);
}

/**********************************************************************
* %FUNCTION: readable
* %ARGUMENTS:
*  es -- event selector
*  fd -- file descriptor
*  flags -- we ignore
*  data -- the L2TP session
* %RETURNS:
*  Nothing
* %DESCRIPTION:
*  Handles readability on PTY; shoots PPP frame over tunnel
***********************************************************************/
static void
readable(EventSelector *es, int fd, unsigned int flags, void *data)
{
    unsigned char buf[4096+EXTRA_HEADER_ROOM];
    int n;
    l2tp_session *ses = (l2tp_session *) data;
    int iters = 5;

    /* It seems to be better to read in a loop than to go
       back to select loop.  However, don't loop forever, or
       we could have a DoS potential */
    while(iters--) {
	/* EXTRA_HEADER_ROOM bytes extra space for l2tp header */
	n = read(fd, buf+EXTRA_HEADER_ROOM, sizeof(buf)-EXTRA_HEADER_ROOM);

	/* TODO: Check this.... */
	if (n <= 2) return;

	if (!ses) continue;

	/* Chop off framing bytes */
	l2tp_dgram_send_ppp_frame(ses, buf+EXTRA_HEADER_ROOM+2, n-2);
    }
}

/**********************************************************************
* %FUNCTION: establish_session
* %ARGUMENTS:
*  ses -- the L2TP session
* %RETURNS:
*  0 if session could be established, -1 otherwise.
* %DESCRIPTION:
*  Forks a pppd process and connects fd to pty
***********************************************************************/
static int
establish_session(l2tp_session *ses)
{
    EventSelector *es = ses->tunnel->es;
    struct master *tun = ses->tunnel->private;
    struct slave *sl = malloc(sizeof(struct slave));
    struct sockaddr_pppol2tp sax;
    int m_pty = -1, s_pty = -1;
    int i, flags;
    char unit[32], fdstr[10];
    char tidstr[10], sidstr[10];
    pid_t pid;

    ses->private = NULL;
    if (!sl) return -1;
    sl->ses = ses;
    sl->es = es;

    /* Get pty */
    if (kernel_mode) {
	if (!tun) goto err;

	sax.sa_family = AF_PPPOX;
	sax.sa_protocol = PX_PROTO_OL2TP;
	sax.pppol2tp.pid = 0;
	sax.pppol2tp.fd = tun->fd;
	sax.pppol2tp.addr.sin_family = AF_INET;
	sax.pppol2tp.addr.sin_addr.s_addr = ses->tunnel->peer_addr.sin_addr.s_addr;
	sax.pppol2tp.addr.sin_port = ses->tunnel->peer_addr.sin_port;
	sax.pppol2tp.s_tunnel  = ses->tunnel->my_id;
	sax.pppol2tp.s_session = ses->my_id;
	sax.pppol2tp.d_tunnel  = ses->tunnel->assigned_id;
	sax.pppol2tp.d_session = ses->assigned_id;

	s_pty = socket(AF_PPPOX, SOCK_DGRAM, PX_PROTO_OL2TP);
	if (s_pty < 0) {
	    l2tp_set_errmsg("Unable to allocate PPPoL2TP socket.");
	    goto err;
	}

	flags = fcntl(s_pty, F_GETFL);
	if (flags < 0 || fcntl(s_pty, F_SETFL, flags | O_NONBLOCK) < 0) {
	    l2tp_set_errmsg("Unable to set PPPoL2TP socket nonblock.");
	    goto err;
	}

	if (connect(s_pty, (struct sockaddr *)&sax, sizeof(sax)) < 0) {
	    l2tp_set_errmsg("Unable to connect PPPoL2TP socket.");
	    goto err;
	}

	snprintf (fdstr, sizeof(fdstr), "%d", s_pty);
	snprintf (tidstr, sizeof(tidstr), "%d", ses->tunnel->my_id);
	snprintf (sidstr, sizeof(sidstr), "%d", ses->my_id);
    } else {
	if (pty_get(&m_pty, &s_pty) < 0) {
	    goto err;
	}
	flags = fcntl(m_pty, F_GETFD);
	if (flags < 0 || fcntl(m_pty, F_SETFD, flags | FD_CLOEXEC) < 0) {
	    l2tp_set_errmsg("Unable to set master socket FD_CLOEXEC.");
	    goto err;
	}
    }

    /* Fork */
    pid = fork();
    if (pid == (pid_t) -1) {
	goto err;
    }

    /* In the parent */
    if (pid) {
	sl->fd = m_pty;
	sl->pid = pid;

	/* Set up handler for when pppd exits */
	Event_HandleChildExit(es, pid, slave_exited, sl);

	/* Close the slave tty */
	close(s_pty);

	if (!kernel_mode) {
	    /* Set slave FD non-blocking */
	    flags = fcntl(m_pty, F_GETFL);
	    if (flags < 0 || fcntl(m_pty, F_SETFL, flags | O_NONBLOCK) < 0) {
		l2tp_set_errmsg("Unable to set master socket nonblock.");
		goto err;
	    }
	    /* Handle readability on slave end */
	    sl->event = Event_AddHandler(es, m_pty, EVENT_FLAG_READABLE,
			 readable, ses);
	} else
	    sl->event = NULL;

	ses->private = sl;
	return 0;

err:
	if (m_pty >= 0) close(m_pty);
	if (s_pty >= 0) close(s_pty);
	free(sl);
	return -1;
    }

    /* In the child.  Exec pppd */
    /* Close all file descriptors except s_pty */
    for (i=0; i<MAX_FDS; i++) {
	if (i != s_pty) close(i);
    }

    /* Dup s_pty onto stdin and stdout */
    if (!kernel_mode) {
    	dup2(s_pty, 0);
    	dup2(s_pty, 1);
        if (s_pty > 1) close(s_pty);
    }

    /* Create unit */
    sprintf(unit, "%d", (int) getpid());

    if (ses->we_are_lac) {
        char **lac_opt;

	/* Push a unit option */
	if (use_unit_option && num_pppd_lac_options <= MAX_OPTS-2) {
	    PUSH_LAC_OPT("unit");
	    PUSH_LAC_OPT(unit);
	}
	/* Push plugin options */
	if (kernel_mode && num_pppd_lac_options <= MAX_OPTS-4) {
	    PUSH_LAC_OPT("plugin");
	    PUSH_LAC_OPT("pppol2tp.so");
	    PUSH_LAC_OPT("pppol2tp");
	    PUSH_LAC_OPT(fdstr);
	}
        /* push peer specific options */
        lac_opt = ses->tunnel->peer->lac_options;
        while (*lac_opt) {
            if (num_pppd_lac_options <= MAX_OPTS-1) {
		PUSH_LAC_OPT(*lac_opt);
		++lac_opt;
	    } else {
		break;
	    }
        }
	if (pppd_path) {
	    execv(pppd_path, pppd_lac_options);
	} else {
	    execv(DEFAULT_PPPD_PATH, pppd_lac_options);
	}
    } else {
        char **lns_opt;

	/* Push a unit option */
	if (use_unit_option && num_pppd_lns_options <= MAX_OPTS-2) {
	    PUSH_LNS_OPT("unit");
	    PUSH_LNS_OPT(unit);
	}
	/* Push plugin options */
	if (kernel_mode && num_pppd_lac_options <= MAX_OPTS-9) {
	    PUSH_LNS_OPT("plugin");
	    PUSH_LNS_OPT("pppol2tp.so");
	    PUSH_LNS_OPT("pppol2tp");
	    PUSH_LNS_OPT(fdstr);
	    PUSH_LNS_OPT("pppol2tp_lns_mode");
	    PUSH_LNS_OPT("pppol2tp_tunnel_id");
	    PUSH_LNS_OPT(tidstr);
	    PUSH_LNS_OPT("pppol2tp_session_id");
	    PUSH_LNS_OPT(sidstr);
	}
        /* push peer specific options */
        lns_opt = ses->tunnel->peer->lns_options;
        while (*lns_opt) {
            if (num_pppd_lns_options <= MAX_OPTS-1) {
		PUSH_LNS_OPT(*lns_opt);
		++lns_opt;
	    } else {
		break;
	    }
        }
	if (pppd_path) {
	    execv(pppd_path, pppd_lns_options);
	} else {
	    execv(DEFAULT_PPPD_PATH, pppd_lns_options);
	}
    }

    /* Doh.. execl failed */
    _exit(1);
}

/**********************************************************************
* %FUNCTION: establish_tunnel
* %ARGUMENTS:
*  tunnel -- the L2TP tunnel
* %RETURNS:
*  0 if tunnel could be established, -1 otherwise.
* %DESCRIPTION:
*  Opens tunnel-specific fds
***********************************************************************/
static int establish_tunnel(l2tp_tunnel *tunnel)
{
    EventSelector *es = tunnel->es;
    struct master *tun;
    struct sockaddr_in addr;
    struct sockaddr_pppol2tp sax;
    int fd = -1, m_fd = -1;
    int flags;

    tunnel->private = NULL;
    if (!kernel_mode) return 0;

    tun = malloc(sizeof(struct master));
    if (!tun) return -1;
    tun->tun = tunnel;

    fd = socket(PF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
	l2tp_set_errmsg("Unable to allocate tunnel UDP socket.");
	goto err;
    }

    flags = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof(flags));
    setsockopt(fd, SOL_SOCKET, SO_NO_CHECK, &flags, sizeof(flags));

    addr.sin_family = AF_INET;
    addr.sin_addr = Settings.listen_addr;
    addr.sin_port = htons((uint16_t) Settings.listen_port);
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
	l2tp_set_errmsg("Unable to bind tunnel UDP socket.");
	goto err;
    }

    addr = tunnel->peer_addr;
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
	l2tp_set_errmsg("Unable to connect tunnel UDP socket.");
	goto err;
    }

    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
	l2tp_set_errmsg("Unable to set tunnel UDP socket nonblock.");
	goto err;
    }

    sax.sa_family = AF_PPPOX;
    sax.sa_protocol = PX_PROTO_OL2TP;
    sax.pppol2tp.pid = 0;
    sax.pppol2tp.fd = fd;
    sax.pppol2tp.addr.sin_family = AF_INET;
    sax.pppol2tp.addr.sin_addr.s_addr = tunnel->peer_addr.sin_addr.s_addr;
    sax.pppol2tp.addr.sin_port = tunnel->peer_addr.sin_port;
    sax.pppol2tp.s_tunnel  = tunnel->my_id;
    sax.pppol2tp.s_session = 0;
    sax.pppol2tp.d_tunnel  = tunnel->assigned_id;
    sax.pppol2tp.d_session = 0;

    m_fd = socket(AF_PPPOX, SOCK_DGRAM, PX_PROTO_OL2TP);
    if (m_fd < 0) {
	l2tp_set_errmsg("Unable to allocate tunnel PPPoL2TP socket.");
	goto err;
    }

    flags = fcntl(m_fd, F_GETFL);
    if (flags < 0 || fcntl(m_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
	l2tp_set_errmsg("Unable to set tunnel PPPoL2TP socket nonblock.");
	goto err;
    }

    if (connect(m_fd, (struct sockaddr *)&sax, sizeof(sax)) < 0) {
	l2tp_set_errmsg("Unable to connect tunnel PPPoL2TP socket.");
	goto err;
    }

    /* Yeah, we don't need to keep it open */
    close(m_fd);

    tun->es = es;
    tun->fd = fd;
    tun->event = Event_AddHandler(es, fd, EVENT_FLAG_READABLE,
				  network_readable, NULL);
    tunnel->private = tun;

    return 0;

err:
    if (fd >= 0) close(fd);
    if (m_fd >= 0) close(m_fd);
    if (tun) free(tun);
    return -1;
}

/**********************************************************************
* %FUNCTION: close_tunnel
* %ARGUMENTS:
*  tunnel -- L2TP tunnel
* %RETURNS:
*  Nothing
* %DESCRIPTION:
*  Handles tunnel closing
***********************************************************************/
static void close_tunnel(l2tp_tunnel *tunnel)
{
    struct master *tun = tunnel->private;
    if (!kernel_mode || !tun) return;

    /* Detach master */
    tunnel->private = NULL;

    if (tun->fd >= 0) close(tun->fd);
    if (tun->event) Event_DelHandler(tun->es, tun->event);

    free(tun);
}

/**********************************************************************
* %FUNCTION: tunnel_socket
* %ARGUMENTS:
*  tunnel -- L2TP tunnel
* %RETURNS:
*  fd of tunnel
* %DESCRIPTION:
*  Returns tunnel fd if available, -1 otherwise
***********************************************************************/
static int tunnel_socket(l2tp_tunnel *tunnel)
{
    struct master *tun = tunnel->private;
    if (!kernel_mode || !tun) return -1;

    return tun->fd;
}

static l2tp_lns_handler my_lns_handler = {
    NULL,
    HANDLER_NAME,
    &my_ops
};

static l2tp_lac_handler my_lac_handler = {
    NULL,
    HANDLER_NAME,
    &my_ops
};

void
handler_init(EventSelector *es)
{
    l2tp_session_register_lns_handler(&my_lns_handler);
    l2tp_session_register_lac_handler(&my_lac_handler);
    l2tp_option_register_section(&my_option_handler);

    PUSH_LNS_OPT("pppd");
    PUSH_LNS_OPT("sync");
    PUSH_LNS_OPT("nodetach");
    PUSH_LNS_OPT("noaccomp");
    PUSH_LNS_OPT("nobsdcomp");
    PUSH_LNS_OPT("nodeflate");
    PUSH_LNS_OPT("nopcomp");
    PUSH_LNS_OPT("novj");
    PUSH_LNS_OPT("novjccomp");
#if 0
    PUSH_LNS_OPT("logfile");
    PUSH_LNS_OPT("/dev/null");
    PUSH_LNS_OPT("nolog");
#endif
    pppd_lns_options[num_pppd_lns_options] = NULL;

    PUSH_LAC_OPT("pppd");
    PUSH_LAC_OPT("sync");
    PUSH_LAC_OPT("nodetach");
    PUSH_LAC_OPT("noaccomp");
    PUSH_LAC_OPT("nobsdcomp");
    PUSH_LAC_OPT("nodeflate");
    PUSH_LAC_OPT("nopcomp");
    PUSH_LAC_OPT("novj");
    PUSH_LAC_OPT("novjccomp");
#if 0
    PUSH_LAC_OPT("logfile");
    PUSH_LAC_OPT("/dev/null");
    PUSH_LAC_OPT("nolog");
#endif
    pppd_lac_options[num_pppd_lac_options] = NULL;
}

