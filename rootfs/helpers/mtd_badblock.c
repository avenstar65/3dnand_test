#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s get|set DEVICE OFFSET\n"
		"       %s oob-read DEVICE PAGE_OFFSET OOB_OFFSET\n"
		"       %s oob-write DEVICE PAGE_OFFSET OOB_OFFSET BYTE\n",
		prog, prog, prog);
	return 2;
}

static int parse_u64(const char *arg, uint64_t *value)
{
	unsigned long long parsed;
	char *end;

	errno = 0;
	parsed = strtoull(arg, &end, 0);
	if (errno || *arg == '\0' || *end != '\0')
		return -1;
	*value = parsed;
	return 0;
}

static int oob_io(int fd, int write, uint64_t page_offset,
		  uint64_t oob_offset, unsigned char *value)
{
	struct mtd_oob_buf64 req = {
		.start = page_offset + oob_offset,
		.length = 1,
		.usr_ptr = (uintptr_t)value,
	};
	int ret;

	if (req.start < page_offset) {
		errno = EOVERFLOW;
		return -1;
	}
	ret = ioctl(fd, write ? MEMWRITEOOB64 : MEMREADOOB64, &req);
	if (!ret && req.length != 1) {
		errno = EIO;
		return -1;
	}
	return ret;
}

int main(int argc, char **argv)
{
	uint64_t page_offset;
	uint64_t oob_offset;
	long long offset;
	unsigned long byte;
	unsigned char value;
	char *end;
	int fd;
	int ret;

	if ((argc == 5 || argc == 6) &&
	    (!strcmp(argv[1], "oob-read") || !strcmp(argv[1], "oob-write"))) {
		int write = !strcmp(argv[1], "oob-write");

		if (argc != (write ? 6 : 5) ||
		    parse_u64(argv[3], &page_offset) ||
		    parse_u64(argv[4], &oob_offset))
			return usage(argv[0]);
		value = 0;
		if (write) {
			errno = 0;
			byte = strtoul(argv[5], &end, 0);
			if (errno || *argv[5] == '\0' || *end != '\0' ||
			    byte > 0xff)
				return usage(argv[0]);
			value = byte;
		}
		fd = open(argv[2], O_RDWR);
		if (fd < 0) {
			perror("open");
			return 1;
		}
		ret = oob_io(fd, write, page_offset, oob_offset, &value);
		if (!ret && !write)
			printf("%02x\n", value);
		if (ret < 0)
			perror("ioctl");
		close(fd);
		return ret < 0 ? 1 : 0;
	}

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
