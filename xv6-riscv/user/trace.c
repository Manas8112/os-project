#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if (argc < 2) {
    fprintf(2, "Usage: trace 0|1\n");
    exit(1);
  }

  trace(atoi(argv[1]));
  exit(0);
}