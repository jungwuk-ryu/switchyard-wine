#include <errno.h>
#include <crt_externs.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "swdbg_experiment.h"

static int report_error(const char *operation)
{
    dprintf(STDERR_FILENO, "swdbg-launcher: %s failed: %s\n", operation, strerror(errno));
    return 125;
}

static int clear_environment(void)
{
    char ***environment = _NSGetEnviron();

    while (environment && *environment && (*environment)[0])
    {
        const char *entry = (*environment)[0];
        const char *separator = strchr(entry, '=');
        char *name;
        size_t length;
        int status;

        if (!separator || separator == entry)
        {
            errno = EINVAL;
            return report_error("inspect inherited environment");
        }
        length = (size_t)(separator - entry);
        if (!(name = malloc(length + 1))) return report_error("copy environment name");
        memcpy(name, entry, length);
        name[length] = '\0';
        status = unsetenv(name);
        if (status == -1)
        {
            int saved_errno = errno;
            free(name);
            errno = saved_errno;
            return report_error("clear inherited environment");
        }
        free(name);
    }
    return 0;
}

static int redirect_output(void)
{
    int descriptor;

    descriptor = open(swdbg_log_path,
                      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                      S_IRUSR | S_IWUSR);
    if (descriptor == -1) return report_error("open log");
    if (dup2(descriptor, STDOUT_FILENO) == -1 || dup2(descriptor, STDERR_FILENO) == -1)
    {
        int saved_errno = errno;
        close(descriptor);
        errno = saved_errno;
        return report_error("redirect output");
    }
    if (close(descriptor) == -1) return report_error("close log");
    return 0;
}

static int write_pid_file(void)
{
    char buffer[64];
    ssize_t length, offset = 0;
    int descriptor;

    descriptor = open(swdbg_pid_path,
                      O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                      S_IRUSR | S_IWUSR);
    if (descriptor == -1) return report_error("open pid file");
    length = snprintf(buffer, sizeof(buffer), "%ld\n", (long)getpid());
    if (length <= 0 || (size_t)length >= sizeof(buffer))
    {
        close(descriptor);
        errno = EOVERFLOW;
        return report_error("format pid");
    }
    while (offset < length)
    {
        ssize_t written = write(descriptor, buffer + offset, (size_t)(length - offset));
        if (written == -1 && errno == EINTR) continue;
        if (written <= 0)
        {
            int saved_errno = written == 0 ? EIO : errno;
            close(descriptor);
            errno = saved_errno;
            return report_error("write pid file");
        }
        offset += written;
    }
    if (fsync(descriptor) == -1)
    {
        int saved_errno = errno;
        close(descriptor);
        errno = saved_errno;
        return report_error("sync pid file");
    }
    if (close(descriptor) == -1) return report_error("close pid file");
    return 0;
}

static int configure_environment(void)
{
    const struct passwd *account = getpwuid(getuid());
    size_t i;

    if (!account || !account->pw_dir || account->pw_dir[0] != '/')
    {
        errno = EINVAL;
        return report_error("resolve user home");
    }
    if (clear_environment()) return 125;
    if (setenv("HOME", account->pw_dir, 1) == -1) return report_error("set HOME");
    if (setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1) == -1)
        return report_error("set PATH");
    if (setenv("TMPDIR", "/tmp", 1) == -1) return report_error("set TMPDIR");
    if (setenv("PWD", swdbg_working_directory, 1) == -1)
        return report_error("set PWD");
    if (setenv("WINEPREFIX", swdbg_prefix, 1) == -1)
        return report_error("set WINEPREFIX");
    for (i = 0; i < swdbg_environment_count; ++i)
        if (setenv(swdbg_environment[i].name, swdbg_environment[i].value, 1) == -1)
            return report_error("set experiment environment");
    return 0;
}

int main(void)
{
    int status;

    status = redirect_output();
    if (status) return status;
    if (chdir(swdbg_working_directory) == -1) return report_error("change directory");
    status = configure_environment();
    if (status) return status;
    status = write_pid_file();
    if (status) return status;
    dprintf(STDERR_FILENO, "swdbg-launcher: experiment=%s pid=%ld runtime=%s\n",
            swdbg_experiment_id, (long)getpid(), swdbg_runtime_executable);
    execv(swdbg_runtime_executable, swdbg_arguments);
    return report_error("exec runtime");
}
