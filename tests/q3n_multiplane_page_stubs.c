#include <errno.h>

#include "qemu_3dnand_hw_multiplane.h"

int q3n_hw_mp_read_page(struct q3n *q3n, const struct q3n_mp_addr *addr,
			void *data, bool raw, struct q3n_mp_result *result)
{
	(void)q3n;
	(void)addr;
	(void)data;
	(void)raw;
	(void)result;
	return -EOPNOTSUPP;
}

int q3n_hw_mp_program_page(struct q3n *q3n, const struct q3n_mp_addr *addr,
			   const void *data, struct q3n_mp_result *result)
{
	(void)q3n;
	(void)addr;
	(void)data;
	(void)result;
	return -EOPNOTSUPP;
}

int q3n_hw_mp_read_oob(struct q3n *q3n, const struct q3n_mp_addr *addr,
			   void *oob, struct q3n_mp_result *result)
{
	(void)q3n;
	(void)addr;
	(void)oob;
	(void)result;
	return -EOPNOTSUPP;
}

int q3n_hw_mp_program_oob(struct q3n *q3n, const struct q3n_mp_addr *addr,
			  const void *oob, struct q3n_mp_result *result)
{
	(void)q3n;
	(void)addr;
	(void)oob;
	(void)result;
	return -EOPNOTSUPP;
}

int q3n_hw_mp_erase_group(struct q3n *q3n, const struct q3n_mp_addr *addr,
			  struct q3n_mp_result *result)
{
	(void)q3n;
	(void)addr;
	(void)result;
	return -EOPNOTSUPP;
}
