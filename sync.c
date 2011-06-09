#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <alpm.h>
#include <curl/curl.h>

#include "curl.h"
#include "download.h"
#include "environment.h"
#include "error.h"
#include "graph.h"
#include "hash.h"
#include "hashdb.h"
#include "json.h"
#include "package.h"
#include "powaur.h"
#include "sync.h"
#include "util.h"

/* Return a list of struct pkginfo to be freed by caller */
static alpm_list_t *check_installed(alpm_list_t *targets)
{
	alpm_list_t *i, *j, *localdb_cache;
	alpm_list_t *ret = NULL;
	pmdb_t *localdb;
	pmpkg_t *pkg;
	struct pkginfo_t *info;

	localdb = alpm_option_get_localdb();
	ASSERT(localdb != NULL, return NULL);

	localdb_cache = alpm_db_get_pkgcache(localdb);
	for (j = targets; j; j = j->next) {
		for (i = localdb_cache; i; i = i->next) {
			pkg = i->data;
			if (!strcmp(j->data, alpm_pkg_get_name(pkg))) {
				info = pkginfo_new(alpm_pkg_get_name(pkg),
								   alpm_pkg_get_version(pkg),
								   alpm_pkg_get_installdate(pkg));

				ret = alpm_list_add(ret, info);
				break;
			}
		}
	}

	return ret;
}

/* Converts a list of char * (pkg names) to a list of struct pkginfo_t *
 */
static alpm_list_t *char_to_pkginfo(alpm_list_t *list)
{
	alpm_list_t *i, *k, *ret = NULL;
	pmdb_t *localdb;
	pmpkg_t *pkg;
	struct pkginfo_t *pkginfo;

	localdb = alpm_option_get_localdb();
	ASSERT(localdb != NULL, RET_ERR(PW_ERR_LOCALDB_NULL, NULL));

	for (i = list; i; i = i->next) {
		for (k = alpm_db_get_pkgcache(localdb); k; k = k->next) {
			pkg = k->data;
			if (!strcmp(i->data, alpm_pkg_get_name(pkg))) {
				pkginfo = pkginfo_new(i->data, alpm_pkg_get_version(pkg),
									  alpm_pkg_get_installdate(pkg));

				ret = alpm_list_add(ret, pkginfo);
				pkginfo = NULL;
			}
		}
	}

	return ret;
}

/* Saves install results in *success and *failure.
 * Returns a list of alpm_list_t * of struct pkginfo_t * to be freed by caller.
 *
 * This is an extremely ugly function.
 *
 * @param universe list of char * (pkg names)
 * @param org_installed SORTED list of struct pkginfo_t * sorted by name
 * @param nonzero_install SORTED list of char * that may be successfully installed
 * @param success pointer to list that will store successful pkgs
 * @param failure pointer to list that will store failed pkgs
 */
static alpm_list_t *get_install_results(alpm_list_t *universe, alpm_list_t *org_installed,
							   alpm_list_t *nonzero_install,
							   alpm_list_t **success, alpm_list_t **failure)
{
	/* success = (nonzero_install AND not(org_installed)) +
	 * 			 (nonzero_install AND org_installed AND install_date changed)
	 *
	 * failure = universe - success - org_installed
	 */

	alpm_list_t *i, *k;
	alpm_list_t *targets, *newly_installed, *updated_pkg, *tmp_failure;
	alpm_list_t *freelist, *new_universe;

	struct pkginfo_t *pkginfo;
	pmdb_t *localdb;
	pmpkg_t *pkg;

	new_universe = freelist = NULL;
	tmp_failure = targets = newly_installed = updated_pkg = NULL;

	/* ALERT: reload libalpm!!!
	 * This is just a hack to get around us having a not updated localdb
	 * even though packages may have been installed.
	 *
	 * So much just to display what packages have been successfully installed
	 * and stuff. I regret doing this but I wrote too much code and spent too
	 * much time on this to just give everything up.
	 */
	if (alpm_reload()) {
		RET_ERR(PW_ERR_ALPM_RELOAD, NULL);
	}

	/* Convert the important lists to pkginfo structs */
	targets = char_to_pkginfo(nonzero_install);

	new_universe = char_to_pkginfo(universe);
	new_universe = alpm_list_msort(new_universe, alpm_list_count(new_universe),
								   (alpm_list_fn_cmp) pkginfo_name_cmp);

	/* Get those in targets but not in org_installed */
	alpm_list_diff_sorted(targets, org_installed, pkginfo_name_cmp,
						  &newly_installed, NULL);

	pw_printf(PW_LOG_DEBUG, "New Packages installed by us:\n");
	if (!newly_installed) {
		pw_printf(PW_LOG_DEBUG, "None\n");
	} else if (config->loglvl & PW_LOG_DEBUG) {
		for (i = targets; i; i = i->next) {
			pw_printf(PW_LOG_DEBUG, "%s\n", ((struct pkginfo_t *)i->data)->name);
		}

		printf("\n");
	}

	/* Get those in targets and org_installed with install date
	 * modified */
	updated_pkg = list_intersect(targets, org_installed, pkginfo_mod_cmp);

	pw_printf(PW_LOG_DEBUG, "Reinstalled packages / Updated packages:\n");
	if (!updated_pkg) {
		pw_printf(PW_LOG_DEBUG, "None\n");
	} else if (config->loglvl & PW_LOG_DEBUG) {
		for (i = updated_pkg; i; i = i->next) {
			pw_printf(PW_LOG_DEBUG, "Install date modified = %s\n",
					  ((struct pkginfo_t *)i->data)->name);
		}

		printf("\n");
	}

	/* Join the 2 lists */
	if (success) {
		*success = alpm_list_join(newly_installed, updated_pkg);
	}

	/* Find the list of failures.
	 * failure = universe - success - org_installed
	 */
	if (failure) {
		tmp_failure = list_diff(new_universe, *success, pkginfo_name_cmp);
		*failure = list_diff(tmp_failure, org_installed, pkginfo_name_cmp);
	}

	alpm_list_free(tmp_failure);

	freelist = alpm_list_add(freelist, targets);
	freelist = alpm_list_add(freelist, new_universe);
	return freelist;
}

/* Installs a single succesfully downloaded PKGBUILD using makepkg -si
 * Assumes that the package has been extracted into its own directory
 *
 * returns 0 if installation is successful
 * returns -1 for errors, -2 for fatal errors
 */
static int install_single_package(char *pkgname)
{
	static const char choices[] = {'y', 'n', 'a'};
	int ret;
	char cwd[PATH_MAX];
	char buf[PATH_MAX];
	char *dotinstall;
	pid_t pid;

	if (!getcwd(cwd, PATH_MAX)) {
		return error(PW_ERR_GETCWD);
	}

	if (chdir(pkgname)) {
		return error(PW_ERR_CHDIR, pkgname);
	}

	/* Ask user to edit PKGBUILD */
	snprintf(buf, PATH_MAX, "Edit PKGBUILD for %s? [Y/n/a]", pkgname);
	ret = mcq(buf, choices, sizeof(choices) / sizeof(*choices), 0);

	switch (ret) {
	case 1:
		goto edit_dotinstall;
	case 2:
		/* Abort, propagate upwards */
		return -2;
	}

	/* Fork editor */
	pid = fork();
	if (pid == (pid_t) -1) {
		return error(PW_ERR_FORK_FAILED);
	} else if (pid == 0) {
		/* Open editor */
		execlp(powaur_editor, powaur_editor, "PKGBUILD", NULL);
	} else {
		ret = wait_or_whine(pid, "vim");
		if (ret) {
			return -1;
		}
	}

edit_dotinstall:
	/* First, check if we have .install file */
	dotinstall = have_dotinstall();
	if (dotinstall) {
		snprintf(buf, PATH_MAX, "Edit .install for %s? [Y/n/a]", pkgname);
		ret = mcq(buf, choices, sizeof(choices) / sizeof(*choices), 0);

		switch (ret) {
		case 1:
			goto fork_pacman;
		case 2:
			free(dotinstall);
			return -2;
		}

		pid = fork();
		if (pid == (pid_t) - 1) {
			return error(PW_ERR_FORK_FAILED);
		} else if (pid == 0) {
			execlp(powaur_editor, powaur_editor, dotinstall, NULL);
		} else {
			ret = wait_or_whine(pid, "vim");
			if (ret) {
				free(dotinstall);
				return -1;
			}
		}
	}

fork_pacman:
	free(dotinstall);

	if (!yesno("Continue installing %s?", pkgname)) {
		return -2;
	}

	pid = fork();
	if (pid == (pid_t) -1) {
		return error(PW_ERR_FORK_FAILED);
	} else if (pid == 0) {
		/* Install using pacman */
		execlp("makepkg", "makepkg", "-si", NULL);
	} else {
		/* Parent process */
		ret = wait_or_whine(pid, "makepkg");
		if (ret) {
			return -1;
		}
	}

	/* Change back to old directory */
	if (chdir(cwd)) {
		RET_ERR(PW_ERR_RESTORECWD, -2);
	}

	return 0;
}


/* Installs a list of successfully downloadaded targets using makepkg -si
 * returns -2 on abort, else returns -1 on error, 0 on success
 *
 * Packages that installed successfully are added to success
 */
static int install_packages(alpm_list_t *targets, alpm_list_t **success)
{
	alpm_list_t *i;
	int ret, status;
	char buf[PATH_MAX];

	ret = status = 0;
	for (i = targets; i; i = i->next) {
		fflush(stdout);
		fprintf(stderr, "\n");
		pw_fprintf(PW_LOG_WARNING, stderr,
				   "Installing unsupported package %s\n         "
				   "You are advised to look through the PKGBUILD.\n", i->data);


		snprintf(buf, PATH_MAX, "%s.tar.gz", i->data);
		if (extract_file(buf)) {
			pw_fprintf(PW_LOG_ERROR, stderr, "Unable to extract package \"%s\"", i->data);
			continue;
		}

		status = install_single_package((char *) i->data);
		if (status == -2) {
			return -2;
		} else if (status == -1) {
			ret = -1;
		} else {
			*success = alpm_list_add(*success, i->data);
		}
	}

	return ret;
}

/* Search sync db for packages. Only works for 1 package now. */
static int sync_search(CURL *curl, alpm_list_t *targets)
{
	alpm_list_t *i, *j, *search_results;
	pmdb_t *db;
	pmpkg_t *spkg;
	struct aurpkg_t *pkg;
	size_t listsz;

	search_results = query_aur(curl, targets->data, AUR_QUERY_SEARCH);
	if (search_results == NULL) {
		printf("Sorry, no results for %s\n", targets->data);
		return 0;
	}

	listsz = alpm_list_count(search_results);
	/* Sort by alphabetical order first */
	search_results = alpm_list_msort(search_results, listsz, aurpkg_name_cmp);

	/* Sort by votes */
	if (config->sort_votes) {
		search_results = alpm_list_msort(search_results, listsz, aurpkg_vote_cmp);
	}

	for (i = search_results; i; i = i->next) {
		pkg = (struct aurpkg_t *) i->data;
		printf("%saur/%s%s%s %s%s %s(%d)%s\n", color.bmag,
			   color.nocolor, color.bold, pkg->name,
			   color.bgreen, pkg->version,
			   color.votecol, pkg->votes, color.nocolor);
		printf("    %s\n", pkg->desc);
	}

	alpm_list_free_inner(search_results, (alpm_list_fn_free) aurpkg_free);
	alpm_list_free(search_results);

	return 0;
}

/* -Si, search inside sync dbs */
static pmpkg_t *search_syncdbs(alpm_list_t *dbs, const char *pkgname)
{
	alpm_list_t *i, *j;
	pmdb_t *sdb;
	pmpkg_t *spkg;

	for (i = dbs; i; i = i->next) {
		sdb = i->data;

		for (j = alpm_db_get_pkgcache(sdb); j; j = j->next) {
			spkg = j->data;
			if (!strcmp(pkgname, alpm_pkg_get_name(spkg))) {
				return spkg;
			}
		}
	}

	return NULL;
}

/* Lists detailed information about targets */
static int sync_info(CURL *curl, alpm_list_t *targets)
{
	int found, ret, pkgcount;
	alpm_list_t *i, *j, *results;
	alpm_list_t *free_list = NULL;
	alpm_list_t *syncdbs = alpm_option_get_syncdbs();
	pmpkg_t *spkg;

	char cwd[PATH_MAX];
	char filename[PATH_MAX];
	char url[PATH_MAX];
	FILE *fp = NULL;
	int fd;
	struct aurpkg_t *pkg;

	if (!getcwd(cwd, PATH_MAX)) {
		return error(PW_ERR_GETCWD);
	}

	if (chdir(powaur_dir)) {
		return error(PW_ERR_CHDIR, powaur_dir);
	}

	found = ret = pkgcount = 0;
	for (i = targets; i; i = i->next, ++pkgcount) {
		/* Search sync dbs first */
		spkg = search_syncdbs(syncdbs, i->data);
		if (spkg) {
			if (found++){
				printf("\n");
			}

			pacman_pkgdump(spkg, PKG_FROM_SYNC);
			spkg = NULL;
			continue;
		}

		results = query_aur(curl, i->data, AUR_QUERY_INFO);
		if (alpm_list_count(results) != 1) {
			if (pkgcount > 0) {
				printf("\n");
			}

			pw_printf(PW_LOG_ERROR, "package %s not found\n", i->data);
			goto garbage_collect;
		}

		snprintf(filename, PATH_MAX, "%s.PKGBUILDXXXXXX", i->data);
		fd = mkstemp(filename);

		if (fd < 0) {
			error(PW_ERR_FOPEN, filename);
			goto garbage_collect;
		}

		fp = fdopen(fd, "w+");
		if (!fp) {
			printf("NO\n");
			error(PW_ERR_FOPEN, filename);
			goto garbage_collect;
		}

		snprintf(url, PATH_MAX, AUR_PKGBUILD_URL, i->data);

		/* Download the PKGBUILD and parse it */
		ret = download_single_file(curl, url, fp);
		if (ret) {
			goto destroy_remnants;
		}

		/* Parse PKGBUILD and get detailed info */
		fseek(fp, 0L, SEEK_SET);

		pkg = results->data;
		parse_pkgbuild(pkg, fp);

		if (found++) {
			printf("\n");
		}

		printf("%s%s %saur%s\n", color.bold, REPO, color.bmag, color.nocolor);
		printf("%s%s %s%s\n", color.bold, NAME, pkg->name, color.nocolor);
		printf("%s%s %s%s%s\n", color.bold, VERSION, color.bgreen,
			   pkg->version, color.nocolor);
		printf("%s%s %s%s%s\n", color.bold, URL, color.bcyan, pkg->url,
			   color.nocolor);
		printf("%s%s%s ", color.bold, A_URL, color.bcyan);
		printf(AUR_PKG_URL, pkg->id);
		printf("%s\n", color.nocolor);

		printf("%s%s %s%s\n", color.bold, LICENSES, color.nocolor, pkg->license);
		printf("%s%s %s%d\n", color.bold, A_VOTES, color.nocolor, pkg->votes);
		printf("%s%s ", color.bold, A_OUTOFDATE);
		if (pkg->outofdate) {
			printf("%s%s", color.bred, "Yes");
		} else {
			printf("%s%s", color.nocolor, "No");
		}

		printf("%s\n", color.nocolor);

		print_list(pkg->provides, PROVIDES);
		print_list(pkg->depends, DEPS);
		print_list(pkg->optdepends, OPTDEPS);
		print_list(pkg->conflicts, CONFLICTS);
		print_list(pkg->replaces, REPLACES);
		print_list(pkg->arch, ARCH);

		printf("%s%s%s %s\n", color.bold, DESC, color.nocolor, pkg->desc);

destroy_remnants:
		fclose(fp);
		fp = NULL;
		unlink(filename);

garbage_collect:
		free_list = alpm_list_add(free_list, results);
	}

cleanup:
	for (i = free_list; i; i = i->next) {
		alpm_list_free_inner(i->data, (alpm_list_fn_free) aurpkg_free);
		alpm_list_free(i->data);
	}

	alpm_list_free(free_list);

	if (chdir(cwd)) {
		return error(PW_ERR_RESTORECWD);
	}

	return found ? 0 : -1;
}

/* Installs packages from the AUR
 * Assumes we are already in directory with all the relevant PKGBUILDS dled
 *
 * @param hashdb hash database
 * @param targets targets to be installed, in topo order
 */
static int topo_install(struct pw_hashdb *hashdb, alpm_list_t *targets)
{
	alpm_list_t *i;
	int ret;

	pw_printf(PW_LOG_INFO, "Upgrading:\n");
	for (i = targets; i; i = i->next) {
		printf("%s%s%s\n", color.bred, i->data, color.nocolor);
	}

	for (i = targets; i; i = i->next) {
		ret = install_single_package(i->data);
		if (ret == -2) {
			return ret;
		}
	}
	return 0;
}

/* Generates the list of packages we are going to install from the AUR, in
 * topological order.
 *
 * @param hashdb hash database
 * @param graph graph of package strings
 * @param topost stack containing topological order of packages
 */
static alpm_list_t *topo_get_targets(struct pw_hashdb *hashdb, struct graph *graph,
									 struct stack *topost)
{
	int curVertex, cnt = 0;
	const char *pkgname;
	enum pkgfrom_t *from = NULL;
	alpm_list_t *final_targets = NULL;

	pw_printf(PW_LOG_INFO, "Dependency graph:\n");
	while (!stack_empty(topost)) {
		stack_pop(topost, &curVertex);
		pkgname = graph_get_vertex_data(graph, curVertex);
		from = hashmap_search(hashdb->pkg_from, (void *) pkgname);

		if (cnt++) {
			printf(" -> ");
		}
		switch (*from) {
		case PKG_FROM_LOCAL:
			pw_printf(PW_LOG_NORM, "%s%s (installed)%s", color.bblue, pkgname, color.nocolor);
			break;
		case PKG_FROM_SYNC:
			pw_printf(PW_LOG_NORM, "%s%s (found in sync)%s", color.byellow, pkgname, color.nocolor);
			break;
		case PKG_FROM_AUR:
			/* Magic happens here */
			if (hash_search(hashdb->aur_outdated, (void *) pkgname)) {
				pw_printf(PW_LOG_NORM, "%s%s (AUR)%s", color.bred, pkgname, color.nocolor);
				final_targets = alpm_list_add(final_targets, (void *) pkgname);
			} else {
				pw_printf(PW_LOG_NORM, "%s%s (installed)%s", color.bblue, pkgname, color.nocolor);
			}
			break;
		default:
			/* Shouldn't happen */
			pw_printf(PW_LOG_NORM, "Unknown");
			break;
		}
	}

	printf("\n\n");
	return final_targets;
}

/* TODO: Resolve all deps instead of one by one
 * -Su, experimental feature
 * @param targets list of struct aurpkg_t *
 */
static int upgrade_pkgs(alpm_list_t *targets, struct pw_hashdb *hashdb)
{
	alpm_list_t *i;
	alpm_list_t *target_pkgs = NULL;
	alpm_list_t *final_targets = NULL;
	struct aurpkg_t *aurpkg;
	struct graph *graph;
	struct stack *topost;
	int ret = 0;

	char cwd[PATH_MAX];
	if (!getcwd(cwd, PATH_MAX)) {
		return error(PW_ERR_GETCWD);
	}

	if (chdir(powaur_dir)) {
		return error(PW_ERR_CHDIR);
	}

	graph = NULL;
	topost = stack_new(sizeof(int));

	for (i = targets; i; i = i->next) {
		aurpkg = i->data;
		target_pkgs = alpm_list_add(target_pkgs, aurpkg->name);
		/* Insert into outdated AUR packages to avoid dling up to date ones */
		hash_insert(hashdb->aur_outdated, (void *) aurpkg->name);
	}

	/* Build dep graph for all packages */
	build_dep_graph(&graph, hashdb, target_pkgs, GRAPH_NOT_DETAILED, NOFORCE);
	ret = graph_toposort(graph, topost);
	if (ret) {
		printf("Cyclic dependencies detected in package \"%s\"\n", aurpkg->name);
		goto cleanup;
	}

	/* TODO: Remove this? Not too informative for users */
	final_targets = topo_get_targets(hashdb, graph, topost);
	if (final_targets) {
		topo_install(hashdb, final_targets);
	}

cleanup:
	/* Install in topo order */
	graph_free(graph);
	stack_free(topost);
	alpm_list_free(target_pkgs);
	alpm_list_free(final_targets);
	if (chdir(cwd)) {
		return error(PW_ERR_RESTORECWD);
	}

	return ret;
}

/* Returns a list of outdated AUR packages.
 * The list and the packages are to be freed by the caller.
 *
 * @param curl curl easy handle
 * @param targets list of strings (package names) that are _definitely_ AUR packages
 */
static alpm_list_t *get_outdated_pkgs(CURL *curl, struct pw_hashdb *hashdb,
									  alpm_list_t *targets)
{
	alpm_list_t *i;
	alpm_list_t *outdated_pkgs = NULL;
	alpm_list_t *pkglist, *targs;
	struct pkgpair pkgpair;
	struct pkgpair *pkgpair_ptr;
	struct aurpkg_t *aurpkg;
	const char *pkgname, *pkgver;

	if (targets) {
		targs = targets;
	} else {
		targs = NULL;
		alpm_list_t *tmp_targs = hash_to_list(hashdb->aur);
		for (i = tmp_targs; i; i = i->next) {
			pkgpair_ptr = i->data;
			targs = alpm_list_add(targs, (void *) pkgpair_ptr->pkgname);
		}
		alpm_list_free(tmp_targs);
	}

	for (i = targs; i; i = i->next) {
		pkglist = query_aur(curl, i->data, AUR_QUERY_INFO);
		if (!pkglist) {
			continue;
		}

		pkgpair.pkgname = i->data;
		pkgpair_ptr = hash_search(hashdb->aur, &pkgpair);
		if (!pkgpair_ptr) {
			/* Shouldn't happen */
			pw_fprintf(PW_LOG_ERROR, stderr, "Unable to find AUR package \"%s\""
					   "in hashdb!\n", i->data);
		}

		aurpkg = pkglist->data;
		pkgver = alpm_pkg_get_version(pkgpair_ptr->pkg);
		pkgname = i->data;

		if (alpm_pkg_vercmp(aurpkg->version, pkgver) > 0) {
			/* Just show outdated package for now */
			pw_printf(PW_LOG_INFO, "%s %s is outdated, %s%s%s%s is available\n",
					  pkgname, pkgver, color.bred, aurpkg->version,
					  color.nocolor, color.bold);

			/* Add to upgrade list */
			outdated_pkgs = alpm_list_add(outdated_pkgs, aurpkg);
			pkglist->data = NULL;
		} else if (config->verbose) {
			pw_printf(PW_LOG_INFO, "%s %s is up to date.\n", pkgname,
					  pkgver);
		}

		alpm_list_free_inner(pkglist, (alpm_list_fn_free) aurpkg_free);
		alpm_list_free(pkglist);
	}

	if (!targets) {
		alpm_list_free(targs);
	}
	return outdated_pkgs;
}

/* -Su, checks AUR packages */
static int sync_upgrade(CURL *curl, alpm_list_t *targets)
{
	int ret = 0;
	int cnt = 0;
	int upgrade_all;
	struct pkgpair pkgpair;
	struct pw_hashdb *hashdb = build_hashdb();
	if (!hashdb) {
		pw_fprintf(PW_LOG_ERROR, stderr, "Failed to build hash database.");
		return -1;
	}

	/* Make sure that packages are from AUR */
	alpm_list_t *i, *new_targs = NULL;
	for (i = targets; i; i = i->next) {
		pkgpair.pkgname = i->data;
		if (!hash_search(hashdb->aur, &pkgpair)) {
			if (cnt++) {
				printf(", ");
			}

			pw_printf(PW_LOG_NORM, "%s", i->data);
		} else {
			new_targs = alpm_list_add(new_targs, i->data);
		}
	}

	if (cnt > 1) {
		printf(" are not AUR packages and will not be checked.\n");
	} else if (cnt == 1) {
		printf(" is not an AUR package and will not be checked.\n");
	}

	alpm_list_t *outdated_pkgs = NULL;
	if (!targets) {
		/* Check all AUR packages */
		outdated_pkgs = get_outdated_pkgs(curl, hashdb, NULL);
	} else {
		if (!new_targs) {
			goto cleanup;
		}

		outdated_pkgs = get_outdated_pkgs(curl, hashdb, new_targs);
	}

	if (!outdated_pkgs) {
		pw_printf(PW_LOG_INFO, "All AUR packages are up to date.\n");
		goto cleanup;
	}

	printf("\n");
	pw_printf(PW_LOG_INFO, "Targets:\n");
	print_aurpkg_list(outdated_pkgs);
	printf("\n");

	/* --check, don't upgrade */
	if (config->op_s_check) {
		goto cleanup;
	}

	upgrade_all = yesno("Do you wish to upgrade the above packages?");
	if (upgrade_all) {
		/* Experimental */
		ret = upgrade_pkgs(outdated_pkgs, hashdb);
	}

cleanup:
	alpm_list_free_inner(outdated_pkgs, (alpm_list_fn_free) aurpkg_free);
	alpm_list_free(outdated_pkgs);
	alpm_list_free(new_targs);
	hashdb_free(hashdb);
	return ret;
}

/* returns 0 upon success.
 * returns -1 upon failure to change dir / download PKGBUILD / install package
 */
int powaur_sync(alpm_list_t *targets)
{
	alpm_list_t *i;
	alpm_list_t *freelist;
	alpm_list_t *org_installed, *dl_failed, *final_targets;
	alpm_list_t *nonzero_install, *success, *failure;
	pmdb_t *localdb;
	int ret, status;
	char orgdir[PATH_MAX];
	CURL *curl;

	freelist = NULL;
	nonzero_install = org_installed = dl_failed = final_targets = NULL;
	success = failure = NULL;
	ret = status = 0;

	curl = curl_easy_new();
	if (!curl) {
		return error(PW_ERR_CURL_INIT);
	}

	/* Makes no sense to run info and search tgt */
	if (config->op_s_search && config->op_s_info) {
		pw_fprintf(PW_LOG_ERROR, stderr,
				   "-s (search) and -i (info) are mutually exclusive\n");
		ret = -1;
		goto final_cleanup;
	} else if (config->op_s_check && !config->op_s_upgrade) {
		pw_fprintf(PW_LOG_ERROR, stderr, "--check must be used with -u!\n");
		ret = -1;
		goto final_cleanup;
	}

	/* -Su, checks packages against AUR */
	if (config->op_s_upgrade) {
		ret = sync_upgrade(curl, targets);
		goto final_cleanup;
	}

	if (targets == NULL) {
		if (config->op_s_search) {
			ret = pacman_db_dump(PKG_FROM_SYNC, DUMP_S_SEARCH);
		} else if (config->op_s_info) {
			ret = pacman_db_dump(PKG_FROM_SYNC, DUMP_S_INFO);
		} else {
			pw_fprintf(PW_LOG_ERROR, stderr, "No targets specified for sync\n");
			ret = -1;
		}

		goto final_cleanup;
	}

	if (config->op_s_search) {
		/* Search for packages on AUR */
		ret = sync_search(curl, targets);
		goto final_cleanup;
	} else if (config->op_s_info) {
		ret = sync_info(curl, targets);
		goto final_cleanup;
	}

	/* Save our current directory */
	if (!getcwd(orgdir, PATH_MAX)) {
		ret = error(PW_ERR_GETCWD);
		goto final_cleanup;
	}

	if (ret = chdir(powaur_dir)) {
		error(PW_ERR_CHDIR, powaur_dir);
		goto cleanup;
	}

	alpm_list_t *iptr;
	struct pkginfo_t *pkginfo;
	org_installed = check_installed(targets);
	org_installed = alpm_list_msort(org_installed, alpm_list_count(org_installed),
									pkginfo_name_cmp);

	/* Check */
	for (iptr = org_installed; iptr; iptr = iptr->next) {
		pkginfo = iptr->data;
		pw_printf(PW_LOG_DEBUG, "Originally installed: %s %s\n",
				  pkginfo->name, pkginfo->version);
	}

	/* DL the packages, get the working ones and install them */
	download_packages(curl, targets, &dl_failed);
	if (pwerrno == PW_ERR_ACCESS) {
		goto cleanup;
	}

	final_targets = list_diff(targets, dl_failed, (alpm_list_fn_cmp) strcmp);

	ret = install_packages(final_targets, &nonzero_install);

	nonzero_install = alpm_list_msort(nonzero_install,
									  alpm_list_count(nonzero_install),
									  (alpm_list_fn_cmp) strcmp);

	freelist = get_install_results(targets, org_installed, nonzero_install,
						&success, &failure);

	if (org_installed) {
		printf("\n");
		pw_printf(PW_LOG_INFO, "These packages were originally installed:\n");
		print_pkginfo(org_installed);
	}

	if (success) {
		printf("\n");
		pw_printf(PW_LOG_INFO,
				  "The following packages were successfully installed\n");
		print_pkginfo(success);
	}

	if (failure) {
		printf("\n");
		pw_printf(PW_LOG_WARNING, "The following packages were not installed:\n");
		print_pkginfo(failure);
	}

cleanup:
	if (chdir(orgdir)) {
		PW_SETERRNO(PW_ERR_RESTORECWD);
	}

	alpm_list_free_inner(org_installed, (alpm_list_fn_free) pkginfo_free_all);
	alpm_list_free(org_installed);

	alpm_list_free(dl_failed);
	alpm_list_free(final_targets);
	alpm_list_free(nonzero_install);

	for (i = freelist; i; i = i->next) {
		alpm_list_free_inner(i->data, (alpm_list_fn_free) pkginfo_free_all);
		alpm_list_free(i->data);
	}

	alpm_list_free(freelist);
	alpm_list_free(success);
	alpm_list_free(failure);

final_cleanup:
	curl_easy_cleanup(curl);

	return ret ? -1 : 0;
}

/* -M (--maintainer) search. */
int powaur_maint(alpm_list_t *targets)
{
	if (!targets) {
		pw_printf(PW_LOG_ERROR, "argument needed for -M\n");
		return -1;
	} else if (alpm_list_count(targets) > 1) {
		pw_printf(PW_LOG_ERROR, "-M only takes 1 argument\n");
		return -1;
	}

	int ret;
	size_t listsz;
	alpm_list_t *i, *results;
	struct aurpkg_t *pkg;
	CURL *curl;

	curl = curl_easy_new();
	if (!curl) {
		return error(PW_ERR_CURL_INIT);
	}

	/* Clear pwerrno */
	CLEAR_ERRNO();
	results = query_aur(curl, targets->data, AUR_QUERY_MSEARCH);

	if (pwerrno != PW_ERR_OK) {
		ret = -1;
		goto cleanup;
	} else if (!results) {
		printf("No packages found.\n");
		ret = -1;
		goto cleanup;
	}

	/* Sort by alphabetical order */
	listsz = alpm_list_count(results);
	results = alpm_list_msort(results, listsz, aurpkg_name_cmp);

	if (config->sort_votes) {
		results = alpm_list_msort(results, listsz, aurpkg_vote_cmp);
	}

	for (i = results; i; i = i->next) {
		pkg = i->data;
		printf("%saur/%s%s%s %s%s %s(%d)%s\n", color.bmag, color.nocolor,
			   color.bold, pkg->name, color.bgreen, pkg->version,
			   color.byellow, pkg->votes, color.nocolor);
		printf("%s%s\n", TAB, pkg->desc);
	}

cleanup:
	alpm_list_free_inner(results, (alpm_list_fn_free) aurpkg_free);
	alpm_list_free(results);
	curl_easy_cleanup(curl);

	return 0;
}
