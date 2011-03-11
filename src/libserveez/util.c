/*
 * util.c - utility function implementation
 *
 * Copyright (C) 2011 Thien-Thi Nguyen
 * Copyright (C) 2000, 2001, 2002, 2003 Stefan Jahn <stefan@lkcc.org>
 * Copyright (C) 2000 Raimund Jacob <raimi@lkcc.org>
 * Copyright (C) 1999 Martin Grabmueller <mgrabmue@cs.tu-berlin.de>
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

#ifdef _AIX
# undef _NO_PROTO
# ifndef _USE_IRS
#  define _USE_IRS 1
# endif
# define _XOPEN_SOURCE_EXTENDED 1
#endif /* _AIX */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <assert.h>

#if HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#if HAVE_SYS_RESOURCE_H && !defined (__MINGW32__)
# include <sys/resource.h>
#endif
#if HAVE_UNISTD_H
# include <unistd.h>
#endif
#if HAVE_STRINGS_H
# include <strings.h>
#endif

#ifndef __MINGW32__
# include <netdb.h>
#endif

#if HAVE_SYS_UTSNAME_H
# include <sys/utsname.h>
#endif

#include "networking-headers.h"
#include "libserveez/alloc.h"
#include "libserveez/boot.h"
#include "libserveez/windoze.h"
#ifdef ENABLE_LOG_MUTEX
# include "libserveez/mutex.h"
#endif
#include "libserveez/util.h"

/*
 * Level of the logging interfaces verbosity:
 * 0 - only fatal error messages
 * 1 - error messages
 * 2 - warnings
 * 3 - informational messages
 * 4 - debugging output
 * Levels always imply numerically lesser levels.
 */

static char log_level[][16] = {
  "fatal",
  "error",
  "warning",
  "notice",
  "debug"
};

/*
 * This is the file all log messages are written to.  Change it with a
 * call to @code{svz_log_setfile}.  By default, all log messages are written
 * to @code{stderr}.
 */
static FILE *svz_logfile = NULL;

/* The logging mutex is necessary only if stdio doesn't do locking.  */
#ifdef ENABLE_LOG_MUTEX

static svz_mutex_t spew_mutex = SVZ_MUTEX_INITIALIZER;
static int spew_mutex_valid;

#define LOCK_LOG_MUTEX() \
  if (spew_mutex_valid) svz_mutex_lock (&spew_mutex)
#define UNLOCK_LOG_MUTEX() \
  if (spew_mutex_valid) svz_mutex_unlock (&spew_mutex)

#else  /* !ENABLE_LOG_MUTEX */

#define LOCK_LOG_MUTEX()
#define UNLOCK_LOG_MUTEX()

#endif  /* !ENABLE_LOG_MUTEX */

#define LOGBUFSIZE  512

void
svz__log_updn (int direction)
{
#ifndef HAVE_FWRITE_UNLOCKED
  (direction
   ? svz_mutex_create
   : svz_mutex_destroy)
    (&spew_mutex);
  spew_mutex_valid = direction;
#endif
}

/*
 * Print a message to the log system.  @var{level} specifies the prefix.
 */
void
svz_log (int level, const char *format, ...)
{
  char buf[LOGBUFSIZE];
  size_t w = 0;
  va_list args;
  time_t tm;
  struct tm *t;

  if (level > svz_config.verbosity || svz_logfile == NULL ||
      feof (svz_logfile) || ferror (svz_logfile))
    return;

  tm = time (NULL);
  t = localtime (&tm);
  w = strftime (buf, LOGBUFSIZE, "[%Y/%m/%d %H:%M:%S]", t);
  w += snprintf (buf + w, LOGBUFSIZE - w, " %s: ", log_level[level]);
  va_start (args, format);
  w += vsnprintf (buf + w, LOGBUFSIZE - w, format, args);
  va_end (args);

  /* Ensure that an overlong message is properly truncated.  */
  if (LOGBUFSIZE > w)
    assert ('\0' == buf[w]);
  else
    {
      w = LOGBUFSIZE - 1;
      buf[w - 1] = '\n';
      buf[w] = '\0';
    }

  /* Write it out.  */
  LOCK_LOG_MUTEX ();
  fwrite (buf, 1, w, svz_logfile);
  fflush (svz_logfile);
  UNLOCK_LOG_MUTEX ();
}

/*
 * Set the file stream @var{file} to the log file all messages are printed
 * to.  Could also be @code{stdout} or @code{stderr}.
 */
void
svz_log_setfile (FILE * file)
{
  svz_logfile = file;
}

#define MAX_DUMP_LINE 16   /* bytes per line */

/*
 * Dump a @var{buffer} with the length @var{len} to the file stream @var{out}.
 * You can specify a description in @var{action}.  The hexadecimal text
 * representation of the given buffer will be either cut at @var{len} or
 * @var{max}.  @var{from} is a numerical identifier of the buffers creator.
 */
int
svz_hexdump (FILE *out,    /* output FILE stream */
             char *action, /* hex dump description */
             int from,     /* who created the dumped data */
             char *buffer, /* the buffer to dump */
             int len,      /* length of that buffer */
             int max)      /* maximum amount of bytes to dump (0 = all) */
{
  int row, col, x, max_col;

  if (!max)
    max = len;
  if (max > len)
    max = len;
  max_col = max / MAX_DUMP_LINE;
  if ((max % MAX_DUMP_LINE) != 0)
    max_col++;

  fprintf (out, "%s [ FROM:0x%08X SIZE:%d ]\n", action, (unsigned) from, len);

  for (x = row = 0; row < max_col && x < max; row++)
    {
      /* print hexdump */
      fprintf (out, "%04X   ", x);
      for (col = 0; col < MAX_DUMP_LINE; col++, x++)
        {
          if (x < max)
            fprintf (out, "%02X ", (unsigned char) buffer[x]);
          else
            fprintf (out, "   ");
        }
      /* print character representation */
      x -= MAX_DUMP_LINE;
      fprintf (out, "  ");
      for (col = 0; col < MAX_DUMP_LINE && x < max; col++, x++)
        {
          fprintf (out, "%c", buffer[x] >= ' ' ? buffer[x] : '.');
        }
      fprintf (out, "\n");
    }

  fflush (out);
  return 0;
}

/* On some platforms @code{hstrerror} can be resolved but is not declared
   anywhere.  That is why we do it here by hand.  */
#if defined (HAVE_HSTRERROR) && !HAVE_DECL_HSTRERROR
extern char * hstrerror (int);
#endif

/*
 * This is the @code{hstrerror} wrapper function, depending on the
 * configuration file @file{config.h}.
 */
char *
svz_hstrerror (void)
{
#if HAVE_HSTRERROR
# if HAVE_DECL_H_ERRNO
  return (char *) hstrerror (h_errno);
# else
  return (char *) hstrerror (errno);
# endif
#else /* not HAVE_HSTRERROR */
# if HAVE_DECL_H_ERRNO
  return (char *) strerror (h_errno);
# else
  return (char *) strerror (errno);
# endif
#endif /* not HAVE_HSTRERROR */
}

/*
 * Transform the given binary data @var{t} (UTC time) to an ASCII time text
 * representation without any trailing characters.
 */
char *
svz_time (long t)
{
  static char *asc;
  char *p;

  p = asc = ctime ((time_t *) &t);
  while (*p)
    p++;
  while (*p < ' ')
    *(p--) = '\0';

  return asc;
}

/*
 * Convert the given string @var{str} to lower case text representation.
 */
char *
svz_tolower (char *str)
{
  char *p = str;

  while (*p)
    {
      *p = (char) (isupper ((svz_uint8_t) * p) ?
                   tolower ((svz_uint8_t) * p) : *p);
      p++;
    }
  return str;
}

#ifdef __MINGW32__
/*
 * This variable contains the last system or network error occurred if
 * it was detected and printed.  Needed for the "Resource unavailable" error
 * condition.
 */
int svz_errno = 0;

#define MESSAGE_BUF_SIZE 256

/*
 * There is no text representation of network (Winsock API) errors in
 * Win32.  That is why we translate it by hand.
 */
static char *
svz_neterror (int error)
{
  static char message[MESSAGE_BUF_SIZE];

  switch (error)
    {
    case WSAEACCES:
      return "Permission denied.";
    case WSAEADDRINUSE:
      return "Address already in use.";
    case WSAEADDRNOTAVAIL:
      return "Cannot assign requested address.";
    case WSAEAFNOSUPPORT:
      return "Address family not supported by protocol family.";
    case WSAEALREADY:
      return "Operation already in progress.";
    case WSAECONNABORTED:
      return "Software caused connection abort.";
    case WSAECONNREFUSED:
      return "Connection refused.";
    case WSAECONNRESET:
      return "Connection reset by peer.";
    case WSAEDESTADDRREQ:
      return "Destination address required.";
    case WSAEFAULT:
      return "Bad address.";
    case WSAEHOSTDOWN:
      return "Host is down.";
    case WSAEHOSTUNREACH:
      return "No route to host.";
    case WSAEINPROGRESS:
      return "Operation now in progress.";
    case WSAEINTR:
      return "Interrupted function call.";
    case WSAEINVAL:
      return "Invalid argument.";
    case WSAEISCONN:
      return "Socket is already connected.";
    case WSAEMFILE:
      return "Too many open files.";
    case WSAEMSGSIZE:
      return "Message too long.";
    case WSAENETDOWN:
      return "Network is down.";
    case WSAENETRESET:
      return "Network dropped connection on reset.";
    case WSAENETUNREACH:
      return "Network is unreachable.";
    case WSAENOBUFS:
      return "No buffer space available.";
    case WSAENOPROTOOPT:
      return "Bad protocol option.";
    case WSAENOTCONN:
      return "Socket is not connected.";
    case WSAENOTSOCK:
      return "Socket operation on non-socket.";
    case WSAEOPNOTSUPP:
      return "Operation not supported.";
    case WSAEPFNOSUPPORT:
      return "Protocol family not supported.";
    case WSAEPROCLIM:
      return "Too many processes.";
    case WSAEPROTONOSUPPORT:
      return "Protocol not supported.";
    case WSAEPROTOTYPE:
      return "Protocol wrong type for socket.";
    case WSAESHUTDOWN:
      return "Cannot send after socket shutdown.";
    case WSAESOCKTNOSUPPORT:
      return "Socket type not supported.";
    case WSAETIMEDOUT:
      return "Connection timed out.";
    case WSAEWOULDBLOCK:
      return "Resource temporarily unavailable.";
    case WSAHOST_NOT_FOUND:
      return "Host not found.";
    case WSANOTINITIALISED:
      return "Successful WSAStartup not yet performed.";
    case WSANO_DATA:
      return "Valid name, no data record of requested type.";
    case WSANO_RECOVERY:
      return "This is a non-recoverable error.";
    case WSASYSNOTREADY:
      return "Network subsystem is unavailable.";
    case WSATRY_AGAIN:
      return "Non-authoritative host not found.";
    case WSAVERNOTSUPPORTED:
      return "WINSOCK.DLL version out of range.";
    case WSAEDISCON:
      return "Graceful shutdown in progress.";
    default:
      sprintf (message, "Network error code %d.", error);
      break;
    }
  return message;
}

/*
 * Routine which forms a valid error message under Win32.  It might either
 * use the @code{GetLastError} or @code{WSAGetLastError} in order to
 * get a valid error code.
 */
char *
svz_syserror (int nr)
{
  static char message[MESSAGE_BUF_SIZE];

  /* save the last error */
  svz_errno = nr;

  /* return a net error if necessary */
  if (nr >= WSABASEERR)
    return svz_neterror (nr);

  /*
   * if the error is not valid (GetLastError returned zero)
   * fall back to the errno variable of the usual crtdll.
   */
  if (!nr)
    nr = errno;

  /* return a sys error */
  if (0 == FormatMessage (FORMAT_MESSAGE_FROM_SYSTEM |
                          FORMAT_MESSAGE_ARGUMENT_ARRAY, NULL, nr,
                          MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),
                          (char *) message, MESSAGE_BUF_SIZE, NULL))
    {
      sprintf (message, "FormatMessage (%d): error code %ld",
               nr, GetLastError ());
      return message;
    }

  message[strlen (message) - 2] = 0;
  return message;
}

/*
 * This variable contains the the runtime detected Win32 version.  Its value
 * is setup in @code{svz_sys_version} and can be @code{Win32s} for Windows 3.x,
 * @code{Win95} for Windows 95, @code{Win98} for Windows 98, @code{WinNT3x}
 * for Windows NT 3.x, @code{WinNT4x} for Windows NT 4.x, @code{Win2k} for
 * Windows 2000, @code{WinXP} for Windows XP and @code{WinME} for Windows ME.
 */
int svz_os_version = 0;

#endif /* __MINGW32__ */

/*
 * This routine is for detecting the operating system version of Win32
 * and all Unices at runtime.  You should call it at least once at startup.
 * It saves its result in the variable @code{svz_os_version} and prints an
 * appropriate message.
 */
char *
svz_sys_version (void)
{
  static char os[256] = ""; /* contains the os string */

#ifdef __MINGW32__
  static char ver[][6] =
    { " 32s", " 95", " 98", " NT", " NT", " 2000", " XP", " ME" };
  OSVERSIONINFO osver;
#elif HAVE_SYS_UTSNAME_H
  struct utsname buf;
#endif

  /* detect only once */
  if (os[0])
    return os;

#ifdef __MINGW32__ /* Windows */
  osver.dwOSVersionInfoSize = sizeof (osver);
  if (!GetVersionEx (&osver))
    {
      svz_log (LOG_ERROR, "GetVersionEx: %s\n", SYS_ERROR);
      sprintf (os, "unknown Windows");
    }
  else
    {
      switch (osver.dwPlatformId)
        {
        case VER_PLATFORM_WIN32_NT: /* NT, Windows 2000 or Windows XP */
          if (osver.dwMajorVersion == 4)
            svz_os_version = WinNT4x;
          else if (osver.dwMajorVersion <= 3)
            svz_os_version = WinNT3x;
          else if (osver.dwMajorVersion == 5 && osver.dwMinorVersion < 1)
            svz_os_version = Win2k;
          else if (osver.dwMajorVersion >= 5)
            svz_os_version = WinXP;
          break;

        case VER_PLATFORM_WIN32_WINDOWS: /* Win95 or Win98 */
          if ((osver.dwMajorVersion > 4) ||
              ((osver.dwMajorVersion == 4) && (osver.dwMinorVersion > 0)))
            {
              if (osver.dwMinorVersion >= 90)
                svz_os_version = WinME;
              else
                svz_os_version = Win98;
            }
          else
            svz_os_version = Win95;
          break;

        case VER_PLATFORM_WIN32s: /* Windows 3.x */
          svz_os_version = Win32s;
          break;
        }

      sprintf (os, "Windows%s %ld.%02ld %s%s(Build %ld)",
               ver[svz_os_version],
               osver.dwMajorVersion, osver.dwMinorVersion,
               osver.szCSDVersion, osver.szCSDVersion[0] ? " " : "",
               osver.dwBuildNumber & 0xFFFF);
    }
#elif HAVE_UNAME /* !__MINGW32__ */
  uname (&buf);
  sprintf (os, "%s %s on %s", buf.sysname, buf.release, buf.machine);
#endif /* not HAVE_UNAME */

  return os;
}

/*
 * Converts an unsigned integer to its decimal string representation
 * returning a pointer to an internal buffer, so copy the result.
 */
char *
svz_itoa (unsigned int i)
{
  static char buffer[32];
  char *p = buffer + sizeof (buffer) - 1;

  *p = '\0';
  do
    {
      p--;
      *p = (char) ((i % 10) + '0');
    }
  while ((i /= 10) != 0);
  return p;
}

/*
 * Converts a given string @var{str} in decimal format to an unsigned integer.
 * Stops conversion on any invalid characters.
 */
unsigned int
svz_atoi (char *str)
{
  unsigned int i = 0;

  while (*str >= '0' && *str <= '9')
    {
      i *= 10;
      i += *str - '0';
      str++;
    }
  return i;
}

/*
 * Returns the current working directory.  The returned pointer needs to be
 * @code{svz_free}'ed after usage.
 */
char *
svz_getcwd (void)
{
  char *buf, *dir;
  int len = 64;

  buf = dir = NULL;
  do
    {
      buf = svz_realloc (buf, len);
      dir = getcwd (buf, len);
      len *= 2;
    }
  while (dir == NULL);

  return dir;
}

/*
 * This routine checks for the current and maximum limit of open files
 * of the current process.  The function heavily depends on the underlying
 * platform.  It tries to set the limit to the given @var{max_sockets}
 * amount.
 */
int
svz_openfiles (int max_sockets)
{
#if HAVE_GETRLIMIT
  struct rlimit rlim;
#endif

#if HAVE_GETDTABLESIZE
  int openfiles;

  if ((openfiles = getdtablesize ()) == -1)
    {
      svz_log (LOG_ERROR, "getdtablesize: %s\n", SYS_ERROR);
      return -1;
    }
  svz_log (LOG_NOTICE, "file descriptor table size: %d\n", openfiles);
#endif /* HAVE_GETDTABLESIZE */

#if HAVE_GETRLIMIT

# ifndef RLIMIT_NOFILE
#  define RLIMIT_NOFILE RLIMIT_OFILE
# endif

  if (getrlimit (RLIMIT_NOFILE, &rlim) == -1)
    {
      svz_log (LOG_ERROR, "getrlimit: %s\n", SYS_ERROR);
      return -1;
    }
  svz_log (LOG_NOTICE, "current open file limit: %d/%d\n",
           rlim.rlim_cur, rlim.rlim_max);

  if ((int) rlim.rlim_max < (int) max_sockets ||
      (int) rlim.rlim_cur < (int) max_sockets)
    {
      rlim.rlim_max = max_sockets;
      rlim.rlim_cur = max_sockets;

      if (setrlimit (RLIMIT_NOFILE, &rlim) == -1)
        {
          svz_log (LOG_ERROR, "setrlimit: %s\n", SYS_ERROR);
          return -1;
        }
      getrlimit (RLIMIT_NOFILE, &rlim);
      svz_log (LOG_NOTICE, "open file limit set to: %d/%d\n",
               rlim.rlim_cur, rlim.rlim_max);
    }

#elif defined (__MINGW32__)     /* HAVE_GETRLIMIT */

  unsigned sockets = 100;

  if (svz_os_version == Win95 ||
      svz_os_version == Win98 || svz_os_version == WinME)
    {
      if (svz_os_version == Win95)
        sockets = svz_windoze_get_reg_unsigned (MaxSocketKey,
                                                MaxSocketSubKey,
                                                MaxSocketSubSubKey, sockets);
      else
        sockets = svz_atoi (svz_windoze_get_reg_string (MaxSocketKey,
                                                        MaxSocketSubKey,
                                                        MaxSocketSubSubKey,
                                                        svz_itoa (sockets)));

      svz_log (LOG_NOTICE, "current open file limit: %u\n", sockets);

      if (sockets < (unsigned) max_sockets)
        {
          sockets = max_sockets;

          if (svz_os_version == Win95)
            svz_windoze_set_reg_unsigned (MaxSocketKey,
                                          MaxSocketSubKey,
                                          MaxSocketSubSubKey, sockets);
          else
            svz_windoze_set_reg_string (MaxSocketKey,
                                        MaxSocketSubKey,
                                        MaxSocketSubSubKey,
                                        svz_itoa (sockets));

          svz_log (LOG_NOTICE, "open file limit set to: %u\n", sockets);
        }
    }
#endif /* MINGW32__ */

  return 0;
}
