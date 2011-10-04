/*
   Unix SMB/Netbios implementation.
   Version 1.9.
   store smbd profiling information in shared memory
   Copyright (C) Andrew Tridgell 1999

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include "includes.h"

#ifdef WITH_PROFILE
#define IPC_PERMS ((SHM_R | SHM_W) | (SHM_R>>3) | (SHM_R>>6))
static int shm_id;
static BOOL read_only;

#endif /* WITH_PROFILE */

struct profile_header *profile_h;
struct profile_stats *profile_p;

BOOL do_profile_flag = False;
BOOL do_profile_times = False;

struct timeval profile_starttime;
struct timeval profile_endtime;
struct timeval profile_starttime_nested;
struct timeval profile_endtime_nested;

/****************************************************************************
receive a set profile level message
****************************************************************************/
void profile_message(int msg_type, pid_t src, void *buf, size_t len)
{
DEBUG(1,("function ommited\n"));
/* function omited*/
}

/****************************************************************************
receive a request profile level message
****************************************************************************/
void reqprofile_message(int msg_type, pid_t src, void *buf, size_t len)
{
DEBUG(1,("function ommited\n"));
	/* function omited*/
}

/*******************************************************************
  open the profiling shared memory area
  ******************************************************************/

#ifdef WITH_PROFILE
BOOL profile_setup(BOOL rdonly)
{
	struct shmid_ds shm_ds;

	read_only = rdonly;

 again:
	/* try to use an existing key */
	shm_id = shmget(PROF_SHMEM_KEY, 0, 0);

	/* if that failed then create one. There is a race condition here
	   if we are running from inetd. Bad luck. */
	if (shm_id == -1) {
		if (read_only) return False;
		shm_id = shmget(PROF_SHMEM_KEY, sizeof(*profile_h),
				IPC_CREAT | IPC_EXCL | IPC_PERMS);
	}

	if (shm_id == -1) {
		DEBUG(0,("Can't create or use IPC area. Error was %s\n",
			 strerror(errno)));
		return False;
	}


	profile_h = (struct profile_header *)shmat(shm_id, 0,
						   read_only?SHM_RDONLY:0);
	if ((long)profile_p == -1) {
		DEBUG(0,("Can't attach to IPC area. Error was %s\n",
			 strerror(errno)));
		return False;
	}

	/* find out who created this memory area */
	if (shmctl(shm_id, IPC_STAT, &shm_ds) != 0) {
		DEBUG(0,("ERROR shmctl : can't IPC_STAT. Error was %s\n",
			 strerror(errno)));
		return False;
	}

#if 0
	if (shm_ds.shm_perm.cuid != 0 || shm_ds.shm_perm.cgid != 0) {
		DEBUG(0,("ERROR: root did not create the shmem\n"));
		return False;
	}
#endif

	if (shm_ds.shm_segsz != sizeof(*profile_h)) {
		DEBUG(0,("WARNING: profile size is %d (expected %d). Deleting\n",
			 (int)shm_ds.shm_segsz, sizeof(*profile_h)));
		if (shmctl(shm_id, IPC_RMID, &shm_ds) == 0) {
			goto again;
		} else {
			return False;
		}
	}

	if (!read_only && (shm_ds.shm_nattch == 1)) {
		memset((char *)profile_h, 0, sizeof(*profile_h));
		profile_h->prof_shm_magic = PROF_SHM_MAGIC;
		profile_h->prof_shm_version = PROF_SHM_VERSION;
		DEBUG(3,("Initialised profile area\n"));
	}

	profile_p = &profile_h->stats;
	message_register(MSG_PROFILE, profile_message);
	message_register(MSG_REQ_PROFILELEVEL, reqprofile_message);
	return True;
}
#endif /* WITH_PROFILE */
