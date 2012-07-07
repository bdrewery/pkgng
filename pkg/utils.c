/*
 * Copyright (c) 2011-2012 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2011-2012 Julien Laffaye <jlaffaye@FreeBSD.org>
 * Copyright (c) 2011-2012 Marin Atanasov Nikolov <dnaeon@gmail.com>
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
#include <sys/stat.h>

#include <libutil.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <pkg.h>

#include "pkgcli.h"

bool
query_yesno(const char *msg, ...)
{
	int c;
	bool r = false;
	va_list ap;

	va_start(ap, msg);
	vprintf(msg, ap);
	va_end(ap);

	c = getchar();
	if (c == 'y' || c == 'Y')
		r = true;
	else if (c == '\n' || c == EOF)
		return false;

	while ((c = getchar()) != '\n' && c != EOF)
		continue;

	return r;
}

char *
absolutepath(const char *src, char *dest, size_t dest_len) {
	char * res;
	size_t res_len, res_size, len;
	char pwd[MAXPATHLEN];
	const char *ptr = src;
	const char *next;
	const char *slash;

	len = strlen(src);

	if (len != 0 && src[0] != '/') {
		if (getcwd(pwd, sizeof(pwd)) == NULL)
			return NULL;

		res_len = strlen(pwd);
		res_size = res_len + 1 + len + 1;
		res = malloc(res_size);
		strlcpy(res, pwd, res_size);
	} else {
		res_size = (len > 0 ? len : 1) + 1;
		res = malloc(res_size);
		res_len = 0;
	}

	next = src;
	for (ptr = src; next != NULL ; ptr = next + 1) {
		next = strchr(ptr, '/');

		if (next != NULL)
			len = next - ptr;
		else
			len = strlen(ptr);

		switch(len) {
			case 2:
				if (ptr[0] == '.' && ptr[1] == '.') {
					slash = strrchr(res, '/');
					if (slash != NULL) {
						res_len = slash - res;
						res[res_len] = '\0';
					}
					continue;
				}
				break;
			case 1:
				if (ptr[0] == '.')
					continue;

				break;
			case 0:
				continue;
		}
		res[res_len++] = '/';
		strlcpy(res + res_len, ptr, res_size);
		res_len += len;
		res[res_len] = '\0';
	}

	if (res_len == 0)
		strlcpy(res, "/", res_size);

	strlcpy(dest, res, dest_len);
	free(res);

	return &dest[0];
}

void
print_info(struct pkg * const pkg, unsigned int options)
{
	struct pkg_category *cat    = NULL;
	struct pkg_dep	    *dep    = NULL;
	struct pkg_dir	    *dir    = NULL;
	struct pkg_file	    *file   = NULL;
	struct pkg_group    *group  = NULL;
	struct pkg_license  *lic    = NULL;
	struct pkg_option   *option = NULL;
	struct pkg_shlib    *shlib  = NULL;
	struct pkg_user	    *user   = NULL;
	bool multirepos_enabled = false;
	char size[7];
	const char *name, *version, *prefix, *origin, *reponame, *repourl;
	const char *maintainer, *www, *comment, *desc, *message;
	char *m;
	unsigned opt;
	int64_t flatsize, newflatsize, newpkgsize;
	lic_t licenselogic;

	pkg_config_bool(PKG_CONFIG_MULTIREPOS, &multirepos_enabled);

	pkg_get(pkg,
		PKG_NAME,          &name,
		PKG_VERSION,       &version,
		PKG_PREFIX,        &prefix,
		PKG_ORIGIN,        &origin,
		PKG_REPONAME,      &reponame,
		PKG_REPOURL,       &repourl,
		PKG_MAINTAINER,    &maintainer,
		PKG_WWW,           &www,
		PKG_COMMENT,       &comment,
		PKG_DESC,          &desc,
		PKG_FLATSIZE,      &flatsize,
		PKG_NEW_FLATSIZE,  &newflatsize,
		PKG_NEW_PKGSIZE,   &newpkgsize,
		PKG_LICENSE_LOGIC, &licenselogic,
		PKG_MESSAGE,       &message);

	if (options & INFO_RAW) { /* Not for remote packages */
		if (pkg_type(pkg) != PKG_REMOTE) {
			pkg_emit_manifest(pkg, &m);
			printf("%s\n", m);
			free(m);
		}
		return;
	}

	/* Print a tag-line identifying the package -- either NAMEVER, ORIGIN
	   or NAME (in that order of preference).  This may be the only
	   output from this function */
	if (options & INFO_TAG_NAMEVER)
		printf("%s-%s", name, version);
	else if (options & INFO_TAG_ORIGIN)
		printf("%s", origin);
	else if (options & INFO_TAG_NAME)
		printf("%s", name);

	/* Any more to print? */
	if (options & INFO_ALL)
		printf(":\n");
	else {
		printf("\n");
		return;
	}

	for (opt = 0x1; opt <= INFO_LASTFIELD; opt <<= 1) {
		if ((opt & options) == 0)
			continue;

		switch (opt) {
		case INFO_NAME:
			if (!quiet)
				printf("%-15s: ", "Name");
			printf("%s\n", name);
			break;
		case INFO_VERSION:
			if (!quiet)
				printf("%-15s: ", "Version");
			printf("%s\n", version);
			break;
		case INFO_ORIGIN:
			if (!quiet)
				printf("%-15s: ", "Origin");
			printf("%s\n", origin);
			break;
		case INFO_PREFIX:
			if (!quiet)
				printf("%-15s: ", "Prefix");
			printf("%s\n", prefix);
			break;
		case INFO_REPOSITORY:
			if (pkg_type(pkg) == PKG_REMOTE &&
			    multirepos_enabled) {
				if (!quiet)
					printf("%-15s: ", "Repository");
				printf("%s [%s]\n", reponame, repourl);
			}
			break;
		case INFO_CATEGORIES:
			if (!pkg_list_is_empty(pkg, PKG_CATEGORIES)) {
				if (!quiet)
					printf("%-15s: ", "Categories");
				if (pkg_categories(pkg, &cat) == EPKG_OK)
					printf("%s", pkg_category_name(cat));
				while (pkg_categories(pkg, &cat) == EPKG_OK)
					printf(" %s", pkg_category_name(cat));
				printf("\n");
			}
			break;
		case INFO_LICENSES:
			if (!pkg_list_is_empty(pkg, PKG_LICENSES)) {
				if (!quiet)
					printf("%-15s: ", "Licenses");
				if (pkg_licenses(pkg, &lic) == EPKG_OK)
					printf("%s", pkg_license_name(lic));
				while (pkg_licenses(pkg, &lic) == EPKG_OK) {
					if (licenselogic != 1)
						printf(" %c", licenselogic);
					printf(" %s", pkg_license_name(lic));
				}
				printf("\n");
			}
			break;
		case INFO_MAINTAINER:
			if (!quiet)
				printf("%-15s: ", "Maintainer");
			printf("%s\n", maintainer);
			break;
		case INFO_WWW:	
			if (!quiet)
				printf("%-15s: ", "WWW");
			printf("%s\n", www);
			break;
		case INFO_COMMENT:
			if (!quiet)
				printf("%-15s: ", "Comment");
			printf("%s\n", comment);
			break;
		case INFO_OPTIONS:
			if (!pkg_list_is_empty(pkg, PKG_OPTIONS)) {
				if (!quiet)
					printf("%-15s:\n", "Options");
				while (pkg_options(pkg, &option) == EPKG_OK)
					printf("\t%-15s: %s\n",
					       pkg_option_opt(option),
					       pkg_option_value(option));
			}
			break;
		case INFO_SHLIBS:
			if (!pkg_list_is_empty(pkg, PKG_SHLIBS)) {
				if (!quiet)
					printf("%-15s: ", "Shared Libs");
				if (pkg_shlibs(pkg, &shlib) == EPKG_OK)
					printf("%s", pkg_shlib_name(shlib));
				while (pkg_shlibs(pkg, &shlib) == EPKG_OK)
					printf(" %s", pkg_shlib_name(shlib));
				printf("\n");
			}
			break;
		case INFO_FLATSIZE:
			if (pkg_type(pkg) == PKG_INSTALLED ||
			    pkg_type(pkg) == PKG_FILE)
				humanize_number(size, sizeof(size),
						flatsize,"B",
						HN_AUTOSCALE, 0);
			else
				humanize_number(size, sizeof(size),
						newflatsize,"B",
						HN_AUTOSCALE, 0);

			if (!quiet)
				printf("%-15s: ", "Flat size");
			printf("%s\n", size);
			break;
		case INFO_PKGSIZE: /* Remote pkgs only */
			if (pkg_type(pkg) == PKG_REMOTE) {
				humanize_number(size, sizeof(size),
						newpkgsize,"B",
						HN_AUTOSCALE, 0);
				if (!quiet)
					printf("%-15s: ", "Pkg size");
				printf("%s\n", size);
			}
			break;
		case INFO_DESCR:
			if (!quiet)
				printf("%-15s:\n", "Description");
			printf("%s\n", desc);
			break;
		case INFO_MESSAGE:
			if (message) {
				if (!quiet)
					printf("%-15s: ", "Message");
				printf("%s\n", message);
			}
			break;
		case INFO_DEPS:
			if (!pkg_list_is_empty(pkg, PKG_DEPS)) {
				if (!quiet)
					printf("%-15s:\n", "Depends on");
				while (pkg_deps(pkg, &dep) == EPKG_OK)
					printf("\t%s-%s\n",
					       pkg_dep_get(dep,	PKG_DEP_NAME),
					       pkg_dep_get(dep, PKG_DEP_VERSION));
				printf("\n");
			}
			break;
		case INFO_RDEPS:
			if (!pkg_list_is_empty(pkg, PKG_RDEPS)) {
				if (!quiet)
					printf("%-15s:\n", "Required by");
				while (pkg_rdeps(pkg, &dep) == EPKG_OK)
					printf("\t%s-%s\n",
					       pkg_dep_get(dep,	PKG_DEP_NAME),
					       pkg_dep_get(dep, PKG_DEP_VERSION));
				printf("\n");
			}
			break;
		case INFO_FILES: /* Installed pkgs only */
			if (pkg_type(pkg) != PKG_REMOTE &&
			    !pkg_list_is_empty(pkg, PKG_FILES)) {
				if (!quiet)
					printf("%-15s: ", "Files");
				while (pkg_files(pkg, &file) == EPKG_OK)
					printf("%s\n",
					       pkg_file_get(file,
							    PKG_FILE_PATH));
				printf("\n");
			}
			break;
		case INFO_DIRS:	/* Installed pkgs only */
			if (pkg_type(pkg) != PKG_REMOTE &&
			    !pkg_list_is_empty(pkg, PKG_DIRS)) {
				if (!quiet)
					printf("%-15s: ", "Directories");
				while (pkg_dirs(pkg, &dir) == EPKG_OK)
					printf("%s\n",
					       pkg_dir_path(dir));
				printf("\n");
			}
			break;
		case INFO_USERS: /* Installed pkgs only */
			if (pkg_type(pkg) != PKG_REMOTE &&
			    !pkg_list_is_empty(pkg, PKG_USERS)) {
				if (!quiet)
					printf("%-15s: ", "Users");
				if (pkg_users(pkg, &user) == EPKG_OK)
					printf("%s", pkg_user_name(user));
				while (pkg_users(pkg, &user) == EPKG_OK)
					printf(" %s", pkg_user_name(user));
				printf("\n");
			}
			break;
		case INFO_GROUPS: /* Installed pkgs only */
			if (pkg_type(pkg) != PKG_REMOTE &&
			    !pkg_list_is_empty(pkg, PKG_GROUPS)) {
				if (!quiet)
					printf("%-15s: ", "Groups");
				if (pkg_groups(pkg, &group) == EPKG_OK)
					printf("%s", pkg_group_name(group));
				while (pkg_groups(pkg, &group) == EPKG_OK)
					printf(" %s", pkg_group_name(group));
				printf("\n");
			}
			break;
		}
	}
}

void
print_jobs_summary(struct pkg_jobs *jobs, pkg_jobs_t type, const char *msg, ...)
{
	struct pkg *pkg = NULL;
	char path[MAXPATHLEN];
	struct stat st;
	const char *name, *version, *newversion, *pkgrepopath, *cachedir;
	int64_t dlsize, oldsize, newsize;
	int64_t flatsize, newflatsize, pkgsize;
	char size[7];
	va_list ap;

	va_start(ap, msg);
	vprintf(msg, ap);
	va_end(ap);

	dlsize = oldsize = newsize = 0;
	flatsize = newflatsize = pkgsize = 0;
	name = version = newversion = NULL;
	
	pkg_config_string(PKG_CONFIG_CACHEDIR, &cachedir);

	while (pkg_jobs(jobs, &pkg) == EPKG_OK) {
		pkg_get(pkg, PKG_NEWVERSION, &newversion, PKG_NAME, &name,
		    PKG_VERSION, &version, PKG_FLATSIZE, &flatsize,
		    PKG_NEW_FLATSIZE, &newflatsize, PKG_NEW_PKGSIZE, &pkgsize,
		    PKG_REPOPATH, &pkgrepopath);

		switch (type) {
		case PKG_JOBS_INSTALL:
			dlsize += pkgsize;
			snprintf(path, MAXPATHLEN, "%s/%s", cachedir, pkgrepopath);
			if (stat(path, &st) != -1)
				dlsize -= st.st_size;

			if (newversion != NULL) {
				switch (pkg_version_cmp(version, newversion)) {
				case 1:
					printf("\tDowngrading %s: %s -> %s\n", name, version, newversion);
					break;
				case 0:
					printf("\tReinstalling %s-%s\n", name, version);
					break;
				case -1:
					printf("\tUpgrading %s: %s -> %s\n", name, version, newversion);
					break;
				}
				oldsize += flatsize;
				newsize += newflatsize;
			} else {
				newsize += flatsize;
				printf("\tInstalling %s: %s\n", name, version);
			}
			break;
		case PKG_JOBS_DEINSTALL:
			oldsize += flatsize;
			newsize += newflatsize;
			
			printf("\t%s-%s\n", name, version);
			break;
		case PKG_JOBS_FETCH:
			dlsize += pkgsize;
			snprintf(path, MAXPATHLEN, "%s/%s", cachedir, pkgrepopath);
			if (stat(path, &st) != -1)
				dlsize -= st.st_size;

			printf("\t%s-%s\n", name, version);
			break;
		}
	}

	if (oldsize > newsize) {
		newsize *= -1;
		humanize_number(size, sizeof(size), oldsize - newsize, "B", HN_AUTOSCALE, 0);

		switch (type) {
		case PKG_JOBS_INSTALL:
			printf("\nThe installation will free %s\n", size);
			break;
		case PKG_JOBS_DEINSTALL:
			printf("\nThe deinstallation will free %s\n", size);
			break;
		case PKG_JOBS_FETCH:
			/* nothing to report here */
			break;
		}
	} else if (newsize > oldsize) {
		humanize_number(size, sizeof(size), newsize - oldsize, "B", HN_AUTOSCALE, 0);

		switch (type) {
		case PKG_JOBS_INSTALL:
			printf("\nThe installation will require %s more space\n", size);
			break;
		case PKG_JOBS_DEINSTALL:
			printf("\nThe deinstallation will require %s more space\n", size);
			break;
		case PKG_JOBS_FETCH:
			/* nothing to report here */
			break;
		}
	}

	if ((type == PKG_JOBS_INSTALL) || (type == PKG_JOBS_FETCH)) {
		humanize_number(size, sizeof(size), dlsize, "B", HN_AUTOSCALE, 0);
		printf("\n%s to be downloaded\n", size);
	}
}
