#include <assert.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>

#include "main.h"
#include "create.h"
#include "delete.h"
#include "info.h"
#include "which.h"
#include "add.h"
#include "version.h"
#include "register.h"
#include "repo.h"

static void usage(void);
static void usage_help(void);
static int exec_help(int, char **);

static struct commands {
	const char *name;
	int (*exec)(int argc, char **argv);
	void (*usage)(void);
} cmd[] = { 
	{ "add", exec_add, usage_add},
	{ "create", exec_create, usage_create},
	{ "delete", exec_delete, usage_delete},
	{ "help", exec_help, usage_help},
	{ "info", exec_info, usage_info},
	{ "register", exec_register, usage_register},
	{ "repo", exec_repo, usage_repo},
	{ "update", NULL, NULL},
	{ "upgrade", NULL, NULL},
	{ "version", exec_version, usage_version},
	{ "which", exec_which, usage_which},
};
#define cmd_len (int)(sizeof(cmd)/sizeof(cmd[0]))

static void
usage(void)
{
	fprintf(stderr, "usage: pkg <command> [<args>]\n\n"
			"Where <command> can be:\n");
	for (int i = 0; i < cmd_len; i++) {
		fprintf(stderr, "  %s\n", cmd[i].name);
	}
	exit(EX_USAGE);
}

static void
usage_help(void)
{
	fprintf(stderr, "help <command>\n");
}

static int
exec_help(int argc, char **argv)
{
	if (argc != 2) {
		usage_help();
		return (EX_USAGE);
	}

	for (int i = 0; i < cmd_len; i++) {
		if (strncmp(cmd[i].name, argv[1], CMD_MAX_LEN) == 0) {
			assert(cmd[i].usage != NULL);
			cmd[i].usage();
			return (0);
		}
	}

	// Command name not found
	warnx("%s is not a valid command", argv[1]);
	return (1);
}

int
main(int argc, char **argv)
{
	int i;
	struct commands *command = NULL;
	int ambiguous = -1;
	size_t len;

	if (argc < 2)
		usage();

	len = strnlen(argv[1], CMD_MAX_LEN);
	for (i = 0; i < cmd_len; i++) {
		if (strncmp(argv[1], cmd[i].name, len) == 0) {
			/* if we have the exact cmd */
			if (len == strnlen(cmd[i].name, CMD_MAX_LEN)) {
				command = &cmd[i];
				ambiguous = 0;
				break;
			}

			/*
			 * we already found a partial match so `argv[1]' is
			 * an ambiguous shortcut
			 */
			if (command != NULL)
				ambiguous = 1;
			else
				ambiguous = 0;

			command = &cmd[i];
		}
	}

	if (command == NULL)
		usage();

	if (ambiguous == 0) {
		argc--;
		argv++;
		assert(command->exec != NULL);
		return (command->exec(argc, argv));
	}

	if (ambiguous == 1) {
		warnx("Ambiguous command: %s, could be:", argv[1]);
		for (i = 0; i < cmd_len; i++)
			if (strncmp(argv[1], cmd[i].name, len) == 0)
				warnx("\t%s",cmd[i].name);
	}

	return (EX_USAGE);
}
