/*
 * tunnel.h - port forward definition header
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
 * $Id: tunnel.h,v 1.5 2000/10/28 13:03:11 ela Exp $
 *
 */

#ifndef __TUNNEL_H__
#define __TUNNEL_H__

#if HAVE_CONFIG_H
# include <config.h>
#endif

#define _GNU_SOURCE
#include "socket.h"
#include "hash.h"
#include "server.h"

/*
 * Tunnel server configuration structure.
 */
typedef struct
{
  portcfg_t *source;  /* the source port to forward from */
  portcfg_t *target;  /* target port to forward to */
  hash_t *client;     /* source client hash */
}
tnl_config_t;

/* the referrer connection structure */
typedef struct
{
  unsigned long ip;     /* the ip address to send to */
  unsigned short port;  /* port to send to */
  socket_t source_sock; /* source socket structure */
  socket_t target_sock; /* target socket */
}
tnl_source_t;

/* tunnel server specific protocol flags */
#define TNL_TIMEOUT       30
#define TNL_FLAG_SRC_TCP  0x0001
#define TNL_FLAG_SRC_UDP  0x0002
#define TNL_FLAG_SRC_ICMP 0x0004
#define TNL_FLAG_TGT_TCP  0x0008
#define TNL_FLAG_TGT_UDP  0x0010
#define TNL_FLAG_TGT_ICMP 0x0020
#define TNL_FLAG_SRC (TNL_FLAG_SRC_TCP | TNL_FLAG_SRC_UDP | TNL_FLAG_SRC_ICMP)
#define TNL_FLAG_TGT (TNL_FLAG_TGT_TCP | TNL_FLAG_TGT_UDP | TNL_FLAG_TGT_ICMP)

/*
 * Basic server callback definitions.
 */
int tnl_init (server_t *server);
int tnl_global_init (void);
int tnl_finalize (server_t *server);
int tnl_global_finalize (void);

/* Rest of all the callbacks. */
int tnl_detect_proto (void *cfg, socket_t sock);
int tnl_connect_socket (void *config, socket_t sock);
int tnl_check_request_tcp_source (socket_t sock);
int tnl_check_request_tcp_target (socket_t sock);
int tnl_handle_request_udp_source (socket_t sock, char *packet, int len);
int tnl_handle_request_udp_target (socket_t sock, char *packet, int len);
int tnl_handle_request_icmp_source (socket_t sock, char *packet, int len);
int tnl_handle_request_icmp_target (socket_t sock, char *packet, int len);
int tnl_disconnect (socket_t sock);
int tnl_idle (socket_t sock);

/*
 * This server's definition.
 */
extern server_definition_t tnl_server_definition;

#endif /* __TUNNEL_H__ */