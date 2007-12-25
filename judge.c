/* Copyright (C) 2006-2008 Brice Arnould.
 *  This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <errno.h>
#include <assert.h>

#include <string.h>		// strdup(), memset()
#include <fcntl.h>		// open()
#include <sys/types.h>		// open(), umask()
#include <dirent.h>		// scandir()
#include <sys/stat.h>		// stat(), umask()
#include <unistd.h>		// stat()
#include <sys/file.h>		// flock()
#include <stdio.h>		// printf(), tmpfile()
#include <error.h>		// error()
#include <limits.h>		// SSIZE_MAX
#include "executive.h"		// fcopy()
#include "judge.h"
#include "linux.h"
#include "msg.h"



/*  This function return a struct wich describe properties
 * of the file who's name is given.
 */
struct accused *
investigate (char *name, struct law *l)
{
  assert (name);
  struct accused *a;
  ino_t inode;			// used to check against race between open and stat
  /* malloc() */
  {
    a = malloc (sizeof (*a));
    if (NULL == a)
      error (1, errno, "%s: malloc() failed", name);
    a->name = strdup (name);
    if (NULL == a->name)
      error (1, errno, "%s: strdup() failed", name);
  }
  /* Set default value */
  {
    a->fd = -1;
    a->blocks = 0;
    a->fragc = 0;
    a->crumbc = 0;
    a->start = 0;
    a->end = 0;
    a->ideal = 0;
    a->atime = 0;
    a->mtime = 0;
    a->age = 0;
    a->poslog = NULL;
    a->sizelog = NULL;
    a->guilty = 0;
  }
  /* this stat() will be applied on all accused, including directory */
  {
    struct stat st;
    if (-1 == lstat (a->name, &st))
      {
	error (0, errno, "%s: lstat() failed", name);
	goto freeall;
      }
    a->mode = st.st_mode;
    a->fs = st.st_dev;
    a->size = st.st_blocks * 512;
    inode = st.st_ino;
  }
  if (!S_ISREG (a->mode) || 0 == a->size)
    return a;
  /* open() */
  if (-1 == (a->fd = open (name, O_NOATIME | O_RDWR)))
    {
      error (0, errno, "%s: open() failed", name);
      goto freeall;
    }
  /* Put the lock */
  if (l->locks && -1 == flock (a->fd, LOCK_EX | LOCK_NB))
    {
      error (0, errno, "%s: failed to acquire a lock", a->name);
      goto freeall;
    }
  /* the stat() will be applied only on regular files */
  {
    struct stat st;
    if (-1 == fstat (a->fd, &st))
      {
	error (0, errno, "%s: lstat() failed", name);
	goto freeall;
      }
    /* Check against race condition */
    if (st.st_ino != inode || st.st_dev != a->fs)
      {
	error (0, errno, "%s: file have moved", name);
	goto freeall;
      }
    a->size = st.st_blocks * 512;
    a->atime = st.st_atime;
    a->mtime = st.st_mtime;
    a->age = time (NULL) - st.st_ctime;
  }
  /* Read ptime - placement time */
  if (l->xattr)
    {
      time_t ptime = get_ptime (a->fd);
      if (ptime != (time_t) - 1)
	a->age = time (NULL) - ptime;
    }
  if (-1 == get_testimony (a, l))
    goto freeall;
  return a;
freeall:
  if (a)
    {
      if (a->fd != -1)
	close (a->fd);
      free (a->name);
      free (a);
    }
  return NULL;
}

/*  This function free structs allocated by
 * investigate().
 */
void
close_case (struct accused *a, struct law *l)
{
  if (!a)
    return;
  free (a->name);
  a->name = NULL;
  if (a->fd >= 0)
    {
      if (l->locks && -1 == flock (a->fd, LOCK_UN))
	error (1, errno, "%s: failed to release lock", a->name);
      close (a->fd);
    }
  a->fd = -1;
  a->mode = 0x42;
  if (a->poslog)
    free (a->poslog);
  if (a->sizelog)
    free (a->sizelog);
  free (a);
}


/*  This function tell return the tolerance, that is a number
 * corresponding to the cost of shake()ing the accused.
 *  It is used by judge().
 */
static double
tol_reg (struct accused *a, struct law *l)
{
  assert (a && l);
  assert (S_ISREG (a->mode));
  double tol = 1.0;
  if (a->size < l->smallsize && l->smallsize)
    tol = l->smallsize_tol;
  else if (a->size > l->bigsize && l->bigsize)
    tol = l->bigsize_tol;
  return tol;
}

/* Return true if the file is guilty, else false.
 */
static bool
judge_reg (struct accused *a, struct law *l)
{
  assert (a && l);
  assert (S_ISREG (a->mode));
  double tol = tol_reg (a, l);
  if (MAX_TOL == tol)
    return false;
  if (l->workaround && strstr (a->name, ".so"))	// TODO: investigate on ld lock
    return false;
  if (a->age < l->new * tol)
    return false;
  if (a->age > l->old * tol)
    return true;
  if (a->fragc > l->maxfragc * tol)
    return true;
  if (a->crumbc > l->maxcrumbc * tol)
    return true;
  if ((l->maxdeviance) && (a->start) && (a->ideal)
      && abs ((int) (a->start - a->ideal)) > (uint) l->maxdeviance * tol)
    return true;
  return false;
}

static int
judge_list (char *restrict * flist, struct law *restrict l)
{
  assert (flist && l);
  int res = 0;			// value returned
  /*  We want "y", to be the file examined, "x" the previous one, and
   * eventually "z" the next one (to take neighboors in account).
   */
  struct accused *x = NULL, *y = NULL, *z = NULL;
  /* check if list is empty */
  if (!flist[0])
    return 0;
  /* Main loop, read every file and their neighboor
   * Typically, x:flist[n-1], y: flist[n], z: flist[n+1]
   */
  z = investigate (flist[0], l);
  for (uint n = 0; flist[n]; n++)
    {
      /* Do we have a file after y ? */
      if (z)
	{
	  close_case (x, l);
	  x = y;
	  y = z;
	}
      /* Try to add a file from the list */
      if (flist[n + 1])
	{
	  z = investigate (flist[n + 1], l);
	  if (!z)
	    continue;		// Try the next file.
	}
      else
	z = NULL;
      /* Do we actually have a file ? */
      if (!y)
	continue;
      /* Do we know where the file should be ? */
      {
	y->ideal = 0;
	if (y->start)
	  {
	    if (x && x->end && abs (x->atime - y->atime) < MAGICTIME)
	      {
		if (z && z->start && abs (z->atime - y->atime) < MAGICTIME)
		  y->ideal = (x->end + z->start) / 2;
		else
		  y->ideal = (x->end + MAGICLEAP);
	      }
	    else if (z && z->start && abs (z->atime - y->atime) < MAGICTIME)
	      y->ideal = (z->start - z->blocks - MAGICLEAP);
	  }
      }
      /* judge */
      if (-1 == judge (y, l))
	{
	  res = -1;
	  break;
	}
    }
  close_case (x, l);
  close_case (y, l);
  close_case (z, l);
  return res;
}

static int
judge_dir (struct accused *a, struct law *l)
{
  assert (a && a->name && l);
  assert ((dev_t) - 1 != a->fs);
  /* check against --one-file-system */
  if ((dev_t) - 1 != l->kingdom && a->fs != l->kingdom)
    return 0;
  else
    {
      int res;
      char **flist = list_dir (a->name, true);
      if (!flist)
	{
	  error (0, 0, "%s: list_dir() failed", a->name);
	  return -1;
	}
      res = judge_list (flist, l);
      close_list (flist);
      return res;
    }
}

int
judge_stdin (struct accused *a, struct law *l)
{
  assert (!a && l);
  int res;
  char **flist = list_stdin ();
  if (!flist)
    {
      error (0, 0, "-: list_stdin() failed");
      return -1;
    }
  res = judge_list (flist, l);
  close_list (flist);
  return res;
}


int
judge (struct accused *a, struct law *l)
{
  assert (a && l);
  if (S_ISLNK (a->mode))
    return 0;
  else if (S_ISDIR (a->mode))
    return judge_dir (a, l);
  else if (S_ISREG (a->mode) && a->size)
    {
      a->guilty = judge_reg (a, l);
      if (a->guilty)
	shake_reg (a, l);
      /*  Show result of investigation, if the file is guilty or if
       * level of verbosity is greater than 2
       */
      if ((a->guilty && l->verbosity) || l->verbosity >= 2)
	show_reg (a, l);
    }
  return a->guilty;
}
