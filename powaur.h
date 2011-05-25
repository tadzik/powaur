#ifndef POWAUR_POWAUR_H
#define POWAUR_POWAUR_H

enum {
	PW_OP_INVAL = 0,
	PW_OP_MAIN,
	PW_OP_SYNC,
	PW_OP_QUERY,
	PW_OP_GET,
	PW_OP_MAINTAINER,
	PW_OP_BACKUP
};

enum {
	OP_DEBUG = 1000,
	OPT_SORT_VOTE,
	OPT_VERBOSE,
	OPT_TARGET_DIR,
	OPT_RESOLVE_DEPS,
	SEARCH_OR,
	SEARCH_AND
};

enum pwloglevel_t {
	PW_LOG_NORM = 1,
	PW_LOG_INFO = (1 << 1),
	PW_LOG_WARNING = (1 << 2),
	PW_LOG_ERROR = (1 << 3),
	PW_LOG_DEBUG = (1 << 4)
};

enum pkgfrom_t {
	PKG_FROM_LOCAL,
	PKG_FROM_SYNC
};

enum _pw_errno_t {
	PW_ERR_OK = 0,

	/* Initialization errors */
	PW_ERR_INIT_CONFIG,
	PW_ERR_INIT_ENV,
	PW_ERR_INIT_HANDLE,
	PW_ERR_INIT_DIR,
	PW_ERR_INIT_LOCALDB,

	/* Command parsing errors */
	PW_ERR_OP_UNKNOWN,
	PW_ERR_OP_MULTI,
	PW_ERR_OP_NULL,

	/* /etc/pacman.conf parsing errors */
	PW_ERR_PM_CONF_OPEN,
	PW_ERR_PM_CONF_PARSE,

	/* Fatal errors */
	PW_ERR_ACCESS,

	/* libalpm errors */
	PW_ERR_ALPM_RELOAD,
	PW_ERR_LOCALDB_NULL,
	PW_ERR_LOCALDB_CACHE_NULL,

	/* General errors */
	PW_ERR_MEMORY,

	/* Path related errors */
	PW_ERR_GETCWD,
	PW_ERR_RESTORECWD,
	PW_ERR_CHDIR,
	PW_ERR_PATH_RESOLVE,

	/* File related errors */
	PW_ERR_ISDIR,
	PW_ERR_FOPEN,
	PW_ERR_FILE_EXTRACT,
	PW_ERR_OPENDIR,
	PW_ERR_STAT,

	/* Fork errors */
	PW_ERR_FORK_FAILED,
	PW_ERR_WAITPID_FAILED,
	PW_ERR_WAITPID_CONFUSED,
	PW_ERR_WAITPID_SIGNAL,

	/* libarchive errors */
	PW_ERR_ARCHIVE_CREATE,
	PW_ERR_ARCHIVE_OPEN,
	PW_ERR_ARCHIVE_ENTRY,
	PW_ERR_ARCHIVE_EXTRACT,

	/* cURL errors */
	PW_ERR_CURL_INIT,
	PW_ERR_CURL_DOWNLOAD,

	/* Download errors */
	PW_ERR_DL_UNKNOWN,

	/* NULL target list */
	PW_ERR_TARGETS_NULL
};

#endif
