/*
 * Copyright (C) 2012-2018 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * This code is a complete clean re-write of the stress tool by
 * Colin Ian King <colin.king@canonical.com> and attempts to be
 * backwardly compatible with the stress tool by Amos Waterland
 * <apw@rossby.metr.ou.edu> but has more stress tests and more
 * functionality.
 */
#include "stress-ng.h"

#if defined(HAVE_MNTENT_H) &&		\
    defined(HAVE_SYS_SELECT_H) && 	\
    defined(HAVE_SYS_FANOTIFY_H) &&	\
    defined(HAVE_FANOTIFY)

#define BUFFER_SIZE	(4096)

/* fanotify stats */
typedef struct {
	uint64_t	open;
	uint64_t	close_write;
	uint64_t	close_nowrite;
	uint64_t	access;
	uint64_t	modify;
} fanotify_account_t;

#endif

#if defined(HAVE_FANOTIFY) &&	\
    defined(HAVE_SYS_SELECT_H)
/*
 *  stress_fanotify_supported()
 *      check if we can run this as root
 */
static int stress_fanotify_supported(void)
{
	if (geteuid() != 0) {
		pr_inf("fanotify stressor will be skipped, "
			"need to be running as root for this stressor\n");
		return -1;
	}
	return 0;
}

/*
 *  fanotify_event_init()
 *	initialize fanotify
 */
static int fanotify_event_init(const char *name)
{
	int fan_fd, count = 0;
	FILE* mounts;
	struct mntent* mnt;

	if ((fan_fd = fanotify_init(0, 0)) < 0) {
		pr_err("%s: cannot initialize fanotify, errno=%d (%s)\n",
			name, errno, strerror(errno));
		return -1;
	}

	/* No paths given, do all mount points */
	if ((mounts = setmntent("/proc/self/mounts", "r")) == NULL) {
		(void)close(fan_fd);
		pr_err("%s: setmntent cannot get mount points from "
			"/proc/self/mounts, errno=%d (%s)\n",
			name, errno, strerror(errno));
		return -1;
	}

	/*
	 *  Gather all mounted file systems and monitor them
	 */
	while ((mnt = getmntent(mounts)) != NULL) {
		int ret;

		ret = fanotify_mark(fan_fd, FAN_MARK_ADD | FAN_MARK_MOUNT,
			FAN_ACCESS| FAN_MODIFY | FAN_OPEN | FAN_CLOSE |
			FAN_ONDIR | FAN_EVENT_ON_CHILD, AT_FDCWD, mnt->mnt_dir);
		if (ret == 0)
			count++;
	}

	if (endmntent(mounts) < 0) {
		(void)close(fan_fd);
		pr_err("%s: endmntent failed, errno=%d (%s)\n",
			name, errno, strerror(errno));
		return -1;
	}

	/* This really should not happen, / is always mounted */
	if (!count) {
		pr_err("%s: no mount points could be monitored\n",
			name);
		(void)close(fan_fd);
		return -1;
	}
	return fan_fd;
}

/*
 *  stress_fanotify()
 *	stress fanotify
 */
static int stress_fanotify(const args_t *args)
{
	char dirname[PATH_MAX - 16], filename[PATH_MAX];
	int ret, fan_fd, pid, rc = EXIT_SUCCESS;
	fanotify_account_t account;

	(void)memset(&account, 0, sizeof(account));

	stress_temp_dir_args(args, dirname, sizeof(dirname));
	(void)snprintf(filename, sizeof(filename), "%s/%s", dirname, "fanotify_file");
	ret = stress_temp_dir_mk_args(args);
	if (ret < 0)
		return exit_status(-ret);

	pid = fork();
	if (pid < 0) {
		pr_err("%s: fork failed: errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		rc = EXIT_NO_RESOURCE;
		goto tidy;
	} else if (pid == 0) {
		/* Child */

		do {
			int fd;
			ssize_t n;
			char buffer[64];

			/* Force FAN_CLOSE_NOWRITE */
			fd = creat(filename, S_IRUSR | S_IWUSR);
			if (fd < 0) {
				pr_fail_err("creat");
				(void)kill(args->ppid, SIGALRM);
				_exit(EXIT_FAILURE);
			}
			(void)close(fd);

			/* Force FAN_CLOSE_WRITE */
			fd = open(filename, O_WRONLY, S_IRUSR | S_IWUSR);
			if (fd < 0) {
				pr_fail_err("open O_WRONLY");
				(void)kill(args->ppid, SIGALRM);
				_exit(EXIT_FAILURE);
			}
			n = write(fd, "test", 4);
			(void)n;
			(void)close(fd);

			/* Force FAN_ACCESS */
			fd = open(filename, O_RDONLY, S_IRUSR | S_IWUSR);
			if (fd < 0) {
				pr_fail_err("open O_RDONLY");
				(void)kill(args->ppid, SIGALRM);
				_exit(EXIT_FAILURE);
			}
			n = read(fd, buffer, sizeof(buffer));
			(void)n;
			(void)close(fd);

			/* Force remove */
			(void)unlink(filename);
		} while (keep_stressing());

		_exit(EXIT_SUCCESS);
	} else {
		void *buffer;

		ret = posix_memalign(&buffer, BUFFER_SIZE, BUFFER_SIZE);
		if (ret != 0 || buffer == NULL) {
			pr_err("%s: posix_memalign: cannot allocate 4K "
				"aligned buffer\n", args->name);
			rc = EXIT_NO_RESOURCE;
			goto tidy;
		}

		fan_fd = fanotify_event_init(args->name);
		if (fan_fd < 0) {
			free(buffer);
			rc = EXIT_FAILURE;
			goto tidy;
		}

		do {
			fd_set rfds;
			ssize_t len;

			FD_ZERO(&rfds);
			FD_SET(fan_fd, &rfds);
			ret = select(fan_fd + 1, &rfds, NULL, NULL, NULL);
			if (ret == -1) {
				if (errno == EINTR)
					continue;
				pr_fail_err("select");
				continue;
			}
			if (ret == 0)
				continue;

#if defined(FIONREAD)
			{
				int isz;

				/*
				 *  Force kernel to determine number
				 *  of bytes that are ready to be read
				 *  for some extra stress
				 */
				ret = ioctl(fan_fd, FIONREAD, &isz);
				(void)ret;
			}
#endif
			if ((len = read(fan_fd, (void *)buffer, BUFFER_SIZE)) > 0) {
				struct fanotify_event_metadata *metadata;
				metadata = (struct fanotify_event_metadata *)buffer;

				while (FAN_EVENT_OK(metadata, len)) {
					if (!g_keep_stressing_flag)
						break;
					if ((metadata->fd != FAN_NOFD) && (metadata->fd >= 0)) {
						if (metadata->mask & FAN_OPEN)
							account.open++;
						if (metadata->mask & FAN_CLOSE_WRITE)
							account.close_write++;
						if (metadata->mask & FAN_CLOSE_NOWRITE)
							account.close_nowrite++;
						if (metadata->mask & FAN_ACCESS)
							account.access++;
						if (metadata->mask & FAN_MODIFY)
							account.modify++;

						inc_counter(args);
						(void)close(metadata->fd);
					}
					metadata = FAN_EVENT_NEXT(metadata, len);
				}
			}
		} while (keep_stressing());

		free(buffer);
		(void)close(fan_fd);
		pr_inf("%s: "
			"%" PRIu64 " open, "
			"%" PRIu64 " close write, "
			"%" PRIu64 " close nowrite, "
			"%" PRIu64 " access, "
			"%" PRIu64 " modify\n",
			args->name,
			account.open,
			account.close_write,
			account.close_nowrite,
			account.access,
			account.modify);
	}
tidy:
	if (pid > 0) {
		int status;

		(void)kill(pid, SIGKILL);
		(void)waitpid(pid, &status, 0);
	}
	(void)unlink(filename);
	(void)stress_temp_dir_rm_args(args);

	return rc;
}

stressor_info_t stress_fanotify_info = {
	.stressor = stress_fanotify,
	.supported = stress_fanotify_supported,
	.class = CLASS_FILESYSTEM | CLASS_SCHEDULER | CLASS_OS
};
#else
stressor_info_t stress_fanotify_info = {
	.stressor = stress_not_implemented,
	.class = CLASS_FILESYSTEM | CLASS_SCHEDULER | CLASS_OS
};
#endif
