/*
 * Copyright (c) 2006 Oracle.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */
#include <linux/kernel.h>
#include <linux/gfp.h>
#include <linux/in.h>
#include <net/tcp.h>

#include "rds.h"
#include "tcp.h"

int rds_tcp_keepalive(struct socket *sock)
{
	/* values below based on xs_udp_default_timeout */
	int keepidle = 5; /* send a probe 'keepidle' secs after last data */
	int keepcnt = 5; /* number of unack'ed probes before declaring dead */
	int keepalive = 1;
	int ret = 0;

	ret = kernel_setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE,
				(char *)&keepalive, sizeof(keepalive));
	if (ret < 0)
		goto bail;

	ret = kernel_setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT,
				(char *)&keepcnt, sizeof(keepcnt));
	if (ret < 0)
		goto bail;

	ret = kernel_setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE,
				(char *)&keepidle, sizeof(keepidle));
	if (ret < 0)
		goto bail;

	/* KEEPINTVL is the interval between successive probes. We follow
	 * the model in xs_tcp_finish_connecting() and re-use keepidle.
	 */
	ret = kernel_setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL,
				(char *)&keepidle, sizeof(keepidle));
bail:
	return ret;
}

int rds_tcp_accept_one(struct socket *sock)
{
	struct socket *new_sock = NULL;
	struct rds_connection *conn;
	int ret;
	struct inet_sock *inet;
	struct rds_tcp_connection *rs_tcp;

	ret = sock_create_lite(sock->sk->sk_family,
			       sock->sk->sk_type, sock->sk->sk_protocol,
			       &new_sock);
	if (ret)
		goto out;

	new_sock->type = sock->type;
	new_sock->ops = sock->ops;
	ret = sock->ops->accept(sock, new_sock, O_NONBLOCK, true);
	if (ret < 0)
		goto out;

	ret = rds_tcp_keepalive(new_sock);
	if (ret < 0)
		goto out;

	rds_tcp_tune(new_sock);

	inet = inet_sk(new_sock->sk);

	rdsdebug("accepted tcp %pI4:%u -> %pI4:%u\n",
		 &inet->inet_saddr, ntohs(inet->inet_sport),
		 &inet->inet_daddr, ntohs(inet->inet_dport));

	conn = rds_conn_create(sock_net(sock->sk),
			       inet->inet_saddr, inet->inet_daddr,
			       &rds_tcp_transport, GFP_KERNEL);
	if (IS_ERR(conn)) {
		ret = PTR_ERR(conn);
		goto out;
	}
	/* An incoming SYN request came in, and TCP just accepted it.
	 *
	 * If the client reboots, this conn will need to be cleaned up.
	 * rds_tcp_state_change() will do that cleanup
	 */
	rs_tcp = (struct rds_tcp_connection *)conn->c_transport_data;
	rds_conn_transition(conn, RDS_CONN_DOWN, RDS_CONN_CONNECTING);
	if (rs_tcp->t_sock) {
		/* Need to resolve a duelling SYN between peers.
		 * We have an outstanding SYN to this peer, which may
		 * potentially have transitioned to the RDS_CONN_UP state,
		 * so we must quiesce any send threads before resetting
		 * c_transport_data.
		 */
		wait_event(conn->c_waitq,
			   !test_bit(RDS_IN_XMIT, &conn->c_flags));
		if (ntohl(inet->inet_saddr) < ntohl(inet->inet_daddr)) {
			struct sock *nsk = new_sock->sk;

			nsk->sk_user_data = NULL;
			nsk->sk_prot->disconnect(nsk, 0);
			tcp_done(nsk);
			new_sock = NULL;
			ret = 0;
			goto out;
		} else if (rs_tcp->t_sock) {
			rds_tcp_restore_callbacks(rs_tcp->t_sock, rs_tcp);
			conn->c_outgoing = 0;
		}
	}
	rds_tcp_set_callbacks(new_sock, conn);
	rds_connect_complete(conn); /* marks RDS_CONN_UP */
	new_sock = NULL;
	ret = 0;

out:
	if (new_sock)
		sock_release(new_sock);
	return ret;
}

void rds_tcp_listen_data_ready(struct sock *sk)
{
	void (*ready)(struct sock *sk);

	rdsdebug("listen data ready sk %p\n", sk);

	read_lock(&sk->sk_callback_lock);
	ready = sk->sk_user_data;
	if (!ready) { /* check for teardown race */
		ready = sk->sk_data_ready;
		goto out;
	}

	/*
	 * ->sk_data_ready is also called for a newly established child socket
	 * before it has been accepted and the accepter has set up their
	 * data_ready.. we only want to queue listen work for our listening
	 * socket
	 */
	if (sk->sk_state == TCP_LISTEN)
		rds_tcp_accept_work(sk);

out:
	read_unlock(&sk->sk_callback_lock);
	ready(sk);
}

struct socket *rds_tcp_listen_init(struct net *net)
{
	struct sockaddr_in sin;
	struct socket *sock = NULL;
	int ret;

	ret = sock_create_kern(net, PF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
	if (ret < 0)
		goto out;

	sock->sk->sk_reuse = SK_CAN_REUSE;
	rds_tcp_nonagle(sock);

	write_lock_bh(&sock->sk->sk_callback_lock);
	sock->sk->sk_user_data = sock->sk->sk_data_ready;
	sock->sk->sk_data_ready = rds_tcp_listen_data_ready;
	write_unlock_bh(&sock->sk->sk_callback_lock);

	sin.sin_family = PF_INET;
	sin.sin_addr.s_addr = (__force u32)htonl(INADDR_ANY);
	sin.sin_port = (__force u16)htons(RDS_TCP_PORT);

	ret = sock->ops->bind(sock, (struct sockaddr *)&sin, sizeof(sin));
	if (ret < 0)
		goto out;

	ret = sock->ops->listen(sock, 64);
	if (ret < 0)
		goto out;

	return sock;
out:
	if (sock)
		sock_release(sock);
	return NULL;
}

void rds_tcp_listen_stop(struct socket *sock)
{
	struct sock *sk;

	if (!sock)
		return;

	sk = sock->sk;

	/* serialize with and prevent further callbacks */
	lock_sock(sk);
	write_lock_bh(&sk->sk_callback_lock);
	if (sk->sk_user_data) {
		sk->sk_data_ready = sk->sk_user_data;
		sk->sk_user_data = NULL;
	}
	write_unlock_bh(&sk->sk_callback_lock);
	release_sock(sk);

	/* wait for accepts to stop and close the socket */
	flush_workqueue(rds_wq);
	sock_release(sock);
}
