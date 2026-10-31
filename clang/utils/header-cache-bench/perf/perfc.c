/* Runs a command and prints its hardware counters, children included:
   perfc cmd args...  ->  stderr: wall_ms cycles instructions llc_refs llc_misses faults */
#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int open_counter(uint32_t type, uint64_t config, int group) {
  struct perf_event_attr a;
  memset(&a, 0, sizeof a);
  a.size = sizeof a;
  a.type = type;
  a.config = config;
  a.disabled = group == -1;
  a.inherit = 1;
  a.enable_on_exec = group == -1;
  a.exclude_kernel = 0;
  return (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  int go[2];
  if (pipe(go)) return 1;
  pid_t child = fork();
  if (child == 0) {
    char c;
    close(go[1]);
    if (read(go[0], &c, 1) < 0) return 1;
    execvp(argv[1], argv + 1);
    _exit(127);
  }
  close(go[0]);
  struct { uint32_t type; uint64_t config; const char *name; } C[] = {
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, "cycles"},
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, "instructions"},
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES, "llc_refs"},
    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES, "llc_misses"},
    {PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS, "faults"},
  };
  int fd[5];
  for (int i = 0; i < 5; ++i) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.type = C[i].type;
    a.config = C[i].config;
    a.inherit = 1;
    a.disabled = 1;
    a.enable_on_exec = 1;
    fd[i] = (int)syscall(SYS_perf_event_open, &a, child, -1, -1, 0);
    if (fd[i] < 0) { perror(C[i].name); return 1; }
  }
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  if (write(go[1], "x", 1) != 1) return 1;
  int status;
  waitpid(child, &status, 0);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  fprintf(stderr, "perfc wall_ms=%.1f", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
  for (int i = 0; i < 5; ++i) {
    uint64_t v = 0;
    if (read(fd[i], &v, sizeof v) != sizeof v) v = 0;
    fprintf(stderr, " %s=%llu", C[i].name, (unsigned long long)v);
  }
  fprintf(stderr, "\n");
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
