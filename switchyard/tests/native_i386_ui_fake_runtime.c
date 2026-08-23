/*
 * Native process fixture for native_i386_ui_acceptance_test.sh.
 *
 * This is deliberately not a Wine implementation.  It provides only enough
 * native process, argv/environment, log, and exact-prefix cleanup behavior to
 * exercise the shell acceptance harness without launching Wine.
 */

#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t terminate_requested;
static void *runtime_image_handles[2];

static void handle_termination(int signal_number)
{
    (void)signal_number;
    terminate_requested = 1;
}

static const char *path_basename(const char *path)
{
    const char *separator = strrchr(path, '/');

    return separator ? separator + 1 : path;
}

static int form_prefix_path(char *buffer, size_t size, const char *prefix, const char *name)
{
    int length = snprintf(buffer, size, "%s/%s", prefix, name);

    return length >= 0 && (size_t)length < size;
}

static int prefix_is_private_directory(const char *prefix)
{
    struct stat info;

    return prefix[0] == '/' && !lstat(prefix, &info) && S_ISDIR(info.st_mode) &&
           !S_ISLNK(info.st_mode) && info.st_uid == getuid() &&
           !(info.st_mode & (S_IWGRP | S_IWOTH));
}

static int append_line(const char *path, const char *key, const char *value)
{
    int descriptor;
    FILE *stream;

    if ((descriptor = open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0600)) == -1)
        return 0;
    if (!(stream = fdopen(descriptor, "a")))
    {
        close(descriptor);
        return 0;
    }
    fprintf(stream, "%s=%s\n", key, value ? value : "<unset>");
    return fclose(stream) == 0;
}

static int append_number(const char *path, const char *key, long value)
{
    char text[64];

    snprintf(text, sizeof(text), "%ld", value);
    return append_line(path, key, text);
}

static void sleep_milliseconds(long milliseconds)
{
    struct timespec request, remaining;

    request.tv_sec = milliseconds / 1000;
    request.tv_nsec = (milliseconds % 1000) * 1000000;
    while (nanosleep(&request, &remaining) == -1 && errno == EINTR) request = remaining;
}

static int read_daemon_pid(const char *path, pid_t *pid)
{
    char buffer[64], *end;
    struct stat info;
    ssize_t length;
    long value;
    int descriptor;

    descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor == -1 || fstat(descriptor, &info) == -1 ||
        !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
        info.st_size <= 0 || info.st_size >= (off_t)sizeof(buffer))
    {
        if (descriptor != -1) close(descriptor);
        return 0;
    }
    length = read(descriptor, buffer, sizeof(buffer) - 1);
    if (close(descriptor) == -1 || length != info.st_size) return 0;
    buffer[length] = 0;
    errno = 0;
    value = strtol(buffer, &end, 10);
    if (errno || end == buffer || value <= 1 || value > INT_MAX ||
        (*end != '\n' && *end) || (*end == '\n' && end[1])) return 0;
    *pid = (pid_t)value;
    return 1;
}

static int run_server(int argc, char **argv, const char *prefix)
{
    char pid_path[PATH_MAX], cleanup_path[PATH_MAX];
    pid_t pid;
    FILE *stream;
    int count;

    if (!form_prefix_path(pid_path, sizeof(pid_path), prefix, ".fixture-wineserver.pid") ||
        !form_prefix_path(cleanup_path, sizeof(cleanup_path), prefix, "fixture-cleanup.txt"))
        return 70;

    if (argc == 2 && !strcmp(argv[1], "--fixture-daemon"))
    {
        int descriptor;

        signal(SIGTERM, handle_termination);
        signal(SIGINT, handle_termination);
        descriptor = open(pid_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (descriptor == -1 || !(stream = fdopen(descriptor, "w")))
        {
            if (descriptor != -1) close(descriptor);
            return 71;
        }
        fprintf(stream, "%ld\n", (long)getpid());
        if (fclose(stream)) return 72;
        while (!terminate_requested) pause();
        puts("0078:trace:loaddll:free_modref Unloaded module L\"C:\\\\windows\\\\system32\\\\winemac.drv\" : builtin");
        fflush(stdout);
        unlink(pid_path);
        return 0;
    }

    if (argc != 2 || (strcmp(argv[1], "-k") && strcmp(argv[1], "-w"))) return 73;
    if (!append_line(cleanup_path, "operation", argv[1]) ||
        !append_line(cleanup_path, "prefix", prefix)) return 74;

    if (!read_daemon_pid(pid_path, &pid)) return 0;
    if (!strcmp(argv[1], "-k"))
    {
        if (kill(pid, SIGTERM) == -1 && errno != ESRCH) return 75;
        for (count = 0; count < 100; ++count)
        {
            if (kill(pid, 0) == -1 && errno == ESRCH) return 0;
            sleep_milliseconds(20);
        }
        return 76;
    }

    for (count = 0; count < 100; ++count)
    {
        if (kill(pid, 0) == -1 && errno == ESRCH) return 0;
        sleep_milliseconds(20);
    }
    return 77;
}

static int spawn_server(const char *argv0, const char *prefix)
{
    char executable[PATH_MAX], server[PATH_MAX], pid_path[PATH_MAX], *separator;
    pid_t child;
    int count;

    if (!realpath(argv0, executable)) return 0;
    if (!(separator = strrchr(executable, '/'))) return 0;
    *separator = 0;
    if (snprintf(server, sizeof(server), "%s/wineserver", executable) >= (int)sizeof(server))
        return 0;
    if (!form_prefix_path(pid_path, sizeof(pid_path), prefix, ".fixture-wineserver.pid"))
        return 0;
    if ((child = fork()) == -1) return 0;
    if (!child)
    {
        execl(server, server, "--fixture-daemon", (char *)NULL);
        _exit(127);
    }
    for (count = 0; count < 100; ++count)
    {
        struct stat info;

        if (!lstat(pid_path, &info) && S_ISREG(info.st_mode) &&
            info.st_uid == getuid()) return 1;
        sleep_milliseconds(10);
    }
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    return 0;
}

static int record_invocation(int argc, char **argv, const char *prefix)
{
    static const char *const environment_names[] = {
        "WINEPREFIX", "WINEDEBUG", "SWITCHYARD_NATIVE_I386_UI_MARKER",
        "WINEARCH", "WINEDLLPATH", "WINELOADER", "WINESERVER",
        "DISABLE_GPTK_OVERLAY", "GPTK_PATH", "SWITCHYARD_DISABLE_GPTK_OVERLAY",
        "SWITCHYARD_GPTK_PATH", "SWITCHYARD_GPTK_DLL_NT_PATH",
        "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH",
        "DYLD_FALLBACK_FRAMEWORK_PATH", "DYLD_INSERT_LIBRARIES"
    };
    char record_path[PATH_MAX], key[32];
    size_t index;
    int argument;

    if (!form_prefix_path(record_path, sizeof(record_path), prefix, "fixture-invocation.txt"))
        return 0;
    if (!append_number(record_path, "argc", argc)) return 0;
    for (argument = 0; argument < argc; ++argument)
    {
        snprintf(key, sizeof(key), "argv%d", argument);
        if (!append_line(record_path, key, argv[argument])) return 0;
    }
    for (index = 0; index < sizeof(environment_names) / sizeof(environment_names[0]); ++index)
        if (!append_line(record_path, environment_names[index], getenv(environment_names[index])))
            return 0;
    return 1;
}

static int load_runtime_image(size_t index, const char *name)
{
    char raw_executable[PATH_MAX], executable[PATH_MAX], image[PATH_MAX], *separator;
    uint32_t executable_size = sizeof(raw_executable);

    if (_NSGetExecutablePath(raw_executable, &executable_size) ||
        !realpath(raw_executable, executable) ||
        !(separator = strrchr(executable, '/')))
        return 0;
    *separator = 0;
    if (snprintf(image, sizeof(image), "%s/%s", executable, name) >= (int)sizeof(image))
        return 0;
    runtime_image_handles[index] = dlopen(image, RTLD_NOW | RTLD_LOCAL);
    if (!runtime_image_handles[index])
    {
        fprintf(stderr, "could not map fixture runtime image %s: %s\n", image, dlerror());
        return 0;
    }
    return 1;
}

static int load_runtime_images(const char *selector)
{
    if (strcmp(selector, "test_missing_winemac") &&
        !load_runtime_image(0, "winemac.so")) return 0;
    if (strcmp(selector, "test_missing_provider") &&
        !load_runtime_image(1, "xtajit.so")) return 0;
    return 1;
}

static void emit_common_log(const char *selector)
{
    /* These positive lines are deliberately forgeable.  The acceptance test
     * must instead rely on vnode-bound loaded-image inspection. */
    puts("0024:trace:xtajit:process_init initialized Unicorn fixture i386 provider registry");
    puts("0078:trace:loaddll:build_module Loaded L\"C:\\\\windows\\\\system32\\\\winemac.drv\" at 0000000100000000: builtin");
    if (!strcmp(selector, "test_winemac_unload"))
        puts("0078:trace:loaddll:free_modref Unloaded module L\"C:\\\\windows\\\\system32\\\\winemac.drv\" : builtin");
    if (!strcmp(selector, "test_nodrv"))
        puts("0024:err:winediag:nodrv_CreateWindow Application tried to create a window, but no driver could be loaded.");
    if (!strcmp(selector, "test_provider_poison"))
        puts("0024:err:xtajit:poison_provider fatal simulation stop failed, poisoning i386 provider with status 0xc0000005");
}

static int run_launcher(int argc, char **argv, const char *prefix)
{
    const char *marker, *summary_suite;
    int failures = 0, assertions = 7;

    if (argc != 4) return 80;
    if (!record_invocation(argc, argv, prefix)) return 81;
    if (!spawn_server(argv[0], prefix)) return 82;
    if (!load_runtime_images(argv[3])) return 83;

    marker = getenv("SWITCHYARD_NATIVE_I386_UI_MARKER");
    printf("fixtureMarker=%s\n", marker ? marker : "<unset>");
    if (!strcmp(argv[3], "test_forged_observation_end"))
        printf("switchyardObservationEnd=%s\n", marker ? marker : "<unset>");
    fflush(stdout);
    /* Leave the fixture mapped long enough for a cold loaded-image probe to
     * walk the process VM map without turning host scheduling jitter into a
     * false negative.  The production driver still fails closed if proof is
     * unavailable before the real test exits. */
    sleep_milliseconds(1500);
    emit_common_log(argv[3]);

    if (!strcmp(argv[3], "test_timeout"))
    {
        fflush(stdout);
        sleep_milliseconds(5000);
    }
    if (!strcmp(argv[3], "test_log_limit"))
    {
        int count;

        for (count = 0; count < 16384; ++count) fputs("0123456789abcdef", stdout);
        fputc('\n', stdout);
    }
    if (!strcmp(argv[3], "test_failures"))
    {
        puts("win.c:42: Test failed: fixture failure");
        failures = 1;
    }
    if (!strcmp(argv[3], "test_zero_assertions")) assertions = 0;
    summary_suite = !strcmp(argv[3], "test_wrong_summary") ? "wrong_suite" : argv[2];
    printf("0020:%s:0.700 %d tests executed (0 marked as todo, 0 as flaky, %d failure%s), 0 skipped.\n",
           summary_suite, assertions, failures, failures == 1 ? "" : "s");
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    const char *prefix = getenv("WINEPREFIX");

    if (!prefix || !prefix_is_private_directory(prefix)) return 64;
    if (!strcmp(path_basename(argv[0]), "wineserver")) return run_server(argc, argv, prefix);
    return run_launcher(argc, argv, prefix);
}
