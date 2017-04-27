/*-
 * Copyright (c) 2017 Kaho Ng <ngkaho1234@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/types.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * Buffer size to deal with incoming and outgoing packets
 *
 * It is set to 1472 bytes as MTU (1500) - Size of IPV4 header -
 * Size of UDP header is 1472
 */
#define MAX_BUFSIZE 1472

/*
 * On failure, modify @ret to @val and jump to specified label
 */
#define ON_ERROR_PERROR_GOTO(label, cond, func, ret, val) {	\
	if (!(cond)) {	\
		fprintf(stderr, "Error happens in %s:%d\n",	\
				__func__, __LINE__);	\
		perror(#func);	\
		(ret) = (val);	\
		goto label;	\
	}	\
}

/*
 * Set a fd to non-blocking mode
 */
static int set_nonblock(int fd)
{
	int	optval = 1;

	return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &optval,
			sizeof(optval));
}

/*
 * Set SO_REUSEPORT flag on a socket fd
 */
static int set_reuseport(int fd)
{
	int	flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
		return -1;
	if(-1 == fcntl(fd, F_SETFL, flags | O_NONBLOCK))
		return -1;

	return 0;
}

int main()
{
	int			epfd;
	int			rc;
	int			ret;
	int			sockfd;
	struct sockaddr_in	socksaddr;
	struct sockaddr		insaddr;
	socklen_t		insaddrlen;
	struct epoll_event	ev;
	struct epoll_event	events;
	void			*buf;
	void			*bufptr;
	size_t			datasize;

	epfd = -1;
	ret = EXIT_SUCCESS;
	buf = malloc(MAX_BUFSIZE);
	if (!buf) {
		fprintf(stderr, "Failed to allocate buffer with size %d!\n",
				MAX_BUFSIZE);
		return EXIT_FAILURE;
	}

	/*
	 * Create a UDP socket for bind() later on
	 */
	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	ON_ERROR_PERROR_GOTO(out, sockfd >= 0, sockfd, ret, EXIT_FAILURE);

	/*
	 * Set REUSEPORT flag on the socket
	 */
	rc = set_reuseport(sockfd);
	ON_ERROR_PERROR_GOTO(out, rc == 0, sockfd, ret, EXIT_FAILURE);

	socksaddr.sin_family = AF_INET;
	socksaddr.sin_port = htons(5123);
	socksaddr.sin_addr.s_addr = inet_addr("127.0.0.1");

	/*
	 * Bind the socket to the address specified by @socksaddr
	 */
	rc = bind(sockfd, (struct sockaddr *)&socksaddr,
			sizeof(struct sockaddr_in));
	ON_ERROR_PERROR_GOTO(out, rc >= 0, bind, ret, EXIT_FAILURE);

	rc = set_nonblock(sockfd);
	ON_ERROR_PERROR_GOTO(out, rc == 0, bind, ret, EXIT_FAILURE);

	/*
	 * Create an epoll file descriptor
	 */
	epfd = epoll_create1(0);
	ON_ERROR_PERROR_GOTO(out, epfd >= 0, epoll_create1, ret, EXIT_FAILURE);

	/*
	 * Add the socket into readable watcher list.
	 *
	 * As we use EPOLLONESHOT here, we need to re-arm the watcher everytime
	 * epoll_wait() returns successfully
	 */

	ev.events = EPOLLIN | EPOLLONESHOT;
	rc = epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);
	ON_ERROR_PERROR_GOTO(out, rc == 0, epoll_ctl, ret, EXIT_FAILURE);

	/*
	 * Now we start our event loop
	 */
	while (1) {
		rc = epoll_wait(epfd, &events, 1, -1);
		if (rc < 0) {
			if (errno != EINTR)
				ON_ERROR_PERROR_GOTO(out, rc >= 0, epoll_wait,
						ret, EXIT_FAILURE);
			/*
			 * In case we receive a signal, we retry.
			 */
			continue;
		}

		if (events.events & EPOLLIN) {
			ssize_t		nread;

			insaddrlen = sizeof(struct sockaddr);

			/*
			 * Receive data any clients sent to us
			 */
			nread = recvfrom(sockfd, buf, MAX_BUFSIZE, 0,
					&insaddr, &insaddrlen);
			if (nread < 0) {
				if (errno != EINTR && errno != EAGAIN)
					ON_ERROR_PERROR_GOTO(out,
						nread >= 0, recvfrom,
						ret, EXIT_FAILURE);

				/*
				 * Actually it is quite awkward for EAGAIN to
				 * happen...
				 *
				 * Anyway, Re-arm the readable watcher again
				 */
				ev.events = EPOLLIN | EPOLLONESHOT;
			} else {
				datasize = nread;
				bufptr = buf;

				/*
				 * We want to arm the writable watcher to
				 * send the data we received just now
				 */
				ev.events = EPOLLOUT | EPOLLONESHOT;
			}
		} else if (events.events & EPOLLOUT) {
			ssize_t		nwritten;

			/*
			 * Send data back to the clients
			 */
			nwritten = sendto(sockfd, bufptr, datasize, 0,
					&insaddr, insaddrlen);
			if (nwritten < 0) {
				if (errno != EINTR && errno != EAGAIN)
					ON_ERROR_PERROR_GOTO(out,
						nwritten >= 0, sendto,
						ret, EXIT_FAILURE);
			} else {
				datasize -= nwritten;
				bufptr += nwritten;
			}

			if (!datasize)
				ev.events = EPOLLIN | EPOLLONESHOT;
			else
				ev.events = EPOLLOUT | EPOLLONESHOT;
		}

		/*
		 * Watchers arming...
		 */
		rc = epoll_ctl(epfd, EPOLL_CTL_MOD, sockfd, &ev);
		ON_ERROR_PERROR_GOTO(out, rc == 0, epoll_ctl, ret, EXIT_FAILURE);
	}
out:
	if (epfd >= 0)
		close(epfd);
	if (sockfd >= 0)
		close(sockfd);
	free(buf);
	return ret;
}
