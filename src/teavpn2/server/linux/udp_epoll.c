// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021  Ammar Faizi
 */

#include <unistd.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <teavpn2/server/common.h>
#include <teavpn2/server/linux/udp.h>


static int epoll_add(int epoll_fd, int fd, uint32_t events, epoll_data_t data)
{
	int ret;
	struct epoll_event evt;

	memset(&evt, 0, sizeof(evt));
	evt.events = events;
	evt.data = data;

	ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &evt);
	if (unlikely(ret < 0)) {
		ret = errno;
		pr_err("epoll_ctl(%d, EPOLL_CTL_ADD, %d, events): " PRERF,
			epoll_fd, fd, PREAR(ret));
		ret = -ret;
	}

	return ret;
}


static int epoll_delete(int epoll_fd, int fd)
{
	int ret;

	ret = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
	if (unlikely(ret < 0)) {
		ret = errno;
		pr_err("epoll_ctl(%d, EPOLL_CTL_ADD, %d, events): " PRERF,
			epoll_fd, fd, PREAR(ret));
		ret = -ret;
	}

	return ret;
}


static int init_epoll_user_data(struct srv_udp_state *state)
{
	struct epld_struct *epl_udata, *udata;
	size_t i, nn = (size_t)state->cfg->sys.thread_num + 10u;

	epl_udata = calloc_wrp(nn, sizeof(*epl_udata));
	if (unlikely(!epl_udata))
		return -errno;

	for (i = 0; i < nn; i++) {
		udata = &epl_udata[i];
		udata->fd = -1;
		udata->type = 0;
		udata->idx = (uint16_t)i;
	}

	state->epl_udata = epl_udata;
	return 0;
}


static int create_epoll_fd(void)
{
	int ret = 0;
	int epoll_fd;

	epoll_fd = epoll_create(255);
	if (unlikely(epoll_fd < 0)) {
		ret = errno;
		pr_err("epoll_create(): " PRERF, PREAR(ret));
		return -ret;
	}

	return epoll_fd;
}


static int register_fd_in_to_epoll(struct epl_thread *thread, int fd)
{
	epoll_data_t data;
	const uint32_t events = EPOLLIN | EPOLLPRI;
	int epoll_fd = thread->epoll_fd;

	memset(&data, 0, sizeof(data));
	data.fd = fd;
	prl_notice(4, "Registering fd (%d) to epoll (for thread %u)...",
		   fd, thread->idx);
	return epoll_add(epoll_fd, fd, events, data);
}


static int init_epoll_thread_data(struct srv_udp_state *state)
{
	int ret = 0;
	int *tun_fds = state->tun_fds;
	struct epl_thread *threads, *thread;
	uint8_t i, nn = (uint8_t)state->cfg->sys.thread_num;

	state->epl_threads = NULL;
	threads = calloc_wrp(nn, sizeof(*threads));
	if (unlikely(!threads))
		return -errno;

	state->epl_threads = threads;

	/*
	 * Initialize all epoll_fd to -1 for graceful clean up in
	 * case we fail to create the epoll instance.
	 */
	for (i = 0; i < nn; i++) {
		threads[i].state = state;
		threads[i].epoll_fd = -1;
	}

	for (i = 0; i < nn; i++) {
		thread = &threads[i];
		thread->idx = i;

		ret = create_epoll_fd();
		if (unlikely(ret < 0))
			goto out;

		thread->epoll_fd = ret;

		if (i == 0) {
			/*
			 * Main thread is at index 0.
			 *
			 * Main thread is responsible to handle packet from UDP
			 * socket, decapsulate it and write it to tun_fd.
			 */
			ret = register_fd_in_to_epoll(thread, state->udp_fd);
		} else {
			ret = register_fd_in_to_epoll(thread, tun_fds[i]);
		}

		if (unlikely(ret))
			goto out;
	}


	if (nn == 1) {
		/*
		 * If we are single-threaded, the main thread is also
		 * responsible to read from TUN fd, encapsulate it and
		 * send it via UDP.
		 */
		ret = register_fd_in_to_epoll(&threads[0], tun_fds[0]);
	} else {
		/*
		 * If we are multithreaded, the subthread is responsible
		 * to read from tun_fds[0]. Don't give this work to the
		 * main thread for better concurrency.
		 */
		ret = register_fd_in_to_epoll(&threads[1], tun_fds[0]);
	}
out:
	return ret;
}


static ssize_t send_to_client(struct epl_thread *thread,
			      struct udp_sess *cur_sess, const void *buf,
			      size_t pkt_len)
{
	int ret;
	ssize_t send_ret;

	send_ret = sendto(thread->state->udp_fd, buf, pkt_len, 0,
			  &cur_sess->addr, sizeof(cur_sess->addr));
	if (unlikely(send_ret <= 0)) {

		if (send_ret == 0) {
			pr_err("UDP socket disconnected!");
			return -ENETDOWN;
		}

		ret = errno;
		if (ret != EAGAIN)
			pr_err("sendto(): " PRERF, PREAR(ret));

		return (ssize_t)-ret;
	}

	pr_debug("sendto(): %zd bytes %x:%hx", send_ret, cur_sess->src_addr,
		 cur_sess->src_port);
	return send_ret;
}


static int send_handshake(struct epl_thread *thread, struct udp_sess *cur_sess)
{
	size_t send_len;
	ssize_t send_ret;
	struct srv_pkt *srv_pkt = &thread->pkt.srv;

	send_len = srv_pprep_handshake(srv_pkt);
	send_ret = send_to_client(thread, cur_sess, srv_pkt, send_len);
	if (unlikely(send_ret < 0))
		return (int)send_ret;

	return 0;
}


static int send_handshake_reject(struct epl_thread *thread,
				 struct udp_sess *cur_sess, uint8_t reason,
				 const char *msg)
{
	size_t send_len;
	ssize_t send_ret;
	struct srv_pkt *srv_pkt = &thread->pkt.srv;

	send_len = srv_pprep_handshake_reject(srv_pkt, reason, msg);
	send_ret = send_to_client(thread, cur_sess, srv_pkt, send_len);
	if (unlikely(send_ret < 0))
		return (int)send_ret;

	return 0;
}


static int handle_client_handshake(struct epl_thread *thread,
				   struct udp_sess *cur_sess)
{
	int ret;
	char rej_msg[512];
	uint8_t rej_reason = 0;
	size_t len = thread->pkt.len;
	struct cli_pkt *cli_pkt = &thread->pkt.cli;
	struct pkt_handshake *hand = &cli_pkt->handshake;
	struct teavpn2_version *cur = &hand->cur;
	const size_t expected_len = sizeof(*hand);

	if (len < (PKT_MIN_LEN + expected_len)) {
		snprintf(rej_msg, sizeof(rej_msg),
			 "Invalid handshake packet length from " PRWIU
			 " (expected at least %zu bytes; actual = %zu bytes)",
			 W_IU(cur_sess), (PKT_MIN_LEN + expected_len), len);

		ret = -EBADMSG;
		rej_reason = TSRV_HREJECT_INVALID;
		goto reject;
	}

	cli_pkt->len = ntohs(cli_pkt->len);
	if (((size_t)cli_pkt->len) != expected_len) {
		snprintf(rej_msg, sizeof(rej_msg),
			 "Invalid handshake packet length from " PRWIU
			 " (expected = %zu; actual: cli_pkt->len = %hu)",
			 W_IU(cur_sess), expected_len, cli_pkt->len);

		ret = -EBADMSG;
		rej_reason = TSRV_HREJECT_INVALID;
		goto reject;
	}

	if (cli_pkt->type != TCLI_PKT_HANDSHAKE) {
		snprintf(rej_msg, sizeof(rej_msg),
			 "Invalid first packet type from " PRWIU
			 " (expected = TCLI_PKT_HANDSHAKE (%u); actual = %hhu)",
			 W_IU(cur_sess), TCLI_PKT_HANDSHAKE, cli_pkt->type);

		ret = -EBADMSG;
		rej_reason = TSRV_HREJECT_INVALID;
		goto reject;
	}

	/* For printing safety! */
	cur->extra[sizeof(cur->extra) - 1] = '\0';
	prl_notice(2, "New connection from " PRWIU
		   " (client version: TeaVPN2-%hhu.%hhu.%hhu%s)",
		   W_IU(cur_sess),
		   cur->ver,
		   cur->patch_lvl,
		   cur->sub_lvl,
		   cur->extra);

	if ((cur->ver != VERSION) || (cur->patch_lvl != PATCHLEVEL) ||
	    (cur->sub_lvl != SUBLEVEL)) {
		ret = -EBADMSG;
		rej_reason = TSRV_HREJECT_VERSION_NOT_SUPPORTED;
		prl_notice(2, "Dropping connection from " PRWIU
			   " (version not supported)...", W_IU(cur_sess));
		goto reject;
	}


	/*
	 * Good handshake packet, send back.
	 */
	return send_handshake(thread, cur_sess);

reject:
	prl_notice(2, "%s", rej_msg);
	send_handshake_reject(thread, cur_sess, rej_reason, rej_msg);
	return ret;
}


static int handle_new_client(struct epl_thread *thread, uint32_t addr,
			     uint16_t port, struct sockaddr_in *saddr)
{
	int ret;
	struct udp_sess *cur_sess;

	cur_sess = get_udp_sess(thread->state, addr, port);
	if (unlikely(!cur_sess))
		return -errno;

	cur_sess->addr = *saddr;
	ret = handle_client_handshake(thread, cur_sess);
	if (ret) {
		/* Handshake failed, drop the client session! */
		put_udp_session(thread->state, cur_sess);
		ret = (ret == -EBADMSG) ? 0 : ret;
	}

	return ret;
}


static int __handle_event_udp(struct epl_thread *thread, struct udp_sess *cur_sess)
{
	return 0;
}


static int _handle_event_udp(struct epl_thread *thread, struct sockaddr_in *saddr)
{
	uint16_t port;
	uint32_t addr;
	struct udp_sess *cur_sess;

	port = ntohs(saddr->sin_port);
	addr = ntohl(saddr->sin_addr.s_addr);
	cur_sess = map_find_udp_sess(thread->state, addr, port);
	if (unlikely(!cur_sess)) {
		int ret;

		/*
		 * It's a new client since we don't find it in
		 * the session entry.
		 */
		ret = handle_new_client(thread, addr, port, saddr);
		return (ret == -EAGAIN) ? 0 : ret;
	}

	udp_sess_tv_update(cur_sess);
	return __handle_event_udp(thread, cur_sess);
}


static int handle_event_udp(int udp_fd, struct epl_thread *thread)
{
	int ret;
	ssize_t recv_ret;
	struct sockaddr_in saddr;
	char *buf = thread->pkt.__raw;
	socklen_t addrlen = sizeof(saddr);
	size_t recv_size = sizeof(thread->pkt.cli.__raw);

	recv_ret = recvfrom(udp_fd, buf, recv_size, 0, (struct sockaddr *)&saddr,
			    &addrlen);
	if (unlikely(recv_ret <= 0)) {

		if (recv_ret == 0) {
			pr_err("UDP socket disconnected!");
			return -ENETDOWN;
		}

		ret = errno;
		if (ret == EAGAIN)
			return 0;

		pr_err("recvfrom(udp_fd) (fd=%d): " PRERF, udp_fd, PREAR(ret));
		return -ret;
	}
	thread->pkt.len = (size_t)recv_ret;

	pr_debug("recvfrom() client %zd bytes", recv_ret);
	return _handle_event_udp(thread, &saddr);
}


static int handle_event_tun(int tun_fd, struct epl_thread *thread)
{
	int ret;
	ssize_t read_ret;
	char *buf = thread->pkt.srv.__raw;
	size_t read_size = sizeof(thread->pkt.srv.__raw);

	read_ret = read(tun_fd, buf, read_size);
	if (unlikely(read_ret < 0)) {
		ret = errno;
		if (likely(ret == EAGAIN))
			return 0;

		pr_err("read(tun_fd) (fd=%d): " PRERF, tun_fd, PREAR(ret));
		return -ret;
	}
	thread->pkt.len = (size_t)read_ret;

	pr_debug("read() from tun_fd %zd bytes", read_ret);
	return 0;
}


/*
 * TL;DR
 * If this function returns non zero, TeaVPN2 process is exiting!
 *
 * -----------------------------------------------
 * This function should only return error code if
 * the error is fatal and need termination entire
 * process!
 *
 * If the error is not fatal, this function must
 * return 0.
 *
 */
static int handle_event(struct epl_thread *thread, struct epoll_event *evt)
{
	int ret = 0;
	int fd = evt->data.fd;

	if (fd == thread->state->udp_fd) {
		ret = handle_event_udp(fd, thread);
	} else {
		/* It's a TUN fd. */
		ret = handle_event_tun(fd, thread);
	}

	return ret;
}


static int _do_epoll_wait(struct epl_thread *thread)
{
	int ret;
	int epoll_fd = thread->epoll_fd;
	int timeout = thread->epoll_timeout;
	struct epoll_event *events = thread->events;

	ret = epoll_wait(epoll_fd, events, EPOLL_EVT_ARR_NUM, timeout);
	if (unlikely(ret < 0)) {
		ret = errno;

		if (likely(ret == EINTR)) {
			prl_notice(2, "[thread=%u] Interrupted!", thread->idx);
			return 0;
		}

		pr_err("[thread=%u] epoll_wait(): " PRERF, thread->idx,
		       PREAR(ret));
		return -ret;
	}

	return ret;
}


static void reap_zombie_sessions(struct epl_thread *thread)
{
	size_t i, n;
	time_t time_diff = 0;
	struct udp_sess *sess, *cur;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	n = (size_t)atomic_load(&thread->state->active_sess);
	if (!n)
		return;

	if (pthread_mutex_trylock(&lock))
		return;

	prl_notice(4, "[thread=%u] Current num of connected client(s): %zu",
		   thread->idx, n);
	sess = thread->state->sess;
	for (i = 0; i < UDP_SESS_NUM; i++) {
		cur = &sess[i];
		if (!atomic_load(&cur->is_connected))
			continue;

		if (get_unix_time(&time_diff))
			continue;

		time_diff -= cur->last_touch;
		if (time_diff < UDP_SESS_TIMEOUT)
			continue;

		prl_notice(2, "Reaping zombie client " PRWIU "...", W_IU(cur));
		put_udp_session(thread->state, cur);
	}
	pthread_mutex_unlock(&lock);
}


static int do_epoll_wait(struct epl_thread *thread)
{
	int ret, i, tmp;
	struct epoll_event *events;

	ret = _do_epoll_wait(thread);
	if (unlikely(ret < 0)) {
		pr_err("_do_epoll_wait(): " PRERF, PREAR(-ret));
		return ret;
	}

	if (ret == 0 && thread->idx > 0) {
		reap_zombie_sessions(thread);
	}

	events = thread->events;
	for (i = 0; i < ret; i++) {
		tmp = handle_event(thread, &events[i]);
		if (unlikely(tmp))
			return tmp;
	}

	return 0;
}


static void thread_wait_or_add_counter(struct epl_thread *thread,
				       struct srv_udp_state *state)
{
	static _Atomic(bool) release_sub_thread = false;
	uint8_t nn = (uint8_t)state->cfg->sys.thread_num;

	atomic_fetch_add(&state->ready_thread, 1);
	if (thread->idx != 0) {
		/*
		 * We are the sub thread.
		 * Waiting for the main thread be ready...
		 */
		while (!atomic_load(&release_sub_thread)) {
			if (unlikely(state->stop))
				return;
			usleep(100000);
		}
		return;
	}

	/*
	 * We are the main thread...
	 */
	while (atomic_load(&state->ready_thread) != nn) {
		prl_notice(2, "(thread=%u) "
			   "Waiting for subthread(s) to be ready...",
			   thread->idx);
		if (unlikely(state->stop))
			return;
		usleep(100000);
	}

	if (nn > 1)
		prl_notice(2, "All threads all are ready!");

	prl_notice(2, "Initialization Sequence Completed");
	atomic_store(&release_sub_thread, true);
}


__no_inline static void *_run_event_loop(void *thread_p)
{
	int ret = 0;
	struct epl_thread *thread = (struct epl_thread *)thread_p;
	struct srv_udp_state *state = thread->state;

	thread_wait_or_add_counter(thread, state);
	thread->epoll_timeout = 1000;

	while (likely(!state->stop)) {
		ret = do_epoll_wait(thread);
		if (unlikely(ret))
			break;
	}

	atomic_fetch_sub(&state->ready_thread, 1);
	return (void *)((intptr_t)ret);
}


static int spawn_thread(struct epl_thread *thread)
{
	int ret;

	prl_notice(2, "Spawning thread %u...", thread->idx);
	ret = pthread_create(&thread->thread, NULL, _run_event_loop, thread);
	if (unlikely(ret)) {
		pr_err("pthread_create(): " PRERF, PREAR(ret));
		return -ret;
	}

	ret = pthread_detach(thread->thread);
	if (unlikely(ret)) {
		pr_err("pthread_detach(): " PRERF, PREAR(ret));
		return -ret;
	}

	return ret;
}


static int run_event_loop(struct srv_udp_state *state)
{
	void *ret_p;
	int ret = 0;
	struct epl_thread *threads = state->epl_threads;
	uint8_t i, nn = (uint8_t)state->cfg->sys.thread_num;

	atomic_store(&state->ready_thread, 0);
	for (i = 1; i < nn; i++) {
		ret = spawn_thread(&threads[i]);
		if (unlikely(ret))
			goto out;
	}

	/*
	 * ret_p is just to shut the clang warning up!
	 */
	ret_p = _run_event_loop(&threads[0]);
	ret   = (int)((intptr_t)ret_p);
out:
	return ret;
}


static void close_epoll_fds(struct epl_thread *threads, uint8_t nn)
{
	uint8_t i;
	struct epl_thread *thread;
	for (i = 0; i < nn; i++) {
		int epoll_fd;
		thread = &threads[i];

		epoll_fd = thread->epoll_fd;
		if (epoll_fd == -1)
			continue;

		close(epoll_fd);
		prl_notice(2, "Closing threads[%hhu].epoll_fd (fd=%d)", i,
			   epoll_fd);
	}
}


static bool wait_for_threads_to_exit(struct srv_udp_state *state)
{
	unsigned wait_c = 0;
	uint16_t thread_on = 0, cc;

	thread_on = atomic_load(&state->ready_thread);
	if (thread_on == 0)
		return true;

	prl_notice(2, "Waiting for %hu thread(s) to exit...", thread_on);
	while ((cc = atomic_load(&state->ready_thread)) > 0) {

		if (cc != thread_on) {
			thread_on = cc;
			prl_notice(2, "Waiting for %hu thread(s) to exit...", cc);
		}

		usleep(100000);
		if (wait_c++ > 1000)
			return false;
	}
	return true;
}


static void destroy_epoll(struct srv_udp_state *state)
{
	struct epl_thread *threads;
	uint8_t nn = (uint8_t)state->cfg->sys.thread_num;

	if (!wait_for_threads_to_exit(state)) {
		/* Thread(s) won't exit, don't free the heap! */
		pr_emerg("Thread(s) won't exit!");
		state->threads_wont_exit = true;
		return;
	}

	threads = state->epl_threads;
	if (threads) {
		close_epoll_fds(threads, nn);
		al64_free(threads);
	}
	al64_free(state->epl_udata);
}


int teavpn2_udp_server_epoll(struct srv_udp_state *state)
{
	int ret = 0;

	ret = init_epoll_user_data(state);
	if (unlikely(ret))
		goto out;
	ret = init_epoll_thread_data(state);
	if (unlikely(ret))
		goto out;

	state->stop = false;
	ret = run_event_loop(state);
out:
	destroy_epoll(state);
	return ret;
}
