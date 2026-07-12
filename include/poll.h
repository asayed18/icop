#ifndef VLC_ICLEAN_POLL_H
#define VLC_ICLEAN_POLL_H 1

#include <stddef.h>

struct pollfd {
    int   fd;
    short events;
    short revents;
};

int poll(struct pollfd *fds, unsigned long nfds, int timeout);

#endif /* VLC_ICLEAN_POLL_H */
