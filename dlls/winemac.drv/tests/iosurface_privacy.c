/*
 * macOS IOSurface privacy tests
 *
 * Copyright 2026 Switchyard project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "config.h"

#include <IOSurface/IOSurface.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#define ok(condition, ...) do { if (!(condition)) { fprintf(stderr, __VA_ARGS__); exit(1); } } while (0)

#include "../iosurface_properties.c"

extern char **environ;

static int run_lookup_child(const char *surface_id, int expected)
{
    IOSurfaceRef surface = IOSurfaceLookup(strtoul(surface_id, NULL, 10));
    int found = surface != NULL;

    if (surface) CFRelease(surface);
    if (found != expected)
        fprintf(stderr, "Expected lookup=%d, got %d for surface %s.\n", expected, found, surface_id);
    return found != expected;
}

static void check_child_lookup(IOSurfaceRef surface, int expected)
{
    char path[4096], surface_id[32], expected_string[2];
    char child_argument[] = "child";
    char *arguments[] = {path, child_argument, surface_id, expected_string, NULL};
    uint32_t path_size = sizeof(path);
    pid_t pid;
    int status;

    ok(!_NSGetExecutablePath(path, &path_size), "Executable path is too long.\n");
    snprintf(surface_id, sizeof(surface_id), "%u", IOSurfaceGetID(surface));
    snprintf(expected_string, sizeof(expected_string), "%d", expected);
    ok(!posix_spawn(&pid, path, NULL, NULL, arguments, environ), "Failed to spawn lookup child.\n");
    ok(waitpid(pid, &status, 0) == pid, "Failed to wait for lookup child.\n");
    ok(WIFEXITED(status) && !WEXITSTATUS(status), "Lookup child failed with status %#x.\n", status);
}

int main(int argc, char **argv)
{
    CFMutableDictionaryRef properties, global_properties;
    IOSurfaceRef private_surface, global_surface;

    if (argc == 4 && !strcmp(argv[1], "child"))
        return run_lookup_child(argv[2], atoi(argv[3]));

    ok(!macdrv_create_private_iosurface_properties(0, 64, 4, 0x42475241),
       "Accepted zero-width IOSurface properties.\n");
    ok(!macdrv_create_private_iosurface_properties(64, 0, 4, 0x42475241),
       "Accepted zero-height IOSurface properties.\n");
    ok(!macdrv_create_private_iosurface_properties(64, 64, 0, 0x42475241),
       "Accepted zero-sized IOSurface elements.\n");

    properties = macdrv_create_private_iosurface_properties(64, 64, 4, 0x42475241);
    ok(properties != NULL, "Failed to create private IOSurface properties.\n");
    private_surface = IOSurfaceCreate(properties);
    ok(private_surface != NULL, "Failed to create private IOSurface.\n");
    check_child_lookup(private_surface, 0);

    global_properties = CFDictionaryCreateMutableCopy(NULL, 0, properties);
    ok(global_properties != NULL, "Failed to copy IOSurface properties.\n");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CFDictionarySetValue(global_properties, kIOSurfaceIsGlobal, kCFBooleanTrue);
#pragma clang diagnostic pop
    global_surface = IOSurfaceCreate(global_properties);
    ok(global_surface != NULL, "Failed to create global IOSurface control.\n");
    check_child_lookup(global_surface, 1);

    CFRelease(global_surface);
    CFRelease(global_properties);
    CFRelease(private_surface);
    CFRelease(properties);
    return 0;
}
