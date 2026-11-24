/*
 * Serial port library, part of mm_manager.
 *
 * www.github.com/hharte/mm_manager
 *
 * Copyright (c) 2020-2023, Howard M. Harte
 */

#include <errno.h>
#include <stdio.h>  /* Standard input/output definitions */
#include <stdlib.h>
#include <stdint.h>
#include <string.h> /* String function definitions */

#include "mm_serial.h"

#ifdef _WIN32
# include <winsock2.h>
# include <ws2tcpip.h>
# define tcp_close(s) closesocket((SOCKET)(s))
# define SOCK(s) ((SOCKET)(s))
#else
# include <sys/socket.h>
# include <sys/select.h>
# include <netdb.h>
# include <netinet/in.h>
# include <netinet/tcp.h>
# define tcp_close(s) close((int)(s))
# define SOCK(s) ((int)(s))
#endif

/* "tcp:HOST:PORT" -> a connected socket, or -1. */
static intptr_t tcp_open(const char *spec) {
    char host[256];
    const char *colon;
    struct addrinfo hints, *res = NULL, *ai;
    intptr_t s = -1;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
#endif
    spec += strlen(MM_TCP_PREFIX);
    colon = strrchr(spec, ':');
    if (colon == NULL || (size_t)(colon - spec) >= sizeof(host)) return -1;
    memcpy(host, spec, colon - spec);
    host[colon - spec] = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, colon + 1, &hints, &res) != 0) return -1;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        s = (intptr_t)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == -1) continue;   /* INVALID_SOCKET is ~0 on Windows */
        if (connect(SOCK(s), ai->ai_addr, (int)ai->ai_addrlen) == 0) {
            int one = 1;
            setsockopt(SOCK(s), IPPROTO_TCP, TCP_NODELAY, (const char *)&one, (int)sizeof(one));
            break;
        }
        tcp_close(s);
        s = -1;
    }
    freeaddrinfo(res);
    return s;
}

/* Up to count bytes, waiting at most a second: 0 is a quiet line, as a
 * serial port's VTIME read; -1 is the connection gone. */
static ssize_t tcp_read(intptr_t s, void *buf, size_t count) {
    fd_set rd;
    struct timeval tv = { 1, 0 };
    int n;

    FD_ZERO(&rd);
    FD_SET(SOCK(s), &rd);
    n = select((int)s + 1, &rd, NULL, NULL, &tv);
    if (n < 0) return -1;
    if (n == 0) return 0;
    n = recv(SOCK(s), (char *)buf, (int)count, 0);
    return n > 0 ? n : -1;
}

static ssize_t tcp_write(intptr_t s, const void *buf, size_t count) {
    size_t done = 0;

    while (done < count) {
        int n = send(SOCK(s), (const char *)buf + done, (int)(count - done), 0);
        if (n <= 0) return done ? (ssize_t)done : -1;
        done += n;
    }
    return (ssize_t)done;
}

/*
 * Open serial port specified in modem_dev.
 *
 * Returns the file descriptor on success or -1 on error.
 */
mm_serial_context_t* open_serial(const char *modem_dev, FILE *logstream, FILE *bytestream) {
    int fd = -1;
    mm_serial_context_t *pserial_context;

    intptr_t sock = -1;
    int is_tcp = bytestream == NULL && strncmp(modem_dev, MM_TCP_PREFIX, strlen(MM_TCP_PREFIX)) == 0;

    if (is_tcp) {
        sock = tcp_open(modem_dev);
        if (sock < 0) return NULL;
    } else if (bytestream == NULL) {
        fd = platform_open_serial(modem_dev);
    }

    pserial_context = (mm_serial_context_t *)calloc(1, sizeof(mm_serial_context_t));

    if (pserial_context == NULL) {
        fprintf(stderr, "%s: Error allocating memory.\n", __func__);
        exit(-ENOMEM);
    }

    pserial_context->is_tcp = is_tcp;
    pserial_context->sock = sock;

    pserial_context->fd = fd;
    pserial_context->logstream  = logstream;
    pserial_context->bytestream = bytestream;

    return pserial_context;
}

int close_serial(mm_serial_context_t *pserial_context) {
    int status = -1;

    if (pserial_context != NULL) {
        status = pserial_context->is_tcp ? tcp_close(pserial_context->sock)
                                         : platform_close_serial(pserial_context->fd);
        free(pserial_context);
    }

    return status;
}

int init_serial(mm_serial_context_t *pserial_context, int baudrate) {
    int status = 0;

    if (pserial_context->is_tcp) {
        status = 0;
    } else if (pserial_context->bytestream == NULL) {
        status = platform_init_serial(pserial_context->fd, baudrate);
    }

    return status;
}

ssize_t read_serial(mm_serial_context_t *pserial_context, void *buf, size_t count, int inject_error) {
    ssize_t bytes_read = -1;

    if (pserial_context->is_tcp) {
        bytes_read = tcp_read(pserial_context->sock, buf, count);
        if (bytes_read <= 0) return bytes_read;
    } else if (pserial_context->bytestream == NULL) {
        bytes_read = platform_read_serial(pserial_context->fd, buf, count);
        if (inject_error) {
            printf("Invert RX data\n");
            /* Force an error by inverting the recevied data */
            ((uint8_t *)buf)[0] = ~((uint8_t*)buf)[0];
        }
    }
    else {
        for (size_t i = 0; i < count; i++) {
            if (feof(pserial_context->bytestream)) {
                printf("%s: Terminating due to EOF.\n", __func__);
                fflush(stdout);
                fclose(pserial_context->bytestream);
                exit(0);
            }
            else {
                char* bytep;
                char testbuf[80];

                if (fgets(testbuf, 80, pserial_context->bytestream) == NULL) {
                    break;
                }

                /* Data that came from the Millennium Terminal. */
                if ((bytep = strstr(testbuf, "RX: ")) != NULL) {
                    uint32_t filebyte;
                    if (sscanf(bytep, "RX: %x", &filebyte) != 1) {
                        fprintf(stderr, "%s: Error parsing bytestream\n", __func__);
                    }
                    ((uint8_t*)buf)[i] = filebyte & 0xFF;;
                }
            }
        }
        bytes_read = count;
    }

    if (pserial_context->logstream != NULL) {
        for (size_t i = 0; i < count; i++) {
            fprintf(pserial_context->logstream, "UART: RX: %02X\n", ((uint8_t*)buf)[i]);
        }
    }
    return bytes_read;
}

ssize_t write_serial(mm_serial_context_t *pserial_context, const void *buf, size_t count) {
    ssize_t bytes_written = count;

    if (pserial_context->logstream != NULL) {
        for (size_t i = 0; i < count; i++) {
            fprintf(pserial_context->logstream, "UART: TX: %02X\n", ((uint8_t*)buf)[i]);
        }
    }

    /* If we are using a serial port, send the data */
    if (pserial_context->is_tcp) {
        bytes_written = tcp_write(pserial_context->sock, buf, count);
    } else if (pserial_context->bytestream == NULL) {
        bytes_written = platform_write_serial(pserial_context->fd, buf, count);
    }

    return bytes_written;
}

int drain_serial(mm_serial_context_t *pserial_context) {
    int status = -1;
    if (pserial_context->is_tcp) return 0;
    if (pserial_context->bytestream == NULL) {
        status = platform_drain_serial(pserial_context->fd);
    }
    return status;
}

int flush_serial(mm_serial_context_t *pserial_context) {
    int status = -1;
    if (pserial_context->is_tcp) return 0;
    if (pserial_context->bytestream == NULL) {
        status = platform_flush_serial(pserial_context->fd);
    }
    return status;
}

int serial_set_dtr(mm_serial_context_t *pserial_context, int set) {
    int status = -1;
    if (pserial_context->is_tcp) return 0;     /* hanging up is the modem's ATH */
    if (pserial_context->bytestream == NULL) {
        status = platform_serial_set_dtr(pserial_context->fd, set);
    }
    return status;
}

int serial_get_modem_status(mm_serial_context_t* pserial_context) {
    int status = -1;
    if (pserial_context->is_tcp) return MS_RLSD_ON;     /* carrier is in-band: NO CARRIER */
    if (pserial_context->bytestream == NULL) {
        status = platform_serial_get_modem_status(pserial_context->fd);
    }
    return status;
}
