/*
 * array.h - array declarations
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
 * $Id: array.h,v 1.12 2001/11/21 14:15:45 raimi Exp $
 *
 */

#ifndef __ARRAY_H__
#define __ARRAY_H__ 1

#include "libserveez/defines.h"

typedef struct svz_array
{
  unsigned long size;
  unsigned long capacity;
  void **data;
}
svz_array_t;

__BEGIN_DECLS

SERVEEZ_API svz_array_t * svz_array_create __P ((unsigned long));
SERVEEZ_API void svz_array_clear __P ((svz_array_t *));
SERVEEZ_API void svz_array_destroy __P ((svz_array_t *));
SERVEEZ_API void *svz_array_get __P ((svz_array_t *, unsigned long));
SERVEEZ_API void *svz_array_set __P ((svz_array_t *, unsigned long, void *));
SERVEEZ_API void svz_array_add __P ((svz_array_t *, void *));
SERVEEZ_API void *svz_array_del __P ((svz_array_t *, unsigned long));
SERVEEZ_API unsigned long svz_array_capacity __P ((svz_array_t *));
SERVEEZ_API unsigned long svz_array_size __P ((svz_array_t *));
SERVEEZ_API unsigned long svz_array_ins __P ((svz_array_t *, unsigned long, 
					      void *));
SERVEEZ_API unsigned long svz_array_idx __P ((svz_array_t *, void *));
SERVEEZ_API unsigned long svz_array_contains __P ((svz_array_t *, void *));
SERVEEZ_API svz_array_t *svz_array_dup __P ((svz_array_t *));
SERVEEZ_API svz_array_t *svz_array_strdup __P ((svz_array_t *));
SERVEEZ_API void **svz_array_values __P ((svz_array_t *));

__END_DECLS

/*
 * This is the iteration macro for the array implementation of the core
 * library. @var{array} specifies the array to iterate, @var{value} the
 * pointer each element of the array gets assigned and @var{i} is the
 * iteration variable.
 */
#define svz_array_foreach(array, value, i)                      \
  for ((i) = 0, (value) = svz_array_get ((array), 0);           \
       (array) && (unsigned long) (i) < svz_array_size (array); \
       ++(i), (value) = svz_array_get ((array), (i)))

#endif /* not __ARRAY_H__ */
