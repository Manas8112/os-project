#include "kernel/types.h"
#include "user/user.h"
int main(int argc, char *argv[])
{
	if (argc < 2) {
		printf("usage: greet name\n");
		exit(1);
	}
	printf("Hello, %s! This is running inside xv6.\n", argv[1]);
	exit(0);
}
