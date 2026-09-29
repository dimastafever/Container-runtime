#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <errno.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

static void die(const char *msg)
{
    fprintf(stderr, "error: %s: %s\n", msg, strerror(errno));
    exit(1);
}

static void warn(const char *msg)
{
    fprintf(stderr, "warning: %s: %s\n", msg, strerror(errno));
}

static void write_file(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "warning: cannot open %s: %s\n", path, strerror(errno));
        return;
    }
    size_t len = strlen(content);
    if (write(fd, content, len) != (ssize_t)len)
        fprintf(stderr, "warning: cannot write %s: %s\n", path, strerror(errno));
    close(fd);
}

static int pivot_root_syscall(const char *new_root, const char *put_old)
{
    return (int)syscall(SYS_pivot_root, new_root, put_old);
}

static void setup_mounts(const char *rootfs)
{
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        warn("mount private");

    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) < 0)
        warn("bind mount rootfs");

    char put_old[PATH_MAX];
    snprintf(put_old, sizeof(put_old), "%s/.pivot_root", rootfs);

    if (mkdir(put_old, 0700) < 0 && errno != EEXIST)
        warn("mkdir put_old");
    if (pivot_root_syscall(rootfs, put_old) < 0) {
        warn("pivot_root");
        if (chroot(rootfs) < 0)
            die("chroot");
        if (chdir("/") < 0)
            die("chdir");
    } else {
        if (chdir("/") < 0)
            die("chdir after pivot_root");
        if (umount2("/.pivot_root", MNT_DETACH) < 0)
            warn("umount old root");
        rmdir("/.pivot_root");
    }
}

static void mount_proc(void)
{
    if (mkdir("/proc", 0755) < 0 && errno != EEXIST)
        warn("mkdir /proc");
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        warn("mount /proc");
}

static void mount_dev(void)
{
    if (mkdir("/dev", 0755) < 0 && errno != EEXIST)
        warn("mkdir /dev");

    if (mount("tmpfs", "/dev", "tmpfs",
              MS_NOSUID | MS_STRICTATIME, "mode=755") < 0) {
        warn("mount /dev");
        return;
    }
    struct { const char *path; mode_t mode; unsigned major, minor; } devs[] = {
        { "/dev/null",    S_IFCHR | 0666, 1, 3 },
        { "/dev/zero",    S_IFCHR | 0666, 1, 5 },
        { "/dev/random",  S_IFCHR | 0666, 1, 8 },
        { "/dev/urandom", S_IFCHR | 0666, 1, 9 },
        { "/dev/tty",     S_IFCHR | 0666, 5, 0 },
    };
    for (size_t i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        if (access(devs[i].path, F_OK) == 0)
            continue;
        if (mknod(devs[i].path, devs[i].mode,
                  makedev(devs[i].major, devs[i].minor)) < 0)
            warn(devs[i].path);
    }

    symlink("/proc/self/fd",    "/dev/fd");
    symlink("/proc/self/fd/0",  "/dev/stdin");
    symlink("/proc/self/fd/1",  "/dev/stdout");
    symlink("/proc/self/fd/2",  "/dev/stderr");
}

static void container_payload(const char *rootfs, char *const argv[])
{
    if (sethostname("container", 9) < 0)
        warn("sethostname");
    setup_mounts(rootfs);
    mount_proc();
    mount_dev();
    execv(argv[0], argv);
    die("execv");
}


int main(int argc, char *argv[])
{
    if (argc < 4 || strcmp(argv[1], "run") != 0) {
        fprintf(stderr, "Usage: %s run <rootfs> <command> [args...]\n", argv[0]);
        return 1;
    }

    const char *rootfs = argv[2];
    char **cmd_argv    = &argv[3];
    int need_userns = (getuid() != 0);

    int c2p[2], p2c[2];
    if (pipe(c2p) < 0) die("pipe c2p");
    if (pipe(p2c) < 0) die("pipe p2c");

    pid_t pid = fork();
    if (pid < 0) die("fork");

    if (pid == 0) {
        close(c2p[0]);
        close(p2c[1]);

        int flags = CLONE_NEWUTS | CLONE_NEWIPC |
                    CLONE_NEWNS  | CLONE_NEWNET | CLONE_NEWPID;
        if (need_userns)
            flags |= CLONE_NEWUSER;

        if (unshare(flags) < 0) {
            perror("unshare");
            exit(1);
        }

        write(c2p[1], "R", 1);
        close(c2p[1]);

        char ch;
        if (read(p2c[0], &ch, 1) != 1) die("read p2c");
        close(p2c[0]);

        pid_t grandchild = fork();
        if (grandchild < 0) die("fork grandchild");

        if (grandchild > 0) {
            int status;
            waitpid(grandchild, &status, 0);
            exit(WIFEXITED(status) ? WEXITSTATUS(status) : 1);
        }

        container_payload(rootfs, cmd_argv);
        return 1;
    }
    close(c2p[1]);
    close(p2c[0]);

    char ch;
    if (read(c2p[0], &ch, 1) != 1) die("read c2p");
    close(c2p[0]);

    if (need_userns) {
        char path[PATH_MAX], buf[64];

        snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
        snprintf(buf,  sizeof(buf),  "0 %d 1\n", getuid());
        write_file(path, buf);

        snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
        write_file(path, "deny");

        snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
        snprintf(buf,  sizeof(buf),  "0 %d 1\n", getgid());
        write_file(path, buf);
    }

    write(p2c[1], "G", 1);
    close(p2c[1]);

    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}