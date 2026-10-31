/* Linux: hostname and kernel version */
#include "sysinfo.h"

#include <sys/utsname.h>
#include <unistd.h>

void si_host_info(char *host, size_t hostlen, char *kernel, size_t kernellen)
{
    if (gethostname(host, hostlen - 1) != 0)
        snprintf(host, hostlen, "unknown");
    host[hostlen - 1] = '\0';
    struct utsname u;
    if (uname(&u) == 0)
        snprintf(kernel, kernellen, "%s %s", u.sysname, u.release);
    else
        snprintf(kernel, kernellen, "Linux");
}
