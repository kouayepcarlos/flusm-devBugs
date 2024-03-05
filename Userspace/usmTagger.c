#define _GNU_SOURCE

#include <dlfcn.h>

#include <sys/msg.h>
#include <linux/userfaultfd.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <poll.h>
#include <errno.h>

#include <sys/stat.h>
#include <sys/un.h>

#include <arpa/inet.h>


#include <stdint.h>
#include <sys/mman.h>
#include <string.h>

#include <fcntl.h>

#include <sys/uio.h>

#include <sys/prctl.h>
#include <signal.h>

//#include <libsyscall_intercept_hook_point.h>
#include <syscall.h>
#include <errno.h>

#include <asm/unistd.h>     


ssize_t my_uffd(int args)
{
    ssize_t ret;
    asm volatile
    (
        "syscall"
        : "=a" (ret)
        : "0"(__NR_userfaultfd), "D"(args)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/*static int
hook(long syscall_number,
			long arg0, long arg1,
			long arg2, long arg3,
			long arg4, long arg5,
			long *result) {
    my_uffd(O_CLOEXEC | O_NONBLOCK | UFFD_USM);
    intercept_hook_point=NULL;
	return 1;
}*/

static __attribute__((constructor)) void
init(void) {
	printf("Loading thingies...\n");
    my_uffd(O_CLOEXEC | O_NONBLOCK | UFFD_USM);
    //userfaultfd(O_CLOEXEC | O_NONBLOCK | UFFD_USM);
	//intercept_hook_point=&hook;
}
