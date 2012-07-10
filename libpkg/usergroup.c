/*
 * Copyright (c) 2011-2012 Baptiste Daroussin <bapt@FreeBSD.org>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/types.h> /* needed type libutl.h on 8.X */

#include <grp.h>
#include <pwd.h>
#include <libutil.h>
#include <string.h>

#ifndef HAVE_GRUTILS
#include "private/gr_util.h"
#endif
#include "pkg.h"
#include "private/event.h"
#include "private/pkg.h"

int
pkg_add_user_group(struct pkg *pkg)
{
	struct pkg_group *g = NULL;
	struct pkg_user *u = NULL;
	struct group *gr = NULL;
	struct passwd *pw = NULL;
	int tfd, pfd;

	/* loop just to check group and users contains string */
	while (pkg_groups(pkg, &g) == EPKG_OK) {
		if (g->gidstr[0] == '\0') {
			/*
			 * old style group ignorring, this is created from
			 * scripts
			 */
			return (EPKG_OK);
		}
	}

	/* loop just to check group and users contains string */
	while (pkg_users(pkg, &u) == EPKG_OK) {
		if (u->uidstr[0] == '\0') {
			/*
			 * old style group ignorring, this is created from
			 * scripts
			 */
			return (EPKG_OK);
		}
	}

	g = NULL;
	u = NULL;

	while (pkg_groups(pkg, &g) == EPKG_OK) {
		char *gr_create_str, *tmp;

		/* simply create the group */
		if ((gr = getgrnam(pkg_group_name(g))) == NULL) {
			/* remove the members if any */
			if (g->gidstr[strlen(g->gidstr) - 1] == ':') {
				gr = gr_scan(g->gidstr);
			} else {
				gr_create_str = strdup(g->gidstr);
				tmp = strrchr(gr_create_str, ':');
				tmp++;
				tmp[0] = '\0';
				gr = gr_scan(gr_create_str);
				free(gr_create_str);
			}
		} else {
			/* no need to create the group */
			continue;
		}

		if (gr == NULL) {
			pkg_emit_error("Bad group line, ignoring");
			continue;
		}
		gr_init(NULL, NULL);
		if ((pfd = gr_lock()) == -1) {
			gr_fini();
			continue;
		}
		if ((tfd = gr_tmp(-1)) == -1) {
			gr_fini();
			continue;
		}
		if (gr_copy(pfd, tfd, gr, NULL) == -1) {
			gr_fini();
			continue;
		}
		if (gr_mkdb() == -1) {
			gr_fini();
			continue;
		}

		free(gr);
		gr_fini();
	}

	while (pkg_users(pkg, &u) == EPKG_OK) {

		if ((pw = getpwnam(pkg_user_name(u))) == NULL) {
			/* simply create the user */
			pw = pw_scan(u->uidstr, PWSCAN_WARN|PWSCAN_MASTER);
		} else {
			/* user already exists */
			continue;
		}

		pw_init(NULL, NULL);
		if ((pfd = pw_lock()) == -1) {
			pw_fini();
			continue;
		}
		if ((tfd = gr_tmp(-1)) == -1) {
			pw_fini();
			continue;
		}
		if (pw_copy(pfd, tfd, pw, NULL) == -1) {
			pw_fini();
			continue;
		}
		if (pw_mkdb(pkg_user_name(u)) == -1) {
			pw_fini();
		}
		pw_fini();
		if (strcmp(pw->pw_dir, "/nonexistent") &&
		    strcmp(pw->pw_dir, "/var/empty")) {
			/* now create the homedir if it doesn't exists */
			/* TODO: do it recursively */
			mkdir(pw->pw_dir, 0644);
			chown(pw->pw_dir, pw->pw_uid, pw->pw_gid);
		}
		free(pw);
	}

	/* now add members to groups if they also are listed in users */
	g = NULL;

	while (pkg_groups(pkg, &g) == EPKG_OK) {
		struct group *grlocal;
		struct group *grnew;
		int i, j;
		int nx = 0;

		if (g->gidstr[strlen(g->gidstr) - 1] == ':')
			continue; /* no members, next */

		gr = gr_scan(g->gidstr);
		grlocal = getgrnam(pkg_group_name(g));
		grnew = NULL;/* gr_dup(grlocal);*/

		u = NULL;
		for (i = 0; gr->gr_mem[i] != NULL; i++) {

			while (pkg_users(pkg, &u) == EPKG_OK) {
				if (strcmp(pkg_user_name(u), gr->gr_mem[i]))
					continue;

				/*
				 * check if the user is not already in the
				 * local group
				 */
				for (j = 0; grlocal->gr_mem[j] != NULL; j++) {
					if (!strcmp(grlocal->gr_mem[j],
					    gr->gr_mem[i]))
						break;
				}

				if (grlocal->gr_mem[j] != NULL)
					continue; /* already in the group */

				/* adding the user to the group */

				if (grnew == NULL) {
					nx = j - 1;
					grnew = gr_dup(grlocal);
				}

				if (nx == 0)
					grnew->gr_mem = NULL;
				nx++;
				grnew->gr_mem = reallocf(grnew->gr_mem,
				    sizeof(*grnew->gr_mem) * (nx + 1));
				grnew->gr_mem[nx - 1] =
				    __DECONST(char *, pkg_user_name(u));
				grnew->gr_mem[nx] = NULL;
			}
		}
		if (grnew == NULL) {
			continue;
		}
		gr_init(NULL, NULL);
		if ((pfd = gr_lock()) == -1) {
			gr_fini();
			continue;
		}
		if ((tfd = gr_tmp(-1)) == -1) {
			gr_fini();
			continue;
		}
		if (gr_copy(pfd, tfd, grnew, grlocal) == -1) {
			gr_fini();
			continue;
		}
		if (gr_mkdb() == -1) {
			gr_fini();
			continue;
		}

		free(grnew);
		gr_fini();
	}

	return (EPKG_OK);
}
