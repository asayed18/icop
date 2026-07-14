#ifndef ICOP_POLL_H
#define ICOP_POLL_H 1

#include <stddef.h>

struct pollfd {
    int   fd;
    short events;
    short revents;
};

int poll(struct pollfd *fds, unsigned long nfds, int timeout);

#endif /* ICOP_POLL_H */
