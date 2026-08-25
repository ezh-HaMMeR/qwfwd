/*
	peer.c
*/

#include "qwfwd.h"

peer_t *peers = NULL;
static int userid = 0;

peer_t	*FWD_peer_by_addr(struct sockaddr_in *from, int listener_socket)
{
	peer_t *p;

	for (p = peers; p; p = p->next)
	{
		if (p->listener_socket == listener_socket && NET_CompareAddress(&p->from, from))
			return p;
	}

	return NULL;
}

static int parse_color(const char *userinfo, const char *key)
{
	char tmp[MAX_INFO_STRING];
	int color;

	Info_ValueForKey(userinfo, key, tmp, sizeof(tmp));
	color = atoi(tmp);

	return (color < 0) ? 0 : ((color > 16) ? 16 : color);
}

peer_t	*FWD_peer_new(const char *remote_host, int remote_port, struct sockaddr_in *from, int listener_socket, const char *userinfo, int qport, protocol_t proto, qbool link)
{
	peer_t *p;
	struct sockaddr_in to;
	int s = INVALID_SOCKET;
	qbool new_peer = false;

	if (!NET_GetSockAddrIn_ByHostAndPort(&to, remote_host, remote_port))
		return NULL; // failed to resolve host name?

	if (!SV_IsWhitelisted(&to))
		return NULL;

	// check for bans.
	if (SV_IsBanned(&to))
		return NULL;

	// we probably already have such peer, reuse it then
	p = FWD_peer_by_addr(from, listener_socket);

	// next check for NEW peer only
	if ( !p )
	{
		new_peer = true; // it will be new peer

		if (FWD_peers_count() >= maxclients->integer)
			return NULL; // we already full!

		// NOTE: socket allocated here! Do not forget free it!!!
		if ((s = NET_UDP_OpenSocket(NULL, 0, false)) == INVALID_SOCKET)
			return NULL; // out of sockets?

		p = Sys_malloc(sizeof(*p)); // alloc peer if needed
	}

	p->s		= ( new_peer ) ? s : p->s; // reuse socket in case of reusing
	p->from		= *from;
	p->to		= to;
	p->listener_socket = listener_socket;
	p->ps		= ( !new_peer && proto == pr_q3 ) ? p->ps : ps_challenge; // do not reset state for q3 in case of peer reusing
	p->qport	= qport;
	p->proto	= proto;
	strlcpy(p->userinfo, userinfo, sizeof(p->userinfo));
	Info_ValueForKey(userinfo, "name", p->name, sizeof(p->name));
	p->top          = parse_color(userinfo, "topcolor");
	p->bottom       = parse_color(userinfo, "bottomcolor");
	p->userid	= ( new_peer ) ? ++userid : p->userid; // do not bump userid in case of peer reusing

	time(&p->last);

	// link only new peer, in case of reusing it already done...
	if (new_peer && link)
	{
		p->next = peers;
		peers = p;
	}

	return p;
}

// free peer data, perform unlink if requested
static void FWD_peer_free(peer_t *peer, qbool unlink)
{
	if (!peer)
		return;

	if (unlink)
	{
		peer_t *next, *prev, *current;

		prev = NULL;
		current = peers;

		for ( ; current; )
		{
			next = current->next;

			if (peer == current)
			{
				if (prev)
					prev->next = next;
				else
					peers = next;

				break;
			}

			prev = current;
			current = next;
		}
	}

	// free all data related to peer
	if (peer->s) // there should be no zero socket, it's stdin
		closesocket(peer->s);
	Sys_free(peer);
}

static void FWD_check_timeout(void)
{
	byte msg_data[6];
	sizebuf_t msg;
	time_t cur_time;
	double d_cur_time;
	peer_t *p;

	SZ_InitEx(&msg, msg_data, sizeof(msg_data), true);

	cur_time = time(NULL);
	d_cur_time = Sys_DoubleTime();

	for (p = peers; p; p = p->next)
	{
		// this is helper for q3 to guess disconnect asap
		if (p->proto == pr_q3)
		{
			if (cur_time - p->last > 1 && d_cur_time - p->q3_disconnect_check > 0.05 && p->ps == ps_connected)
			{
				p->q3_disconnect_check = d_cur_time;
				SZ_Clear(&msg);
				MSG_WriteLong(&msg, 0);
				MSG_WriteShort(&msg, p->qport);
				NET_SendPacket(p->s, msg.cursize, msg.data, &p->to);
			}
		}

		if (cur_time - p->last < 15) // few seconds timeout
			continue;

		Sys_DPrintf("peer %s:%d timed out\n", inet_ntoa(p->from.sin_addr), (int)ntohs(p->from.sin_port));

		p->ps = ps_drop;
	}
}

static void FWD_check_drop(void)
{
	peer_t *p, *next;

	for (p = peers; p; p = next)
	{
		next = p->next;

		if (p->ps != ps_drop)
			continue;

		Sys_DPrintf("peer %s:%d dropped\n", inet_ntoa(p->from.sin_addr), (int)ntohs(p->from.sin_port));
		FWD_peer_free(p, true); // NOTE: 'p' is not valid after this function, so we remember 'next' before this function
	}
}

static void FWD_read_listener(int listener_socket)
{
	qbool connectionless;
	int cnt;
	peer_t *p;

	for (;;)
	{
		if (!NET_GetPacket(listener_socket, &net_message))
			break;

		// check for bans.
		if (SV_IsBanned(&net_from))
			continue;

		if (net_message.cursize == 1 && net_message.data[0] == A2A_ACK)
		{
			QRY_SV_PingReply();
			continue;
		}

		MSG_BeginReading();
		connectionless = (MSG_ReadLong() == -1);

		if (connectionless)
		{
			if (MSG_BadRead())
				continue;

			if (!SV_ConnectionlessPacket())
				continue; // seems we do not need forward it
		}

		p = FWD_peer_by_addr(&net_from, listener_socket);
		if (!p)
			continue;

		// forward data to the server/proxy
		if (p->ps >= ps_connected)
		{
			cnt = 1; // one packet by default

			// check for "drop" aka client disconnect,
			// first 10 bytes for NON connectionless packet is netchan related shit in QW
			if (p->proto == pr_qw && !connectionless && net_message.cursize > 10 && net_message.data[10] == clc_stringcmd)
			{
				if (!strcmp((char*)net_message.data + 10 + 1, "drop"))
				{
					p->ps = ps_drop; // drop peer ASAP
					cnt = 3; // send few packets due to possibile packet lost
				}
			}

			for ( ; cnt > 0; cnt--)
				NET_SendPacket(p->s, net_message.cursize, net_message.data, &p->to);
		}

		time(&p->last);
	}
}

static void FWD_network_update(void)
{
	fd_set rfds;
	struct timeval tv;
	int retval;
	int i, i1;
	peer_t *p;

	FD_ZERO(&rfds);

	// select on all server sockets
	i1 = 0;
	for (i = 0; i < net_listener_count; i++)
	{
		FD_SET(net_listeners[i].socket, &rfds);
		if (net_listeners[i].socket >= i1)
			i1 = net_listeners[i].socket + 1;
	}

	for (p = peers; p; p = p->next)
	{
		// select on peers sockets
		FD_SET(p->s, &rfds);
		if (p->s >= i1)
			i1 = p->s + 1;
	}

// if not DLL - read stdin
#ifndef APP_DLL
	#ifndef _WIN32
	// try read stdin only if connected to a terminal.
	if (isatty(STDIN) && isatty(STDOUT))
	{
		FD_SET(STDIN, &rfds);
		if (STDIN >= i1)
			i1 = STDIN + 1;
	}
	#endif // _WIN32
#endif

	/* Sleep for some time, wake up immidiately if there input packet. */
	tv.tv_sec = 0;
	tv.tv_usec = 100000; // 100 ms

retry:
	retval = select(i1, &rfds, (fd_set *)0, (fd_set *)0, &tv);
	if (retval < 0)
	{
		if (errno == EINTR)
		{
			goto retry;
		}
		perror("select");
		return;
	}

	// read console input.
	// NOTE: we do not do that if we are in DLL mode...
	Sys_ReadSTDIN(&ps, rfds);

	if (retval <= 0)
		return;

	// read packets from every configured server/proxy socket
	for (i = 0; i < net_listener_count; i++)
	{
		if (FD_ISSET(net_listeners[i].socket, &rfds))
			FWD_read_listener(net_listeners[i].socket);
	}

	// now lets check peers sockets, perhaps we have input packets too
	for (p = peers; p; p = p->next)
	{
		if(FD_ISSET(p->s, &rfds))
		{
			// yeah, we have packet, read it then
			for (;;)
			{
				if (!NET_GetPacket(p->s, &net_message))
					break;

				// check for bans.
				if (SV_IsBanned(&net_from))
					continue;

				// we should check is this packet from remote server, this may be some evil packet from haxors...
				if (!NET_CompareAddress(&p->to, &net_from))
					continue;

				MSG_BeginReading();
				if (MSG_ReadLong() == -1)
				{
					if (MSG_BadRead())
						continue;

					if (!CL_ConnectionlessPacket(p))
						continue; // seems we do not need forward it

					NET_SendPacket(p->listener_socket, net_message.cursize, net_message.data, &p->from);
					continue;
				}

				if (p->ps >= ps_connected)
					NET_SendPacket(p->listener_socket, net_message.cursize, net_message.data, &p->from);

// qqshka: commented out
//				time(&p->last);

			} // for (;;)
		} // if(FD_ISSET(p->s, &rfds))

		if (p->ps == ps_challenge)
		{
			// send challenge time to time
			if (time(NULL) - p->connect > 2)
			{
				p->connect = time(NULL);
				Netchan_OutOfBandPrint(p->s, &p->to, "getchallenge%s", p->proto == pr_qw ? "\n" : "");
			}
		}
	} // for (p = peers; p; p = p->next)
}

int FWD_peers_count(void)
{
	int cnt;
	peer_t *p;

	for (cnt = 0, p = peers; p; p = p->next)
	{
		cnt++;
	}

	return cnt;
}

//======================================================

static void FWD_Cmd_ClList_f(void)
{
	peer_t *p;
	char ipport1[] = "xxx.xxx.xxx.xxx:xxxxx";
	char ipport2[] = "xxx.xxx.xxx.xxx:xxxxx";
	int idx;
	time_t current = time(NULL);

	Sys_Printf("=== client list ===\n");
	Sys_Printf("##id## %-*s %-*s time name\n", sizeof(ipport1)-1, "address from", sizeof(ipport2)-1, "address to");
	Sys_Printf("-----------------------------------------------------------------------\n");

	for (idx = 1, p = peers; p; p = p->next, idx++)
	{
		Sys_Printf("%6d %-*s %-*s %4d %s\n",
			p->userid,
			sizeof(ipport1)-1, NET_AdrToString(&p->from, ipport1, sizeof(ipport1)),
			sizeof(ipport2)-1, NET_AdrToString(&p->to,   ipport2, sizeof(ipport2)),
			(int)(current - p->connect)/60, p->name);
	}

	Sys_Printf("-----------------------------------------------------------------------\n");
	Sys_Printf("%d clients\n", idx-1);
}

//======================================================

void FWD_update_peers(void)
{
	FWD_network_update();
	FWD_check_timeout();
	FWD_check_drop();
}

//======================================================

void FWD_Init(void)
{
	peers = NULL;
	userid = 0;

	Cmd_AddCommand("cllist", FWD_Cmd_ClList_f);
}

