/*
 * guile.c - interface to guile core library
 *
 * Copyright (C) 2001 Stefan Jahn <stefan@lkcc.org>
 * Copyright (C) 2001 Raimund Jacob <raimi@lkcc.org>
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this package; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * $Id: guile.c,v 1.6 2001/04/13 22:17:42 raimi Exp $
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#define _GNU_SOURCE
#include <guile/gh.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "libserveez.h"

#define valid_port(port) ((port) > 0 && (port) < 65536)

/*
 * What is an 'optionhash' ?
 * We build up that data structure from a scheme pairlist. The pairlist has to
 * be an alist: a key => value mapping.
 * We read that mapping and construct a svz_hash_t from it. The values of
 * that hash are value_t struct*.
 * The value_t struct contains a 'defined' field which counts the number of
 * occurences of the key. Use validate_optionhash to make sure it is 1 for
 * each key.
 * The 'use' field is to make sure that each key was needed exactly once.
 * Use validate_optionhash again to find out which ones were not needed.
 */

/*
 * Used as value in option-hashes.
 */
typedef struct
{
  SCM value;     /* the scheme value itself, invalid when defined != 1 */
  int defined;   /* the number of definitions. 1 to be valid */
  int use;       /* how often was it looked up in the hash. 1 to be valid */
} value_t;


/*
 * create a new value_t with the given value. initializes use to 0.
 * define-counter is set to 1.
 */
static value_t *
new_value_t (SCM value)
{
  value_t *v = (value_t*) svz_malloc (sizeof (value_t));
  v->value = value;
  v->defined = 1;
  v->use = 0;
  return v;
}

/*
 * Report some error at the current scheme position. Prints to stderr
 * but lets the program continue. Format does not need trailing newline.
 */
static void
report_error (const char* format, ...)
{
  va_list args;
  SCM lp = scm_current_load_port ();
  char *file = SCM_PORTP (lp) ? gh_scm2newstr (SCM_FILENAME (lp), NULL) : NULL;

  fprintf (stderr, "%s:%d:%d: ", file ? file : "file", 
	   SCM_PORTP (lp) ? SCM_LINUM (lp) : 0, 
	   SCM_PORTP (lp) ? SCM_COL (lp) : 0);
  if (file)
    free (file);

  va_start (args, format);
  vfprintf (stderr, format, args);
  va_end (args);

  fprintf (stderr, "\n");
}

/* ********************************************************************** */

/*
 * converts SCM to char * no matter if it is String or Symbol. returns NULL
 * when it was neither. new string must be explicitly freed.
 */
#define guile2str(scm) \
  (gh_string_p (scm) ? gh_scm2newstr (scm, NULL) : \
  (gh_symbol_p (scm) ? gh_symbol2newstr (scm, NULL) : NULL))


/* ********************************************************************** */

/*
 * validate the values of the optionhhash:
 * what = 0: check if all 'use' fields are 1
 * what = 1: check if all 'defined' fields are 1
 * type    : what kind of thing the optionhash belongs to
 * name    : current variable name
 * returns number of errors
 */
static int
validate_optionhash (svz_hash_t *hash, int what, char *type, char *name)
{
  int errors = 0;
  char **keys; int i;
  svz_hash_foreach_key (hash, keys, i)
    {
      char *key = keys[i];
      value_t *value = (value_t*) svz_hash_get (hash, key);

      if (what)
	{
	  if (value->defined != 1)
	    {
	      errors++;
	      report_error("`%s' is defined multiple times in %s `%s'",
			   key, type, name);
	    }
	}
      else
	{
	  if (value->use == 0)
	    {
	      errors++;
	      report_error("`%s' is defined but never used in %s `%s'",
			   key, type, name);
	    }
	}

    }

  return errors;
}


/*
 * get from an optionhash and increment the 'use' field.
 * returns SCM_UNSPECIFIED when key was not found.
 */
static SCM
optionhash_get (svz_hash_t *hash, char *key)
{
  value_t *val = (value_t*) svz_hash_get (hash, key);
  if (NULL != val)
    {
      val->use++;
      return val->value;
    }

  return SCM_UNSPECIFIED;
}


/*
 * traverse a scheme pairlist that is an associative list and build up
 * a hash from it. emits error messages and returns NULL when it did so.
 * hash keys are the key names. hash values are value_t struct *.
 */
static svz_hash_t *
guile2optionhash (SCM pairlist)
{
  svz_hash_t *hash = svz_hash_create (10);
  value_t *old_value;
  value_t *new_value;
  int err = 0;

  /* have to unpack that list again... why ? */
  pairlist = gh_car (pairlist);

  for ( ; gh_pair_p (pairlist); pairlist = gh_cdr (pairlist))
    {
      SCM pair = gh_car (pairlist);
      SCM key, val;
      char *tmp = NULL;

      /* the car must be another pair which contains key and value */
      if (!gh_pair_p (pair))
	{
	  report_error ("not a pair");
	  err = 1;
	  break;
	}
      key = gh_car (pair);
      val = gh_cdr (pair);

      if (NULL == (tmp = guile2str (key)))
	{
	  /* unknown key type. must be string or symbol */
	  report_error ("must be string or symbol");
	  err = 1;
	  break;
	}

      /* remember key and free it */
      new_value = new_value_t (val);
      old_value = svz_hash_get (hash, tmp);

      if (NULL != old_value)
	{
	  /* multiple definition. let caller croak about that error. */
	  new_value->defined += old_value->defined;
	  svz_free_and_zero (old_value);
	}
      svz_hash_put (hash, tmp, (void*) new_value);
      free (tmp);
    }

  /* pairlist must be gh_null_p() now or that was not a good pairlist... */
  if (!err && !gh_null_p (pairlist))
    {
      report_error ("invalid pairlist");
      err = 1;
    }

  if (err)
    {
      svz_hash_destroy (hash);
      return NULL;
    }

  return hash;
}


/* ********************************************************************** */

/*
 * Parse an integer value from a scheme cell. returns zero when succesful.
 * Does not emit error messages.
 */
static int
guile2int (SCM scm, int *target)
{
  int err = 0;
  char *asstr = NULL;
  if (gh_number_p (scm))                       /* yess... we got it */
    {
      *target = gh_scm2int (scm);
    }
  else if (NULL != (asstr = guile2str (scm)))  /* try harder        */
    {
      char *endp;
      *target = strtol (asstr, &endp, 10);
      if (*endp != '\0' || errno == ERANGE)
	{
	  /* not parsable... */
	  err = 1;
	}
      free (asstr);
    }
  else                                         /* no chance         */
    {
      err = 2;
    }
  return err;
}


/*
 * Extract an integer value from an option hash. returns zero if it worked.
 */
static int
optionhash_extract_int (svz_hash_t *hash,
			char *key,          /* the key to find      */
			int hasdef,         /* is there a default ? */
			int defvar,         /* the default          */
			int *target,        /* where to put it      */
			char *msg           /* appended to errormsg */
			)
{
  int err = 0;
  SCM hvalue = optionhash_get (hash, key);

  if (SCM_UNSPECIFIED == hvalue)
    { /* nothing in hash, try to use default */
      if (hasdef)
	{
	  *target = defvar;
	}
      else
	{
	  err = 1;
	  report_error ("No default value for integer `%s' %s", key, msg);
	}
    }
  else
    { /* convert something              */
      if (guile2int (hvalue, target))
	{
	  err = 1;
	  report_error ("Invalid integer value for `%s' %s", key, msg);
	}
    }

  return err;
}


/*
 * Exctract a string value from an option hash. returns zero if it worked.
 * the memory for the string is newly allocated. no matter where it came
 * from.
 */
static int
optionhash_extract_string (svz_hash_t *hash,
			   char *key,        /* the key to find       */
			   int hasdef,       /* if there is a default */
			   char *defvar,     /* default               */
			   char **target,    /* where to put it       */
			   char *msg         /* appended to errormsg  */
			   )
{
  int err = 0;
  char *tmp = NULL;
  SCM hvalue = optionhash_get (hash, key);

  if (SCM_UNSPECIFIED == hvalue)
    { /* nothing in hash, try to use default */
      if (hasdef)
	{
	  *target = svz_strdup (defvar);
	}
      else
	{
	  err = 1;
	  report_error ("No default value for string `%s' %s", key, msg);
	}
    }
  else
    {
      if (NULL == (tmp = guile2str (hvalue)))
	{
	  err = 1;
	  report_error ("`%s' must be a string value %s", key, msg);
	}
      else
	{
	  *target = svz_strdup (tmp);
	  free (tmp);
	}
    }

  return err;
}


/* ********************************************************************** */


/*
 * Generic server definition...
 */
#define FUNC_NAME "define-server!"
SCM
guile_define_server_x (SCM type, SCM name, SCM alist)
{
  char * ctype;
  char * cname;
  SCM cpair;

  SCM_VALIDATE_STRING_COPY (1, type, ctype);
  SCM_VALIDATE_STRING_COPY (2, name, cname);
  SCM_VALIDATE_LIST (3, alist);

  while (gh_pair_p (alist))
    {
      SCM_VALIDATE_ALISTCELL_COPYSCM (3, alist, cpair);
      alist = gh_cdr (alist);
    }
  log_printf (LOG_DEBUG, "defining server `%s' of type `%s'\n",
	      cname, ctype);
  return SCM_UNSPECIFIED;
}
#undef FUNC_NAME

/*
 * Generic port configuration definition...
 */
#define FUNC_NAME "define-port!"
static SCM
define_port (SCM symname, SCM args)
{
  svz_portcfg_t* prev = NULL;
  svz_portcfg_t* cfg = svz_portcfg_create ();
  svz_hash_t* options = NULL;
  /* FIXME: use #t and #f values here */
  SCM retval_ok = SCM_UNSPECIFIED;
  SCM retval_fail = SCM_UNSPECIFIED;
  SCM retval = retval_ok;
  char *portname = guile2str (symname);

  if (portname == NULL)
    {
      report_error ("First argument to define-port! must be String or Symbol");
      retval = retval_fail;
      goto out;
    }

  if (NULL == (options = guile2optionhash (args)))
    {
      /* FIXME: message ? */
      retval = retval_fail;
      goto out;
    }

  if (0 != validate_optionhash (options, 1, "port", portname))
    {
      retval = retval_fail;
    }

  /* find out what protocol this portcfg will be about */
  do {
    char *proto = guile2str (optionhash_get (options, "proto"));
    char *msg = svz_malloc (256);
    snprintf (msg, 256, "when defining port `%s'", portname);

    if (NULL == proto) {
      report_error ("Port `%s' requires a \"proto\" field", portname);
      retval = retval_fail;
      goto out;
    }

    if (!strcmp (proto, "tcp"))
      {
	int port;
	cfg->proto = PROTO_TCP;
	optionhash_extract_int (options, "port", 0, 0, &port, msg);
	cfg->tcp_port = (short) port;
	if (!valid_port (port))
	  {
	    report_error ("Invalid port number %s", msg);
	    retval = retval_fail;
	  }
	optionhash_extract_int (options, "backlog", 1, 0, &(cfg->tcp_backlog),
				msg);
	optionhash_extract_string (options, "ipaddr", 1, "*",
				   &(cfg->tcp_ipaddr), msg);
	svz_portcfg_mkaddr (cfg);
      }
    else if (!strcmp (proto, "udp"))
      {
	int port;
	cfg->proto = PROTO_UDP;
	optionhash_extract_int (options, "port", 0, 0, &port,msg);
	cfg->udp_port = (short) port;
	if (!valid_port (port))
	  {
	    report_error ("Invalid port number %s", msg);
	    retval = retval_fail;
	  }
	optionhash_extract_string (options, "ipaddr", 1, "*",
				   &(cfg->udp_ipaddr), msg);
	svz_portcfg_mkaddr (cfg);
      }
    else if (!strcmp (proto, "icmp"))
      {
	int type;
	cfg->proto = PROTO_ICMP;
	optionhash_extract_string (options, "ipaddr", 1, "*",
				   &(cfg->icmp_ipaddr), msg);
	/* FIXME: default value */
	optionhash_extract_int (options, "type", 1, 42, &type, msg);
	if ((type & ~0xFF) != 0)
	  {
	    report_error ("Key 'type' must be a byte %s, msg");
	    retval = retval_fail;
	  }
	cfg->icmp_type = (char)(type & 0xFF);
	svz_portcfg_mkaddr (cfg);
      }
    else if (!strcmp (proto, "raw"))
      {
	cfg->proto = PROTO_RAW;
	optionhash_extract_string (options, "ipaddr", 1, "*",
				   &(cfg->raw_ipaddr), msg);
	svz_portcfg_mkaddr (cfg);
      }
    else if (!strcmp (proto, "pipe"))
      {
	cfg->proto = PROTO_PIPE;
	/* FIXME: implement me */
      }
    else
      {
	report_error ("Invalid \"proto\" field `%s' in `%s'.",
		      proto, portname);
	return SCM_UNSPECIFIED;
      }

    svz_free (msg);
    free (proto);
  } while (0);


  /* check if too much was defined */
  if (0 != validate_optionhash (options, 0, "port", portname))
    {
      retval = retval_fail;
      goto out;
    }

  /* now rememver the name and add that config */
  cfg->name = svz_strdup (portname);

  /* FIXME: remove when it works */
  svz_portcfg_print (cfg, stdout);
  prev = svz_portcfg_add (portname, cfg);

  if (prev != cfg)
    {
      /* we've overwritten something. report and dispose */
      report_error ("Overwriting previous defintion of port `%s'", portname);
      svz_portcfg_destroy (prev);
    }

  free(portname);

 out:
  return retval;
}
#undef FUNC_NAME

/*
 * Generic server -> port binding...
 */
#define FUNC_NAME "bind-server!"
SCM
guile_bind_server_x (SCM type, SCM name, SCM alist)
{
  return SCM_UNSPECIFIED;
}
#undef FUNC_NAME

/*
 * Initialize Guile.
 */
static void
guile_init (void)
{
  SCM def_serv;

  /* define some variables */
  gh_define ("serveez-version", gh_str02scm (svz_version));
  gh_define ("guile-version", scm_version ());
  gh_define ("have-debug", gh_bool2scm (have_debug));
  gh_define ("have-win32", gh_bool2scm (have_win32));

  /* export some new procedures */
  def_serv = gh_new_procedure ("define-port!", define_port, 1, 0, 2);
  def_serv = gh_new_procedure3_0 ("define-server!", guile_define_server_x);
  def_serv = gh_new_procedure3_0 ("bind-server!", guile_bind_server_x);
}

/*
 * Exception handler for guile. It is called if the evaluation of the given
 * file failed.
 */
static SCM 
guile_exception (void *data, SCM tag, SCM args)
{
  fprintf (stderr, "exception: ");
  gh_display (tag);
  fprintf (stderr, ": ");
  gh_display (args);
  gh_newline ();
  return SCM_UNDEFINED;
}

/*
 * Get server settings from the file @var{cfgfile} and instantiate servers 
 * as needed. Return non-zero on errors.
 */
int
guile_load_config (char *cfgfile)
{
  guile_init ();
  if (gh_eval_file_with_catch (cfgfile, guile_exception) == SCM_UNDEFINED)
    return -1;
  return 0;
}
