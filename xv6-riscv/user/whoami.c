#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int fc = getforkcount();
  int pids[16];
  int count = getancestors(pids, 16);

  printf("pid %d, forkcount %d, ancestors:", getpid(), fc);
  for (int i = 0; i < count; i++) {
    printf(" %d", pids[i]);
  }
  printf("\n");

  exit(0);
}