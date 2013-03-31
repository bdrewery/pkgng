/*-
 * Copyright (c) 2012 Julien Laffaye <jlaffaye@FreeBSD.org>
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

#include <sys/stat.h>
#include <sys/param.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <archive.h>
#include <archive_entry.h>

#include "pkg.h"
#include "private/event.h"
#include "private/utils.h"
#include "private/pkgdb.h"
#include "private/repodb.h"

//#define EXTRACT_ARCHIVE_FLAGS  (ARCHIVE_EXTRACT_OWNER |ARCHIVE_EXTRACT_PERM)

/* Add indexes to the repo */
static int
remote_add_indexes(const char *reponame)
{
	struct pkgdb *db = NULL;
	int ret = EPKG_FATAL;

	if (pkgdb_open(&db, PKGDB_REMOTE) != EPKG_OK)
		goto cleanup;

	/* Initialize the remote remote */
	if (pkgdb_remote_init(db, reponame) != EPKG_OK)
		goto cleanup;

	ret = EPKG_OK;

	cleanup:
	if (db)
		pkgdb_close(db);
	return (ret);
}

/* Return opened file descriptor */
static int
repo_fetch_remote_tmp(const char *reponame, const char *filename, const char *extension, time_t *t, int *rc)
{
	char url[MAXPATHLEN];
	char tmp[MAXPATHLEN];
	int fd;
	const char *tmpdir;

	snprintf(url, MAXPATHLEN, "%s/%s.%s", reponame, filename, extension);

	tmpdir = getenv("TMPDIR");
	if (tmpdir == NULL)
		tmpdir = "/tmp";
	snprintf(tmp, MAXPATHLEN, "%s/%s.%s.XXXXXX", tmpdir, filename, extension);

	fd = mkstemp(tmp);
	if (fd == -1) {
		pkg_emit_error("Could not create temporary file %s, "
		    "aborting update.\n", tmp);
		*rc = EPKG_FATAL;
		return -1;
	}
	(void)unlink(tmp);

	if ((*rc = pkg_fetch_file_to_fd(url, fd, t)) != EPKG_OK) {
		close(fd);
		fd = -1;
	}

	return fd;
}

static int
repo_archive_extract_file(int fd, const char *file, const char *dest)
{
	struct archive *a = NULL;
	struct archive_entry *ae = NULL;
	const char *repokey;
	unsigned char *sig = NULL;
	int siglen = 0, ret, rc = EPKG_OK;

	a = archive_read_new();
	archive_read_support_filter_all(a);
	archive_read_support_format_tar(a);

	/* Seek to the begin of file */
	(void)lseek(fd, SEEK_SET, 0);
	archive_read_open_fd(a, fd, 4096);

	while (archive_read_next_header(a, &ae) == ARCHIVE_OK) {
		if (strcmp(archive_entry_pathname(ae), file) == 0) {
			archive_entry_set_pathname(ae, dest);
			/*
			 * The repo should be owned by root and not writable
			 */
			archive_entry_set_uid(ae, 0);
			archive_entry_set_gid(ae, 0);
			archive_entry_set_perm(ae, 0644);

			archive_read_extract(a, ae, EXTRACT_ARCHIVE_FLAGS);
		}
		if (strcmp(archive_entry_pathname(ae), "signature") == 0) {
			siglen = archive_entry_size(ae);
			sig = malloc(siglen);
			archive_read_data(a, sig, siglen);
		}
	}

	if (pkg_config_string(PKG_CONFIG_REPOKEY, &repokey) != EPKG_OK) {
		free(sig);
		pkg_emit_error("Cannot get repository key.");
		rc = EPKG_FATAL;
		goto cleanup;
	}

	if (repokey != NULL) {
		if (sig != NULL) {
			ret = rsa_verify(dest, repokey,
					sig, siglen - 1);
			if (ret != EPKG_OK) {
				pkg_emit_error("Invalid signature, "
						"removing repository.");
				unlink(dest);
				free(sig);
				rc = EPKG_FATAL;
				goto cleanup;
			}
			free(sig);
		} else {
			pkg_emit_error("No signature found in the repository.  "
					"Can not validate against %s key.", repokey);
			rc = EPKG_FATAL;
			unlink(dest);
			goto cleanup;
		}
	}
cleanup:
	if (a != NULL)
		archive_read_free(a);

	return rc;
}

static int
pkg_update_full(const char *repofile, const char *name, const char *packagesite, time_t *mtime)
{
	char repofile_unchecked[MAXPATHLEN];
	int fd = -1, rc = EPKG_FATAL;
	sqlite3 *sqlite = NULL;
	sqlite3_stmt *stmt;
	char *req = NULL;
	char *bad_abis = NULL;
	const char *myarch;
	const char sql[] = ""
	    "INSERT OR REPLACE INTO repodata (key, value) "
	    "VALUES (\"packagesite\", ?1);";

	snprintf(repofile_unchecked, sizeof(repofile_unchecked),
			"%s.unchecked", repofile);

	/* If the repo.sqlite file exists, test that we can write to
		   it.  If it doesn't exist, assume we can create it */

	if (eaccess(repofile, F_OK) == 0 && eaccess(repofile, W_OK) == -1) {
		pkg_emit_error("Insufficient privilege to update %s\n",
				repofile);
		rc = EPKG_ENOACCESS;
		goto cleanup;
	}

	if ((fd = repo_fetch_remote_tmp(packagesite, repo_db_archive, "txz", mtime, &rc)) == -1) {
		goto cleanup;
	}

	if ((rc = repo_archive_extract_file(fd, repo_db_file, repofile_unchecked)) != EPKG_OK) {
		goto cleanup;
	}

	/* check is the repository is for valid architecture */
	if (access(repofile_unchecked, R_OK|W_OK) == -1) {
		pkg_emit_error("Archive file does not have repo.sqlite file");
		rc = EPKG_FATAL;
		goto cleanup;
	}
	if (sqlite3_open(repofile_unchecked, &sqlite) != SQLITE_OK) {
		unlink(repofile_unchecked);
		pkg_emit_error("Corrupted repository");
		rc = EPKG_FATAL;
		goto cleanup;
	}

	pkg_config_string(PKG_CONFIG_ABI, &myarch);

	req = sqlite3_mprintf("select group_concat(arch, ', ') from "
			"(select arch from packages "
			"where arch not GLOB '%q')", myarch);
	if (get_sql_string(sqlite, req, &bad_abis) != EPKG_OK) {
		sqlite3_free(req);
		pkg_emit_error("Unable to query repository");
		rc = EPKG_FATAL;
		sqlite3_close(sqlite);
		goto cleanup;
	}

	if (bad_abis != NULL) {
		pkg_emit_error("At least one of the packages provided by "
				"the repository is not compatible with your ABI:\n"
				"    Your ABI: %s\n"
				"    Incompatible ABIs found: %s",
				myarch, bad_abis);
		rc = EPKG_FATAL;
		sqlite3_close(sqlite);
		goto cleanup;
	}

	/* register the packagesite */
	if (sql_exec(sqlite, "CREATE TABLE IF NOT EXISTS repodata ("
			"   key TEXT UNIQUE NOT NULL,"
			"   value TEXT NOT NULL"
			");") != EPKG_OK) {
		pkg_emit_error("Unable to register the packagesite in the "
				"database");
		rc = EPKG_FATAL;
		sqlite3_close(sqlite);
		goto cleanup;
	}

	if (sqlite3_prepare_v2(sqlite, sql, -1, &stmt, NULL) != SQLITE_OK) {
		ERROR_SQLITE(sqlite);
		rc = EPKG_FATAL;
		sqlite3_close(sqlite);
		goto cleanup;
	}

	sqlite3_bind_text(stmt, 1, packagesite, -1, SQLITE_STATIC);

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		ERROR_SQLITE(sqlite);
		rc = EPKG_FATAL;
		sqlite3_close(sqlite);
		goto cleanup;
	}

	sqlite3_finalize(stmt);

	sqlite3_close(sqlite);
	sqlite3_shutdown();


	if (rename(repofile_unchecked, repofile) != 0) {
		pkg_emit_errno("rename", "");
		rc = EPKG_FATAL;
		goto cleanup;
	}

	if ((rc = remote_add_indexes(name)) != EPKG_OK)
		goto cleanup;
	rc = EPKG_OK;

	cleanup:
	if (fd != -1)
		(void)close(fd);

	return (rc);
}

static int
pkg_add_from_manifest(FILE *f, const char *origin, long offset,
		const char *manifest_digest, sqlite3 *sqlite)
{
	int rc = EPKG_OK;
	struct pkg *pkg;
	const char *local_origin;

	if (fseek(f, offset, SEEK_SET) == -1) {
		pkg_emit_errno("fseek", "invalid manifest offset");
		return (EPKG_FATAL);
	}

	rc = pkg_new(&pkg, PKG_REMOTE);
	if (rc != EPKG_OK)
		return (EPKG_FATAL);

	rc = pkg_parse_manifest_file(pkg, f);
	if (rc != EPKG_OK) {
		rc = EPKG_FATAL;
		goto cleanup;
	}

	/* Ensure that we have a proper origin */
	pkg_get(pkg, PKG_ORIGIN, &local_origin);
	if (local_origin == NULL || strcmp(local_origin, origin) != 0) {
		pkg_emit_error("manifest contains origin %s while we wanted to add origin %s",
				local_origin ? local_origin : "NULL", origin);
		rc = EPKG_FATAL;
		goto cleanup;
	}

	rc = pkgdb_repo_add_package(pkg, NULL, sqlite, manifest_digest, true);

cleanup:
	pkg_free(pkg);

	return (rc);
}

static int
pkg_update_incremental(const char *name, const char *packagesite, time_t *mtime)
{
	FILE *fmanifest = NULL, *fdigests = NULL;
	int fd_manifest, fd_digests;
	sqlite3 *sqlite = NULL;
	struct pkg *local_pkg;
	int rc = EPKG_FATAL, ret;
	const char *local_origin, *local_digest;
	struct pkgdb_it *it;
	char linebuf[1024], *digest_origin, *digest_digest, *digest_offset, *p;
	int updated = 0, removed = 0, added = 0;
	long num_offset;

	if ((rc = pkgdb_repo_open(name, true, &sqlite)) != EPKG_OK)
		goto cleanup;

	if ((rc = pkgdb_repo_init(sqlite)) != EPKG_OK)
		goto cleanup;

	it = pkgdb_repo_origins(sqlite);
	if (it == NULL) {
		rc = EPKG_FATAL;
		goto cleanup;
	}

	/* Try to get a single entry in repository to ensure that we can read checksums */
	pkgdb_it_next(it, &local_pkg, PKG_LOAD_BASIC);
	pkg_get(local_pkg, PKG_ORIGIN, &local_origin, PKG_DIGEST, &local_digest);
	if (local_digest == NULL || local_origin == NULL) {
		pkg_emit_notice("incremental update is not possible as "
				"repo format is inappropriate, trying full upgrade");
		rc = EPKG_FATAL;
		goto cleanup;
	}

	if ((fd_digests = repo_fetch_remote_tmp(packagesite, repo_digests_archive,
			"txz", mtime, &rc)) == -1)
		goto cleanup;
	if ((fd_manifest = repo_fetch_remote_tmp(packagesite, repo_packagesite_archive,
			"txz", mtime, &rc)) == -1)
		goto cleanup;

	do {
		pkg_get(local_pkg, PKG_ORIGIN, &local_origin, PKG_DIGEST, &local_digest);
		/* Read a line from digests file */
		if (fgets(linebuf, sizeof(linebuf) - 1, fdigests) == NULL) {
			while ((ret = pkgdb_it_next(it, &local_pkg, PKG_LOAD_BASIC)) == EPKG_OK) {
				/* Remove packages */
				rc = pkgdb_repo_remove_package(local_pkg);
				if (rc != EPKG_OK)
					goto cleanup;
				removed ++;
			}
		}
		p = linebuf;
		digest_origin = strsep(&p, ":");
		digest_digest = strsep(&p, ":");
		digest_offset = strsep(&p, ":");
		if (digest_origin == NULL || digest_digest == NULL ||
				digest_offset == NULL) {
			pkg_emit_error("invalid digest file format");
			rc = EPKG_FATAL;
			goto cleanup;
		}
		errno = 0;
		num_offset = (long)strtoul(digest_offset, NULL, 10);
		if (errno != 0) {
			pkg_emit_errno("strtoul", "digest format error");
			rc = EPKG_FATAL;
			goto cleanup;
		}
		/*
		 * Now we have local and remote origins that are sorted,
		 * so here are possible cases:
		 * 1) local == remote, but hashes are different: upgrade
		 * 2) local > remote, delete packages till local == remote
		 * 3) local < remote, insert new packages till local == remote
		 * 4) local == remote and hashes are the same, skip
		 */
		ret = strcmp(local_origin, digest_origin);
		if (ret == 0) {
			if (strcmp(digest_digest, local_digest) != 0) {
				/* Do upgrade */
				rc = pkg_add_from_manifest(fmanifest, digest_origin,
						num_offset, digest_digest, sqlite);
				updated ++;
			}
			else {
				/* Skip to next iteration */
				ret = pkgdb_it_next(it, &local_pkg, PKG_LOAD_BASIC);
			}
		}
		else if (ret > 0) {
			do {
				/* Remove packages */
				pkg_get(local_pkg, PKG_ORIGIN, &local_origin, PKG_DIGEST, &local_digest);
				if (strcmp(local_origin, digest_origin) <= 0) {
					break;
				}
				rc = pkgdb_repo_remove_package(local_pkg);
				if (rc != EPKG_OK)
					goto cleanup;
				removed ++;
				ret = pkgdb_it_next(it, &local_pkg, PKG_LOAD_BASIC);
			} while (ret == EPKG_OK);
		}
		else if (ret < 0) {
			for (;;) {
				if (strcmp(local_origin, digest_origin) < 0) {
					/* Insert a package from manifest */
					added ++;
					rc = pkg_add_from_manifest(fmanifest, digest_origin,
							num_offset, digest_digest, sqlite);
				}
				else {
					break;
				}
				if (fgets(linebuf, sizeof(linebuf) - 1, fdigests) == NULL) {
					break;
				}
				p = linebuf;
				digest_origin = strsep(&p, ":");
				digest_digest = strsep(&p, ":");
				digest_offset = strsep(&p, ":");
				if (digest_origin == NULL || digest_digest == NULL ||
						digest_offset == NULL) {
					pkg_emit_error("invalid digest file format");
					rc = EPKG_FATAL;
					goto cleanup;
				}
				errno = 0;
				num_offset = (long)strtoul(digest_offset, NULL, 10);
				if (errno != 0) {
					pkg_emit_errno("strtoul", "digest format error");
					rc = EPKG_FATAL;
					goto cleanup;
				}
			}
		}
	} while (ret == EPKG_OK);

	rc = EPKG_OK;
cleanup:
	if (pkgdb_repo_close(sqlite, rc == EPKG_OK) != EPKG_OK) {
		rc = EPKG_FATAL;
	}
	if (fmanifest)
		fclose(fmanifest);
	if (fdigests)
		fclose(fdigests);

	return (rc);
}

int
pkg_update(const char *name, const char *packagesite, bool force)
{
	char repofile[MAXPATHLEN];

	const char *dbdir = NULL;
	struct stat st;
	time_t t = 0;
	sqlite3 *sqlite = NULL;
	char *req = NULL;
	int64_t res;

	sqlite3_initialize();

	if (pkg_config_string(PKG_CONFIG_DBDIR, &dbdir) != EPKG_OK) {
		pkg_emit_error("Cant get dbdir config entry");
		return (EPKG_FATAL);
	}

	snprintf(repofile, sizeof(repofile), "%s/%s.sqlite", dbdir, name);

	if (!force)
		if (stat(repofile, &st) != -1)
			t = st.st_mtime;

	if (t != 0) {
		if (sqlite3_open(repofile, &sqlite) != SQLITE_OK) {
			pkg_emit_error("Unable to open local database");
			return (EPKG_FATAL);
		}

		if (get_pragma(sqlite, "SELECT count(name) FROM sqlite_master "
		    "WHERE type='table' AND name='repodata';", &res) != EPKG_OK) {
			pkg_emit_error("Unable to query repository");
			sqlite3_close(sqlite);
			return (EPKG_FATAL);
		}
		if (res != 1)
			t = 0;
	}

	if (t != 0) {
		req = sqlite3_mprintf("select count(key) from repodata "
		    "WHERE key = \"packagesite\" and value = '%q'", packagesite);

		if (get_pragma(sqlite, req, &res) != EPKG_OK) {
			sqlite3_free(req);
			pkg_emit_error("Unable to query repository");
			sqlite3_close(sqlite);
			return (EPKG_FATAL);
		}
		sqlite3_free(req);
		if (res != 1)
			t = 0;

		if (sqlite != NULL)
			sqlite3_close(sqlite);
	}
	if (false) {
		/* XXX: Still disabled by default */
		res = pkg_update_incremental(name, packagesite, &t);
		if (res != EPKG_OK) {
			/* Still try to do full upgrade */
			if ((res = pkg_update_full(repofile, name, packagesite, &t)) != EPKG_OK)
				goto cleanup;
		}
	}
	else {
		if ((res = pkg_update_full(repofile, name, packagesite, &t)) != EPKG_OK)
			goto cleanup;
	}

	res = EPKG_OK;
cleanup:
	/* Set mtime from http request if possible */
	if (t != 0) {
		struct timeval ftimes[2] = {
			{
			.tv_sec = t,
			.tv_usec = 0
			},
			{
			.tv_sec = t,
			.tv_usec = 0
			}
		};
		utimes(repofile, ftimes);
	}

	return (res);
}
