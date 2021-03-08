
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <stdalign.h>
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <sys/sysinfo.h>
#include <teavpn2/base.h>
#include <teavpn2/net/iface.h>
#include <teavpn2/client/tcp.h>
#include <teavpn2/net/tcp_pkt.h>


#define EPOLL_IN_EVT	(EPOLLIN | EPOLLPRI)


struct cli_tcp_state {
	pid_t			pid;		/* Main process PID           */
	int			epl_fd;		/* Epoll fd                   */
	int			net_fd;		/* Main TCP socket fd         */
	int			tun_fd;		/* TUN/TAP fd                 */
	bool			stop;		/* Stop the event loop?       */
	bool			reconn;		/* Reconnect if conn dropped? */
	uint8_t			reconn_c;	/* Reconnect count            */
	struct_pad(0, 5);
	struct cli_cfg		*cfg;		/* Config                     */
	uint32_t		send_c;		/* Number of send()           */
	uint32_t		recv_c;		/* Number of recv()           */
	size_t			recv_s;		/* Active bytes in recv_buf   */
	tsrv_pkt		recv_buf;	/* Server packet from recv()  */
	tcli_pkt		send_buf;	/* Client packet to send()    */
};


static struct cli_tcp_state *g_state;


static void interrupt_handler(int sig)
{
	struct cli_tcp_state *state = g_state;

	state->stop = true;
	putchar('\n');
	pr_notice("Signal %d (%s) has been caught", sig, strsignal(sig));
}


static int init_state(struct cli_tcp_state *state)
{
	state->pid      = getpid();
	state->epl_fd   = -1;
	state->net_fd   = -1;
	state->tun_fd   = -1;
	state->stop     = false;
	state->reconn   = true;
	state->reconn_c = 0;
	state->send_c   = 0;
	state->recv_c   = 0;
	state->recv_s   = 0;

	prl_notice(0, "My PID is %d", state->pid);

	return 0;
}


static int init_iface(struct cli_tcp_state *state)
{
	int fd;
	struct cli_iface_cfg *j = &state->cfg->iface;

	prl_notice(0, "Creating virtual network interface: \"%s\"...", j->dev);
	fd = tun_alloc(j->dev, IFF_TUN);
	if (fd < 0)
		return -1;
	if (fd_set_nonblock(fd) < 0)
		goto out_err;

	state->tun_fd = fd;
	return 0;
out_err:
	close(fd);
	return -1;
}


static int socket_setup(int fd, struct cli_cfg *cfg)
{
	int rv;
	int err;
	int y;
	socklen_t len = sizeof(y);
	const void *pv = (const void *)&y;

	y = 1;
	rv = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, pv, len);
	if (unlikely(rv < 0))
		goto out_err;

	y = 1;
	rv = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, pv, len);
	if (unlikely(rv < 0))
		goto out_err;

	y = 0;
	rv = setsockopt(fd, SOL_SOCKET, SO_INCOMING_CPU, pv, len);
	if (unlikely(rv < 0))
		goto out_err;

	y = 1024 * 1024 * 2;
	rv = setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, pv, len);
	if (unlikely(rv < 0))
		goto out_err;

	y = 1024 * 1024 * 2;
	rv = setsockopt(fd, SOL_SOCKET, SO_SNDBUFFORCE, pv, len);
	if (unlikely(rv < 0))
		goto out_err;

	y = 30000;
	rv = setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, pv, len);
	if (unlikely(rv < 0))
		goto out_err;

	/*
	 * TODO: Utilize `cfg` to set some socket options from config
	 */
	(void)cfg;
	return rv;

out_err:
	err = errno;
	pr_error("setsockopt(): " PRERF, PREAR(err));
	return rv;
}


static int init_socket(struct cli_tcp_state *state)
{
	int fd;
	int err;
	int retval;
	struct sockaddr_in addr;
	struct cli_sock_cfg *sock = &state->cfg->sock;
	char *server_addr = sock->server_addr;
	uint16_t server_port = sock->server_port;
	socklen_t addrlen = sizeof(struct sockaddr_in);

	prl_notice(0, "Creating TCP socket...");
	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
	if (unlikely(fd < 0)) {
		err = errno;
		retval = -err;
		pr_error("socket(): " PRERF, PREAR(err));
		goto out_err;
	}

	prl_notice(0, "Setting up socket file descriptor...");
	retval = socket_setup(fd, state->cfg);
	if (unlikely(retval < 0))
		goto out_err;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(server_port);
	if (!inet_pton(AF_INET, server_addr, &addr.sin_addr)) {
		err = EINVAL;
		retval = -err;
		pr_error("inet_pton(%s): " PRERF, server_addr, PREAR(err));
		goto out_err;
	}

	prl_notice(0, "Connecting to %s:%d...", server_addr, server_port);
again:
	retval = connect(fd, &addr, addrlen);
	if (retval < 0) {
		err = errno;
		if ((err == EINPROGRESS) || (err == EALREADY)) {
			usleep(1000);
			goto again;
		}

		retval = -err;
		pr_error("connect(): " PRERF, PREAR(err));
		goto out_err;
	}

	state->net_fd = fd;
	prl_notice(0, "Connection established!");
	return 0;

out_err:
	if (fd > 0)
		close(fd);
	return retval;
}


static int epoll_add(int epl_fd, int fd, uint32_t events)
{
	int err;
	struct epoll_event event;

	/* Shut the valgrind up! */
	memset(&event, 0, sizeof(struct epoll_event));

	event.events = events;
	event.data.fd = fd;
	if (unlikely(epoll_ctl(epl_fd, EPOLL_CTL_ADD, fd, &event) < 0)) {
		err = errno;
		pr_err("epoll_ctl(EPOLL_CTL_ADD): " PRERF, PREAR(err));
		return -1;
	}
	return 0;
}


static int init_epoll(struct cli_tcp_state *state)
{
	int err;
	int ret;
	int epl_fd = -1;
	int tun_fd = state->tun_fd;
	int net_fd = state->net_fd;

	prl_notice(0, "Initializing epoll fd...");
	epl_fd = epoll_create(3);
	if (unlikely(epl_fd < 0))
		goto out_create_err;

	ret = epoll_add(epl_fd, tun_fd, EPOLL_IN_EVT);
	if (unlikely(ret < 0))
		goto out_err;

	ret = epoll_add(epl_fd, net_fd, EPOLL_IN_EVT);
	if (unlikely(ret < 0))
		goto out_err;

	state->epl_fd = epl_fd;
	return 0;

out_create_err:
	err = errno;
	pr_err("epoll_create(): " PRERF, PREAR(err));
out_err:
	if (epl_fd > 0)
		close(epl_fd);
	return -1;
}


static ssize_t send_to_server(struct cli_tcp_state *state, tcli_pkt *cli_pkt,
			      size_t len)
{
	int err;
	int net_fd = state->net_fd;
	ssize_t send_ret;

	state->send_c++;

	send_ret = send(net_fd, cli_pkt, len, 0);
	if (unlikely(send_ret < 0)) {
		err = errno;
		if (err == EAGAIN) {
			/*
			 * TODO: Handle pending buffer
			 *
			 * For now, let it fallthrough to error.
			 */
		}

		pr_err("send(fd=%d)" PRERF, net_fd, PREAR(err));
		return -1;
	}

	prl_notice(5, "[%10" PRIu32 "] send(fd=%d) %ld bytes to server",
		   state->send_c, net_fd, send_ret);

	return send_ret;
}


static int send_hello(struct cli_tcp_state *state)
{
	size_t send_len;
	uint16_t data_len;
	tcli_pkt *cli_pkt;
	struct tcli_hello_pkt *hlo_pkt;

	prl_notice(2, "Sending hello packet to server...");

	cli_pkt = &state->send_buf;
	hlo_pkt = &cli_pkt->hello_pkt;

	/*
	 * Tell the server what my version is.
	 */
	memset(&hlo_pkt->v, 0, sizeof(hlo_pkt->v));
	hlo_pkt->v.ver       = VERSION;
	hlo_pkt->v.patch_lvl = PATCHLEVEL;
	hlo_pkt->v.sub_lvl   = SUBLEVEL;
	strncpy(hlo_pkt->v.extra, EXTRAVERSION, sizeof(hlo_pkt->v.extra) - 1);
	hlo_pkt->v.extra[sizeof(hlo_pkt->v.extra) - 1] = '\0';

	data_len        = sizeof(struct tcli_hello_pkt);
	cli_pkt->type   = TCLI_PKT_HELLO;
	cli_pkt->npad   = 0;
	cli_pkt->length = htons(data_len);
	send_len        = TCLI_PKT_MIN_L + data_len;

	return (send_to_server(state, cli_pkt, send_len) > 0) ? 0 : -1;
}


static int handle_recv_server(int net_fd, struct cli_tcp_state *state)
{
	int err;
	size_t recv_s;
	char *recv_buf;
	size_t recv_len;
	ssize_t recv_ret;

	recv_s   = state->recv_s;
	recv_len = TCLI_PKT_RECV_L - recv_s;
	recv_buf = state->srv_pkt.raw_buf;


	return 0;
}


static int handle_event(struct cli_tcp_state *state, struct epoll_event *event)
{
	int fd;
	bool is_err;
	uint32_t revents;
	const uint32_t errev = EPOLLERR | EPOLLHUP;

	fd      = event->data.fd;
	revents = event->events;
	is_err  = ((revents & errev) != 0);

	if (fd == state->net_fd) {
		return handle_recv_server(fd, state);
	} else
	if (fd == state->tun_fd) {
		
	}

	return 0;
}


static int event_loop(struct cli_tcp_state *state)
{
	int err;
	int retval = 0;
	int maxevents = 32;

	int epl_ret;
	int epl_fd = state->epl_fd;
	struct epoll_event events[2];


	while (likely(!state->stop)) {
		epl_ret = epoll_wait(epl_fd, events, maxevents, 3000);
		if (unlikely(epl_ret == 0)) {
			/*
			 * epoll reached timeout.
			 *
			 * TODO: Do something meaningful here...
			 * Maybe keep alive ping to clients?
			 */
			continue;
		}


		if (unlikely(epl_ret < 0)) {
			err = errno;
			if (err == EINTR) {
				retval = 0;
				prl_notice(0, "Interrupted!");
				continue;
			}

			retval = -err;
			pr_error("epoll_wait(): " PRERF, PREAR(err));
			break;
		}

		for (int i = 0; likely(i < epl_ret); i++) {
			retval = handle_event(state, &events[i]);
			if (unlikely(retval < 0))
				goto out;
		}
	}

out:
	return retval;
}


static void destroy_state(struct cli_tcp_state *state)
{
	int epl_fd = state->epl_fd;
	int tun_fd = state->tun_fd;
	int net_fd = state->net_fd;

	if (likely(tun_fd != -1)) {
		prl_notice(0, "Closing state->tun_fd (%d)", tun_fd);
		close(tun_fd);
	}

	if (likely(net_fd != -1)) {
		prl_notice(0, "Closing state->net_fd (%d)", net_fd);
		close(net_fd);
	}

	if (likely(epl_fd != -1)) {
		prl_notice(0, "Closing state->epl_fd (%d)", epl_fd);
		close(epl_fd);
	}
}


int teavpn_client_tcp_handler(struct cli_cfg *cfg)
{
	int retval = 0;
	struct cli_tcp_state state;

	/* Shut the valgrind up! */
	memset(&state, 0, sizeof(struct cli_tcp_state));

	state.cfg = cfg;
	g_state = &state;
	signal(SIGHUP, interrupt_handler);
	signal(SIGINT, interrupt_handler);
	signal(SIGPIPE, interrupt_handler);
	signal(SIGTERM, interrupt_handler);
	signal(SIGQUIT, interrupt_handler);

	retval = init_state(&state);
	if (unlikely(retval < 0))
		goto out;
	retval = init_iface(&state);
	if (retval < 0)
		goto out;
	retval = init_socket(&state);
	if (unlikely(retval < 0))
		goto out;
	retval = init_epoll(&state);
	if (unlikely(retval < 0))
		goto out;
	retval = send_hello(&state);
	if (unlikely(retval < 0))
		goto out;
	prl_notice(0, "Waiting for server welcome...");
	retval = event_loop(&state);
out:
	destroy_state(&state);
	return retval;
}
