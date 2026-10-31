#define _GNU_SOURCE
/* Linux: /proc/self/mounts + statvfs */
#include "sysinfo.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

/* Filesystems that do not represent disk space. */
static int is_pseudo(const char *type)
{
    static const char *skip[] = {
        "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "ramfs", "cgroup", "cgroup2", "mqueue",
        "debugfs", "tracefs", "securityfs", "pstore", "bpf", "configfs", "fusectl", "hugetlbfs",
        "autofs", "binfmt_misc", "rpc_pipefs", "nsfs", "efivarfs", "selinuxfs", "squashfs",
        "fuse.gvfsd-fuse", "fuse.portal", "fuse.lxcfs", NULL,
    };
    for (int i = 0; skip[i]; i++)
        if (strcmp(type, skip[i]) == 0)
            return 1;
    return 0;
}

/* /proc/mounts escapes space, tab, newline and backslash as \ooo. */
static void unescape(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (p[0] == '\\' && p[1] >= '0' && p[1] <= '7' && p[2] >= '0' && p[2] <= '7' &&
            p[3] >= '0' && p[3] <= '7') {
            *o++ = (char)((p[1] - '0') * 64 + (p[2] - '0') * 8 + (p[3] - '0'));
            p += 3;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

int si_disks_read(si_disk **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    FILE *f = fopen("/proc/self/mounts", "r");
    if (!f)
        return -1;
    size_t cap = 0;
    dev_t *devs = NULL;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        char *save, *dev = strtok_r(line, " ", &save);
        char *mnt = strtok_r(NULL, " ", &save);
        char *type = strtok_r(NULL, " ", &save);
        if (!dev || !mnt || !type || is_pseudo(type))
            continue;
        unescape(dev);
        unescape(mnt);
        struct stat st;
        struct statvfs vfs;
        /* Containers bind-mount single files like /etc/hosts; those are not disks. */
        if (stat(mnt, &st) != 0 || !S_ISDIR(st.st_mode) || statvfs(mnt, &vfs) != 0 ||
            vfs.f_blocks == 0)
            continue;
        /* The same filesystem mounted twice (bind mounts) is listed once. */
        int dup = 0;
        for (size_t i = 0; i < *count; i++)
            if (devs[i] == st.st_dev)
                dup = 1;
        if (dup)
            continue;
        if (*count == cap) {
            cap = cap ? cap * 2 : 8;
            *out = xrealloc(*out, cap * sizeof **out);
            devs = xrealloc(devs, cap * sizeof *devs);
        }
        uint64_t fr = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
        si_disk *d = &(*out)[*count];
        d->device = xstrdup(dev);
        d->mount = xstrdup(mnt);
        d->fstype = xstrdup(type);
        d->total = (uint64_t)vfs.f_blocks * fr;
        d->used = (uint64_t)(vfs.f_blocks - vfs.f_bfree) * fr;
        d->free = (uint64_t)vfs.f_bavail * fr;
        devs[(*count)++] = st.st_dev;
    }
    fclose(f);
    free(devs);
    return 0;
}
