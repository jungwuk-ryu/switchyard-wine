// SPDX-License-Identifier: MIT
// The lookup caches and call/return predictor require immediate zeroes, not
// merely permission for the kernel to reclaim old contents at some later time.
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef SWITCHYARD_TEST_ZERO_UNSUPPORTED
static int unsupported_zero(void* address, size_t length, int advice) {
  (void)address;
  (void)length;
  (void)advice;
  errno = ENOTSUP;
  return -1;
}
#define madvise unsupported_zero
#endif
#include <FEXCore/Utils/AllocatorHooks.h>
#ifdef SWITCHYARD_TEST_ZERO_UNSUPPORTED
#undef madvise
#endif

static bool check_range(size_t page, size_t offset, size_t length, bool recommit) {
  const size_t size = page * 8;
  auto* data = static_cast<unsigned char*>(::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (data == MAP_FAILED) return false;
  bool passed = true;
  for (unsigned int repeat = 0; repeat < 8 && passed; ++repeat) {
    std::memset(data, 0xa5, size);
    FEXCore::Allocator::VirtualDontNeed(data + offset, length, recommit);
    for (size_t index = 0; index < size; ++index) {
      const unsigned char expected = index >= offset && index - offset < length ? 0 : 0xa5;
      if (data[index] != expected) {
        std::fprintf(stderr, "zero contract failed: offset=%zu length=%zu recommit=%u index=%zu got=%u\n",
                     offset, length, static_cast<unsigned int>(recommit), index,
                     static_cast<unsigned int>(data[index]));
        passed = false;
        break;
      }
    }
  }
  if (::munmap(data, size)) return false;
  return passed;
}

struct worker_context {
  size_t page;
  bool passed;
};

static void* check_worker(void* argument) {
  auto* worker = static_cast<worker_context*>(argument);
  const size_t page = worker->page;
  worker->passed = check_range(page, page, page * 2, true) &&
                   check_range(page, page, page * 2, false) &&
                   check_range(page, page + 7, page * 3 - 19, false) &&
                   check_range(page, page / 4, page / 2, true) &&
                   check_range(page, page + 1, 0, false);
  return nullptr;
}

static bool check_sparse(size_t page) {
  const size_t size = 64 * 1024 * 1024;
  auto* data = static_cast<unsigned char*>(::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (data == MAP_FAILED) return false;
  data[page] = 1;
  data[size / 2] = 2;
  data[size - page - 1] = 3;
  data[0] = 0xa5;
  data[size - 1] = 0x5a;
  FEXCore::Allocator::VirtualDontNeed(data + page, size - page * 2, false);
  const bool passed = !data[page] && !data[size / 2] && !data[size - page - 1] &&
                      data[0] == 0xa5 && data[size - 1] == 0x5a;
  return ::munmap(data, size) == 0 && passed;
}

static void expected_abort(int signal_number) {
  (void)signal_number;
  _exit(99);  // Do not generate crash reports for the expected fail-closed case.
}

static bool check_invalid_range(void* address, size_t length) {
  const pid_t child = ::fork();
  if (child < 0) return false;
  if (!child) {
    struct sigaction action {};
    action.sa_handler = expected_abort;
    if (sigemptyset(&action.sa_mask) || ::sigaction(SIGABRT, &action, nullptr)) _exit(2);
    FEXCore::Allocator::VirtualDontNeed(address, length);
    _exit(1);
  }
  int status = 0;
  pid_t result;
  do {
    result = ::waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);
  return result == child && WIFEXITED(status) && WEXITSTATUS(status) == 99;
}

int main() {
  const long host_page = ::sysconf(_SC_PAGESIZE);
  if (host_page <= 0 || host_page > 1024 * 1024) return 2;
  const auto page = static_cast<size_t>(host_page);
  FEXCore::Allocator::VirtualDontNeed(nullptr, 0);
  if (!check_invalid_range(nullptr, 1) ||
      !check_invalid_range(reinterpret_cast<void*>(UINTPTR_MAX - 7), 16)) return 1;
  if (!check_sparse(page)) return 1;
  pthread_t threads[8] {};
  worker_context workers[8] {};
  unsigned int created = 0;
  bool passed = true;
  for (; created < 8; ++created) {
    workers[created].page = page;
    if (pthread_create(&threads[created], nullptr, check_worker, &workers[created])) {
      passed = false;
      break;
    }
  }
  for (unsigned int index = 0; index < created; ++index) {
    if (pthread_join(threads[index], nullptr) || !workers[index].passed) passed = false;
  }
  if (!passed) return 1;
  std::puts("FEX_ALLOCATOR_ZERO_PASS");
  return 0;
}
