/*
 * portcfg.c - port configuration implementation
 *
 * Copyright (C) 2011 Thien-Thi Nguyen
 * Copyright (C) 2001, 2002, 2003 Stefan Jahn <stefan@lkcc.org>
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this package.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <stdio.h>
#include "timidity.h"
#include <string.h>
#include <sys/types.h>

#ifndef __MINGW32__
# include <sys/socket.h>
#endif

#include "networking-headers.h"
#include "libserveez/alloc.h"
#include "libserveez/util.h"
#include "libserveez/core.h"
#include "libserveez/vector.h"
#include "libserveez/array.h"
#include "libserveez/portcfg.h"
#include "libserveez/udp-socket.h"
#include "libserveez/icmp-socket.h"
#include "libserveez/pipe-socket.h"
#include "libserveez/interface.h"

/* How much data is accepted before valid detection.  */
#define SOCK_MAX_DETECTION_FILL 16
/* How much time is accepted before valid detection.  */
#define SOCK_MAX_DETECTION_WAIT 30

static int
any_p (const char *addr)
{
  return !strcmp (SVZ_PORTCFG_ANY, addr);
}

static int
no_ip_p (const char *addr)
{
  return !strcmp (SVZ_PORTCFG_NOIP, addr);
}

/*
 * This hash holds all port configurations created by the configuration
 * file.
 */
static svz_hash_t *svz_portcfgs = NULL;

/*
 * Return the pointer of the @code{sockaddr_in} structure of the given
 * port configuration @var{port} if it is a network port configuration.
 * Otherwise return @code{NULL}.
 */
struct sockaddr_in *
svz_portcfg_addr (svz_portcfg_t *port)
{
#define SIMPLE(up,dn)                                           \
  case SVZ_PROTO_ ## up :  return &port->protocol. dn .addr

  switch (port->proto)
    {
      SIMPLE (TCP, tcp);
      SIMPLE (UDP, udp);
      SIMPLE (ICMP, icmp);
      SIMPLE (RAW, raw);
    default: return NULL;
    }
#undef SIMPLE
}

/*
 * Return the pointer to the ip address @code{ipaddr} of the given
 * port configuration @var{port} if it is a network port configuration.
 * Otherwise return @code{NULL}.
 */
char *
svz_portcfg_ipaddr (svz_portcfg_t *port)
{
#define SIMPLE(up,dn)                                           \
  case SVZ_PROTO_ ## up :  return port->protocol. dn .ipaddr

  switch (port->proto)
    {
      SIMPLE (TCP, tcp);
      SIMPLE (UDP, udp);
      SIMPLE (ICMP, icmp);
      SIMPLE (RAW, raw);
    default: return NULL;
    }
#undef SIMPLE
}

/*
 * Return the network device name stored in the given port
 * configuration @var{port} if it is a network port configuration.  The
 * returned pointer can be @code{NULL} if there is no such device set
 * or if the port configuration is not a network port configuration.
 */
char *
svz_portcfg_device (svz_portcfg_t *port)
{
#define SIMPLE(up,dn)                                           \
  case SVZ_PROTO_ ## up :  return port->protocol. dn .device

  switch (port->proto)
    {
      SIMPLE (TCP, tcp);
      SIMPLE (UDP, udp);
      SIMPLE (ICMP, icmp);
      SIMPLE (RAW, raw);
    default: return NULL;
    }
#undef SIMPLE
}

/*
 * Return the UDP or TCP port of the given port configuration or zero
 * if it neither TCP nor UDP.
 */
unsigned short
svz_portcfg_port (svz_portcfg_t *port)
{
#define SIMPLE(up,dn)                                           \
  case SVZ_PROTO_ ## up :  return port->protocol. dn .port

  switch (port->proto)
    {
      SIMPLE (TCP, tcp);
      SIMPLE (UDP, udp);
    default: return 0;
    }
#undef SIMPLE
}

/*
 * Create a new blank port configuration.
 */
svz_portcfg_t *
svz_portcfg_create (void)
{
  svz_portcfg_t *port = svz_calloc (sizeof (svz_portcfg_t));
  return port;
}

/*
 * Return 1 if the devices of port-configs A and B are the same, else 0.
 */
static int
same_devices (svz_portcfg_t *a, svz_portcfg_t *b)
{
  char *sa = svz_portcfg_device (a);
  char *sb = svz_portcfg_device (b);

  /* Apparently, ‘svz_portcfg_device’ can return NULL,
     which elicits a warning from gcc for ‘strcmp’.  */
  return !strcmp (sa ? sa : "",
                  sb ? sb : "");
}

/*
 * Check if two given port configurations structures are equal i.e.
 * specifying the same network port or pipe files.  Returns
 * @code{SVZ_PORTCFG_EQUAL} if @var{a} and @var{b} are identical,
 * @code{SVZ_PORTCFG_MATCH} if the network address of either port
 * configurations contains the other (INADDR_ANY match) and otherwise
 * @code{SVZ_PORTCFG_NOMATCH} or possibly @code{SVZ_PORTCFG_CONFLICT}.
 */
int
svz_portcfg_equal (svz_portcfg_t *a, svz_portcfg_t *b)
{
  struct sockaddr_in *a_addr, *b_addr;

  if ((a->proto & (SVZ_PROTO_TCP | SVZ_PROTO_UDP |
                   SVZ_PROTO_ICMP | SVZ_PROTO_RAW)) &&
      (a->proto == b->proto))
    {
      /* Two network ports are equal if both local port and IP address
         are equal or one of them is INADDR_ANY.  */
      a_addr = svz_portcfg_addr (a);
      b_addr = svz_portcfg_addr (b);

      switch (a->proto)
        {
        case SVZ_PROTO_UDP:
        case SVZ_PROTO_TCP:
          if (a_addr->sin_port == b_addr->sin_port)
            {
              if ((a->flags & PORTCFG_FLAG_DEVICE) ||
                  (b->flags & PORTCFG_FLAG_DEVICE))
                {
                  if ((a->flags & PORTCFG_FLAG_DEVICE) &&
                      (b->flags & PORTCFG_FLAG_DEVICE))
                    {
                      if (same_devices (a, b))
                        return SVZ_PORTCFG_EQUAL;
                      return SVZ_PORTCFG_NOMATCH;
                    }
                  return SVZ_PORTCFG_CONFLICT;
                }
              if (a_addr->sin_addr.s_addr == b_addr->sin_addr.s_addr)
                return SVZ_PORTCFG_EQUAL;
              if (a->flags & PORTCFG_FLAG_ANY || b->flags & PORTCFG_FLAG_ANY)
                return SVZ_PORTCFG_MATCH;
            }
          break;
        case SVZ_PROTO_ICMP:
          if (a->icmp_type == b->icmp_type)
            {
              if ((a->flags & PORTCFG_FLAG_DEVICE) ||
                  (b->flags & PORTCFG_FLAG_DEVICE))
                {
                  if ((a->flags & PORTCFG_FLAG_DEVICE) &&
                      (b->flags & PORTCFG_FLAG_DEVICE) &&
                      same_devices (a, b))
                    return SVZ_PORTCFG_EQUAL;
                  return SVZ_PORTCFG_CONFLICT;
                }
              if (a_addr->sin_addr.s_addr == b_addr->sin_addr.s_addr)
                return SVZ_PORTCFG_EQUAL;
              if (a->flags & PORTCFG_FLAG_ANY || b->flags & PORTCFG_FLAG_ANY)
                return SVZ_PORTCFG_MATCH;
            }
          break;
        case SVZ_PROTO_RAW:
          if ((a->flags & PORTCFG_FLAG_DEVICE) ||
              (b->flags & PORTCFG_FLAG_DEVICE))
            {
              if ((a->flags & PORTCFG_FLAG_DEVICE) &&
                  (b->flags & PORTCFG_FLAG_DEVICE) &&
                  same_devices (a, b))
                return SVZ_PORTCFG_EQUAL;
              return SVZ_PORTCFG_CONFLICT;
            }
          if (a_addr->sin_addr.s_addr == b_addr->sin_addr.s_addr)
            return SVZ_PORTCFG_EQUAL;
          if (a->flags & PORTCFG_FLAG_ANY || b->flags & PORTCFG_FLAG_ANY)
            return SVZ_PORTCFG_MATCH;
          break;
        }
    }
  else if (a->proto & SVZ_PROTO_PIPE && a->proto == b->proto)
    {
#define XNAME(p,member)  SVZ_CFG_PIPE (p, member).name
      /*
       * Two pipe configs are equal if they use the same files.
       */
      if (!strcmp (XNAME (a, recv), XNAME (b, recv)) &&
          !strcmp (XNAME (a, send), XNAME (b, send)))
        return SVZ_PORTCFG_EQUAL;
#undef XNAME
    }

  /* Do not even the same proto flag -> cannot be equal.  */
  return SVZ_PORTCFG_NOMATCH;
}

/*
 * Add the given port configuration @var{port} associated with the name
 * @var{name} to the list of known port configurations.  Return @code{NULL}
 * on errors.  If the return port configuration equals the given port
 * configuration  the given one has been successfully added.
 */
svz_portcfg_t *
svz_portcfg_add (char *name, svz_portcfg_t *port)
{
  svz_portcfg_t *replace;

  /* Do not handle invalid arguments.  */
  if (name == NULL || port == NULL)
    return NULL;

  /* Check if the port configuration hash is inited.  */
  if (svz_portcfgs == NULL)
    {
      if ((svz_portcfgs = svz_hash_create (4, (svz_free_func_t)
                                           svz_portcfg_free)) == NULL)
        return NULL;
    }

  /* Try adding a new port configuration.  */
  if ((replace = svz_hash_get (svz_portcfgs, name)) != NULL)
    {
#if ENABLE_DEBUG
      svz_log (SVZ_LOG_DEBUG, "portcfg `%s' already registered\n", name);
#endif
      svz_hash_put (svz_portcfgs, name, port);
      return replace;
    }
  svz_hash_put (svz_portcfgs, name, port);
  return port;
}

/*
 * This function can be used to set the character string representation
 * of a the port configuration @var{this} in dotted decimal form
 * (@var{ipaddr}).  Returns zero on success, non-zero otherwise.
 */
static int
svz_portcfg_set_ipaddr (svz_portcfg_t *this, char *ipaddr)
{
  if (!this || !ipaddr)
    return -1;

  switch (this->proto)
    {
    case SVZ_PROTO_TCP:
      svz_free_and_zero (this->tcp_ipaddr);
      this->tcp_ipaddr = ipaddr;
      break;
    case SVZ_PROTO_UDP:
      svz_free_and_zero (this->udp_ipaddr);
      this->udp_ipaddr = ipaddr;
      break;
    case SVZ_PROTO_ICMP:
      svz_free_and_zero (this->icmp_ipaddr);
      this->icmp_ipaddr = ipaddr;
      break;
    case SVZ_PROTO_RAW:
      svz_free_and_zero (this->raw_ipaddr);
      this->raw_ipaddr = ipaddr;
      break;
    default:
      return -1;
    }
  return 0;
}

/*
 * Expand the given port configuration @var{this} if it is a network port
 * configuration and if the network ip address is @code{INADDR_ANY}.  Return
 * an array of port configurations which are copies of the given.
 */
svz_array_t *
svz_portcfg_expand (svz_portcfg_t *this)
{
  svz_array_t *ports = svz_array_create (1, NULL);
  svz_portcfg_t *port;
  struct sockaddr_in *addr;
  int n;
  svz_interface_t *ifc;

  /* Is this a network port configuration and should it be expanded?  */
  if ((addr = svz_portcfg_addr (this)) != NULL &&
      (this->flags & PORTCFG_FLAG_ALL) && !(this->flags & PORTCFG_FLAG_DEVICE))
    {
      svz_vector_foreach (svz_interfaces, ifc, n)
        {
          port = svz_portcfg_dup (this);
          addr = svz_portcfg_addr (port);
          addr->sin_addr.s_addr = ifc->ipaddr;
          svz_portcfg_set_ipaddr (port,
                                  svz_strdup (svz_inet_ntoa (ifc->ipaddr)));
          svz_array_add (ports, port);
        }
    }
  /* No, just add the given port configuration.  */
  else
    {
      port = svz_portcfg_dup (this);
      svz_array_add (ports, port);
    }
  return ports;
}

/*
 * Make a copy of the given port configuration @var{port}.  This function
 * is used in @code{svz_portcfg_expand}.
 */
svz_portcfg_t *
svz_portcfg_dup (svz_portcfg_t *port)
{
  svz_portcfg_t *copy;

  /* Return NULL if necessary.  */
  if (port == NULL)
    return NULL;

  /* First plain copy.  */
  copy = svz_malloc (sizeof (svz_portcfg_t));
  memcpy (copy, port, sizeof (svz_portcfg_t));
  copy->name = svz_strdup (port->name);

  /* Depending on the protocol, copy various strings.  */
  switch (port->proto)
    {
    case SVZ_PROTO_TCP:
      copy->tcp_ipaddr = svz_strdup (port->tcp_ipaddr);
      copy->tcp_device = svz_strdup (port->tcp_device);
      break;
    case SVZ_PROTO_UDP:
      copy->udp_ipaddr = svz_strdup (port->udp_ipaddr);
      copy->udp_device = svz_strdup (port->udp_device);
      break;
    case SVZ_PROTO_ICMP:
      copy->icmp_ipaddr = svz_strdup (port->icmp_ipaddr);
      copy->icmp_device = svz_strdup (port->icmp_device);
      break;
    case SVZ_PROTO_RAW:
      copy->raw_ipaddr = svz_strdup (port->raw_ipaddr);
      copy->raw_device = svz_strdup (port->raw_device);
      break;
    case SVZ_PROTO_PIPE:
#define COPY(x)                                                         \
      SVZ_CFG_PIPE (copy, x) = svz_strdup (SVZ_CFG_PIPE (port, x))

      COPY (recv.name);
      COPY (recv.user);
      COPY (recv.group);
      COPY (send.name);
      COPY (send.user);
      COPY (send.group);
      break;
#undef COPY
    }

  copy->accepted = NULL;

  /* Make a copy of the "deny" and "allow" access lists.  */
  copy->allow = svz_array_strdup (port->allow);
  copy->deny = svz_array_strdup (port->deny);

  return copy;
}

/*
 * This function frees all resources allocated by the given port
 * configuration @var{port}.
 */
void
svz_portcfg_free (svz_portcfg_t *port)
{
  /* Free the name of the port configuration.  */
  svz_free (port->name);

  /* Depending on the type of configuration perform various operations.  */
  switch (port->proto)
    {
    case SVZ_PROTO_TCP:
      svz_free (port->tcp_ipaddr);
      svz_free (port->tcp_device);
      break;
    case SVZ_PROTO_UDP:
      svz_free (port->udp_ipaddr);
      svz_free (port->udp_device);
      break;
    case SVZ_PROTO_ICMP:
      svz_free (port->icmp_ipaddr);
      svz_free (port->icmp_device);
      break;
    case SVZ_PROTO_RAW:
      svz_free (port->raw_ipaddr);
      svz_free (port->raw_device);
      break;
    case SVZ_PROTO_PIPE:
#define FREE(x)                                 \
      svz_free (SVZ_CFG_PIPE (port, x))

      FREE (recv.user);
      FREE (recv.name);
      FREE (recv.group);
      FREE (send.user);
      FREE (send.name);
      FREE (send.group);
      break;
#undef FREE
    }

  /* Destroy access and connection list.  */
  if (port->deny)
    {
      svz_array_destroy (port->deny);
      port->deny = NULL;
    }
  if (port->allow)
    {
      svz_array_destroy (port->allow);
      port->allow = NULL;
    }
  if (port->accepted)
    {
      svz_hash_destroy (port->accepted);
      port->accepted = NULL;
    }

  /* Free the port configuration itself.  */
  svz_free (port);
}

/*
 * This function makes the given port configuration @var{port} completely
 * unusable.  No operation is performed if @var{port} is @code{NULL}.  If the
 * port configuration is part of the list of known port configurations it
 * it thrown out of them.
 */
void
svz_portcfg_destroy (svz_portcfg_t *port)
{
  char *name;

  /* Return here if NULL pointer given.  */
  if (port == NULL)
    return;

  /* Delete from port configuration hash if necessary.  */
  if (svz_portcfgs && (name = svz_hash_contains (svz_portcfgs, port)) != NULL)
    svz_hash_delete (svz_portcfgs, name);

  /* Free the port configuration.  */
  svz_portcfg_free (port);
}

/*
 * Return the port configuration associated with the given name @var{name}.
 * This function returns @code{NULL} on errors.
 */
svz_portcfg_t *
svz_portcfg_get (char *name)
{
  /* Do not handle invalid arguments.  */
  if (name == NULL || svz_portcfgs == NULL)
    return NULL;

  return svz_hash_get (svz_portcfgs, name);
}

/*
 * Delete the list of known port configurations.  This routine should
 * definitely called from the core library's finalizer.
 */
void
svz_portcfg_finalize (void)
{
  if (svz_portcfgs != NULL)
    {
      svz_hash_destroy (svz_portcfgs);
      svz_portcfgs = NULL;
    }
}

/*
 * Converts the given network address @var{str} either given in dotted
 * decimal form or as network interface name and saves the result in the
 * @code{sockaddr_in.sin_addr.s_addr} field.  Return zero on success.
 */
static int
svz_portcfg_convert_addr (char *str, struct sockaddr_in *addr)
{
  svz_interface_t *ifc;

  if ((ifc = svz_interface_search (str)) != NULL)
    {
#if ENABLE_DEBUG
      svz_log (SVZ_LOG_DEBUG, "`%s' is %s\n", ifc->description,
               svz_inet_ntoa (ifc->ipaddr));
#endif
      addr->sin_addr.s_addr = ifc->ipaddr;
      return 0;
    }
  return svz_inet_aton (str, addr);
}

/*
 * Construct the @code{sockaddr_in} fields from the @code{ipaddr} field.
 * Returns zero if it worked.  If it does not work the @code{ipaddr} field
 * did not consist of an ip address in dotted decimal form.
 */
int
svz_portcfg_mkaddr (svz_portcfg_t *this)
{
  int err = 0;

  switch (this->proto)
    {
      /* For all network protocols we assign AF_INET as protocol family,
         determine the network port (if necessary) and put the ip address.  */
    case SVZ_PROTO_TCP:
      this->tcp_addr.sin_family = AF_INET;
      if (svz_portcfg_device (this))
        {
          this->flags |= PORTCFG_FLAG_DEVICE;
          this->tcp_addr.sin_addr.s_addr = INADDR_ANY;
        }
      else if (this->tcp_ipaddr == NULL)
        {
          svz_log (SVZ_LOG_ERROR, "%s: no TCP/IP address given\n", this->name);
          err = -1;
        }
      else if (any_p (this->tcp_ipaddr))
        {
          this->flags |= PORTCFG_FLAG_ANY;
          this->tcp_addr.sin_addr.s_addr = INADDR_ANY;
        }
      else if (no_ip_p (this->tcp_ipaddr))
        {
          this->flags |= PORTCFG_FLAG_ALL;
          this->tcp_addr.sin_addr.s_addr = INADDR_ANY;
        }
      else
        {
          err = svz_portcfg_convert_addr (this->tcp_ipaddr, &this->tcp_addr);
          if (err)
            {
              svz_log (SVZ_LOG_ERROR, "%s: `%s' is not a valid IP address\n",
                       this->name, this->tcp_ipaddr);
            }
        }
      this->tcp_addr.sin_port = htons (this->tcp_port);
      if (this->tcp_backlog > SOMAXCONN)
        {
          svz_log (SVZ_LOG_ERROR, "%s: TCP backlog out of range (1..%d)\n",
                   this->name, SOMAXCONN);
          err = -1;
        }
      break;
    case SVZ_PROTO_UDP:
      this->udp_addr.sin_family = AF_INET;
      if (svz_portcfg_device (this))
        {
          this->flags |= PORTCFG_FLAG_DEVICE;
          this->udp_addr.sin_addr.s_addr = INADDR_ANY;
        }
      else if (this->udp_ipaddr == NULL)
        {
          svz_log (SVZ_LOG_ERROR, "%s: no UDP/IP address given\n", this->name);
          err = -1;
        }
      else if (any_p (this->udp_ipaddr))
        {
          this->flags |= PORTCFG_FLAG_ANY;
          this->udp_addr.sin_addr.s_addr = INADDR_ANY;
        }
      else if (no_ip_p (this->tcp_ipaddr))
        {
          this->flags |= PORTCFG_FLAG_ALL;
          this->udp_addr.sin_addr.s_addr = INADDR_ANY;
        }
      else
        {
          err = svz_portcfg_convert_addr (this->udp_ipaddr, &this->udp_addr);
          if (err)
            {
              svz_log (SVZ_LOG_ERROR, "%s: `%s' is not a valid IP address\n",
                       this->name, this->udp_ipaddr);
            }
        }
      this->udp_addr.sin_port = htons (this->udp_port);
      break;
    case SVZ_PROTO_ICMP:
      if (svz_portcfg_device (this))
        {
          this->flags |= PORTCFG_FLAG_DEVICE;
          this->icmp_addr.sin_addr.s_addr = INADDR_ANY;
        }
      else if (this->icmp_ipaddr == NULL)
        {
          svz_log (SVZ_LOG_ERROR, "%s: no ICMP/IP address given\n", this->name);
          err = -1;
        }
      else
        {
          err = svz_portcfg_convert_addr (this->icmp_ipaddr, &this->icmp_addr);
          if (err)
            {
              svz_log (SVZ_LOG_ERROR, "%s: `%s' is not a valid IP address\n",
                       this->name, this->icmp_ipaddr);
            }
        }
      this->icmp_addr.sin_family = AF_INET;
      break;
    case SVZ_PROTO_RAW:
      if (svz_portcfg_device (this))
        {
          this->flags |= PORTCFG_FLAG_DEVICE;
          this->raw_addr.sin_addr.s_addr = INADDR_ANY;
        }
      else if (this->raw_ipaddr == NULL)
        {
          svz_log (SVZ_LOG_ERROR, "%s: no IP address given\n", this->name);
          err = -1;
        }
      else
        {
          err = svz_portcfg_convert_addr (this->raw_ipaddr, &this->raw_addr);
          if (err)
            {
              svz_log (SVZ_LOG_ERROR, "%s: `%s' is not a valid IP address\n",
                       this->name, this->raw_ipaddr);
            }
        }
      this->raw_addr.sin_family = AF_INET;
      break;
      /* The pipe protocol needs a check for the validity of the permissions,
         the group and user names and its id's.  */
    case SVZ_PROTO_PIPE:
      {
        svz_pipe_t *r = &SVZ_CFG_PIPE (this, recv);
        svz_pipe_t *s = &SVZ_CFG_PIPE (this, send);

        if (r->name == NULL)
          {
            svz_log (SVZ_LOG_ERROR, "%s: no receiving pipe file given\n",
                     this->name);
            err = -1;
          }
        else
          {
            err |= svz_pipe_check_user (r);
            err |= svz_pipe_check_group (r);
          }
        if (s->name == NULL)
          {
            svz_log (SVZ_LOG_ERROR, "%s: no sending pipe file given\n",
                     this->name);
            err = -1;
          }
        else
          {
            err |= svz_pipe_check_user (s);
            err |= svz_pipe_check_group (s);
          }
      }
      break;
    default:
      err = 0;
    }
  return err;
}

/*
 * Prepare the given port configuration @var{port}.  Fill in default values
 * for yet undefined variables.
 */
void
svz_portcfg_prepare (svz_portcfg_t *port)
{
  /* Check the TCP backlog value.  */
  if (port->proto & SVZ_PROTO_TCP)
    {
      if (port->tcp_backlog <= 0 || port->tcp_backlog > SOMAXCONN)
        port->tcp_backlog = SOMAXCONN;
    }
  /* Check the detection barriers for pipe and tcp sockets.  */
  if (port->proto & (SVZ_PROTO_PIPE | SVZ_PROTO_TCP))
    {
      if (port->detection_fill <= 0 ||
          port->detection_fill > SOCK_MAX_DETECTION_FILL)
        port->detection_fill = SOCK_MAX_DETECTION_FILL;
      if (port->detection_wait <= 0 ||
          port->detection_wait > SOCK_MAX_DETECTION_WAIT)
        port->detection_wait = SOCK_MAX_DETECTION_WAIT;
    }
  /* Check the initial send and receive buffer sizes.  */
  if (port->send_buffer_size <= 0 ||
      port->send_buffer_size >= MAX_BUF_SIZE)
    {
      if (port->proto & (SVZ_PROTO_TCP | SVZ_PROTO_PIPE))
        port->send_buffer_size = SEND_BUF_SIZE;
      else if (port->proto & SVZ_PROTO_UDP)
        port->send_buffer_size = SVZ_UDP_BUF_SIZE;
      else if (port->proto & (SVZ_PROTO_ICMP | SVZ_PROTO_RAW))
        port->send_buffer_size = ICMP_BUF_SIZE;
    }
  if (port->recv_buffer_size <= 0 ||
      port->recv_buffer_size >= MAX_BUF_SIZE)
    {
      if (port->proto & (SVZ_PROTO_TCP | SVZ_PROTO_PIPE))
        port->recv_buffer_size = RECV_BUF_SIZE;
      else if (port->proto & SVZ_PROTO_UDP)
        port->recv_buffer_size = SVZ_UDP_BUF_SIZE;
      else if (port->proto & (SVZ_PROTO_ICMP | SVZ_PROTO_RAW))
        port->recv_buffer_size = ICMP_BUF_SIZE;
    }
  /* Check the connection frequency.  */
  if (port->connect_freq <= 0)
    {
      /* Sane value is: 100 connections per second.  */
      port->connect_freq = 100;
    }
}

/*
 * Helper function for the below routine.  Converts a Internet network
 * address or the appropiate device into a text representation.
 */
static char *
svz_portcfg_addr_text (svz_portcfg_t *port, struct sockaddr_in *addr)
{
  if (port->flags & PORTCFG_FLAG_DEVICE)
    return svz_portcfg_device (port);
  else if (port->flags & PORTCFG_FLAG_ANY)
    return "*";
  return svz_inet_ntoa (addr->sin_addr.s_addr);
}

/*
 * This function returns a simple text representation of the given port
 * configuration @var{port}.  The returned character string is statically
 * allocated.  Thus you cannot use it twice in argument lists.
 */
char *
svz_portcfg_text (svz_portcfg_t *port, int *lenp)
{
#define TEXT_SIZE  128
  static char text[TEXT_SIZE];
  struct sockaddr_in *addr;
  int len;

  /* TCP and UDP */
  if (port->proto & (SVZ_PROTO_TCP | SVZ_PROTO_UDP))
    {
      addr = svz_portcfg_addr (port);
      len = snprintf (text, TEXT_SIZE, "%s:[%s:%d]",
                      (port->proto & SVZ_PROTO_TCP) ? "TCP" : "UDP",
                      svz_portcfg_addr_text (port, addr),
                      ntohs (addr->sin_port));
    }
  /* RAW and ICMP */
  else if (port->proto & (SVZ_PROTO_RAW | SVZ_PROTO_ICMP))
    {
      int icmp_p = port->proto & SVZ_PROTO_ICMP;

      addr = svz_portcfg_addr (port);
      len = snprintf (text, TEXT_SIZE, "%s:[%s%s%s]",
                      (port->proto & SVZ_PROTO_RAW) ? "RAW" : "ICMP",
                      svz_portcfg_addr_text (port, addr),
                      icmp_p ? "/" : "",
                      icmp_p ? svz_itoa (port->icmp_type) : "");
    }
  /* PIPE */
  else if (port->proto & SVZ_PROTO_PIPE)
    len = snprintf (text, TEXT_SIZE, "PIPE:[%s]-[%s]",
                    SVZ_CFG_PIPE (port, recv.name),
                    SVZ_CFG_PIPE (port, send.name));
  else
    {
      text[0] = '\0';
      len = 0;
    }
  if (lenp)
    *lenp = len;
  return text;
#undef TEXT_SIZE
}

/*
 * Debug helper: Emit a printable representation of the the given
 * port configuration to the given FILE stream @var{f}.
 */
void
svz_portcfg_print (svz_portcfg_t *this, FILE *f)
{
  if (NULL == this)
    {
      fprintf (f, "portcfg is NULL\n");
      return;
    }

  switch (this->proto)
    {
    case SVZ_PROTO_TCP:
      fprintf (f, "portcfg `%s': TCP (%s|%s):%d\n", this->name,
               this->tcp_ipaddr,
               svz_inet_ntoa (this->tcp_addr.sin_addr.s_addr),
               this->tcp_port);
      break;
    case SVZ_PROTO_UDP:
      fprintf (f, "portcfg `%s': UDP (%s|%s):%d\n", this->name,
               this->udp_ipaddr,
               svz_inet_ntoa (this->udp_addr.sin_addr.s_addr),
               this->udp_port);
      break;
    case SVZ_PROTO_ICMP:
      fprintf (f, "portcfg `%s': ICMP (%s|%s)\n", this->name,
               this->icmp_ipaddr,
               svz_inet_ntoa (this->icmp_addr.sin_addr.s_addr));
      break;
    case SVZ_PROTO_RAW:
      fprintf (f, "portcfg `%s': RAW (%s|%s)\n", this->name,
               this->raw_ipaddr,
               svz_inet_ntoa (this->raw_addr.sin_addr.s_addr));
      break;
    case SVZ_PROTO_PIPE:
      {
        svz_pipe_t *r = &SVZ_CFG_PIPE (this, recv);
        svz_pipe_t *s = &SVZ_CFG_PIPE (this, send);

        fprintf (f, "portcfg `%s': PIPE "
                 "(\"%s\", \"%s\" (%d), \"%s\" (%d), %04o)<->"
                 "(\"%s\", \"%s\" (%d), \"%s\" (%d), %04o)\n", this->name,
                 r->name, r->user, r->uid, r->group, r->gid, r->perm,
                 s->name, s->user, s->uid, s->group, s->gid, s->perm);
      }
      break;
    default:
      fprintf (f, "portcfg has invalid proto field %d\n", this->proto);
    }
}
