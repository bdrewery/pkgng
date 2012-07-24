/*-
 * Copyright (c) 2011-2012 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2011-2012 Julien Laffaye <jlaffaye@FreeBSD.org>
 * Copyright (c) 2012 Matthew Seaman <matthew@FreeBSD.org>
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

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include <pkg.h>

#include "pkgcli.h"

static bool
is_url(const char * const pattern)
{
	if (strncmp(pattern, "http://", 7) == 0 ||
		strncmp(pattern, "https://", 8) == 0 ||
		strncmp(pattern, "ftp://", 6) == 0 ||
		strncmp(pattern, "file://", 7) == 0)
		return (true);

	return (false);
}

/* Try downloading (if necessary) and opening the package.  */
static int
download_and_open(const char *urlpath, struct pkg **pkg)
{
	if (is_url(urlpath)) {
		char		 path[MAXPATHLEN +1];
		const char	*cachedir;

		if (pkg_config_string(PKG_CONFIG_CACHEDIR, &cachedir) !=
		    EPKG_OK)
			return (EPKG_FATAL);

		snprintf(path, sizeof(path), "%s/%s", cachedir, 
		    basename(urlpath));
		if (pkg_fetch_file(urlpath, path, 0) != EPKG_OK)
			return (EPKG_FATAL);

		return (pkg_open(pkg, path));
	} else {
		if (access(urlpath, F_OK) != 0) {
			warn("%s", urlpath);
			if (errno == ENOENT)
				warnx("Did you mean 'pkg install %s'?",
				    urlpath);
			return (EPKG_FATAL);
		}

		return (pkg_open(pkg, urlpath));
	}
}

/* Recurse through dependencies of pkg, attempting to download
   and open a pkg for any that are not installed. */
static int download_missing_dependencies(struct pkgdb *db,
    struct pkg_jobs *jobs, const char *base_urlpath, const char *pkg_ext);

static int
download_missing_dependencies(struct pkgdb *db, struct pkg_jobs *jobs,
    const char *base_urlpath, const char *pkg_ext)
{
	struct pkg_dep *dep = NULL;
	struct pkg *this_pkg = NULL;
	struct pkg *next_pkg = NULL;
	char next_urlpath[MAXPATHLEN + 1];
	int ret = EPKG_OK;

	assert(db != NULL);

	/* this_pkg == NULL means get the first in the queue */
	pkg_jobs(jobs, &this_pkg);

	if (pkg_list_is_empty(this_pkg, PKG_DEPS))
		return (EPKG_OK);

	while (pkg_deps(this_pkg, &dep) == EPKG_OK) {
		if (pkg_dep_already_installed(db, dep) == EPKG_OK)
			continue;

		if (pkg_jobs_already_queued(jobs, pkg_dep_origin(dep)))
			continue;

		snprintf(next_urlpath, sizeof(next_urlpath), "%s/%s-%s%s",
			 base_urlpath, pkg_dep_name(dep), pkg_dep_version(dep),
			 pkg_ext);

		ret = download_and_open(next_urlpath, &next_pkg);
		if (ret == EPKG_OK) {
			/* Set dependencies to automatic */
			pkg_set(next_pkg, PKG_AUTOMATIC, true);
			pkg_jobs_queue(jobs, next_pkg);
			ret = download_missing_dependencies(db, jobs,
			          base_urlpath, pkg_ext);
		}

		if (ret != EPKG_OK) {
			warnx("Cannot access dependency package %s",
			      next_urlpath);
			break;
		}
	}

	if (next_pkg != NULL)
		pkg_free(next_pkg);

	return (ret);
}

/* Ensure that all required packages are available (downloading as
   required) including any missing dependencies */
static int
generate_worklist(struct pkgdb *db, struct pkg_jobs *jobs, int argc,
    char **argv, bool force)
{
	struct pkg	*pkg;
	int		 i, retcode = EPKG_OK;
	const char	*base_urlpath;
	const char	*pkg_ext;

	for (i = 0; i < argc; i++) {
		pkg = NULL;	/* Always allocate a new struct pkg */

		if (download_and_open(argv[i], &pkg) != EPKG_OK) {
			warnx("Cannot install package from %s", argv[i]);
			retcode = EPKG_END;
			continue;
		}

		base_urlpath = dirname(argv[i]);
		pkg_ext = strrchr(argv[i], '.');
		
		if (pkg_ext == NULL) {
			warnx("Missing extension for %s", argv[i]);
			pkg_ext = "";
		}

		pkg_jobs_queue(jobs, pkg);
				
		if (download_missing_dependencies(db, jobs, base_urlpath,
		    pkg_ext) != EPKG_OK) {
			warnx("Missing dependency for %s", argv[i]);
			retcode = EPKG_END;
			if (!force)
				continue;
		}
	}

	return (retcode);
}

void
usage_add(void)
{
	fprintf(stderr, "usage: pkg add [-AfInqy] <pkg-name>\n");
	fprintf(stderr, "       pkg add [-AfInqy] <protocol>://<path>/<pkg-name>\n\n");
	fprintf(stderr, "For more information see 'pkg help add'.\n");
}

int
exec_add(int argc, char **argv)
{
	struct pkgdb	*db = NULL;
	struct pkg_jobs *jobs = NULL;
	struct sbuf     *failedpkgs = NULL;
	pkg_flags	 f = PKG_FLAG_NONE;

	int	 retcode = EX_SOFTWARE;
	int	 ch;
	int	 i;
	int	 failedpkgcount = 0;
	bool	 force = false;
	bool	 dry_run = false;
	bool	 yes;

	pkg_config_bool(PKG_CONFIG_ASSUME_ALWAYS_YES, &yes);

	while ((ch = getopt(argc, argv, "AfInqy")) != -1) {
		switch (ch) {
		case 'A':
			f |= PKG_FLAG_AUTOMATIC;
			break;
		case 'f':
			force = true;
			break;
		case 'I':
			f |= PKG_ADD_NOSCRIPT;
			break;
		case 'n':
			dry_run = true;
			break;
		case 'q':
			quiet = true;
			break;
		case 'y':
			yes = true;
			break;
		default:
			usage_add();
			return (EX_USAGE);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		usage_add();
		return (EX_USAGE);
	}

	retcode = pkgdb_access(PKGDB_MODE_READ  |
			       PKGDB_MODE_WRITE |
			       PKGDB_MODE_CREATE,
			       PKGDB_DB_LOCAL);
	if (retcode == EPKG_ENOACCESS) {
		warnx("Insufficient privilege to add packages");
		return (EX_NOPERM);
	} else if (retcode != EPKG_OK)
		return (EX_IOERR);

	if (pkgdb_open(&db, PKGDB_DEFAULT) != EPKG_OK)
		return (EX_IOERR);

	failedpkgs = sbuf_new_auto();
	for (i = 0; i < argc; i++) {
		if (is_url(argv[i]) == EPKG_OK) {
			snprintf(path, sizeof(path), "./%s", basename(argv[i]));
			if ((retcode = pkg_fetch_file(argv[i], path, 0)) != EPKG_OK)
				break;

			file = path;
		} else {
			file = argv[i];
			if (access(file, F_OK) != 0) {
				warn("%s",file);
				if (errno == ENOENT)
					warnx("Did you mean 'pkg install %s'?", file);
				sbuf_cat(failedpkgs, argv[i]);
				if (i != argc - 1)
					sbuf_printf(failedpkgs, ", ");
				failedpkgcount++;
				continue;
			}

		}

		if ((retcode = pkg_add(db, file, f)) != EPKG_OK) {
			sbuf_cat(failedpkgs, argv[i]);
			if (i != argc - 1)
				sbuf_printf(failedpkgs, ", ");
			failedpkgcount++;
		}

	if (pkg_jobs_new(&jobs, PKG_JOBS_ADD, db) != EPKG_OK)
		goto cleanup;

	if (generate_worklist(db, jobs, argc, argv, force) != EPKG_OK)
		goto cleanup;

	if (pkg_jobs_is_empty(jobs))
		goto cleanup;

	pkgdb_close(db);
	
	if(failedpkgcount > 0) {
		sbuf_finish(failedpkgs);
		printf("\nFailed to install the following %d package(s): %s\n", failedpkgcount, sbuf_data(failedpkgs));
		retcode = EPKG_FATAL;

	if (!quiet || dry_run) {
		print_jobs_summary(jobs, PKG_JOBS_ADD,
		    "The following packages will be added:\n\n");

		if (!yes && !dry_run)
			yes = query_yesno(
				"\nProceed with adding packages [y/N]: ");
		if (dry_run)
			yes = false;
	}

	if (yes)
		if (pkg_jobs_apply(jobs, NULL, force) != EPKG_OK)
			goto cleanup;

	if (messages != NULL) {
		sbuf_finish(messages);
		printf("%s", sbuf_data(messages));
	}

	retcode = EX_OK;

cleanup:
	pkg_jobs_free(jobs);
	pkgdb_close(db);

	return (retcode);
}

