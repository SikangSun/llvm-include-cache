/* Runs a command with address-space randomization off and samples its
   instruction pointer every PERIOD retired instructions (user space only).
   psample OUT PERIOD cmd args...  ->  OUT: one hex address per sample;
   a PIE without randomization loads at 0x555555554000. With PSAMPLE_STACK=1
   each line also holds the call sites of the sampled call stack, from the
   CPU's last-branch records, innermost first: "ip caller1 caller2 ...". */
#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define DATA_PAGES 4096

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: psample OUT PERIOD cmd args...\n");
    return 2;
  }
  FILE *Out = fopen(argv[1], "w");
  if (!Out) {
    perror(argv[1]);
    return 1;
  }
  uint64_t Period = strtoull(argv[2], 0, 10);
  int Go[2];
  if (pipe(Go))
    return 1;
  pid_t Child = fork();
  if (Child == 0) {
    char C;
    close(Go[1]);
    personality(ADDR_NO_RANDOMIZE);
    if (read(Go[0], &C, 1) < 0)
      _exit(1);
    execvp(argv[3], argv + 3);
    _exit(127);
  }
  close(Go[0]);
  struct perf_event_attr A;
  memset(&A, 0, sizeof A);
  A.size = sizeof A;
  A.type = PERF_TYPE_HARDWARE;
  A.config = PERF_COUNT_HW_INSTRUCTIONS;
  A.sample_period = Period;
  A.sample_type = PERF_SAMPLE_IP;
  int Stack = getenv("PSAMPLE_STACK") && *getenv("PSAMPLE_STACK") == '1';
  if (Stack) {
    A.sample_type |= PERF_SAMPLE_BRANCH_STACK;
    A.branch_sample_type = PERF_SAMPLE_BRANCH_USER | PERF_SAMPLE_BRANCH_CALL_STACK;
  }
  A.disabled = 1;
  A.enable_on_exec = 1;
  A.inherit = 0; /* the kernel refuses mmap of an inherited per-task event */
  A.exclude_kernel = 1;
  A.exclude_hv = 1;
  A.mmap = 1; /* executable mappings, to name library addresses */
  int Fd = (int)syscall(SYS_perf_event_open, &A, Child, -1, -1, 0);
  if (Fd < 0) {
    perror("perf_event_open");
    return 1;
  }
  long Page = sysconf(_SC_PAGESIZE);
  size_t Len = (size_t)(DATA_PAGES + 1) * Page;
  void *Map = mmap(0, Len, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0);
  if (Map == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  struct perf_event_mmap_page *Meta = Map;
  unsigned char *Data = (unsigned char *)Map + Page;
  uint64_t Size = (uint64_t)DATA_PAGES * Page, Tail = 0, Samples = 0, Lost = 0;
  if (write(Go[1], "x", 1) != 1)
    return 1;
  int Status = 0, Done = 0;
  while (1) {
    if (!Done && waitpid(Child, &Status, WNOHANG) == Child)
      Done = 1;
    uint64_t Head = __atomic_load_n(&Meta->data_head, __ATOMIC_ACQUIRE);
    while (Tail < Head) {
      struct perf_event_header H;
      for (size_t K = 0; K < sizeof H; ++K)
        ((unsigned char *)&H)[K] = Data[(Tail + K) % Size];
      if (H.type == PERF_RECORD_SAMPLE) {
        uint64_t Ip = 0;
        for (size_t K = 0; K < sizeof Ip; ++K)
          ((unsigned char *)&Ip)[K] = Data[(Tail + sizeof H + K) % Size];
        fprintf(Out, "%llx", (unsigned long long)Ip);
        if (Stack) {
          /* u64 nr, then nr entries of from, to, flags. */
          uint64_t Nr = 0, At = Tail + sizeof H + 8;
          for (size_t K = 0; K < 8; ++K)
            ((unsigned char *)&Nr)[K] = Data[(At + K) % Size];
          At += 8;
          for (uint64_t E = 0; E < Nr && E < 64; ++E) {
            uint64_t From = 0;
            for (size_t K = 0; K < 8; ++K)
              ((unsigned char *)&From)[K] = Data[(At + 24 * E + K) % Size];
            fprintf(Out, " %llx", (unsigned long long)From);
          }
        }
        fputc('\n', Out);
        ++Samples;
      } else if (H.type == PERF_RECORD_MMAP) {
        /* pid, tid, addr, len, pgoff, filename */
        unsigned char Rec[4096 + 64];
        size_t N = H.size < sizeof Rec ? H.size : sizeof Rec;
        for (size_t K = 0; K < N; ++K)
          Rec[K] = Data[(Tail + K) % Size];
        uint64_t Addr, MapLen, PgOff;
        memcpy(&Addr, Rec + sizeof H + 8, 8);
        memcpy(&MapLen, Rec + sizeof H + 16, 8);
        memcpy(&PgOff, Rec + sizeof H + 24, 8);
        Rec[N - 1] = 0;
        fprintf(Out, "M %llx %llx %llx %s\n", (unsigned long long)Addr,
                (unsigned long long)MapLen, (unsigned long long)PgOff,
                (char *)(Rec + sizeof H + 32));
      } else if (H.type == PERF_RECORD_LOST) {
        ++Lost;
      }
      Tail += H.size;
    }
    __atomic_store_n(&Meta->data_tail, Tail, __ATOMIC_RELEASE);
    if (Done)
      break;
    usleep(1000);
  }
  fclose(Out);
  fprintf(stderr, "psample samples=%llu lost_records=%llu\n",
          (unsigned long long)Samples, (unsigned long long)Lost);
  return WIFEXITED(Status) ? WEXITSTATUS(Status) : 1;
}
