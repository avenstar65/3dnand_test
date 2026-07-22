#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-user.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s get|set DEVICE OFFSET\n"
		"       %s oob-read MODE DEVICE PAGE_OFFSET OOB_OFFSET LENGTH\n"
		"       %s oob-read-unchecked MODE DEVICE PAGE_OFFSET OOB_OFFSET LENGTH\n"
		"       %s oob-write MODE DEVICE PAGE_OFFSET OOB_OFFSET LENGTH BYTE\n"
		"       %s page-read|page-write MODE DEVICE PAGE_OFFSET DATA_BYTE OOB_OFFSET OOB_LENGTH OOB_BYTE\n"
		"       %s span-read|span-write MODE DEVICE PAGE_OFFSET PAGE_COUNT OOB_OFFSET OOB_BYTE\n"
		"MODE is place or raw; page OOB commands never wrap to another page.\n",
		prog, prog, prog, prog, prog, prog);
	return 2;
}

static int parse_u64(const char *arg, uint64_t *value)
{
	unsigned long long parsed;
	char *end;

	if (*arg == '-')
		return -1;
	errno = 0;
	parsed = strtoull(arg, &end, 0);
	if (errno || *arg == '\0' || *end != '\0')
		return -1;
	*value = parsed;
	return 0;
}

static int parse_byte(const char *arg, unsigned char *value)
{
	uint64_t parsed;

	if (parse_u64(arg, &parsed) || parsed > 0xff)
		return -1;
	*value = parsed;
	return 0;
}

static int parse_mode(const char *arg, uint8_t *mode)
{
	if (!strcmp(arg, "place")) {
		*mode = MTD_OPS_PLACE_OOB;
		return 0;
	}
	if (!strcmp(arg, "raw")) {
		*mode = MTD_OPS_RAW;
		return 0;
	}
	return -1;
}

static int get_info(int fd, struct mtd_info_user *info)
{
	memset(info, 0, sizeof(*info));
	return ioctl(fd, MEMGETINFO, info);
}

static int validate_page_oob(const struct mtd_info_user *info,
			     uint64_t page_offset, uint64_t oob_offset,
			     uint64_t length)
{
	if (!info->writesize || !info->oobsize ||
	    page_offset % info->writesize || !length ||
	    oob_offset >= info->oobsize || length > info->oobsize - oob_offset) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int set_oob_file_mode(int fd, uint8_t mode)
{
	if (mode != MTD_OPS_RAW)
		return 0;
	return ioctl(fd, MTDFILEMODE, MTD_FILE_MODE_RAW);
}

static int oob_io(int fd, int write, uint8_t mode,
		  const struct mtd_info_user *info, uint64_t page_offset,
		  uint64_t oob_offset, uint64_t length, unsigned char value,
		  int validate_logical_bounds)
{
	struct mtd_oob_buf64 req;
	unsigned char *buf;
	uint64_t i;
	int ret;

	if ((validate_logical_bounds &&
	     validate_page_oob(info, page_offset, oob_offset, length)) ||
	    (!validate_logical_bounds &&
	     (!info->writesize || !info->oobsize ||
	      page_offset % info->writesize || !length || length > 4096)) ||
	    page_offset + oob_offset < page_offset)
		return -1;
	buf = malloc(length);
	if (!buf)
		return -1;
	memset(buf, value, length);
	memset(&req, 0, sizeof(req));
	req.start = page_offset + oob_offset;
	req.length = length;
	req.usr_ptr = (uintptr_t)buf;
	ret = set_oob_file_mode(fd, mode);
	if (!ret)
		ret = ioctl(fd, write ? MEMWRITEOOB64 : MEMREADOOB64, &req);
	if (!ret && req.length != length) {
		errno = EIO;
		ret = -1;
	}
	if (!ret && !write) {
		for (i = 0; i < length; i++)
			printf("%02x", buf[i]);
		putchar('\n');
	}
	free(buf);
	return ret;
}

static int bytes_are(const unsigned char *buf, size_t length,
		     unsigned char expected)
{
	size_t i;

	for (i = 0; i < length; i++)
		if (buf[i] != expected)
			return 0;
	return 1;
}

static int page_io(int fd, int write, uint8_t mode,
		   const struct mtd_info_user *info, uint64_t page_offset,
		   unsigned char data_value, uint64_t oob_offset,
		   uint64_t oob_length, unsigned char oob_value)
{
	unsigned char *data;
	unsigned char *oob;
	int ret;

	if (validate_page_oob(info, page_offset, oob_offset, oob_length))
		return -1;
	data = malloc(info->writesize);
	oob = malloc(info->oobsize);
	if (!data || !oob) {
		free(data);
		free(oob);
		return -1;
	}
	memset(data, write ? data_value : 0, info->writesize);
	memset(oob, write ? 0xff : 0, info->oobsize);
	if (write)
		memset(oob + oob_offset, oob_value, oob_length);
	if (write) {
		struct mtd_write_req req = {
			.start = page_offset,
			.len = info->writesize,
			.ooblen = info->oobsize,
			.usr_data = (uintptr_t)data,
			.usr_oob = (uintptr_t)oob,
			.mode = mode,
		};

		ret = ioctl(fd, MEMWRITE, &req);
	} else {
		struct mtd_read_req req = {
			.start = page_offset,
			.len = info->writesize,
			.ooblen = info->oobsize,
			.usr_data = (uintptr_t)data,
			.usr_oob = (uintptr_t)oob,
			.mode = mode,
		};

		ret = ioctl(fd, MEMREAD, &req);
		if (!ret &&
		    (!bytes_are(data, info->writesize, data_value) ||
		     !bytes_are(oob + oob_offset, oob_length, oob_value))) {
			errno = EIO;
			ret = -1;
		}
	}
	free(data);
	free(oob);
	return ret;
}

static int span_io(int fd, int write, uint8_t mode,
		   const struct mtd_info_user *info, uint64_t page_offset,
		   uint64_t page_count, uint64_t oob_offset,
		   unsigned char oob_value)
{
	unsigned char *oob;
	size_t total;
	uint64_t page;
	int ret;

	if (!info->writesize || !info->oobsize || !page_count ||
	    page_count > 32 || page_offset % info->writesize ||
	    oob_offset >= info->oobsize ||
	    page_count > SIZE_MAX / info->oobsize) {
		errno = EINVAL;
		return -1;
	}
	total = page_count * info->oobsize;
	oob = malloc(total);
	if (!oob)
		return -1;
	memset(oob, write ? 0xff : 0, total);
	if (write)
		for (page = 0; page < page_count; page++)
			oob[page * info->oobsize + oob_offset] = oob_value;
	if (write) {
		struct mtd_write_req req = {
			.start = page_offset,
			.ooblen = total,
			.usr_oob = (uintptr_t)oob,
			.mode = mode,
		};

		ret = ioctl(fd, MEMWRITE, &req);
	} else {
		struct mtd_read_req req = {
			.start = page_offset,
			.ooblen = total,
			.usr_oob = (uintptr_t)oob,
			.mode = mode,
		};

		ret = ioctl(fd, MEMREAD, &req);
		if (!ret)
			for (page = 0; page < page_count; page++)
				if (oob[page * info->oobsize + oob_offset] !=
				    oob_value) {
					errno = EIO;
					ret = -1;
					break;
				}
	}
	free(oob);
	return ret;
}

static int run_extended(int argc, char **argv)
{
	struct mtd_info_user info;
	uint64_t page_offset;
	uint64_t first;
	uint64_t second;
	unsigned char data_value;
	unsigned char oob_value;
	uint8_t mode;
	int write;
	int unchecked;
	int fd;
	int ret;

	if (argc < 5)
		return usage(argv[0]);
	write = strstr(argv[1], "write") != NULL;
	unchecked = !strcmp(argv[1], "oob-read-unchecked");
	if (parse_mode(argv[2], &mode) || parse_u64(argv[4], &page_offset))
		return usage(argv[0]);
	fd = open(argv[3], O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	if (get_info(fd, &info)) {
		perror("MEMGETINFO");
		close(fd);
		return 1;
	}

	if (!strcmp(argv[1], "oob-read") || !strcmp(argv[1], "oob-write") ||
	    unchecked) {
		if (argc != (write ? 8 : 7) || parse_u64(argv[5], &first) ||
		    parse_u64(argv[6], &second) ||
		    (write && parse_byte(argv[7], &oob_value))) {
			close(fd);
			return usage(argv[0]);
		}
		ret = oob_io(fd, write, mode, &info, page_offset, first,
			     second, write ? oob_value : 0, !unchecked);
	} else if (!strcmp(argv[1], "page-read") ||
		   !strcmp(argv[1], "page-write")) {
		if (argc != 9 || parse_byte(argv[5], &data_value) ||
		    parse_u64(argv[6], &first) || parse_u64(argv[7], &second) ||
		    parse_byte(argv[8], &oob_value)) {
			close(fd);
			return usage(argv[0]);
		}
		ret = page_io(fd, write, mode, &info, page_offset, data_value,
			      first, second, oob_value);
	} else if (!strcmp(argv[1], "span-read") ||
		   !strcmp(argv[1], "span-write")) {
		if (argc != 8 || parse_u64(argv[5], &first) ||
		    parse_u64(argv[6], &second) ||
		    parse_byte(argv[7], &oob_value)) {
			close(fd);
			return usage(argv[0]);
		}
		ret = span_io(fd, write, mode, &info, page_offset, first,
			      second, oob_value);
	} else {
		close(fd);
		return usage(argv[0]);
	}
	if (ret)
		perror("ioctl");
	close(fd);
	return ret ? 1 : 0;
}

int main(int argc, char **argv)
{
	long long offset;
	char *end;
	int fd;
	int ret;

	if (argc >= 3 &&
	    (!strncmp(argv[1], "oob-", 4) ||
	     !strncmp(argv[1], "page-", 5) ||
	     !strncmp(argv[1], "span-", 5)))
		return run_extended(argc, argv);

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
