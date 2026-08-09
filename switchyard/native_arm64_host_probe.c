/*
 * Native ARM64 macOS capability probe for the Switchyard Wine runtime.
 *
 * The low-address probe follows Wine's Darwin reservation sequence: first
 * reserve the exact range without VM_FLAGS_OVERWRITE, then replace only that
 * owned reservation with mmap(MAP_FIXED).  It must never replace a mapping
 * that was already present in the process.
 */

#include <Availability.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/mach_vm.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#if defined(__arm64__) && defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && \
    __MAC_OS_X_VERSION_MAX_ALLOWED >= 260400 && __has_include(<os/arch/arm64.h>)
# include <os/arch/arm64.h>
# define HAVE_CUSTOM_X18_API 1
#else
# define HAVE_CUSTOM_X18_API 0
#endif

#define KUSER_SHARED_DATA_ADDRESS UINT64_C(0x7ffe0000)

struct mapping_result
{
    long page_size;
    bool reservation_succeeded;
    bool mmap_attempted;
    bool mappable;
    bool cleanup_succeeded;
    const char *stage;
    const char *error_domain;
    int error_code;
};

struct x18_result
{
    bool api_available;
    bool preemption_observed;
    bool preemption_preserved;
    const char *state;
    int error_code;
};

static void set_mapping_error(struct mapping_result *result, const char *stage,
                              const char *domain, int code)
{
    result->stage = stage;
    result->error_domain = domain;
    result->error_code = code;
}

static struct mapping_result probe_kuser_shared_data(void)
{
    const mach_vm_address_t target = KUSER_SHARED_DATA_ADDRESS;
    struct mapping_result result = {0, false, false, false, true,
                                    "not-started", "none", 0};
    mach_vm_address_t reservation = target;
    kern_return_t mach_status;
    void *mapping;
    int saved_errno;

    errno = 0;
    result.page_size = sysconf(_SC_PAGESIZE);
    if (result.page_size <= 0)
    {
        set_mapping_error(&result, "page-size", "errno", errno ? errno : EINVAL);
        fprintf(stderr, "cannot determine the macOS host page size: %s\n",
                strerror(result.error_code));
        return result;
    }
    if (target % (mach_vm_address_t)result.page_size)
    {
        set_mapping_error(&result, "alignment", "internal", EINVAL);
        fprintf(stderr,
                "KUSER_SHARED_DATA address 0x%llx is not aligned to the host page size %ld\n",
                (unsigned long long)target, result.page_size);
        return result;
    }

    mach_status = mach_vm_map(mach_task_self(), &reservation,
                              (mach_vm_size_t)result.page_size, 0, VM_FLAGS_FIXED,
                              MEMORY_OBJECT_NULL, 0, false,
                              VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL,
                              VM_INHERIT_COPY);
    if (mach_status != KERN_SUCCESS)
    {
        set_mapping_error(&result, "reservation", "mach", mach_status);
        fprintf(stderr,
                "cannot reserve Windows KUSER_SHARED_DATA at 0x%llx without overwrite: "
                "mach_vm_map returned %d (%s); this host cannot run the native ARM64 "
                "Wine path without a supported low-VA mapping policy\n",
                (unsigned long long)target, mach_status, mach_error_string(mach_status));
        return result;
    }
    result.reservation_succeeded = true;
    if (reservation != target)
    {
        set_mapping_error(&result, "reservation-address", "internal", EFAULT);
        if (mach_vm_deallocate(mach_task_self(), reservation,
                               (mach_vm_size_t)result.page_size) != KERN_SUCCESS)
            result.cleanup_succeeded = false;
        fprintf(stderr,
                "non-overwriting reservation returned 0x%llx instead of 0x%llx; "
                "fixed mmap was not attempted\n",
                (unsigned long long)reservation, (unsigned long long)target);
        return result;
    }

    result.mmap_attempted = true;
    mapping = mmap((void *)(uintptr_t)target, (size_t)result.page_size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (mapping == MAP_FAILED)
    {
        saved_errno = errno;
        set_mapping_error(&result, "mmap", "errno", saved_errno);
        if (mach_vm_deallocate(mach_task_self(), reservation,
                               (mach_vm_size_t)result.page_size) != KERN_SUCCESS)
            result.cleanup_succeeded = false;
        fprintf(stderr,
                "mmap could not place Windows KUSER_SHARED_DATA at 0x%llx: %s; "
                "the non-overwriting reservation was released\n",
                (unsigned long long)target, strerror(saved_errno));
        return result;
    }

    if (mapping != (void *)(uintptr_t)target)
    {
        set_mapping_error(&result, "address", "internal", EFAULT);
        if (munmap(mapping, (size_t)result.page_size) != 0)
            result.cleanup_succeeded = false;
        if (mach_vm_deallocate(mach_task_self(), reservation,
                               (mach_vm_size_t)result.page_size) != KERN_SUCCESS)
            result.cleanup_succeeded = false;
        fprintf(stderr, "mmap returned %p instead of the required address 0x%llx\n",
                mapping, (unsigned long long)target);
        return result;
    }

    *(volatile unsigned char *)mapping = 0xa5;
    if (*(volatile unsigned char *)mapping != 0xa5)
    {
        set_mapping_error(&result, "access", "internal", EIO);
        fprintf(stderr, "the exact KUSER_SHARED_DATA mapping failed its write/read check\n");
    }
    else if (mprotect(mapping, (size_t)result.page_size, PROT_READ) != 0)
    {
        set_mapping_error(&result, "protection", "errno", errno);
        fprintf(stderr, "the exact KUSER_SHARED_DATA mapping could not become read-only: %s\n",
                strerror(result.error_code));
    }
    else
    {
        result.mappable = true;
        result.stage = "complete";
    }

    if (munmap(mapping, (size_t)result.page_size) != 0)
    {
        result.cleanup_succeeded = false;
        result.mappable = false;
        set_mapping_error(&result, "cleanup", "errno", errno);
        fprintf(stderr, "cannot release the KUSER_SHARED_DATA probe mapping: %s\n",
                strerror(errno));
    }
    return result;
}

#if HAVE_CUSTOM_X18_API
API_AVAILABLE(macos(26.4))
static void measure_custom_x18(struct x18_result *result)
{
    const uint64_t sentinel = UINT64_C(0x18c0ffee12345678);
    struct rusage before, after;
    uint64_t observed = 0;
    bool enabled, disabled;

    if (getrusage(RUSAGE_SELF, &before) != 0)
    {
        result->state = "getrusage-before-failed";
        result->error_code = errno;
        return;
    }

    os_set_custom_x18_abi_enabled(true);
    if (!os_custom_x18_abi_enabled())
    {
        result->state = "custom-mode-enable-rejected";
        result->error_code = 19;
        return;
    }
    /*
     * Spend a bounded two seconds in custom mode, deriving the interval from
     * the architectural counter frequency instead of assuming a timer rate.
     * The assembly makes no macOS calls. The explicit x18 clobber prevents the
     * compiler from treating the Windows platform register as unchanged; the
     * build suppresses only the warning for that intentional
     * Darwin-reserved-register use.
     */
    __asm__ volatile(
        "mov x18, %[sentinel]\n"
        "mrs x9, CNTVCT_EL0\n"
        "mrs x10, CNTFRQ_EL0\n"
        "lsl x10, x10, #1\n"
        "1: mrs x11, CNTVCT_EL0\n"
        "sub x11, x11, x9\n"
        "cmp x11, x10\n"
        "b.lo 1b\n"
        :
        : [sentinel] "r"(sentinel)
        : "x9", "x10", "x11", "x18", "cc", "memory");
    enabled = os_custom_x18_abi_enabled();
    if (enabled)
    {
        __asm__ volatile("mov %0, x18" : "=r"(observed) : : "memory");
        os_set_custom_x18_abi_enabled(false);
    }
    disabled = !os_custom_x18_abi_enabled();
    if (getrusage(RUSAGE_SELF, &after) != 0)
    {
        result->state = "getrusage-after-failed";
        result->error_code = errno;
        return;
    }
    result->preemption_observed = after.ru_nivcsw > before.ru_nivcsw;
    if (!enabled)
    {
        result->state = "custom-mode-lost";
        result->error_code = 20;
        return;
    }
    if (observed != sentinel)
    {
        result->state = "x18-marker-lost";
        result->error_code = 22;
        return;
    }
    if (!disabled)
    {
        result->state = "disable-failed";
        result->error_code = 24;
        return;
    }
    if (!result->preemption_observed)
    {
        result->state = "preemption-not-observed";
        result->error_code = EAGAIN;
        return;
    }
    result->preemption_preserved = true;
    result->state = "preemption-preserved";
}
#endif

static struct x18_result probe_custom_x18(void)
{
    struct x18_result result = {false, false, false, "api-unavailable", 0};

#if HAVE_CUSTOM_X18_API
    if (__builtin_available(macOS 26.4, *))
    {
        result.api_available = true;
        measure_custom_x18(&result);
    }
#endif
    if (result.api_available && !result.preemption_preserved)
        fprintf(stderr,
                "the public macOS custom-x18 API is available, but x18 did not survive a "
                "measured scheduler preemption (%s, code %d); verify the signed helper carries "
                "only the required com.apple.security.custom-x18-abi-toggle entitlement\n",
                result.state, result.error_code);
    else if (!result.api_available)
        fprintf(stderr,
                "the public macOS custom-x18 API is unavailable; it requires an ARM64 "
                "macOS 26.4+ SDK and runtime\n");
    return result;
}

static const char *boolean(bool value)
{
    return value ? "true" : "false";
}

int main(int argc, char **argv)
{
    struct mapping_result mapping;
    struct x18_result x18;

    if (argc != 1)
    {
        fprintf(stderr, "usage: %s\n", argv[0]);
        return 2;
    }

    mapping = probe_kuser_shared_data();
    x18 = probe_custom_x18();

    printf("kuserSharedDataAddress=0x%016llx\n",
           (unsigned long long)KUSER_SHARED_DATA_ADDRESS);
    printf("hostPageSize=%ld\n", mapping.page_size);
    printf("kuserSharedDataReservationSucceeded=%s\n",
           boolean(mapping.reservation_succeeded));
    printf("kuserSharedDataMmapAttempted=%s\n", boolean(mapping.mmap_attempted));
    printf("kuserSharedDataMappable=%s\n", boolean(mapping.mappable));
    printf("kuserSharedDataCleanupSucceeded=%s\n",
           boolean(mapping.cleanup_succeeded));
    printf("kuserSharedDataProbeStage=%s\n", mapping.stage);
    printf("kuserSharedDataErrorDomain=%s\n", mapping.error_domain);
    printf("kuserSharedDataErrorCode=%d\n", mapping.error_code);
    printf("customX18ApiCompiled=%s\n", boolean(HAVE_CUSTOM_X18_API));
    printf("customX18ApiAvailable=%s\n", boolean(x18.api_available));
    printf("customX18EntitlementRequired=true\n");
    printf("customX18EntitlementName=com.apple.security.custom-x18-abi-toggle\n");
    printf("customX18EntitlementState=%s\n",
           x18.preemption_preserved ? "effective" : "not-effective");
    printf("customX18PreemptionObserved=%s\n",
           boolean(x18.preemption_observed));
    printf("customX18Preserved=%s\n", boolean(x18.preemption_preserved));
    printf("customX18PreservationProven=%s\n",
           boolean(x18.preemption_preserved));
    printf("customX18ProbeState=%s\n", x18.state);
    printf("customX18ProbeErrorCode=%d\n", x18.error_code);
    return 0;
}
