/*
 * Native Switchyard runtime process-architecture probe
 *
 * Copyright 2026 Switchyard contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <mach/machine.h>
#include <mach/vm_prot.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_LOADED_IMAGE_REGIONS 262144
#define MAX_REQUIRED_LOADED_IMAGES 8
#define MAX_WRAPPER_BOOTSTRAP_ARGUMENTS 64
#define MAX_PROCESS_ARGUMENT_BYTES (16 * 1024 * 1024)
#define PROCESS_LIST_PID_RESERVE 4096

struct loaded_image_identity
{
    struct stat stat;
    const char *path;
    int descriptor;
    int found;
};

static int same_process_identity( const struct proc_bsdinfo *left,
                                  const struct proc_bsdinfo *right )
{
    return left->pbi_pid == right->pbi_pid &&
           left->pbi_ppid == right->pbi_ppid &&
           left->pbi_uid == right->pbi_uid &&
           left->pbi_start_tvsec == right->pbi_start_tvsec &&
           left->pbi_start_tvusec == right->pbi_start_tvusec;
}

static int image_identity_matches_vnode( const struct loaded_image_identity *image,
                                         const struct vinfo_stat *vnode )
{
    return (uint32_t)image->stat.st_dev == vnode->vst_dev &&
           (uint64_t)image->stat.st_ino == vnode->vst_ino;
}

static int region_path_is_terminated( const char path[MAXPATHLEN] )
{
    return memchr( path, 0, MAXPATHLEN ) != NULL;
}

static int path_has_component_suffix( const char *path, const char *suffix )
{
    size_t path_length = strlen( path ), suffix_length = strlen( suffix );

    return path_length >= suffix_length &&
           !strcmp( path + path_length - suffix_length, suffix ) &&
           (path_length == suffix_length || path[path_length - suffix_length - 1] == '/');
}

static int path_has_line_break( const char *path )
{
    return strchr( path, '\n' ) != NULL || strchr( path, '\r' ) != NULL;
}

static int open_loaded_image( struct loaded_image_identity *image )
{
    struct stat named;

    if (path_has_line_break( image->path ))
    {
        fprintf( stderr, "required loaded-image path contains a line break\n" );
        return 0;
    }
    if ((image->descriptor = open( image->path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC )) == -1)
    {
        fprintf( stderr, "cannot open required loaded image %s: %s\n",
                 image->path, strerror( errno ) );
        return 0;
    }
    if (fstat( image->descriptor, &image->stat ) == -1 ||
        lstat( image->path, &named ) == -1 ||
        !S_ISREG( image->stat.st_mode ) || !S_ISREG( named.st_mode ) ||
        image->stat.st_size <= 0 || named.st_size <= 0 ||
        image->stat.st_dev != named.st_dev || image->stat.st_ino != named.st_ino)
    {
        fprintf( stderr, "required loaded image is not one stable regular file: %s\n",
                 image->path );
        close( image->descriptor );
        image->descriptor = -1;
        return 0;
    }
    return 1;
}

static int validate_loaded_image_process( int pid, const struct proc_bsdinfo *expected,
                                          const struct proc_archinfo *architecture,
                                          const char *expected_executable )
{
    struct proc_bsdinfo current;
    struct proc_archinfo current_architecture;
    char process_path[PROC_PIDPATHINFO_MAXSIZE], executable_path[PATH_MAX];

    if (proc_pidinfo( pid, PROC_PIDTBSDINFO, 0, &current, sizeof(current) ) != sizeof(current) ||
        proc_pidinfo( pid, PROC_PIDARCHINFO, 0, &current_architecture,
                      sizeof(current_architecture) ) != sizeof(current_architecture))
    {
        fprintf( stderr, "cannot query the loaded-image target process %d\n", pid );
        return 0;
    }
    if (!same_process_identity( expected, &current ))
    {
        fprintf( stderr, "loaded-image target process %d changed identity\n", pid );
        return 0;
    }
    if ((current.pbi_flags & P_TRANSLATED) || current_architecture.p_cputype != CPU_TYPE_ARM64 ||
        architecture->p_cputype != current_architecture.p_cputype ||
        architecture->p_cpusubtype != current_architecture.p_cpusubtype)
    {
        fprintf( stderr, "loaded-image target process %d is not stable native ARM64\n", pid );
        return 0;
    }
    if (!realpath( expected_executable, executable_path ) ||
        proc_pidpath( pid, process_path, sizeof(process_path) ) <= 0 ||
        strcmp( process_path, executable_path ))
    {
        fprintf( stderr, "loaded-image target process %d is not the expected runtime executable\n",
                 pid );
        return 0;
    }
    return 1;
}

static int loaded_image_region_is_valid( const struct proc_regionwithpathinfo *region,
                                         uint64_t address, unsigned int region_count,
                                         uint64_t *next )
{
    if (region_count >= MAX_LOADED_IMAGE_REGIONS ||
        region->prp_prinfo.pri_address < address || !region->prp_prinfo.pri_size ||
        region->prp_prinfo.pri_address > UINT64_MAX - region->prp_prinfo.pri_size ||
        (*next = region->prp_prinfo.pri_address + region->prp_prinfo.pri_size) <= address ||
        !region_path_is_terminated( region->prp_vip.vip_path )) return 0;
    return 1;
}

static int loaded_image_region_result( int result, int error )
{
    if (result == sizeof(struct proc_regionwithpathinfo)) return 1;
    if (!result && error == EINVAL) return 0;
    return -1;
}

static int loaded_image_is_stable( const struct loaded_image_identity *image )
{
    struct stat opened, named;

    return fstat( image->descriptor, &opened ) != -1 &&
           lstat( image->path, &named ) != -1 &&
           S_ISREG( opened.st_mode ) && S_ISREG( named.st_mode ) &&
           opened.st_dev == image->stat.st_dev && opened.st_ino == image->stat.st_ino &&
           opened.st_size == image->stat.st_size &&
           named.st_dev == image->stat.st_dev && named.st_ino == image->stat.st_ino &&
           named.st_size == image->stat.st_size;
}

static int inspect_loaded_images( int pid, const struct proc_bsdinfo *expected,
                                  const char *expected_executable,
                                  struct loaded_image_identity *images, size_t image_count )
{
    static const char forbidden_suffix[] = "x86_64-unix/winemetal.so";
    struct proc_regionwithpathinfo region;
    struct proc_bsdinfo before, after;
    struct proc_archinfo architecture;
    uint64_t address = 0;
    unsigned int region_count = 0;
    size_t i;
    int result, saved_errno;

    if (proc_pidinfo( pid, PROC_PIDTBSDINFO, 0, &before, sizeof(before) ) != sizeof(before) ||
        proc_pidinfo( pid, PROC_PIDARCHINFO, 0, &architecture,
                      sizeof(architecture) ) != sizeof(architecture))
    {
        fprintf( stderr, "cannot capture loaded-image target process %d\n", pid );
        return 1;
    }
    if (!same_process_identity( expected, &before ) ||
        before.pbi_pid != (unsigned int)pid || before.pbi_uid != getuid() ||
        (before.pbi_flags & P_TRANSLATED) || architecture.p_cputype != CPU_TYPE_ARM64)
    {
        fprintf( stderr, "loaded-image target process %d is not the current user's native ARM64 process\n",
                 pid );
        return 1;
    }
    for (i = 0; i < image_count; ++i) images[i].descriptor = -1;
    for (i = 0; i < image_count; ++i)
    {
        images[i].found = 0;
        if (!open_loaded_image( &images[i] )) goto failed;
    }

    for (;;)
    {
        uint64_t next;

        memset( &region, 0, sizeof(region) );
        errno = 0;
        result = proc_pidinfo( pid, PROC_PIDREGIONPATHINFO, address,
                               &region, sizeof(region) );
        saved_errno = errno;
        switch (loaded_image_region_result( result, saved_errno ))
        {
        case 0:
            if (saved_errno != EINVAL ||
                proc_pidinfo( pid, PROC_PIDTBSDINFO, 0, &after, sizeof(after) ) != sizeof(after) ||
                !same_process_identity( &before, &after ))
            {
                fprintf( stderr, "loaded-image region enumeration ended without a stable process\n" );
                goto failed;
            }
            goto enumeration_complete;
        case -1:
            fprintf( stderr, "loaded-image region enumeration returned an invalid or partial record\n" );
            goto failed;
        default:
            break;
        }
        if (!loaded_image_region_is_valid( &region, address, region_count, &next ))
        {
            fprintf( stderr, "loaded-image region enumeration returned an invalid record\n" );
            goto failed;
        }
        ++region_count;
        address = next;
        if (path_has_component_suffix( region.prp_vip.vip_path, forbidden_suffix ))
        {
            fprintf( stderr, "loaded-image target mapped forbidden image %s\n",
                     region.prp_vip.vip_path );
            goto failed;
        }
        if (!(region.prp_prinfo.pri_protection & VM_PROT_EXECUTE)) continue;
        for (i = 0; i < image_count; ++i)
        {
            if (image_identity_matches_vnode( &images[i], &region.prp_vip.vip_vi.vi_stat ))
            {
                if (strcmp( images[i].path, region.prp_vip.vip_path ))
                {
                    fprintf( stderr, "required loaded-image identity appeared at an unexpected path: %s\n",
                             region.prp_vip.vip_path );
                    goto failed;
                }
                images[i].found = 1;
            }
        }
    }
enumeration_complete:
    if (!validate_loaded_image_process( pid, &before, &architecture,
                                         expected_executable )) goto failed;
    for (i = 0; i < image_count; ++i)
    {
        if (!images[i].found || !loaded_image_is_stable( &images[i] ))
        {
            fprintf( stderr, "required executable loaded image was not mapped with stable identity: %s\n",
                     images[i].path );
            goto failed;
        }
    }
    for (i = 0; i < image_count; ++i) close( images[i].descriptor );
    printf( "Loaded-image proof passed for native ARM64 pid %d across %u regions.\n",
            pid, region_count );
    return 0;

failed:
    for (i = 0; i < image_count; ++i)
        if (images[i].descriptor != -1) close( images[i].descriptor );
    return 1;
}

struct process_record
{
    struct proc_bsdinfo bsd_info;
    struct proc_archinfo arch_info;
    char path[PROC_PIDPATHINFO_MAXSIZE];
    int pid;
    int has_bsd_info;
    int has_arch_info;
    int has_path;
};

static int path_is_below( const char *path, const char *root )
{
    size_t len = strlen( root );

    return !strncmp( path, root, len ) && (!path[len] || path[len] == '/');
}

static const char *path_basename( const char *path )
{
    const char *name = strrchr( path, '/' );

    return name ? name + 1 : path;
}

static const struct process_record *find_process( const struct process_record *records, int count, int pid )
{
    int i;

    for (i = 0; i < count; ++i) if (records[i].pid == pid) return &records[i];
    return NULL;
}

static int process_is_descendant( const struct process_record *records, int count, int pid, int root_pid )
{
    int depth;

    for (depth = 0; pid > 0 && depth <= count; ++depth)
    {
        const struct process_record *record;

        if (pid == root_pid) return 1;
        if (!(record = find_process( records, count, pid )) || !record->has_bsd_info) return 0;
        if (record->bsd_info.pbi_ppid == (unsigned int)pid) return 0;
        pid = record->bsd_info.pbi_ppid;
    }
    return 0;
}

static int bounded_process_list_capacity( int bytes )
{
    const int reserve = PROCESS_LIST_PID_RESERVE * (int)sizeof(int);

    if (bytes <= 0 || bytes > INT_MAX - reserve) return 0;
    return bytes + reserve;
}

static int process_has_prefix( int pid, const char *prefix, char *buffer, size_t buffer_size )
{
    int argc, mib[] = { CTL_KERN, KERN_PROCARGS2, pid };
    char *cursor, *end, *terminator;
    size_t size = buffer_size, prefix_len = strlen( prefix );
    int i;

    if (sysctl( mib, 3, buffer, &size, NULL, 0 ) == -1 || size <= sizeof(argc)) return 0;
    memcpy( &argc, buffer, sizeof(argc) );
    if (argc < 0) return 0;

    cursor = buffer + sizeof(argc);
    end = buffer + size;
    if (!(terminator = memchr( cursor, 0, end - cursor ))) return 0;
    cursor = terminator + 1;
    while (cursor < end && !*cursor) ++cursor;

    for (i = 0; i < argc && cursor < end; ++i)
    {
        if (!(terminator = memchr( cursor, 0, end - cursor ))) return 0;
        cursor = terminator + 1;
        while (cursor < end && !*cursor) ++cursor;
    }

    while (cursor < end)
    {
        size_t len;

        while (cursor < end && !*cursor) ++cursor;
        if (cursor == end || !(terminator = memchr( cursor, 0, end - cursor ))) break;
        len = terminator - cursor;
        if (len == sizeof("WINEPREFIX=") - 1 + prefix_len &&
            !memcmp( cursor, "WINEPREFIX=", sizeof("WINEPREFIX=") - 1 ) &&
            !memcmp( cursor + sizeof("WINEPREFIX=") - 1, prefix, prefix_len )) return 1;
        cursor = terminator + 1;
    }
    return 0;
}

static int runtime_launcher_argument_is_canonical( const char *argument, const char *runtime_root )
{
    char wine_path[PATH_MAX], switchyard_wine_path[PATH_MAX];
    int wine_length, switchyard_wine_length;

    wine_length = snprintf( wine_path, sizeof(wine_path), "%s/bin/wine", runtime_root );
    switchyard_wine_length = snprintf( switchyard_wine_path, sizeof(switchyard_wine_path),
                                       "%s/bin/switchyard-wine", runtime_root );
    if (wine_length < 0 || (size_t)wine_length >= sizeof(wine_path) ||
        switchyard_wine_length < 0 || (size_t)switchyard_wine_length >= sizeof(switchyard_wine_path))
        return 0;
    return !strcmp( argument, wine_path ) || !strcmp( argument, switchyard_wine_path );
}

static int environment_name_is_valid( const char *name, size_t length )
{
    size_t i;

    if (!length || !((name[0] >= 'A' && name[0] <= 'Z') ||
                     (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_')) return 0;
    for (i = 1; i < length; ++i)
        if (!((name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '_')) return 0;
    return 1;
}

static int argument_is_environment_assignment( const char *argument )
{
    const char *separator = strchr( argument, '=' );

    return separator && environment_name_is_valid( argument, (size_t)(separator - argument) );
}

static int wrapper_bootstrap_argv_is_trusted( int pid, int required_pid, const char *process_path,
                                              const char *env_path, const char *bash_path,
                                              const char *runtime_root, int argc,
                                              const char *const *arguments )
{
    int env_process = !strcmp( process_path, env_path );
    int bash_process = !strcmp( process_path, bash_path );
    int command_index;

    if (pid != required_pid) return 0;
    if (!env_process && !bash_process) return 0;
    if (argc <= 0 || argc > MAX_WRAPPER_BOOTSTRAP_ARGUMENTS) return 0;

    /* PROC_PIDPATHINFO and KERN_PROCARGS2 are separate snapshots. Accept the
     * exact next bootstrap shape if the required PID execs between queries. */
    if (runtime_launcher_argument_is_canonical( arguments[0], runtime_root )) return 1;
    if (argc >= 2 && (!strcmp( arguments[0], "bash" ) ||
                      !strcmp( arguments[0], bash_path )) &&
        runtime_launcher_argument_is_canonical( arguments[1], runtime_root )) return 1;
    if (!env_process || (strcmp( arguments[0], "env" ) &&
                         strcmp( arguments[0], env_path ))) return 0;

    command_index = 1;
    while (command_index < argc)
    {
        if (!strcmp( arguments[command_index], "-u" ))
        {
            if (command_index + 1 >= argc ||
                !environment_name_is_valid( arguments[command_index + 1],
                                            strlen( arguments[command_index + 1] ) )) return 0;
            command_index += 2;
            continue;
        }
        if (argument_is_environment_assignment( arguments[command_index] ))
        {
            ++command_index;
            continue;
        }
        break;
    }
    if (command_index >= argc) return 0;
    if (runtime_launcher_argument_is_canonical( arguments[command_index], runtime_root )) return 1;
    return command_index + 1 < argc &&
           (!strcmp( arguments[command_index], "bash" ) ||
            !strcmp( arguments[command_index], bash_path )) &&
           runtime_launcher_argument_is_canonical( arguments[command_index + 1], runtime_root );
}

static int process_is_trusted_wrapper_bootstrap( int pid, int required_pid, const char *process_path,
                                                 const char *env_path, const char *bash_path,
                                                 const char *runtime_root, char *buffer,
                                                 size_t buffer_size )
{
    int argc, mib[] = { CTL_KERN, KERN_PROCARGS2, pid };
    char *cursor, *end, *terminator;
    const char *arguments[MAX_WRAPPER_BOOTSTRAP_ARGUMENTS];
    size_t size = buffer_size;
    int i;

    if (pid != required_pid) return 0;
    if (sysctl( mib, 3, buffer, &size, NULL, 0 ) == -1 || size <= sizeof(argc)) return 0;
    memcpy( &argc, buffer, sizeof(argc) );
    if (argc <= 0 || argc > MAX_WRAPPER_BOOTSTRAP_ARGUMENTS) return 0;

    cursor = buffer + sizeof(argc);
    end = buffer + size;
    if (!(terminator = memchr( cursor, 0, end - cursor ))) return 0;
    cursor = terminator + 1;
    while (cursor < end && !*cursor) ++cursor;

    for (i = 0; i < argc; ++i)
    {
        if (cursor == end || !(terminator = memchr( cursor, 0, end - cursor ))) return 0;
        arguments[i] = cursor;
        cursor = terminator + 1;
    }
    return wrapper_bootstrap_argv_is_trusted( pid, required_pid, process_path, env_path, bash_path,
                                              runtime_root, argc, arguments );
}

static int validate_matched_process( const struct process_record *record, const char *runtime_root,
                                     int runtime_process, int descendant, int prefix_process,
                                     int wrapper_bootstrap )
{
    int failed = 0;
    int pid = record->pid;
    int safe_path = record->has_path && !path_has_line_break( record->path );

    /* BSD and architecture metadata are captured together before path and
     * argument matching, and remain evidence if the process exits. */
    if (record->bsd_info.pbi_flags & P_TRANSLATED)
    {
        fprintf( stderr, "runtime process %d is Rosetta translated\n", pid );
        failed = 1;
    }
    if ((!safe_path || !path_is_below( record->path, runtime_root )) && !wrapper_bootstrap)
    {
        fprintf( stderr, "runtime-associated process %d is an unexpected external helper: %s\n",
                 pid, safe_path ? record->path : "<unavailable-or-unsafe>" );
        failed = 1;
    }
    if (!record->has_arch_info)
    {
        fprintf( stderr,
                 "cannot query captured matched runtime process %d; native architecture proof is unproven\n",
                 pid );
        return 1;
    }

    printf( "pid=%d cpuType=%#x translated=%s runtime=%s descendant=%s prefix=%s "
            "wrapperBootstrap=%s path=%s\n",
            pid, record->arch_info.p_cputype,
            (record->bsd_info.pbi_flags & P_TRANSLATED) ? "true" : "false",
            runtime_process ? "true" : "false", descendant ? "true" : "false",
            prefix_process ? "true" : "false", wrapper_bootstrap ? "true" : "false",
            safe_path ? record->path : "<unavailable-or-unsafe>" );
    if (record->arch_info.p_cputype != CPU_TYPE_ARM64)
    {
        fprintf( stderr, "runtime process %d is not native ARM64 (cpu type %#x)\n",
                 pid, record->arch_info.p_cputype );
        failed = 1;
    }
    return failed;
}

static int run_exited_matched_process_regression_fixture(void)
{
    struct process_record record;

    memset( &record, 0, sizeof(record) );
    record.pid = 4242;
    record.has_path = 1;
    record.bsd_info.pbi_flags = P_TRANSLATED;
    snprintf( record.path, sizeof(record.path), "%s", "/usr/bin/switchyard-probe-fixture-helper" );

    if (!validate_matched_process( &record, "/tmp/switchyard-probe-fixture-runtime",
                                   1, 1, 1, 0 ))
    {
        fprintf( stderr, "exited matched-process regression fixture unexpectedly passed\n" );
        return 1;
    }

    record.bsd_info.pbi_flags = 0;
    record.has_arch_info = 1;
    record.arch_info.p_cputype = CPU_TYPE_ARM64;
    snprintf( record.path, sizeof(record.path), "%s/bin/wine",
              "/tmp/switchyard-probe-fixture-runtime" );
    if (validate_matched_process( &record, "/tmp/switchyard-probe-fixture-runtime",
                                  1, 1, 1, 0 ))
    {
        fprintf( stderr, "captured native architecture did not survive the exit fixture\n" );
        return 1;
    }
    return 0;
}

static int run_wrapper_bootstrap_regression_fixture(void)
{
    static const char runtime_root[] = "/tmp/switchyard-probe-fixture-runtime";
    static const char wine_path[] = "/tmp/switchyard-probe-fixture-runtime/bin/wine";
    static const char switchyard_wine_path[] =
        "/tmp/switchyard-probe-fixture-runtime/bin/switchyard-wine";
    struct process_record record;
    char env_path[PATH_MAX], bash_path[PATH_MAX];
    const char *env_arguments[] = { NULL, "bash", wine_path, "cmd" };
    const char *clean_env_arguments[] = {
        NULL, "-u", "WINEDLLPATH", "-u", "DYLD_LIBRARY_PATH",
        "WINEPREFIX=/tmp/switchyard-prefix", "WINEDEBUG=-all", switchyard_wine_path, "cmd"
    };
    const char *bash_arguments[] = { "bash", switchyard_wine_path, "cmd" };
    const char *transitioned_bash_arguments[] = { "bash", wine_path };
    const char *transitioned_wine_arguments[] = { switchyard_wine_path, "cmd" };
    const char *external_arguments[] = { NULL, "bash", "/tmp/wine", "cmd" };
    const char *misplaced_launcher_arguments[] = { "tool", "--flag", wine_path };

    if (!realpath( "/usr/bin/env", env_path ) || !realpath( "/bin/bash", bash_path ))
    {
        fprintf( stderr, "cannot resolve trusted wrapper interpreters for regression fixture\n" );
        return 1;
    }
    env_arguments[0] = env_path;
    clean_env_arguments[0] = env_path;
    external_arguments[0] = env_path;
    if (!wrapper_bootstrap_argv_is_trusted( 4242, 4242, env_path, env_path, bash_path,
                                            runtime_root, 4, env_arguments ) ||
        !wrapper_bootstrap_argv_is_trusted( 4242, 4242, env_path, env_path, bash_path,
                                            runtime_root, 9, clean_env_arguments ) ||
        !wrapper_bootstrap_argv_is_trusted( 4242, 4242, bash_path, env_path, bash_path,
                                            runtime_root, 3, bash_arguments ) ||
        !wrapper_bootstrap_argv_is_trusted( 4242, 4242, env_path, env_path, bash_path,
                                            runtime_root, 2, transitioned_bash_arguments ) ||
        !wrapper_bootstrap_argv_is_trusted( 4242, 4242, bash_path, env_path, bash_path,
                                            runtime_root, 2, transitioned_wine_arguments ))
    {
        fprintf( stderr, "canonical required wrapper bootstrap fixture was rejected\n" );
        return 1;
    }
    if (wrapper_bootstrap_argv_is_trusted( 4243, 4242, env_path, env_path, bash_path,
                                           runtime_root, 4, env_arguments ) ||
        wrapper_bootstrap_argv_is_trusted( 4242, 4242, "/tmp/bash", env_path, bash_path,
                                           runtime_root, 2, transitioned_bash_arguments ) ||
        wrapper_bootstrap_argv_is_trusted( 4242, 4242, env_path, env_path, bash_path,
                                           runtime_root, 4, external_arguments ) ||
        wrapper_bootstrap_argv_is_trusted( 4242, 4242, env_path, env_path, bash_path,
                                           runtime_root, 3, misplaced_launcher_arguments ) ||
        wrapper_bootstrap_argv_is_trusted( 4242, 4242, env_path, env_path, bash_path,
                                           runtime_root, MAX_WRAPPER_BOOTSTRAP_ARGUMENTS + 1,
                                           env_arguments ))
    {
        fprintf( stderr, "bounded wrapper bootstrap allowlist accepted a forbidden fixture\n" );
        return 1;
    }

    memset( &record, 0, sizeof(record) );
    record.pid = 4242;
    record.has_path = 1;
    record.has_arch_info = 1;
    snprintf( record.path, sizeof(record.path), "%s", env_path );
    record.arch_info.p_cputype = CPU_TYPE_ARM64;
    if (validate_matched_process( &record, runtime_root, 0, 1, 1, 1 ))
    {
        fprintf( stderr, "native required wrapper bootstrap fixture unexpectedly failed\n" );
        return 1;
    }
    record.bsd_info.pbi_flags = P_TRANSLATED;
    if (!validate_matched_process( &record, runtime_root, 0, 1, 1, 1 ))
    {
        fprintf( stderr, "translated wrapper bootstrap fixture unexpectedly passed\n" );
        return 1;
    }
    record.bsd_info.pbi_flags = 0;
    record.arch_info.p_cputype = CPU_TYPE_X86_64;
    if (!validate_matched_process( &record, runtime_root, 0, 1, 1, 1 ))
    {
        fprintf( stderr, "non-ARM64 wrapper bootstrap fixture unexpectedly passed\n" );
        return 1;
    }
    return 0;
}

static int run_process_list_capacity_regression_fixture(void)
{
    int minimum = (int)sizeof(int);

    if (bounded_process_list_capacity( minimum ) !=
        minimum + PROCESS_LIST_PID_RESERVE * (int)sizeof(int) ||
        bounded_process_list_capacity( 0 ) || bounded_process_list_capacity( -1 ) ||
        bounded_process_list_capacity( INT_MAX ))
    {
        fprintf( stderr, "bounded process-list capacity fixture failed\n" );
        return 1;
    }
    return 0;
}

static int parse_pid( const char *text, int *pid )
{
    char *end;
    long value;

    errno = 0;
    value = strtol( text, &end, 10 );
    if (errno || end == text || *end || value <= 0 || value > INT_MAX) return 0;
    *pid = value;
    return 1;
}

static int parse_minimum_start( const char *text, unsigned long long *minimum_start )
{
    char *end;
    unsigned long long value;

    errno = 0;
    value = strtoull( text, &end, 10 );
    if (errno || end == text || *end || !value) return 0;
    *minimum_start = value;
    return 1;
}

static int capture_process_identity( int pid, struct proc_bsdinfo *identity )
{
    if (proc_pidinfo( pid, PROC_PIDTBSDINFO, 0, identity, sizeof(*identity) ) !=
        sizeof(*identity) || identity->pbi_pid != (unsigned int)pid ||
        identity->pbi_uid != getuid())
    {
        fprintf( stderr, "cannot capture current-user process identity for pid %d\n", pid );
        return 0;
    }
    return 1;
}

static int parse_process_identity( const char *text, struct proc_bsdinfo *identity )
{
    unsigned int pid, ppid, uid;
    unsigned long long seconds, microseconds;
    char trailing;

    memset( identity, 0, sizeof(*identity) );
    if (sscanf( text, "%u:%u:%u:%llu:%llu%c", &pid, &ppid, &uid,
                &seconds, &microseconds, &trailing ) != 5 || !pid ||
        microseconds >= 1000000) return 0;
    identity->pbi_pid = pid;
    identity->pbi_ppid = ppid;
    identity->pbi_uid = uid;
    identity->pbi_start_tvsec = seconds;
    identity->pbi_start_tvusec = microseconds;
    return 1;
}

static int run_loaded_image_regression_fixture(void)
{
    static const char expected_path[] = "/private/tmp/runtime/aarch64-unix/winemetal.so";
    struct loaded_image_identity image;
    struct proc_regionwithpathinfo region;
    struct proc_bsdinfo left, right;
    uint64_t next;

    memset( &image, 0, sizeof(image) );
    memset( &region, 0, sizeof(region) );
    image.path = expected_path;
    image.stat.st_dev = 17;
    image.stat.st_ino = 42;
    region.prp_vip.vip_vi.vi_stat.vst_dev = 17;
    region.prp_vip.vip_vi.vi_stat.vst_ino = 42;
    snprintf( region.prp_vip.vip_path, sizeof(region.prp_vip.vip_path), "%s", expected_path );
    region.prp_prinfo.pri_address = 0x10000;
    region.prp_prinfo.pri_size = 0x4000;
    region.prp_prinfo.pri_protection = VM_PROT_READ | VM_PROT_EXECUTE;
    if (!image_identity_matches_vnode( &image, &region.prp_vip.vip_vi.vi_stat ) ||
        !loaded_image_region_is_valid( &region, 0, 0, &next ) || next != 0x14000 ||
        path_has_component_suffix( expected_path, "x86_64-unix/winemetal.so" ))
    {
        fprintf( stderr, "valid loaded-image identity fixture was rejected\n" );
        return 1;
    }
    region.prp_vip.vip_vi.vi_stat.vst_ino = 43;
    if (image_identity_matches_vnode( &image, &region.prp_vip.vip_vi.vi_stat ))
    {
        fprintf( stderr, "loaded-image fixture accepted a different vnode\n" );
        return 1;
    }
    region.prp_vip.vip_vi.vi_stat.vst_ino = 42;
    region.prp_prinfo.pri_size = 0;
    if (loaded_image_region_is_valid( &region, 0, 0, &next ))
    {
        fprintf( stderr, "loaded-image fixture accepted a zero-size region\n" );
        return 1;
    }
    if (loaded_image_region_result( sizeof(region), 0 ) != 1 ||
        loaded_image_region_result( 0, EINVAL ) != 0 ||
        loaded_image_region_result( 0, 0 ) != -1 ||
        loaded_image_region_result( sizeof(region) - 1, 0 ) != -1)
    {
        fprintf( stderr, "loaded-image fixture accepted a partial or ambiguous enumeration result\n" );
        return 1;
    }
    region.prp_prinfo.pri_size = 0x4000;
    if (loaded_image_region_is_valid( &region, 0x20000, 0, &next ) ||
        loaded_image_region_is_valid( &region, 0, MAX_LOADED_IMAGE_REGIONS, &next ))
    {
        fprintf( stderr, "loaded-image fixture accepted a nonmonotonic or excess region\n" );
        return 1;
    }
    region.prp_prinfo.pri_address = UINT64_MAX - 0x1000;
    region.prp_prinfo.pri_size = 0x2000;
    if (loaded_image_region_is_valid( &region, 0, 0, &next ))
    {
        fprintf( stderr, "loaded-image fixture accepted an overflowing region\n" );
        return 1;
    }
    memset( region.prp_vip.vip_path, 'x', sizeof(region.prp_vip.vip_path) );
    region.prp_prinfo.pri_address = 0x10000;
    region.prp_prinfo.pri_size = 0x4000;
    if (loaded_image_region_is_valid( &region, 0, 0, &next ))
    {
        fprintf( stderr, "loaded-image fixture accepted an unterminated path\n" );
        return 1;
    }
    if (!path_has_component_suffix( "/tmp/x86_64-unix/winemetal.so",
                                     "x86_64-unix/winemetal.so" ) ||
        path_has_component_suffix( "/tmp/not-x86_64-unix/winemetal.so",
                                   "x86_64-unix/winemetal.so" ) ||
        path_has_component_suffix( "/tmp/x86_64-unix/winemetal.so.lookalike",
                                   "x86_64-unix/winemetal.so" ))
    {
        fprintf( stderr, "loaded-image forbidden-path fixture is not component exact\n" );
        return 1;
    }
    memset( &left, 0, sizeof(left) );
    left.pbi_pid = 42;
    left.pbi_ppid = 7;
    left.pbi_uid = 501;
    left.pbi_start_tvsec = 123;
    left.pbi_start_tvusec = 456;
    right = left;
    if (!same_process_identity( &left, &right ))
    {
        fprintf( stderr, "identical process-start fixture was rejected\n" );
        return 1;
    }
    ++right.pbi_start_tvusec;
    if (same_process_identity( &left, &right ))
    {
        fprintf( stderr, "changed process-start fixture was accepted\n" );
        return 1;
    }
    return 0;
}

static int run_self_loaded_image_regression_fixture( const char *program )
{
    struct loaded_image_identity images[3];
    struct proc_bsdinfo identity;
    char executable[PATH_MAX];
    size_t i;

    if (!realpath( program, executable ) ||
        !capture_process_identity( getpid(), &identity )) return 1;
    memset( images, 0, sizeof(images) );
    for (i = 0; i < sizeof(images) / sizeof(images[0]); ++i) images[i].path = executable;
    return inspect_loaded_images( getpid(), &identity, executable,
                                  images, sizeof(images) / sizeof(images[0]) );
}

int main( int argc, char **argv )
{
    char runtime_root[PATH_MAX], env_path[PATH_MAX], bash_path[PATH_MAX];
    char prefix[PATH_MAX];
    struct stat runtime_info, prefix_info;
    char *args_buffer = NULL;
    struct process_record *records;
    int *pids, required_pid, bytes, capacity, count, i, argmax;
    size_t argmax_size = sizeof(argmax);
    unsigned long long minimum_start;
    int matched = 0, found_required = 0, found_server = 0, failed = 0;

    if (argc == 2 && !strcmp( argv[1], "--regression-fixture-exited-matched-process" ))
        return run_exited_matched_process_regression_fixture();
    if (argc == 2 && !strcmp( argv[1], "--regression-fixture-wrapper-bootstrap" ))
        return run_wrapper_bootstrap_regression_fixture();
    if (argc == 2 && !strcmp( argv[1], "--regression-fixture-process-list-capacity" ))
        return run_process_list_capacity_regression_fixture();
    if (argc == 2 && !strcmp( argv[1], "--regression-fixture-loaded-images" ))
        return run_loaded_image_regression_fixture();
    if (argc == 2 && !strcmp( argv[1], "--regression-self-loaded-image" ))
        return run_self_loaded_image_regression_fixture( argv[0] );
    if (argc == 3 && !strcmp( argv[1], "--capture-process-identity" ))
    {
        struct proc_bsdinfo identity;

        if (!parse_pid( argv[2], &required_pid ) ||
            !capture_process_identity( required_pid, &identity )) return 1;
        printf( "%u:%u:%u:%llu:%llu\n", identity.pbi_pid, identity.pbi_ppid,
                identity.pbi_uid, identity.pbi_start_tvsec, identity.pbi_start_tvusec );
        return 0;
    }
    if (argc >= 6 && argc <= 5 + MAX_REQUIRED_LOADED_IMAGES &&
        !strcmp( argv[1], "--loaded-images" ))
    {
        struct loaded_image_identity images[MAX_REQUIRED_LOADED_IMAGES];
        struct proc_bsdinfo identity;
        size_t image_count = (size_t)argc - 5;
        size_t j;

        if (!parse_pid( argv[2], &required_pid ) ||
            !parse_process_identity( argv[3], &identity ) ||
            identity.pbi_pid != (unsigned int)required_pid)
        {
            fprintf( stderr, "invalid loaded-image process identity\n" );
            return 2;
        }
        memset( images, 0, sizeof(images) );
        for (i = 0; i < (int)image_count; ++i) images[i].path = argv[5 + i];
        for (i = 0; i < (int)image_count; ++i)
            for (j = (size_t)i + 1; j < image_count; ++j)
                if (!strcmp( images[i].path, images[j].path ))
                {
                    fprintf( stderr, "required loaded-image paths are not distinct\n" );
                    return 2;
                }
        return inspect_loaded_images( required_pid, &identity, argv[4],
                                      images, image_count );
    }

    if (argc != 5)
    {
        fprintf( stderr, "usage: %s RUNTIME_ROOT REQUIRED_PID MINIMUM_START_SECONDS PREFIX\n"
                         "       %s --capture-process-identity PID\n"
                         "       %s --loaded-images PID IDENTITY EXECUTABLE IMAGE [IMAGE ...]\n",
                 argv[0], argv[0], argv[0] );
        return 2;
    }
    if (!realpath( argv[1], runtime_root ) || path_has_line_break( runtime_root ) ||
        !strcmp( runtime_root, "/" ) ||
        stat( runtime_root, &runtime_info ) == -1 || !S_ISDIR( runtime_info.st_mode ))
    {
        fprintf( stderr, "cannot resolve a bounded runtime root %s\n", argv[1] );
        return 2;
    }
    if (!realpath( "/usr/bin/env", env_path ) || !realpath( "/bin/bash", bash_path ))
    {
        fprintf( stderr, "cannot resolve trusted wrapper interpreters\n" );
        return 2;
    }
    if (!parse_pid( argv[2], &required_pid ))
    {
        fprintf( stderr, "invalid required pid %s\n", argv[2] );
        return 2;
    }
    if (!parse_minimum_start( argv[3], &minimum_start ))
    {
        fprintf( stderr, "invalid minimum start time %s\n", argv[3] );
        return 2;
    }
    if (!realpath( argv[4], prefix ) || path_has_line_break( prefix ) ||
        stat( prefix, &prefix_info ) == -1 ||
        !S_ISDIR( prefix_info.st_mode ))
    {
        fprintf( stderr, "cannot resolve prefix directory %s\n", argv[4] );
        return 2;
    }
    if (sysctlbyname( "kern.argmax", &argmax, &argmax_size, NULL, 0 ) == -1 || argmax <= 0 ||
        argmax > MAX_PROCESS_ARGUMENT_BYTES ||
        !(args_buffer = malloc( argmax )))
    {
        fprintf( stderr, "cannot allocate process-argument buffer\n" );
        return 1;
    }

    bytes = proc_listpids( PROC_ALL_PIDS, 0, NULL, 0 );
    if (bytes <= 0)
    {
        fprintf( stderr, "proc_listpids sizing failed\n" );
        free( args_buffer );
        return 1;
    }
    if (!(capacity = bounded_process_list_capacity( bytes )))
    {
        fprintf( stderr, "process snapshot size exceeds its bounded allocation\n" );
        free( args_buffer );
        return 1;
    }
    if (!(pids = calloc( 1, capacity )))
    {
        fprintf( stderr, "cannot allocate process list\n" );
        free( args_buffer );
        return 1;
    }
    bytes = proc_listpids( PROC_ALL_PIDS, 0, pids, capacity );
    if (bytes <= 0)
    {
        fprintf( stderr, "proc_listpids failed\n" );
        free( args_buffer );
        free( pids );
        return 1;
    }
    if (bytes >= capacity || bytes % (int)sizeof(*pids))
    {
        fprintf( stderr, "process snapshot exceeded its bounded allocation\n" );
        free( args_buffer );
        free( pids );
        return 1;
    }

    count = bytes / (int)sizeof(*pids);
    if (getenv( "SWITCHYARD_PROCESS_PROBE_DEBUG" ))
    {
        int max_pid = 0;
        for (i = 0; i < count; ++i) if (pids[i] > max_pid) max_pid = pids[i];
        fprintf( stderr, "process snapshot bytes=%d capacity=%d count=%d required=%d max=%d\n",
                 bytes, capacity, count, required_pid, max_pid );
    }
    if (!(records = calloc( count, sizeof(*records) )))
    {
        fprintf( stderr, "cannot allocate process metadata\n" );
        free( args_buffer );
        free( pids );
        return 1;
    }
    for (i = 0; i < count; ++i)
    {
        char resolved_path[PATH_MAX];

        records[i].pid = pids[i];
        if (records[i].pid <= 0) continue;
        records[i].has_bsd_info = proc_pidinfo( records[i].pid, PROC_PIDTBSDINFO, 0,
                                                &records[i].bsd_info,
                                                sizeof(records[i].bsd_info) ) == sizeof(records[i].bsd_info);
        if (records[i].has_bsd_info &&
            records[i].bsd_info.pbi_start_tvsec >= minimum_start)
            records[i].has_arch_info =
                proc_pidinfo( records[i].pid, PROC_PIDARCHINFO, 0,
                              &records[i].arch_info, sizeof(records[i].arch_info) ) ==
                sizeof(records[i].arch_info);
        records[i].has_path = proc_pidpath( records[i].pid, records[i].path,
                                            sizeof(records[i].path) ) > 0;
        if (records[i].has_path && realpath( records[i].path, resolved_path ))
            snprintf( records[i].path, sizeof(records[i].path), "%s", resolved_path );
        if (records[i].pid == required_pid && getenv( "SWITCHYARD_PROCESS_PROBE_DEBUG" ))
            fprintf( stderr, "required snapshot bsd=%d arch=%d path=%d ppid=%u start=%llu value=%s\n",
                     records[i].has_bsd_info, records[i].has_arch_info, records[i].has_path,
                     records[i].bsd_info.pbi_ppid, records[i].bsd_info.pbi_start_tvsec,
                     records[i].has_path ? records[i].path : "<unavailable>" );
    }

    for (i = 0; i < count; ++i)
    {
        const struct process_record *record = &records[i];
        int descendant, prefix_process, runtime_process, wrapper_bootstrap;
        int pid = record->pid;

        if (pid <= 0 || !record->has_bsd_info) continue;
        descendant = process_is_descendant( records, count, pid, required_pid );
        prefix_process = record->bsd_info.pbi_uid == getuid() &&
                         record->bsd_info.pbi_start_tvsec >= minimum_start &&
                         process_has_prefix( pid, prefix, args_buffer, argmax );
        runtime_process = record->has_path && path_is_below( record->path, runtime_root ) &&
                          record->bsd_info.pbi_start_tvsec >= minimum_start;
        wrapper_bootstrap = record->has_path &&
                            record->bsd_info.pbi_start_tvsec >= minimum_start &&
                            process_is_trusted_wrapper_bootstrap(
                                pid, required_pid, record->path, env_path, bash_path, runtime_root,
                                args_buffer, argmax );
        if (!descendant && !prefix_process && !runtime_process) continue;

        ++matched;
        if (pid == required_pid) found_required = 1;
        if (record->has_path && runtime_process &&
            !strcmp( path_basename( record->path ), "wineserver" ))
            found_server = 1;
        if (validate_matched_process( record, runtime_root, runtime_process, descendant,
                                      prefix_process, wrapper_bootstrap ))
            failed = 1;
    }
    free( records );
    free( args_buffer );
    free( pids );

    if (!matched)
    {
        fprintf( stderr, "no process from runtime root %s was observed\n", runtime_root );
        failed = 1;
    }
    if (!found_required)
    {
        fprintf( stderr, "required runtime process %d was not observed\n", required_pid );
        failed = 1;
    }
    if (!found_server)
    {
        fprintf( stderr, "native wineserver was not observed under runtime root %s\n", runtime_root );
        failed = 1;
    }
    return failed ? 1 : 0;
}
