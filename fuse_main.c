#define FUSE_USE_VERSION 31

#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <sys/types.h>
#include <unistd.h>

struct global_state {
	/* file descriptor for
	 * "/dev/shm/pushdown_command" */
	int command_fd;
	/* file descriptor for "/dev/shm/pushdown_res" */
	int res_fd;
};

static int pseudo_getattr(const char *path, struct stat *stbuf,
			  struct fuse_file_info *fi)
{
	memset(stbuf, 0, sizeof(struct stat));

	if (strcmp(path, "/") == 0) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else if (strcmp(path + 1, "command") == 0) {
		struct global_state *state =
			(struct global_state *)fuse_get_context()->private_data;
		int r = fstat(state->command_fd, stbuf);
		if (r != 0) {
			return -1 * errno;
		}
		stbuf->st_mode = S_IFREG | 0666;
	} else if (strcmp(path + 1, "result") == 0) {
		struct global_state *state =
			(struct global_state *)fuse_get_context()->private_data;
		int r = fstat(state->res_fd, stbuf);
		if (r != 0) {
			return -1 * errno;
		}
		stbuf->st_mode = S_IFREG | 0444;
	} else {
		return -ENOENT;
	}

	return 0;
}

static int pseudo_chmod(const char *path, mode_t mode,
			struct fuse_file_info *fi)
{
	if (strcmp(path + 1, "command") == 0) {
		struct global_state *state =
			(struct global_state *)fuse_get_context()->private_data;
		int r = fchmod(state->command_fd, mode);
		if (r != 0) {
			return -1 * errno;
		}
	} else if (strcmp(path + 1, "result") == 0) {
		struct global_state *state =
			(struct global_state *)fuse_get_context()->private_data;
		int r = fchmod(state->res_fd, mode);
		if (r != 0) {
			return -1 * errno;
		}
	} else {
		return -ENOENT;
	}

	return 0;
}

static int pseudo_chown(const char *path, uid_t uid, gid_t gid,
			struct fuse_file_info *fi)
{
	if (strcmp(path + 1, "command") == 0) {
		struct global_state *state =
			(struct global_state *)fuse_get_context()->private_data;
		int r = fchown(state->command_fd, uid, gid);
		if (r != 0) {
			return -1 * errno;
		}
	} else if (strcmp(path + 1, "result") == 0) {
		struct global_state *state =
			(struct global_state *)fuse_get_context()->private_data;
		int r = fchown(state->res_fd, uid, gid);
		if (r != 0) {
			return -1 * errno;
		}
	} else {
		return -ENOENT;
	}

	return 0;
}

static int pseudo_truncate(const char *path, off_t off,
			   struct fuse_file_info *fi)
{
	if (strcmp(path + 1, "command") == 0) {
		struct global_state *state =
			(struct global_state *)fuse_get_context()->private_data;
		int r = ftruncate(state->command_fd, off);
		if (r != 0) {
			return -1 * errno;
		}
	} else if (strcmp(path + 1, "result") == 0) {
		struct global_state *state =
			(struct global_state *)fuse_get_context()->private_data;
		int r = ftruncate(state->res_fd, off);
		if (r != 0) {
			return -1 * errno;
		}
	} else {
		return -ENOENT;
	}

	return 0;
}

static int pseudo_open(const char *path, struct fuse_file_info *fi)
{
	if (strcmp(path + 1, "command") == 0) {
		fi->fh = open("/dev/shm/pushdown_command", fi->flags);
		if (fi->fh == -1) {
			return -1 * errno;
		}
	} else if (strcmp(path + 1, "result") == 0) {
		fi->fh = open("/dev/shm/pushdown_res", fi->flags);
		if (fi->fh == -1) {
			return -1 * errno;
		}
	} else {
		return -ENOENT;
	}

	return 0;
}

static int pseudo_read(const char *path, char *buf, size_t size, off_t offset,
		       struct fuse_file_info *fi)
{
	if (fi->fh != -1) {
		ssize_t nr = pread(fi->fh, buf, size, offset);
		if (nr == -1) {
			return -1 * errno;
		}
		return nr;
	} else {
		return -ENOENT;
	}
}

static int pseudo_write(const char *path, const char *buf, size_t size,
			off_t off, struct fuse_file_info *fi)
{
	if (fi->fh != -1) {
		ssize_t nw = pwrite(fi->fh, buf, size, off);
		if (nw == -1) {
			return -1 * errno;
		}
		return nw;
	} else {
		return -ENOENT;
	}
}

static int pseudo_flush(const char *path, struct fuse_file_info *fi)
{
	if (fi->fh != -1) {
		if (strcmp(path + 1, "command") != 0) {
			return 0;
		}

		// Make sure something has been written
		struct stat stbuf;
		int r = fstat(fi->fh, &stbuf);
		if (r != 0) {
			return -1 * errno;
		}

		if (stbuf.st_size != 0) {
			char *command = "Offloader";
			char *argument_list[] = { "Offloader",
						  "/dev/shm/pushdown_command",
						  "/dev/shm/pushdown_res",
						  NULL };
			pid_t pid = fork();
			if (pid == 0) { // Child
				int rr = execvp(command, argument_list);
				if (rr == -1) {
					exit(EXIT_FAILURE);
				}
			} else { // Parent process
				int rr;
				waitpid(pid, &rr, 0);
				if (rr != 0) {
					return -EIO;
				}
			}
		}

		return 0;
	} else {
		return -ENOENT;
	}
}

static int pseudo_release(const char *path, struct fuse_file_info *fi)
{
	if (fi->fh != -1) {
		close(fi->fh);
		return 0;
	} else {
		return -ENOENT;
	}
}

static int pseudo_fsync(const char *path, int datasync,
			struct fuse_file_info *fi)
{
	return pseudo_flush(path, fi);
}

static int pseudo_setxattr(const char *path, const char *name,
			   const char *value, size_t size, int flags)
{
	if (strcmp(path + 1, "command") == 0) {
		struct global_state *state =
			(struct global_state *)fuse_get_context()->private_data;
		int r = fsetxattr(state->command_fd, name, value, size, flags);
		if (r != 0) {
			return -1 * errno;
		}
	} else if (strcmp(path + 1, "result") == 0) {
		struct global_state *state =
			(struct global_state *)fuse_get_context()->private_data;
		int r = fsetxattr(state->res_fd, name, value, size, flags);
		if (r != 0) {
			return -1 * errno;
		}
	} else {
		return -ENOENT;
	}

	return 0;
}

static int pseudo_getxattr(const char *path, const char *name, char *value,
			   size_t size)
{
	if (strcmp(path + 1, "command") == 0) {
		struct global_state *state =
			(struct global_state *)fuse_get_context()->private_data;
		ssize_t r = fgetxattr(state->command_fd, name, value, size);
		if (r == -1) {
			return -1 * errno;
		}
	} else if (strcmp(path + 1, "result") == 0) {
		struct global_state *state =
			(struct global_state *)fuse_get_context()->private_data;
		ssize_t r = fgetxattr(state->res_fd, name, value, size);
		if (r == -1) {
			return -1 * errno;
		}
	} else {
		return -ENOENT;
	}

	return 0;
}

static int pseudo_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			  off_t offset, struct fuse_file_info *fi,
			  enum fuse_readdir_flags flags)
{
	if (strcmp(path, "/") != 0)
		return -ENOENT;

	filler(buf, "command", NULL, 0, 0);
	filler(buf, "result", NULL, 0, 0);

	return 0;
}

static void *pseudo_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	cfg->kernel_cache = 0;
	int command_fd = open("/dev/shm/pushdown_command",
			      O_RDWR | O_CREAT | O_TRUNC, 0666);
	if (command_fd == -1) {
		exit(EXIT_FAILURE);
	}
	int res_fd =
		open("/dev/shm/pushdown_res", O_RDWR | O_CREAT | O_TRUNC, 0666);
	if (res_fd == -1) {
		exit(EXIT_FAILURE);
	}
	struct global_state *state =
		(struct global_state *)malloc(sizeof(struct global_state));
	if (!state) {
		exit(EXIT_FAILURE);
	} else {
		state->command_fd = command_fd;
		state->res_fd = res_fd;
	}
	return state;
}

static void pseudo_destroy(void *private_data)
{
	struct global_state *state = (struct global_state *)private_data;
	close(state->command_fd);
	close(state->res_fd);
	free(state);
}

static int pseudo_utimens(const char *path, const struct timespec tv[2],
			  struct fuse_file_info *fi)
{
	if (strcmp(path + 1, "command") == 0) {
		struct global_state *state =
			(struct global_state *)fuse_get_context()->private_data;
		int r = futimens(state->command_fd, tv);
		if (r != 0) {
			return -1 * errno;
		}
	} else if (strcmp(path + 1, "result") == 0) {
		struct global_state *state =
			(struct global_state *)fuse_get_context()->private_data;
		int r = futimens(state->res_fd, tv);
		if (r != 0) {
			return -1 * errno;
		}
	} else {
		return -ENOENT;
	}

	return 0;
}

static const struct fuse_operations pseudo_oper = {
	.getattr = pseudo_getattr,
	.chmod = pseudo_chmod,
	.chown = pseudo_chown,
	.truncate = pseudo_truncate,
	.open = pseudo_open,
	.read = pseudo_read,
	.write = pseudo_write,
	.flush = pseudo_flush,
	.release = pseudo_release,
	.fsync = pseudo_fsync,
	.setxattr = pseudo_setxattr,
	.getxattr = pseudo_getxattr,
	.readdir = pseudo_readdir,
	.init = pseudo_init,
	.destroy = pseudo_destroy,
	.utimens = pseudo_utimens,
};

int main(int argc, char *argv[])
{
	umask(0); // Reset umask
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	const int ret = fuse_main(args.argc, args.argv, &pseudo_oper, NULL);
	fuse_opt_free_args(&args);
	return ret;
}
