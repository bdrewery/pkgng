/*
 * Copyright (c) 2011-2012 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2011-2012 Julien Laffaye <jlaffaye@FreeBSD.org>
 * Copyright (c) 2011-2012 Marin Atanasov Nikolov <dnaeon@gmail.com>
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

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sysexits.h>

#include <pkg.h>

#include "pkgcli.h"

void
usage_search(void)
{
	fprintf(stderr, "usage: pkg search [-r repo] [-egxX] [search] [label] [modifier]... <pkg-name>\n");
	fprintf(stderr, "       pkg search [-r repo] [-egxX] [-qcdfDsop] <pattern>\n");
	fprintf(stderr, "       Search options: -So -Sn -Sp -Sc -Sd\n");
	fprintf(stderr, "       Label options: -Lo -Ln -Lp -Lc -Ld\n");
	fprintf(stderr, "       Modifiers: -Mc -Mf -Md -Mr -Ms -MS -MR\n");
	fprintf(stderr, "For more information see 'pkg help search'.\n");
}

int
exec_search(int argc, char **argv)
{
	const char *pattern = NULL;
	const char *reponame = NULL;
	int ret = EPKG_OK, ch;
	int flags = PKG_LOAD_BASIC;
	unsigned int opt = 0;
	match_t match = MATCH_REGEX;
	pkgdb_field search = FIELD_NONE;
	pkgdb_field label = FIELD_NONE;
	struct pkgdb *db = NULL;
	struct pkgdb_it *it = NULL;
	struct pkg *pkg = NULL;
	bool atleastone = false;

	while ((ch = getopt(argc, argv, "egxXr:S:L:M:cdfDsopq")) != -1) {
		switch (ch) {
		case 'e':
			match = MATCH_EXACT;
			break;
		case 'g':
			match = MATCH_GLOB;
			break;
		case 'x':
			match = MATCH_REGEX;
			break;
		case 'X':
			match = MATCH_EREGEX;
			break;
		case 'r':
			reponame = optarg;
			break;
		case 'S':
			/* search options */
			switch(optarg[0]) {
			case 'o':
				search = FIELD_ORIGIN;
				break;
			case 'n':
				search = FIELD_NAME;
				break;
			case 'p':
				search = FIELD_NAMEVER;
				break;
			case 'c':
			opt_S_c:
				search = FIELD_COMMENT;
				break;
			case 'd':
			opt_S_d:
				search = FIELD_DESC;
				break;
			default:
				usage_search();
				return (EX_USAGE);
			}
			break;
		case 'L':
			/* label options */
			switch(optarg[0]) {
			case 'o':
			opt_L_o:
				label = FIELD_ORIGIN;
				break;
			case 'n':
				label = FIELD_NAME;
				break;
			case 'p':
				label = FIELD_NAMEVER;
				break;
			case 'c':
				label = FIELD_COMMENT;
				break;
			case 'd':
				label = FIELD_DESC;
				break;
			default:
				usage_search();
				return (EX_USAGE);
			}
			break;
		case 'M':
			/* label modifiers */
			switch(optarg[0]) {
			case 'c':
				opt |= INFO_COMMENT;
				break;
			case 'f':
			opt_M_f:
				opt |= INFO_FULL;
				flags |= PKG_LOAD_CATEGORIES |
					PKG_LOAD_LICENSES    |
					PKG_LOAD_OPTIONS     |
					PKG_LOAD_SHLIBS;
				break;
			case 'd':
			opt_M_d:
				opt |= INFO_DEPS;
				flags |= PKG_LOAD_DEPS;
				break;
			case 'r':
				opt |= INFO_RDEPS;
				flags |= PKG_LOAD_RDEPS;
				break;
			case 's':
			opt_M_s:
				opt |= INFO_FLATSIZE;
				break;
			case 'S':
				opt |= INFO_PKGSIZE;
				break;
			case 'p':
			opt_M_p:
				opt |= INFO_PREFIX;
				break;
			case 'R':
				opt |= INFO_REPOSITORY;
				break;
			default:
				usage_search();
				return (EX_USAGE);
			}
			break;
		case 'c':	/* Same as -Sc */
			goto opt_S_c;
		case 'd':	/* Same as -Sd */
			goto opt_S_d;
		case 'f':	/* Same as -Mf */
			goto opt_M_f;
		case 'D':	/* Same as -MD */
			goto opt_M_d;
		case 's':	/* Same as -Ms */
			goto opt_M_s;
		case 'o':	/* Same as -Lo */
			goto opt_L_o;
		case 'p':	/* Same as -Mp */
			goto opt_M_p;
		case 'q':
			quiet = true;
			break;
		default:
			usage_search();
			return (EX_USAGE);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc != 1) {
		usage_search();
		return (EX_USAGE);
	}

	pattern = argv[0];
	if (pattern[0] == '\0') {
		fprintf(stderr, "Pattern must not be empty!\n");
		return (EX_USAGE);
	}
	if (search == FIELD_NONE) {
		if (strchr(pattern, '/') != NULL)
			search = FIELD_ORIGIN;
		else
			search = FIELD_NAMEVER; /* Default search */
	}
	if (label == FIELD_NONE)
		label = search; /* By default, show what was searched  */

	switch(label) {
	case FIELD_NONE:
		break;		/* should never happen */
	case FIELD_ORIGIN:
		opt |= INFO_TAG_ORIGIN;
		break;
	case FIELD_NAME:
		opt |= INFO_TAG_NAME;
		break;
	case FIELD_NAMEVER:
		opt |= INFO_TAG_NAMEVER;
		break;
	case FIELD_COMMENT:
		opt |= INFO_TAG_NAMEVER|INFO_COMMENT;
		break;
	case FIELD_DESC:
		opt |= INFO_TAG_NAMEVER|INFO_DESCR;
		break;
	}

	if (pkgdb_open(&db, PKGDB_REMOTE) != EPKG_OK)
		return (EX_IOERR);

	if ((it = pkgdb_search(db, pattern, match, search, reponame)) == NULL) {
		pkgdb_close(db);
		return (EX_IOERR);
	}

	while ((ret = pkgdb_it_next(it, &pkg, flags)) == EPKG_OK) {
		print_info(pkg, opt);
		atleastone = true;
	}

	pkg_free(pkg);
	pkgdb_it_free(it);
	pkgdb_close(db);

	if (!atleastone)
		ret = EPKG_FATAL;

	return ((ret == EPKG_OK || ret == EPKG_END) ? EX_OK : EX_SOFTWARE);
}
