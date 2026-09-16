/**
 * @file
 * @brief Creates file /dev/urandom
 *
 * @date 24.05.24
 * @author Anton Bondarev
 *
 * WHAT THIS IS, said plainly because the name promises otherwise: a linear
 * congruential generator stirred with the system clock. It is not a CSPRNG and
 * there is no entropy pool behind it. It is enough for a hash seed or a stack
 * cookie, and it is not enough for a key.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <drivers/char_dev.h>
#include <hal/ipl.h>
#include <kernel/spinlock.h>

/* The state is shared by every reader, so two of them on two cores must not be
 * able to interleave into the same bytes -- which for a generator whose whole
 * job is to hand out values nobody else got is the one thing that must hold. */
static uint32_t rand_seed = 0xdeadbeaf;
static spinlock_t rand_lock = SPIN_STATIC_UNLOCKED;

static ssize_t urandom_read(struct char_dev *cdev, void *buf, size_t nbyte,
    int flags) {
	size_t i;
	ipl_t ipl;

	ipl = spin_lock_ipl(&rand_lock);
	for (i = 0; i < nbyte; i += sizeof(rand_seed)) {
		size_t chunk = nbyte - i;

		rand_seed += clock();
		rand_seed >>= 4;
		rand_seed *= 0x837478;

		/* The seed is four bytes. Copying "everything that is left" out of it
		 * reads past the object on every request longer than that. */
		if (chunk > sizeof(rand_seed)) {
			chunk = sizeof(rand_seed);
		}
		memcpy((char *)buf + i, &rand_seed, chunk);
	}
	spin_unlock_ipl(&rand_lock, ipl);

	return nbyte;
}

static ssize_t urandom_write(struct char_dev *cdev, const void *buf,
    size_t nbyte, int flags) {
	return nbyte;
}

static const struct char_dev_ops urandom_cdev_ops = {
    .read = urandom_read,
    .write = urandom_write,
};

static struct char_dev urandom_cdev = CHAR_DEV_INIT("urandom", &urandom_cdev_ops);

CHAR_DEV_REGISTER(&urandom_cdev);
