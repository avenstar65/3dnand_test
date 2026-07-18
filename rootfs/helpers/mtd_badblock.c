#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int usage(const char *prog)
{
	fprintf(stderr, "usage: %s get|set DEVICE OFFSET\n", prog);
	return 2;
}

int main(int argc, char **argv)
{
	long long offset;
	char *end;
	int fd;
	int ret;

	if (argc != 4)
		return usage(argv[0]);
	errno = 0;
	offset = strtoll(argv[3], &end, 0);
	if (errno || *argv[3] == '\0' || *end != '\0' || offset < 0)
		return usage(argv[0]);
	if (strcmp(argv[1], "get") && strcmp(argv[1], "set"))
		return usage(argv[0]);

	fd = open(argv[2], O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	if (!strcmp(argv[1], "get")) {
		ret = ioctl(fd, MEMGETBADBLOCK, &offset);
		if (ret >= 0)
			printf("%d\n", !!ret);
	} else {
		ret = ioctl(fd, MEMSETBADBLOCK, &offset);
		if (ret >= 0)
			puts("marked");
	}
	if (ret < 0)
		perror("ioctl");
	close(fd);
	return ret < 0 ? 1 : 0;
}
