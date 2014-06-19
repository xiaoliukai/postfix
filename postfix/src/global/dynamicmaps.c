/*++
/* NAME
/*	dynamicmaps 3
/* SUMMARY
/*	load dictionaries dynamically
/* SYNOPSIS
/*	#include <dynamicmaps.h>
/*
/*	void dymap_init(const char *path)
/* DESCRIPTION
/*	This module reads the dynamicmaps.cf file and performs
/*	run-time loading of Postfix dictionaries. Each dynamicmaps.cf
/*	entry specifies the name of a dictionary type, the pathname
/*	of a shared-library object, the name of a "dict_open"
/*	function for access to individual dictionary entries, and
/*	optionally the name of a "mkmap_open" function for bulk-mode
/*	dictionary creation. The configuration file's parent directory
/*	is the default directory for shared-library objects with a
/*	relative pathname.
/*
/*	A dictionary may be installed without editing the file
/*	dynamicmaps.cf, by placing a configuration file under the
/*	directory dynamicmaps.cf.d, with the same format as
/*	dynamicmaps.cf.  These configuration file names must end in
/*	".cf".  As before, a configuration file's parent directory
/*	is the default directory for shared-library objects with a
/*	relative pathname. Thus, the directory dynamicmaps.cf.d may
/*	contain both configuration files and shared-library object
/*	files.
/*
/*	dymap_init() reads the specified configuration file which
/*	is in dynamicmaps.cf format, and hooks itself into the
/*	dict_open(), dict_mapnames(), and mkmap_open() functions.
/*
/*	dymap_init() may be called multiple times during a process
/*	lifetime, but it will not "unload" dictionaries that have
/*	already been linked into the process address space, nor
/*	will it hide their dictionaries types from later "open"
/*	requests.
/* SEE ALSO
/*	load_lib(3) low-level run-time linker adapter
/* DIAGNOSTICS
/*	Fatal errors: memory allocation problem, dictionary or
/*	dictionary function not available.  Panic: invalid use.
/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	LaMont Jones
/*	Hewlett-Packard Company
/*	3404 Harmony Road
/*	Fort Collins, CO 80528, USA
/*
/*	Wietse Venema
/*	IBM T.J. Watson Research
/*	P.O. Box 704
/*	Yorktown Heights, NY 10598, USA
/*--*/

 /*
  * System library.
  */
#include <sys_defs.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>

 /*
  * Utility library.
  */
#include <msg.h>
#include <mymalloc.h>
#include <htable.h>
#include <argv.h>
#include <dict.h>
#include <load_lib.h>
#include <vstring.h>
#include <vstream.h>
#include <vstring_vstream.h>
#include <stringops.h>
#include <split_at.h>
#include <scan_dir.h>

 /*
  * Global library.
  */
#include <mkmap.h>
#include <dynamicmaps.h>

#ifdef USE_DYNAMIC_MAPS

 /*
  * Contents of one dynamicmaps.cf entry.
  */
typedef struct {
    char   *soname;			/* shared-object file name */
    char   *dict_name;			/* dict_xx_open() function name */
    char   *mkmap_name;			/* mkmap_xx_open() function name */
} DYMAP_INFO;

static HTABLE *dymap_info;
static int dymap_hooks_done = 0;
static DICT_OPEN_EXTEND_FN saved_dict_open_hook = 0;
static MKMAP_OPEN_EXTEND_FN saved_mkmap_open_hook = 0;
static DICT_MAPNAMES_EXTEND_FN saved_dict_mapnames_hook = 0;

 /*
  * Mandatory dynamicmaps.cf.d/ configuration file suffix.
  */
#define DYMAP_CF_SUFFIX	".cf"

#define STREQ(x, y) (strcmp((x), (y)) == 0)

/* dymap_dict_lookup - look up "dict_foo_open" function */

static DICT_OPEN_FN dymap_dict_lookup(const char *dict_type)
{
    struct stat st;
    LIB_FN  fn[2];
    DICT_OPEN_FN dict_open_fn;
    DYMAP_INFO *dp;

    /*
     * Respect the hook nesting order.
     */
    if (saved_dict_open_hook != 0
	&& (dict_open_fn = saved_dict_open_hook(dict_type)) != 0)
	return (dict_open_fn);

    /*
     * Allow for graceful degradation when a database is unavailable. This
     * allows Postfix daemon processes to continue handling email with
     * reduced functionality.
     */
    if ((dp = (DYMAP_INFO *) htable_find(dymap_info, dict_type)) == 0)
	return (0);
    if (stat(dp->soname, &st) < 0) {
	msg_warn("unsupported dictionary type: %s (%s: %m)",
		 dict_type, dp->soname);
	return (0);
    }
    if (st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
	msg_warn("unsupported dictionary type: %s "
		 "(%s: file is writable by non-root users)",
		 dict_type, dp->soname);
	return (0);
    }
    fn[0].name = dp->dict_name;
    fn[1].name = 0;
    load_library_symbols(dp->soname, fn, (LIB_DP *) 0);
    return ((DICT_OPEN_FN) fn[0].fptr);
}

/* dymap_mkmap_lookup - look up "mkmap_foo_open" function */

static MKMAP_OPEN_FN dymap_mkmap_lookup(const char *dict_type)
{
    struct stat st;
    LIB_FN  fn[2];
    MKMAP_OPEN_FN mkmap_open_fn;
    DYMAP_INFO *dp;

    /*
     * Respect the hook nesting order.
     */
    if (saved_mkmap_open_hook != 0
	&& (mkmap_open_fn = saved_mkmap_open_hook(dict_type)) != 0)
	return (mkmap_open_fn);

    /*
     * All errors are fatal. If the postmap(1) or postalias(1) command can't
     * create the requested database, then graceful degradation is not
     * useful.
     */
    if ((dp = (DYMAP_INFO *) htable_find(dymap_info, dict_type)) == 0)
	msg_fatal("unsupported dictionary type: %s. "
		  "Is the postfix-%s package installed?",
		  dict_type, dict_type);
    if (!dp->mkmap_name)
	msg_fatal("unsupported dictionary type: %s does not support "
		  "bulk-mode creation.", dict_type);
    if (stat(dp->soname, &st) < 0)
	msg_fatal("unsupported dictionary type: %s (%s: %m). "
		  "Is the postfix-%s package installed?",
		  dict_type, dp->soname, dict_type);
    if (st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0)
	msg_fatal("unsupported dictionary type: %s "
		  "(%s: file is writable by non-root users)",
		  dict_type, dp->soname);
    fn[0].name = dp->mkmap_name;
    fn[1].name = 0;
    load_library_symbols(dp->soname, fn, (LIB_DP *) 0);
    return ((MKMAP_OPEN_FN) fn[0].fptr);
}

/* dymap_list - enumerate dynamically-linked database type names */

static void dymap_list(ARGV *map_names)
{
    HTABLE_INFO **ht_list, **ht;

    /*
     * Respect the hook nesting order.
     */
    if (saved_dict_mapnames_hook != 0)
	saved_dict_mapnames_hook(map_names);

    for (ht_list = ht = htable_list(dymap_info); *ht != 0; ht++)
	argv_add(map_names, ht[0]->key, ARGV_END);
    myfree((char *) ht_list);
}

/* dymap_entry_alloc - allocate dynamicmaps.cf entry */

static DYMAP_INFO *dymap_entry_alloc(char **argv)
{
    DYMAP_INFO *dp;

    dp = (DYMAP_INFO *) mymalloc(sizeof(*dp));
    dp->soname = mystrdup(argv[0]);
    dp->dict_name = mystrdup(argv[1]);
    dp->mkmap_name = argv[2] ? mystrdup(argv[2]) : 0;
    return (dp);
}

/* dymap_entry_free - htable(3) call-back to destroy dynamicmaps.cf entry */

static void dymap_entry_free(char *ptr)
{
    DYMAP_INFO *dp = (DYMAP_INFO *) ptr;

    myfree(dp->soname);
    myfree(dp->dict_name);
    if (dp->mkmap_name)
	myfree(dp->mkmap_name);
    myfree((char *) dp);
}

/* dymap_read_conf - read dynamicmaps.cf-like file */

static void dymap_read_conf(const char *path, const char *path_base)
{
    VSTREAM *fp;
    VSTRING *buf;
    char   *cp;
    ARGV   *argv;
    int     linenum = 0;
    struct stat st;

    /*
     * Silently ignore a missing dynamicmaps.cf file, but be explicit about
     * problems when the file does exist.
     */
    if ((fp = vstream_fopen(path, O_RDONLY, 0)) != 0) {
	if (fstat(vstream_fileno(fp), &st) < 0)
	    msg_fatal("%s: fstat failed; %m", path);
	if (st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
	    msg_warn("%s: file is writable by non-root users"
		     " -- skipping this file", path);
	} else {
	    buf = vstring_alloc(100);
	    while (vstring_get_nonl(buf, fp) != VSTREAM_EOF) {
		cp = vstring_str(buf);
		linenum++;
		if (*cp == '#' || *cp == '\0')
		    continue;
		argv = argv_split(cp, " \t");
		if (argv->argc != 3 && argv->argc != 4)
		    msg_fatal("%s, line %d: Expected \"dict-type .so-name dict"
			      "-function [mkmap-function]\"", path, linenum);
		if (!ISALNUM(argv->argv[0][0]))
		    msg_fatal("%s, line %d: unsupported syntax \"%s\"",
			      path, linenum, argv->argv[0]);
		if (argv->argv[1][0] != '/') {
		    cp = concatenate(path_base, "/", argv->argv[1], (char *) 0);
		    argv_replace_one(argv, 1, cp);
		    myfree(cp);
		}
		if (htable_locate(dymap_info, argv->argv[0]) != 0)
		    msg_warn("%s: ignoring duplicate entry for \"%s\"",
			     path, argv->argv[0]);
		else
		    htable_enter(dymap_info, argv->argv[0],
				 (char *) dymap_entry_alloc(argv->argv + 1));
		argv_free(argv);
	    }
	    vstring_free(buf);

	    /*
	     * Once-only: hook into the dict_open(3) and mkmap_open(3)
	     * infrastructure,
	     */
	    if (dymap_hooks_done == 0) {
		dymap_hooks_done = 1;
		saved_dict_open_hook = dict_open_extend(dymap_dict_lookup);
		saved_mkmap_open_hook = mkmap_open_extend(dymap_mkmap_lookup);
		saved_dict_mapnames_hook = dict_mapnames_extend(dymap_list);
	    }
	}
	vstream_fclose(fp);
    } else if (errno != ENOENT) {
	msg_fatal("%s: file open failed: %m", path);
    }
}

/* dymap_init - initialize dictionary type to soname etc. mapping */

void    dymap_init(const char *path)
{
    const char myname[] = "dymap_init";
    SCAN_DIR *dir;
    char   *path_base;
    char   *path_d;
    const char *conf_name;
    char   *path_d_conf;
    char   *suffix;

    /*
     * Reload dynamicsmaps.cf, but don't reload already-loaded plugins.
     */
    if (dymap_info != 0)
	htable_free(dymap_info, dymap_entry_free);
    dymap_info = htable_create(3);

    /*
     * Read dynamicmaps.cf.
     */
    path_base = mystrdup(path);
    (void) split_at_right(path_base, '/');
    dymap_read_conf(path, path_base);
    myfree(path_base);

    /*
     * Read dynamicmaps.cf.d/filename entries. We allow shared-object files
     * in dynamicmaps.cf.d. Therefore, configuration file names must have a
     * distinct suffix.
     */
    path_d = concatenate(path, ".d", (char *) 0);
    if ((dir = scan_dir_open(path_d)) != 0) {
	while ((conf_name = scan_dir_next(dir)) != 0) {
	    if ((suffix = strrchr(conf_name, '.')) != 0
		&& strcmp(suffix, DYMAP_CF_SUFFIX) == 0) {
		path_d_conf = concatenate(path_d, "/", conf_name, (char *) 0);
		dymap_read_conf(path_d_conf, path_d);
		myfree(path_d_conf);
	    } else if (errno != 0) {
		/* Don't crash all programs - degrade gracefully. */
		msg_warn("%s: directory read error: %m", path_d);
	    }
	}
	scan_dir_close(dir);
    } else if (errno != ENOENT) {
	/* Don't crash all programs - degrade gracefully. */
	msg_warn("%s: directory open failed: %m", path_d);
    }
    myfree(path_d);

    /*
     * Future proofing, in case someone "improves" the code. We can't hook
     * into other functions without initializing our private lookup table.
     */
    if (dymap_hooks_done != 0 && dymap_info == 0)
	msg_panic("%s: post-condition botch", myname);
}

#endif