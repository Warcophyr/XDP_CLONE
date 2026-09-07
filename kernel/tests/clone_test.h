#ifndef CLONE_TEST_H
#define CLONE_TEST_H

#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

/* The clone actions carry the number of copies in the upper bits of the return
 * value. Defined locally so the tests build against stock kernel headers too,
 * i.e. before the patch is installed on the system.
 */
#define ACT_CLONE_PASS	5
#define ACT_CLONE_TX	6
#define CLONE_PASS(n)	(((int)(n) << 5) | ACT_CLONE_PASS)
#define CLONE_TX(n)	(((int)(n) << 5) | ACT_CLONE_TX)

#endif
