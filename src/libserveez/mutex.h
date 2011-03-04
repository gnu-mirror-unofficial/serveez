/*
 * mutex.h - thread mutex definitions
 *
 * Copyright (C) 2011 Thien-Thi Nguyen
 * Copyright (C) 2003 Stefan Jahn <stefan@lkcc.org>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this package.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __MUTEX_H__
#define __MUTEX_H__ 1

/* begin svzint */
#include "libserveez/defines.h"
/* end svzint */

#if SVZ_HAVE_PTHREAD_H
# include <pthread.h>
#endif

#ifdef __MINGW32__
# include <windows.h>
#endif

#if SVZ_HAVE_THREADS

# ifdef __MINGW32__ /* Windows native */

typedef svz_t_handle svz_mutex_t;
# define SVZ_MUTEX_INITIALIZER NULL

# else /* POSIX threads */

typedef pthread_mutex_t svz_mutex_t;
# define SVZ_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER

#endif

#else /* !SVZ_HAVE_THREADS */

typedef void * svz_mutex_t;
# define SVZ_MUTEX_INITIALIZER NULL

#endif

/* Declares a @var{mutex} object externally.  This is useful when the
   @var{mutex} object is defined in another file.  */
#define svz_mutex_declare(mutex) \
  extern svz_mutex_t mutex;

/* Defines a @var{mutex} object globally.  */
#define svz_mutex_define(mutex) \
  svz_mutex_t mutex = SVZ_MUTEX_INITIALIZER;

__BEGIN_DECLS

SERVEEZ_API int svz_mutex_create (svz_mutex_t *);
SERVEEZ_API int svz_mutex_destroy (svz_mutex_t *);
SERVEEZ_API int svz_mutex_lock (svz_mutex_t *);
SERVEEZ_API int svz_mutex_unlock (svz_mutex_t *);

__END_DECLS

#endif /* not __MUTEX_H__ */
