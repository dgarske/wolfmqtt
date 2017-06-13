/* mqttnet.c
 *
 * Copyright (C) 2006-2016 wolfSSL Inc.
 *
 * This file is part of wolfMQTT.
 *
 * wolfMQTT is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfMQTT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */

/* Include the autoconf generated config.h */
#ifdef HAVE_CONFIG_H
    #include <config.h>
#endif

#include "wolfmqtt/mqtt_client.h"
#include "examples/mqttnet.h"
#include "examples/mqttexample.h"

/* FreeRTOS and LWIP */
#ifdef FREERTOS
    /* Scheduler includes. */
    #include "FreeRTOS.h"
    #include "task.h"
    #include "semphr.h"

    /* lwIP includes. */
    #include "lwip/api.h"
    #include "lwip/tcpip.h"
    #include "lwip/memp.h"
    #include "lwip/stats.h"
    #include "lwip/sockets.h"
    #include "lwip/netdb.h"

/* Windows */
#elif defined(USE_WINDOWS_API)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <stdio.h>
    #define SOCKET_T        SOCKET
    #define SOERROR_T       char
    #define SELECT_FD(fd)   (fd)
    #define SOCKET_INVALID  ((SOCKET_T)INVALID_SOCKET)
    #define SOCK_CLOSE      closesocket
    #define SOCK_SEND(s,b,l,f) send((s), (const char*)(b), (size_t)(l), (f))
    #define SOCK_RECV(s,b,l,f) recv((s), (char*)(b), (size_t)(l), (f))

/* Freescale MQX / RTCS */
#elif defined(FREESCALE_MQX) || defined(FREESCALE_KSDK_MQX)
    #if defined(FREESCALE_MQX)
        #include <posix.h>
    #endif
    #include <rtcs.h>
    /* Note: Use "RTCS_geterror(sock->fd);" to get error number */

/* Microchip MPLABX Harmony, TCP/IP */
#elif defined(MICROCHIP_MPLAB_HARMONY)
    #include "app.h"
    #include "wolfmqtt/mqtt_client.h"
    #include "mqttnet.h"

    #include "system_config.h"
    #include "tcpip/tcpip.h"
    #include <sys/errno.h>
    #include <errno.h>
    struct timeval {
        int tv_sec;
        int tv_usec;
    };

    #define SOCKET_INVALID (-1)
    #define SO_ERROR 0
    #define SOERROR_T uint8_t
    #undef  FD_ISSET
    #define FD_ISSET(f1, f2) (1==1)

/* Nucleus */
#elif defined(NUCLEUS)
    #include "nucleus.h"
    #include "networking/nu_networking.h"

    /* If the Nucleus POSIX layer is not enabled, then map to NU_ functions */
    #ifndef CFG_NU_OS_SVCS_POSIX_ENABLE
        #define SOCK_CONNECT    NU_Connect
        #define SOCK_SEND(s,b,l,f) NU_Send((s), (CHAR*)(b), (UINT16)(l), (INT16)(f))
        #define SOCK_RECV(s,b,l,f) NU_Recv((s), (CHAR*)(b), (UINT16)(l), (INT16)(f))
        #define SOCK_CLOSE      NU_Close_Socket
        #define ADDRINFO        struct addr_struct
        #define SOCKADDR        struct addr_struct
        #define SOCKADDR_IN     struct addr_struct
        #define SOCK_STREAM     NU_TYPE_STREAM
    #else
        #include "sys/types.h"
        #include "sys/time.h"
        #include "errno.h"
        #include "signal.h"
    #endif /* CFG_NU_OS_SVCS_POSIX_ENABLE */
    #define WOLFMQTT_NO_TIMEOUT
    #define NO_SOCK_ERROR
    #define SOCKET_INVALID  ((SOCKET_T)-1)

    #undef fd_set
    #undef FD_ZERO
    #undef FD_SET
    #undef FD_ISSET
    #define fd_set              FD_SET
    #define FD_ZERO(set)        NU_FD_Init(set)
    #define FD_SET(fd, set)     NU_FD_Set(fd, set)
    #define FD_ISSET(fd, set)   NU_FD_Check(fd, set)

/* Linux */
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/param.h>
    #include <sys/time.h>
    #include <sys/select.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <signal.h>
#endif

/* Setup defaults */
#ifndef SOCKET_T
    #define SOCKET_T        int
#endif
#ifndef SOERROR_T
    #define SOERROR_T       int
#endif
#ifndef SELECT_FD
    #define SELECT_FD(fd)   ((fd) + 1)
#endif
#ifndef SOCKET_INVALID
    #define SOCKET_INVALID  ((SOCKET_T)0)
#endif
#ifndef ADDRINFO
    #define ADDRINFO        struct addrinfo
#endif
#ifndef SOCKADDR
    #define SOCKADDR        struct sockaddr
#endif
#ifndef SOCKADDR_IN
    #define SOCKADDR_IN     struct sockaddr_in
#endif
#ifndef SOCK_CONNECT
    #define SOCK_CONNECT    connect
#endif
#ifndef SOCK_SEND
    #define SOCK_SEND(s,b,l,f) send((s), (b), (size_t)(l), (f))
#endif
#ifndef SOCK_RECV
    #define SOCK_RECV(s,b,l,f) recv((s), (b), (size_t)(l), (f))
#endif
#ifndef SOCK_CLOSE
    #define SOCK_CLOSE      close
#endif


/* Include the example code */
#include "examples/mqttexample.h"

/* Local context for Net callbacks */
typedef enum {
    SOCK_BEGIN = 0,
    SOCK_CONN,
} NB_Stat;

typedef struct _SocketContext {
    SOCKET_T fd;
    NB_Stat stat;
    int bytes;
    struct sockaddr_in addr;
} SocketContext;

/* Private functions */
#ifndef WOLFMQTT_NO_TIMEOUT
static void setup_timeout(struct timeval* tv, int timeout_ms)
{
    tv->tv_sec = timeout_ms / 1000;
    tv->tv_usec = (timeout_ms % 1000) * 1000;

    /* Make sure there is a minimum value specified */
    if (tv->tv_sec < 0 || (tv->tv_sec == 0 && tv->tv_usec <= 0)) {
        tv->tv_sec = 0;
        tv->tv_usec = 100;
    }
}
#endif /* !WOLFMQTT_NO_TIMEOUT */

static int sock_get_addrinfo(SOCKADDR_IN* addr, const char* host, word16 port)
{
    int rc;
#if defined(NUCLEUS)
    NU_HOSTENT *hentry;
    INT family;
    CHAR tmp_ip[MAX_ADDRESS_SIZE] = {0};
#elif defined(MICROCHIP_MPLAB_HARMONY)
    struct hostent *hostInfo;
#else
    ADDRINFO *result = NULL;
    ADDRINFO hints;
#endif

    XMEMSET(addr, 0, sizeof(SOCKADDR_IN));

#if defined(NUCLEUS)
    /* Determine the IP address of the foreign server to which to
     * make the connection.
     */
#if (INCLUDE_IPV6 == NU_TRUE)
    /* Search for a ':' to determine if the address is IPv4 or IPv6. */
    if (XMEMCHR(host, (int)':', MAX_ADDRESS_SIZE) != NU_NULL) {
        family = NU_FAMILY_IP6;
    }
    else
#endif
    {
#if (INCLUDE_IPV4 == NU_FALSE)
        /* An IPv6 address was not passed into the routine. */
        return -1;
#else
        family = NU_FAMILY_IP;
#endif
    }

    /* Convert the string to an array. */
    rc = NU_Inet_PTON(family, (char*)host, tmp_ip);

    /* If the URI contains an IP address, copy it into the server structure. */
    if (rc == NU_SUCCESS) {
        XMEMCPY(addr->id.is_ip_addrs, tmp_ip, MAX_ADDRESS_SIZE);
    }

    /* If the application did not pass in an IP address, resolve the host
     * name into a valid IP address.
     */
    else {
        /* If IPv6 is enabled, default to IPv6.  If the host does not have
         * an IPv6 address, an IPv4-mapped IPv6 address will be returned that
         * can be used as an IPv6 address.
         */
#if (INCLUDE_IPV6 == NU_TRUE)
        family = NU_FAMILY_IP6;
#else
        family = NU_FAMILY_IP;
#endif

        /* Try getting host info by name */
        hentry = NU_Get_IP_Node_By_Name((char*)host, family, DNS_V4MAPPED, &rc);
        if (hentry) {
            /* Copy the hentry data into the server structure */
            memcpy(addr->id.is_ip_addrs, *hentry->h_addr_list, hentry->h_length);
            family = hentry->h_addrtype;

            /* Free the memory associated with the host entry returned */
            NU_Free_Host_Entry(hentry);
        }

        /* If the host name could not be resolved, return an error. */
        else {
            return -1;
        }
    }

    addr->family = family;
    addr->port = port;

#elif defined(MICROCHIP_MPLAB_HARMONY)
    hostInfo = gethostbyname((char *)host);
    if (hostInfo != NULL) {
        addr->sin_port = port; /* htons(port); */
        addr->sin_family = AF_INET;
        XMEMCPY(&addr->sin_addr.S_un,
                    *(hostInfo->h_addr_list), sizeof(IPV4_ADDR));
    }
    else {
        return MQTT_CODE_CONTINUE;
    }
#else
    XMEMSET(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    rc = getaddrinfo(host, NULL, &hints, &result);
    if (rc >= 0 && result != NULL) {
        struct addrinfo* res = result;

        /* prefer ip4 addresses */
        while (res) {
            if (res->ai_family == AF_INET) {
                result = res;
                break;
            }
            res = res->ai_next;
        }

        if (result->ai_family == AF_INET) {
            addr->sin_port = htons(port);
            addr->sin_family = AF_INET;
            addr->sin_addr =
                ((struct sockaddr_in*)(result->ai_addr))->sin_addr;
        }
        else {
            rc = -1;
        }

        freeaddrinfo(result);
    }
#endif

    return rc;
}

#if !defined(WOLFMQTT_NO_TIMEOUT) && defined(WOLFMQTT_NONBLOCK)
static void sock_set_nonblocking(SOCKET_T* sockfd)
{
    int rc;
#ifdef USE_WINDOWS_API
    unsigned long blocking = 1;
    rc = ioctlsocket(*sockfd, FIONBIO, &blocking);
    if (rc == SOCKET_ERROR)
        PRINTF("ioctlsocket failed!");
#elif defined(MICROCHIP_MPLAB_HARMONY)
    /* Do nothing */
#elif defined(NUCLEUS)
    rc = NU_Fcntl(*sockfd, NU_SETFLAG, NU_NO_BLOCK);
    if (rc != NU_SUCCESS) {
        PRINTF("NU_Fcntl set NO_BLOCK failed!");
    }
#else
    rc = fcntl(*sockfd, F_GETFL, 0);
    if (rc < 0)
        PRINTF("fcntl get failed!");
    rc = fcntl(*sockfd, F_SETFL, rc | O_NONBLOCK);
    if (rc < 0)
        PRINTF("fcntl set failed!");
#endif
}
#endif /* !WOLFMQTT_NO_TIMEOUT && WOLFMQTT_NONBLOCK */

#ifndef WOLFMQTT_NO_TIMEOUT
static int sock_select(int nfds, fd_set *readfds, fd_set *writefds,
        fd_set *exceptfds, struct timeval *timeout)
{
    int rc;
#if defined(NUCLEUS)
    UNSIGNED nu_timeout;

    if (timeout) {
        nu_timeout = (timeout->tv_sec * NU_PLUS_TICKS_PER_SEC) +
                ((timeout->tv_usec * NU_PLUS_TICKS_PER_SEC) / 1000000);
    } else {
        nu_timeout = NU_SUSPEND;
    }

    /* Exception FD's are not used on Nucleus */
    if (exceptfds) {
        FD_ZERO(exceptfds);
        exceptfds = NULL;
    }

    rc = NU_Select(nfds, readfds, writefds, exceptfds, nu_timeout);

    /* On failure, clear all the descriptor sets to make the behavior
     * consistent with the *NIX implementation. */
    if (rc != NU_SUCCESS) {
        if (readfds) {
            FD_ZERO(readfds);
        }
        if (writefds) {
            FD_ZERO(writefds);
        }

        /* If the error is due to no sockets being specified, then sleep
         * for the specified duration to be consistent with the *NIX
         * implementation. */
        if (rc == NU_NO_SOCKETS) {
            if (nu_timeout != 0) {
                NU_Sleep(nu_timeout);
            }
            rc = 0;
        }
        else if (rc == NU_NO_DATA) {
            rc = MQTT_CODE_ERROR_TIMEOUT;
        }
    }
    else {
        rc = 1; /* For compatibility return 1 on success */
    }
#else
    rc = select(nfds, readfds, writefds, exceptfds, timeout);
#endif /* NUCLEUS */
    return rc;
}
#endif /* !WOLFMQTT_NO_TIMEOUT */

static int sock_create(SOCKADDR_IN* addr, int type, int protocol)
{
    int rc;

    /* Create socket */
#if defined(NUCLEUS)
    rc = NU_Socket(addr->family, type, protocol);
#elif defined(MICROCHIP_MPLAB_HARMONY)
    rc = socket(AF_INET, type, IPPROTO_TCP);
    (void)protocol;
#else
    rc = socket(addr->sin_family, type, protocol);
#endif
    return rc;
}

static int NetConnect(void *context, const char* host, word16 port,
    int timeout_ms)
{
    SocketContext *sock = (SocketContext*)context;
    SOCKADDR_IN address;
    int rc;

    /* Get address information for host and locate IPv4 */
    switch(sock->stat) {
        case SOCK_BEGIN:
        {
            rc = sock_get_addrinfo(&address, host, port);
            if (rc != 0) goto exit;

            /* Default to error */
            rc = -1;

            /* Create socket */
            sock->fd = sock_create(&address, SOCK_STREAM, 0);
            if (sock->fd <= SOCKET_INVALID)
                goto exit;

            sock->stat = SOCK_CONN;
            /* fall-through */
        }

        case SOCK_CONN:
        {
        #ifndef WOLFMQTT_NO_TIMEOUT
            fd_set fdset;
            struct timeval tv;

            /* Setup timeout and FD's */
            setup_timeout(&tv, timeout_ms);
            FD_ZERO(&fdset);
            FD_SET(sock->fd, &fdset);
        #endif /* !WOLFMQTT_NO_TIMEOUT */

        #if !defined(WOLFMQTT_NO_TIMEOUT) && defined(WOLFMQTT_NONBLOCK)
            /* Set socket as non-blocking */
            sock_set_nonblocking(&sock->fd);
        #endif

            /* Start connect */
            rc = SOCK_CONNECT(sock->fd, (SOCKADDR*)&address, sizeof(address));

#if defined(MICROCHIP_MPLAB_HARMONY)
            if (rc)
#elif !defined(WOLFMQTT_NO_TIMEOUT)
            /* Wait for connect */
            if (rc < 0 || sock_select((int)SELECT_FD(sock->fd), NULL, &fdset, NULL, &tv) > 0)
#else
            if (rc < 0)
#endif
            {
        #ifndef NO_SOCK_ERROR
                SOERROR_T so_error = 0;
                socklen_t len = sizeof(so_error);
            #if defined(MICROCHIP_MPLAB_HARMONY)
                if (errno == EINPROGRESS) {
                    return MQTT_CODE_CONTINUE;
                }
            #else
                /* Check for error */
                getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
                if (so_error == 0) {
                    rc = 0; /* Success */
                }
            #endif
        #else
                rc = 0;
        #endif /* NO_SOCK_ERROR */
            }
            break;
        }

        default:
            rc = -1;
    } /* switch */

    (void)timeout_ms;

exit:

    /* Show error */
    if (rc != 0) {
        PRINTF("NetConnect: Rc=%d", rc);
    }

    return rc;
}

static int NetWrite(void *context, const byte* buf, int buf_len,
    int timeout_ms)
{
    SocketContext *sock = (SocketContext*)context;
    int rc;
#ifndef WOLFMQTT_NO_TIMEOUT
    struct timeval tv;
#endif

    if (context == NULL || buf == NULL || buf_len <= 0) {
        return MQTT_CODE_ERROR_BAD_ARG;
    }

#ifndef WOLFMQTT_NO_TIMEOUT
    /* Setup timeout */
    setup_timeout(&tv, timeout_ms);
    setsockopt(sock->fd, SOL_SOCKET, SO_SNDTIMEO, (char *)&tv, sizeof(tv));
#endif

    rc = (int)SOCK_SEND(sock->fd, buf, buf_len, 0);
    if (rc == -1) {
#ifndef NO_SOCK_ERROR
        /* Get error */
        SOERROR_T so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (so_error == 0) {
            rc = 0; /* Handle signal */
        }
        else {
            rc = MQTT_CODE_ERROR_NETWORK;
            PRINTF("NetWrite: Error %d", so_error);
        }
#endif
    }

    (void)timeout_ms;

    return rc;
}

static int NetRead(void *context, byte* buf, int buf_len,
    int timeout_ms)
{
    SocketContext *sock = (SocketContext*)context;
    int rc = -1, timeout = 0;
#if !defined(WOLFMQTT_NO_TIMEOUT) && !defined(WOLFMQTT_NONBLOCK)
    fd_set recvfds;
    fd_set errfds;
    struct timeval tv;
#endif

    if (context == NULL || buf == NULL || buf_len <= 0) {
        return MQTT_CODE_ERROR_BAD_ARG;
    }

    sock->bytes = 0;

#if !defined(WOLFMQTT_NO_TIMEOUT) && !defined(WOLFMQTT_NONBLOCK)
    /* Setup timeout and FD's */
    setup_timeout(&tv, timeout_ms);
    FD_ZERO(&recvfds);
    FD_SET(sock->fd, &recvfds);
    FD_ZERO(&errfds);
    FD_SET(sock->fd, &errfds);

    #ifdef WOLFMQTT_ENABLE_STDIN_CAP
        FD_SET(STDIN, &recvfds);
    #endif

#else
    (void)timeout_ms;
#endif /* !WOLFMQTT_NO_TIMEOUT && !WOLFMQTT_NONBLOCK */

    /* Loop until buf_len has been read, error or timeout */
    while (sock->bytes < buf_len) {

    #if !defined(WOLFMQTT_NO_TIMEOUT) && !defined(WOLFMQTT_NONBLOCK)
        /* Wait for rx data to be available */
        rc = sock_select((int)SELECT_FD(sock->fd), &recvfds, NULL, &errfds, &tv);
        if (rc > 0) {
            /* Check if rx or error */
            if (FD_ISSET(sock->fd, &recvfds)) {
    #endif /* !WOLFMQTT_NO_TIMEOUT && !WOLFMQTT_NONBLOCK */

                /* Try and read number of buf_len provided,
                    minus what's already been read */
                rc = (int)SOCK_RECV(sock->fd,
                               &buf[sock->bytes],
                               buf_len - sock->bytes,
                               0);
                if (rc <= 0) {
                    rc = -1;
                    goto exit; /* Error */
                }
                else {
                    sock->bytes += rc; /* Data */
                }

    #if !defined(WOLFMQTT_NO_TIMEOUT) && !defined(WOLFMQTT_NONBLOCK)
            }
        #ifdef WOLFMQTT_ENABLE_STDIN_CAP
            else if (FD_ISSET(STDIN, &recvfds)) {
                return MQTT_CODE_STDIN_WAKE;
            }
        #endif
            if (FD_ISSET(sock->fd, &errfds)) {
                rc = -1;
                break;
            }
        }
        else {
            timeout = 1;
            break; /* timeout or signal */
        }
    #else
        /* non-blocking should always exit loop */
        break;
    #endif /* !WOLFMQTT_NO_TIMEOUT && !WOLFMQTT_NONBLOCK */
    } /* while */

exit:

    if (rc == 0 && timeout) {
        rc = MQTT_CODE_ERROR_TIMEOUT;
    }
    else if (rc < 0) {
    #ifndef NO_SOCK_ERROR
        /* Get error */
        SOERROR_T so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(sock->fd, SOL_SOCKET, SO_ERROR, &so_error, &len);

        if (so_error == 0) {
            rc = 0; /* Handle signal */
        }
        else {
        #ifdef WOLFMQTT_NONBLOCK
            if (so_error == EWOULDBLOCK) {
                return MQTT_CODE_CONTINUE;
            }
        #endif
            rc = MQTT_CODE_ERROR_NETWORK;
            PRINTF("NetRead: Error %d", so_error);
        }
#endif
    }
    else {
        rc = sock->bytes;
    }
    sock->bytes = 0;

    return rc;
}

static int NetDisconnect(void *context)
{
    SocketContext *sock = (SocketContext*)context;
    if (sock) {
        if (sock->fd != SOCKET_INVALID) {
            SOCK_CLOSE(sock->fd);
            sock->fd = -1;
        }

        sock->stat = SOCK_BEGIN;
        sock->bytes = 0;
    }
    return 0;
}

/* Public Functions */
int MqttClientNet_Init(MqttNet* net)
{
#ifdef USE_WINDOWS_API
    WSADATA wsd;
    WSAStartup(0x0002, &wsd);
#endif

#ifdef MICROCHIP_MPLAB_HARMONY
    static IPV4_ADDR    dwLastIP[2] = { {-1}, {-1} };
    IPV4_ADDR           ipAddr;
    int Dummy;
    int nNets;
    int i;
    SYS_STATUS          stat;
    TCPIP_NET_HANDLE    netH;

    stat = TCPIP_STACK_Status(sysObj.tcpip);
    if (stat < 0) {
        return MQTT_CODE_CONTINUE;
    }

    nNets = TCPIP_STACK_NumberOfNetworksGet();
    for (i = 0; i < nNets; i++) {
        netH = TCPIP_STACK_IndexToNet(i);
        ipAddr.Val = TCPIP_STACK_NetAddress(netH);
        if (ipAddr.v[0] == 0) {
            return MQTT_CODE_CONTINUE;
        }
        if (dwLastIP[i].Val != ipAddr.Val) {
            dwLastIP[i].Val = ipAddr.Val;
            PRINTF("%s", TCPIP_STACK_NetNameGet(netH));
            PRINTF(" IP Address: ");
            PRINTF("%d.%d.%d.%d\n", ipAddr.v[0], ipAddr.v[1], ipAddr.v[2], ipAddr.v[3]);
        }
    }
#endif /* MICROCHIP_MPLAB_HARMONY */

    if (net) {
        XMEMSET(net, 0, sizeof(MqttNet));
        net->connect = NetConnect;
        net->read = NetRead;
        net->write = NetWrite;
        net->disconnect = NetDisconnect;
        net->context = (SocketContext *)WOLFMQTT_MALLOC(sizeof(SocketContext));
        if (net->context == NULL) {
            return MQTT_CODE_ERROR_MEMORY;
        }
        XMEMSET(net->context, 0, sizeof(SocketContext));

        ((SocketContext*)(net->context))->stat = SOCK_BEGIN;
    }

    return MQTT_CODE_SUCCESS;
}

int MqttClientNet_DeInit(MqttNet* net)
{
    if (net) {
        if (net->context) {
            WOLFMQTT_FREE(net->context);
        }
        XMEMSET(net, 0, sizeof(MqttNet));
    }
    return 0;
}
