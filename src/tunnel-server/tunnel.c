/*
 * tunnel.c - port forward implementations
 *
 * Copyright (C) 2000 Stefan Jahn <stefan@lkcc.org>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this package; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.  
 *
 * $Id: tunnel.c,v 1.8 2000/11/12 01:48:54 ela Exp $
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#if ENABLE_TUNNEL

#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#ifdef __MINGW32__
# include <winsock.h>
#endif

#ifndef __MINGW32__
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
#endif

#include "util.h"
#include "hash.h"
#include "alloc.h"
#include "socket.h"
#include "connect.h"
#include "udp-socket.h"
#include "icmp-socket.h"
#include "server-core.h"
#include "server.h"
#include "tunnel.h"

tnl_config_t tnl_config = 
{
  NULL, /* the source port to forward from */
  NULL, /* target port to forward to */
  NULL  /* the source client socket hash */
};

/*
 * Defining configuration file associations with key-value-pairs.
 */
key_value_pair_t tnl_config_prototype [] = 
{
  REGISTER_PORTCFG ("source", tnl_config.source, NOTDEFAULTABLE),
  REGISTER_PORTCFG ("target", tnl_config.target, NOTDEFAULTABLE),
  REGISTER_END ()
};

/*
 * Definition of this server.
 */
server_definition_t tnl_server_definition =
{
  "tunnel server",
  "tunnel",
  tnl_global_init,
  tnl_init,
  tnl_detect_proto,
  tnl_connect_socket,
  tnl_finalize,
  tnl_global_finalize,
  NULL,
  NULL,
  NULL,
  tnl_handle_request_udp_source,
  &tnl_config,
  sizeof (tnl_config),
  tnl_config_prototype
};

int
tnl_global_init (void)
{
  return 0;
}

int
tnl_global_finalize (void)
{
  return 0;
}

/*
 * Tunnel server instance initializer. Check the configuration.
 */
int
tnl_init (server_t *server)
{
  tnl_config_t *cfg = server->cfg;

  /* protocol supported ? */
  if (!(cfg->source->proto & (PROTO_TCP | PROTO_ICMP | PROTO_UDP)) ||
      !(cfg->target->proto & (PROTO_TCP | PROTO_ICMP | PROTO_UDP)))
    {
      log_printf (LOG_ERROR, "tunnel: protocol not supported\n");
      return -1;
    }

  /* check identity of source and target port configurations */
  if (server_portcfg_equal (cfg->source, cfg->target))
    {
      log_printf (LOG_ERROR, "tunnel: source and target identical\n");
      return -1;
    }

  /* broadcast target ip address not allowed */
  if (cfg->target->localaddr->sin_addr.s_addr == INADDR_ANY)
    {
      log_printf (LOG_ERROR, "tunnel: broadcast target ip not allowed\n");
      return -1;
    }

  /* create source client hash (for UDP and ICMP only) */
  cfg->client = hash_create (4);
  
  /* assign the appropriate handle request routine of the server */
  if (cfg->source->proto & PROTO_UDP)
    server->handle_request = tnl_handle_request_udp_source;
  if (cfg->source->proto & PROTO_ICMP)
    server->handle_request = tnl_handle_request_icmp_source;

  /* bind the source port */
  server_bind (server, cfg->source);
  return 0;
}

int
tnl_finalize (server_t *server)
{
  tnl_config_t *cfg = server->cfg;
  tnl_source_t **source;
  int n;

  /* release source connection hash if necessary */
  if ((source = (tnl_source_t **) hash_values (cfg->client)) != NULL)
    {
      for (n = 0; n < hash_size (cfg->client); n++)
	{
	  xfree (source[n]);
	}
      hash_xfree (source);
    }
  hash_destroy (cfg->client);

  return 0;
}

/*
 * Create a hash string (key) for the source client hash. Identifiers
 * are the remote ip address and port.
 */
static char *
tnl_addr (socket_t sock)
{
  static char addr[24];

  sprintf (addr, "%s:%u", util_inet_ntoa (sock->remote_addr), 
	   ntohs (sock->remote_port));
  return addr;
}

/*
 * Depending on the given sockets structure target flag this routine
 * tries to connect to the servers target configuration and delivers a
 * new socket structure or NULL if it failed.
 */ 
static socket_t
tnl_create_socket (socket_t sock, int source)
{
  tnl_config_t *cfg = sock->cfg;
  unsigned long ip = cfg->target->localaddr->sin_addr.s_addr;
  unsigned short port = cfg->target->localaddr->sin_port;
  socket_t xsock = NULL;

  /*
   * Depending on the target configuration we assign different
   * callbacks, set other flags and use various connection routines.
   */
  switch (cfg->target->proto)
    {
    case PROTO_TCP:
      sock->userflags |= TNL_FLAG_TGT_TCP;
      break;
    case PROTO_UDP:
      sock->userflags |= TNL_FLAG_TGT_UDP;
      break;
    case PROTO_ICMP:
      sock->userflags |= TNL_FLAG_TGT_ICMP;
      break;
    default:
      log_printf (LOG_ERROR, "tunnel: invalid target configuration\n");
      return NULL;
    }

  /* target is a TCP connection */
  if (sock->userflags & TNL_FLAG_TGT_TCP)
    {
      if ((xsock = sock_connect (ip, port)) == NULL)
	{
	  log_printf (LOG_ERROR, "tunnel: tcp: cannot connect to %s:%u\n",
		      util_inet_ntoa (ip), ntohs (port));
	  return NULL;
	}

#if ENABLE_DEBUG
      log_printf (LOG_DEBUG, "tunnel: tcp: connecting to %s:%u\n",
		  util_inet_ntoa (ip), ntohs (port));
#endif /* ENABLE_DEBUG */
      xsock->check_request = tnl_check_request_tcp_target;
      sock_resize_buffers (xsock, UDP_BUF_SIZE, UDP_BUF_SIZE);
    }

  /* target is an UDP connection */
  else if (sock->userflags & TNL_FLAG_TGT_UDP)
    {
      if ((xsock = udp_connect (ip, port)) == NULL)
	{
	  log_printf (LOG_ERROR, "tunnel: udp: cannot connect to %s:%u\n",
		      util_inet_ntoa (ip), ntohs (port));
	  return NULL;
	}

#if ENABLE_DEBUG
      log_printf (LOG_DEBUG, "tunnel: udp: connecting to %s:%u\n",
		  util_inet_ntoa (ip), ntohs (port));
#endif /* ENABLE_DEBUG */
      xsock->handle_request = tnl_handle_request_udp_target;
      xsock->idle_func = tnl_idle;
      xsock->idle_counter = TNL_TIMEOUT;
    }

  /* target is an ICMP connection */
  else if (sock->userflags & TNL_FLAG_TGT_ICMP)
    {
      if ((xsock = icmp_connect (ip, port)) == NULL)
	{
	  log_printf (LOG_ERROR, "tunnel: icmp: cannot connect to %s\n",
		      util_inet_ntoa (ip));
	  return NULL;
	}

#if ENABLE_DEBUG
      log_printf (LOG_DEBUG, "tunnel: icmp: connecting to %s\n",
		  util_inet_ntoa (ip));
#endif /* ENABLE_DEBUG */
      xsock->handle_request = tnl_handle_request_icmp_target;
      xsock->idle_func = tnl_idle;
      xsock->idle_counter = TNL_TIMEOUT;
    }

  xsock->cfg = cfg;
  xsock->flags |= SOCK_FLAG_NOFLOOD;
  xsock->userflags = (sock->userflags | source) & ~(TNL_FLAG_TGT);
  xsock->disconnected_socket = tnl_disconnect;
  xsock->referer = sock;
  sock->referer = xsock;

  return xsock;
}

/*
 * Forward a given packet with a certain length to a target connection.
 * This routine can be used by all source connection handler passing
 * the targets socket and its own userflags.
 */
static int
tnl_send_request_source (socket_t sock, char *packet, int len, int flag)
{
  /* target is TCP */
  if (flag & TNL_FLAG_TGT_TCP)
    {
      if (sock_write (sock, packet, len) == -1)
	return -1;
    }
  /* target is UDP */
  else if (flag & TNL_FLAG_TGT_UDP)
    {
      if (udp_write (sock, packet, len) == -1)
	return -1;
    }
  /* target is ICMP */
  else if (flag & TNL_FLAG_TGT_ICMP)
    {
      if (icmp_write (sock, packet, len) == -1)
	return -1;
    }

  return 0;
}

/*
 * Forward a given packet with a certain length to a source connection.
 * This routine can be used by all target connection handler passing
 * the sources socket and its own userflags.
 */
static int
tnl_send_request_target (socket_t sock, char *packet, int len, int flag)
{
  /* source is TCP */
  if (flag & TNL_FLAG_SRC_TCP)
    {
      if (sock_write (sock, packet, len) == -1)
	return -1;
    }
  /* source is UDP */
  else if (flag & TNL_FLAG_SRC_UDP)
    {
      if (udp_write (sock, packet, len) == -1)
	return -1;
    }
  /* source is ICMP */
  else if (flag & TNL_FLAG_SRC_ICMP)
    {
      if (icmp_write (sock, packet, len) == -1)
	return -1;
    }

  return 0;
}

/*
 * Tunnel server TCP detection routine. It is greedy. Thus it cannot share
 * the port with other TCP servers.
 */
int
tnl_detect_proto (void *cfg, socket_t sock)
{
  log_printf (LOG_NOTICE, "tunnel: tcp connection accepted\n");
  return -1;
}

/*
 * If any TCP connection has been accepted this routine is called to setup
 * the tunnel server specific callbacks.
 */
int
tnl_connect_socket (void *config, socket_t sock)
{
  tnl_config_t *cfg = config;
  
  sock->flags |= SOCK_FLAG_NOFLOOD;
  sock->check_request = tnl_check_request_tcp_source;
  sock->disconnected_socket = tnl_disconnect;
  sock_resize_buffers (sock, UDP_BUF_SIZE, UDP_BUF_SIZE);

  /* try connecting to target */
  if (tnl_create_socket (sock, TNL_FLAG_SRC_TCP) == NULL)
    return -1;

  return 0;
}

/*
 * The tunnel servers TCP check_request routine for target connections.
 * Each TCP target connection gets assigned this function in order to
 * send data back to its source connection.
 */
int
tnl_check_request_tcp_target (socket_t sock)
{
  socket_t xsock = NULL;
  tnl_source_t *source;

  /* source is a TCP connection */
  if (sock->userflags & TNL_FLAG_SRC_TCP)
    {
      xsock = sock->referer;
    }
  /* source is an UDP connection */
  else if (sock->userflags & (TNL_FLAG_SRC_UDP | TNL_FLAG_SRC_ICMP))
    {
      source = sock->data;
      xsock = source->source_sock;
      xsock->remote_addr = source->ip;
      xsock->remote_port = source->port;
    }

  /* forward data to source connection */
  if (tnl_send_request_target (xsock, sock->recv_buffer, 
			       sock->recv_buffer_fill, 
			       sock->userflags) == -1)
    {
      sock_schedule_for_shutdown (xsock);
      return -1;
    }

  sock->recv_buffer_fill = 0;
  return 0;
}

/*
 * The tunnel servers TCP check_request routine for the source connections.
 * It simply copies the received data to the send buffer of the target
 * connection.
 */
int
tnl_check_request_tcp_source (socket_t sock)
{
  tnl_config_t *cfg = sock->cfg;

  /* forward data to target connection */
  if (tnl_send_request_source (sock->referer, sock->recv_buffer, 
			       sock->recv_buffer_fill, 
			       sock->userflags) == -1)
    {
      return -1;
    }

  sock->recv_buffer_fill = 0;
  return 0;
}

/*
 * This function is the handle_request routine for target UDP sockets.
 */
int
tnl_handle_request_udp_target (socket_t sock, char *packet, int len)
{
  tnl_config_t *cfg = sock->cfg;
  tnl_source_t *source;
  socket_t xsock = NULL;

  /* the source connection is TCP */
  if (sock->userflags & TNL_FLAG_SRC_TCP)
    {
      if ((xsock = sock->referer) == NULL)
	return -1;
    }
  /* source connection is UDP or ICMP */
  else if (sock->userflags & (TNL_FLAG_SRC_UDP | TNL_FLAG_SRC_ICMP))
    {
      /* get source connection from data field */
      source = sock->data;
      xsock = source->source_sock;
      xsock->remote_addr = source->ip;
      xsock->remote_port = source->port;
    } 

  /* forward packet data to source connection */
  if (tnl_send_request_target (xsock, packet, len, sock->userflags) == -1)
    {
      if (sock->userflags & TNL_FLAG_SRC_TCP)
	sock_schedule_for_shutdown (xsock);
      return -1;
    }

  return 0;
}

/*
 * This function is the handle_request routine for source UDP sockets.
 * It accepts UDP connections (listening connection) or forwards data
 * to existing target sockets.
 */
int
tnl_handle_request_udp_source (socket_t sock, char *packet, int len)
{
  tnl_config_t *cfg = sock->cfg;
  tnl_source_t *source;
  socket_t xsock = NULL;

  /* check if there is such a connection in the source hash already */
  source = hash_get (cfg->client, tnl_addr (sock));
  if (source)
    {
      /* get existing target socket */
      xsock = source->target_sock;
    }
  else
    {
      /* start connecting */
      if ((xsock = tnl_create_socket (sock, TNL_FLAG_SRC_UDP)) == NULL)
	return 0;

      /* foreign address not in hash, create new target connection */
      source = xmalloc (sizeof (tnl_source_t));
      source->source_sock = sock;
      source->ip = sock->remote_addr;
      source->port = sock->remote_port;
      hash_put (cfg->client, tnl_addr (sock), source);

      /* put the source connection into data field */
      xsock->data = source;
      source->target_sock = xsock;
    }

  /* forward packet data to target connection */
  if (tnl_send_request_source (xsock, packet, len, sock->userflags) == -1)
    {
      sock_schedule_for_shutdown (xsock);
      return 0;
    }

  return 0;
}

/*
 * This function is the handle_request routine for target ICMP sockets.
 */
int
tnl_handle_request_icmp_target (socket_t sock, char *packet, int len)
{
  tnl_config_t *cfg = sock->cfg;
  tnl_source_t *source;
  socket_t xsock = NULL;

  /* the source connection is TCP */
  if (sock->userflags & TNL_FLAG_SRC_TCP)
    {
      if ((xsock = sock->referer) == NULL)
	return -1;
    }
  /* source connection is UDP or ICMP */
  else if (sock->userflags & (TNL_FLAG_SRC_UDP | TNL_FLAG_SRC_ICMP))
    {
      /* get source connection from data field */
      source = sock->data;
      xsock = source->source_sock;
      xsock->remote_addr = source->ip;
      xsock->remote_port = source->port;
    } 

  /* forward packet data to source connection */
  if (tnl_send_request_target (xsock, packet, len, sock->userflags) == -1)
    {
      if (sock->userflags & TNL_FLAG_SRC_TCP)
	sock_schedule_for_shutdown (xsock);
      return -1;
    }

  return 0;
}

/*
 * This function is the handle_request routine for source ICMP sockets.
 * It accepts ICMP connections (listening connection) or forwards data
 * to existing target sockets.
 */
int
tnl_handle_request_icmp_source (socket_t sock, char *packet, int len)
{
  tnl_config_t *cfg = sock->cfg;
  tnl_source_t *source;
  socket_t xsock = NULL;

  /* check if there is such a connection in the source hash already */
  source = hash_get (cfg->client, tnl_addr (sock));
  if (source)
    {
      /* get existing target socket */
      xsock = source->target_sock;
    }
  else
    {
      /* start connecting */
      if ((xsock = tnl_create_socket (sock, TNL_FLAG_SRC_ICMP)) == NULL)
	return 0;

      /* foreign address not in hash, create new target connection */
      source = xmalloc (sizeof (tnl_source_t));
      source->source_sock = sock;
      source->ip = sock->remote_addr;
      source->port = sock->remote_port;
      hash_put (cfg->client, tnl_addr (sock), source);

      /* put the source connection into data field */
      xsock->data = source;
      source->target_sock = xsock;
    }

  /* forward packet data to target connection */
  if (tnl_send_request_source (xsock, packet, len, sock->userflags) == -1)
    {
      sock_schedule_for_shutdown (xsock);
      return 0;
    }

  return 0;
}

/*
 * What happens if a connection gets lost.
 */
int
tnl_disconnect (socket_t sock)
{
  tnl_config_t *cfg = sock->cfg;
  char *key;

  /* do not anything if we are shutting down */
  if (server_nuke_happened)
    return 0;

  /* if the source connection is ICMP send a disconnection message */
  if (sock->userflags & TNL_FLAG_SRC_ICMP && sock->referer)
    {
#if ENABLE_DEBUG
      log_printf (LOG_DEBUG, "tunnel: sending icmp disconnect\n");
#endif
      icmp_write (sock->referer, NULL, 0);
    }

  /* if source is TCP shutdown referring connection */
  if (sock->userflags & TNL_FLAG_SRC_TCP && sock->referer)
    {
#if ENABLE_DEBUG
      log_printf (LOG_DEBUG, "tunnel: shutdown referrer id %d\n",
		  sock->referer->id);
#endif
      sock_schedule_for_shutdown (sock->referer);
    }
  else
    {
      if ((key = hash_contains (cfg->client, sock->data)) != NULL)
	{
	  hash_delete (cfg->client, key);
	  xfree (sock->data);
	  sock->data = NULL;
	}
    }

  /* disable the referring socket structure */
  if (sock->referer)
    {
      sock->referer->referer = NULL;
      sock->referer = NULL;
    }
  return 0;
}

/*
 * Because UDP and ICMP sockets cannot not be detected as being closed
 * we need to shutdown target sockets ourselves.
 */
int
tnl_idle (socket_t sock)
{
  time_t t = time (NULL);

  if (t - sock->last_recv < TNL_TIMEOUT ||
      t - sock->last_send < TNL_TIMEOUT)
    {
      sock->idle_counter = TNL_TIMEOUT;
      return 0;
    }
  return -1;
}

#else /* not ENABLE_TUNNEL */

int tunnel_dummy; /* Shut compiler warnings up. */

#endif /* not ENABLE_TUNNEL */