
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <linux/if.h>
#include <sys/ioctl.h>
#include <linux/if_tun.h>
#include <teavpn2/common.h>
#include <teavpn2/lib/string.h>
#include <teavpn2/net/linux/iface.h>


/* https://www.kernel.org/doc/Documentation/networking/tuntap.txt
 *
 * Flags: IFF_TUN   - TUN device (no Ethernet headers)
 *        IFF_TAP   - TAP device
 *
 *        IFF_NO_PI - Do not provide packet information
 *        IFF_MULTI_QUEUE - Create a queue of multiqueue device
 */
int tun_alloc(const char *dev, short flags)
{
	int fd;
	int err;
	struct ifreq ifr;

	if (unlikely((dev == NULL) || (*dev == '\0'))) {
		pr_error("tun_alloc(): dev cannot be empty");
		return -EINVAL;
	}

	fd = open("/dev/net/tun", O_RDWR);
	if (unlikely(fd < 0)) {
		err = errno;
		pr_error("open(\"/dev/net/tun\", O_RDWR): " PRERF, PREAR(err));
		return -err;
	}

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);
	trim_cpy(ifr.ifr_name);
	ifr.ifr_flags = flags;

	if (unlikely(ioctl(fd, TUNSETIFF, &ifr) < 0)) {
		err = errno;
		pr_error("ioctl(%d, TUNSETIFF, &ifr): " PRERF, fd, PREAR(err));
		return -err;
	}

	return fd;
}


int fd_set_nonblock(int fd)
{
	int err;
	int flags;

	/* If we have O_NONBLOCK, use the POSIX way to do it */
#if defined(O_NONBLOCK)
	/*
	 * Fixme:
	 * O_NONBLOCK is defined but broken on SunOS 4.1.x and AIX 3.2.5.
	 */

	flags = fcntl(fd, F_GETFL, 0);
	if (unlikely(flags < 0)) {
		err = errno;
		pr_error("fcntl(%d, F_GETFL, 0): " PRERF, fd, PREAR(err));
		return -err;
	}
	
	flags = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	if (unlikely(flags < 0)) {
		err = errno;
		pr_error("fcntl(%d, F_SETFL, %d): " PRERF, fd, flags,
			 PREAR(err));
		return -err;
	}

	return flags;
#else
	/* Otherwise, use the old way of doing it */
	flags = 1;
	if (ioctl(fd, FIONBIO, &flags) < 0) {
		err = errno;
		pr_error("ioctl(%d, FIONBIO, &flags): " PRERF, fd, PREAR(err));
		return -err;
	}

	return 0;
#endif
}

