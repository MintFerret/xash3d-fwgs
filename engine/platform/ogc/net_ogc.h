/*
net_ogc.h - Network stub for libogc

Copyright (C) 2026 mintferret

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

*/
#ifndef NET_OGC_H
#define NET_OGC_H

#include <network.h>

#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>

#include <errno.h>
#include <unistd.h>

// return the wii's ip address
static inline int ogc_net_gethostname( char *name, size_t len )
{
	struct in_addr addr;
	addr.s_addr = net_gethostip();
	strncpy( name, inet_ntoa(addr), len );

	if( len > 0 )
		name[len - 1] = '\0';

	return 0;
}

//net_socket is already defined
static inline int ogc_net_socket(int d, int t, int p)
{
    return net_socket(d, t, 0);
}


 //Note: below here is stuff adapted from the quake3 port

// mascara de red de la Wii (en orden de red), la fija net_ws.c tras if_config.
// Sirve para calcular el broadcast dirigido (192.168.x.255), porque algunos
// routers/APs no reenvian 255.255.255.255 y la lista LAN quedaba vacia.
static uint32_t ogc_net_netmask;

static inline void ogc_net_set_netmask( const char *mask )
{
	ogc_net_netmask = inet_addr( mask );
	if( ogc_net_netmask == (uint32_t)-1 )
		ogc_net_netmask = 0;
}

// libogc devuelve -errno; el motor espera -1 y errno (WSAGetLastError). Sin
// esto NET_SendPacket nunca veia el error y no se imprimia nada al fallar.
static inline int ogc_net_fix_ret( int ret )
{
	if( ret >= 0 )
		return ret;
	errno = ( ret == -1 ) ? EIO : -ret;
	return -1;
}

//build 8-byte POSIX addr for IOS (no sin_len prefix, tolen=8)
static inline int ogc_net_sendto_one(int s, const void *b, int l, int f, uint32_t addr, uint16_t port)
{
	uint8_t ios_addr[8];

	ios_addr[0] = 8;                 // sa_len
	ios_addr[1] = (uint8_t)AF_INET;  // sa_family
	memcpy(ios_addr + 2, &port, 2);
	memcpy(ios_addr + 4, &addr, 4);

	return ogc_net_fix_ret( net_sendto(s, (void *)b, l, f, (struct sockaddr *)ios_addr, 8) );
}

static inline int ogc_net_sendto(int s, const void *b, int l, int f, const struct sockaddr *a, int al)
{
	if (a && a->sa_family == AF_INET) {
		const struct sockaddr_in *sin4 = (const struct sockaddr_in *)a;
		uint32_t dst = sin4->sin_addr.s_addr;
		int ret = ogc_net_sendto_one(s, b, l, f, dst, sin4->sin_port);

		// broadcast limitado: mandar tambien el dirigido de la subred
		if (dst == 0xFFFFFFFFu && ogc_net_netmask) {
			uint32_t directed = ( (uint32_t)net_gethostip() & ogc_net_netmask ) | ~ogc_net_netmask;
			if (directed != 0xFFFFFFFFu) {
				int ret2 = ogc_net_sendto_one(s, b, l, f, directed, sin4->sin_port);
				if (ret < 0 && ret2 >= 0)
					ret = ret2;
			}
		}
		return ret;
	}

	errno = EAFNOSUPPORT;
	return -1;
}

//recvfrom: libogc deja que IOS escriba la direccion directamente en el buffer
//que le pasamos. Si ese buffer es la pila (sin alinear a 32 bytes), la
//invalidacion de cache de IPC se lleva por delante variables vecinas y
//corrompe la pila (retorno a PC=0). Usamos un buffer propio alineado.
static inline int ogc_net_recvfrom(int s, void *b, int l, int f, struct sockaddr *a, socklen_t *al)
{
	static uint8_t from_buf[32] __attribute__((aligned(32)));
	socklen_t flen = 8;
	int ret;

	memset( from_buf, 0, sizeof( from_buf ));
	from_buf[0] = 8;
	from_buf[1] = (uint8_t)AF_INET;

	ret = net_recvfrom(s, b, l, f, a ? (struct sockaddr *)from_buf : NULL, a ? &flen : NULL);
	if (ret >= 0) {
		if (a) {
			// struct sockaddr_in: len, family, port, addr (8 bytes)
			socklen_t cap = ( al && *al < sizeof( struct sockaddr_in )) ? *al : sizeof( struct sockaddr_in );
			memset( a, 0, cap );
			memcpy( a, from_buf, cap < 8 ? cap : 8 );
			if( al ) *al = sizeof( struct sockaddr_in );
		}
		return ret;
	}

	// sin datos (o cualquier error): el motor lo trata como EAGAIN
	errno = EAGAIN;
	return -1;
}

// TCP (descargas HTTP, rcon): libogc devuelve -errno y no toca errno, asi que el
// motor veia un errno viejo (por ejemplo ENOENT de un archivo, o EAGAIN que deja
// recvfrom) y cualquier connect() en curso se tomaba por un error
static inline int ogc_net_connect( int s, const struct sockaddr *a, socklen_t l )
{
    return ogc_net_fix_ret( net_connect( s, (struct sockaddr *)a, l ));
}

static inline int ogc_net_send( int s, const void *b, int l, int f )
{
    return ogc_net_fix_ret( net_send( s, b, l, f ));
}

static inline int ogc_net_recv( int s, void *b, int l, int f )
{
    return ogc_net_fix_ret( net_recv( s, b, l, f ));
}

#define ioctlsocket net_ioctl
#define closesocket net_close

#define select(n,r,w,e,t)           net_select((n),(r),(w),(e),(t))

#define gethostname(n,l)            ogc_net_gethostname((n),(l))
#define socket(d,t,p)               ogc_net_socket((d),(t),(p))
#define sendto(s,b,l,f,a,al)        ogc_net_sendto((s),(b),(l),(f),(a),(al))
#define recvfrom(s,b,l,f,a,al)      ogc_net_recvfrom((s),(b),(l),(f),(a),(al))

#define bind(s,a,l)                 net_bind((s),(a),(l))
#define fcntl(s,cmd,arg)            net_fcntl((s),(cmd),(arg))

#define connect(s,a,l)              ogc_net_connect((s),(a),(l))
#define listen(s,b)                 net_listen((s),(b))
#define accept(s,a,l)               ogc_net_fix_ret( net_accept((s),(a),(l)))
#define send(s,b,l,f)               ogc_net_send((s),(b),(l),(f))
#define recv(s,b,l,f)               ogc_net_recv((s),(b),(l),(f))
#define setsockopt(s,lv,o,v,l)      net_setsockopt((s),(lv),(o),(v),(l))
#define getsockopt(s,lv,o,v,l)      net_getsockopt((s),(lv),(o),(v),(l))
#define getsockname(s,a,l)          net_getsockname((s),(a),(l))
#define gethostbyname(n)            net_gethostbyname(n)

#define WSAGetLastError()  errno
#define WSAEINTR           EINTR
#define WSAEBADF           EBADF
#define WSAEACCES          EACCES
#define WSAEFAULT          EFAULT
#define WSAEINVAL          EINVAL
#define WSAEMFILE          EMFILE
#define WSAEWOULDBLOCK     EWOULDBLOCK
#define WSAEINPROGRESS     EINPROGRESS
#define WSAEALREADY        EALREADY
#define WSAENOTSOCK        ENOTSOCK
#define WSAEDESTADDRREQ    EDESTADDRREQ
#define WSAEMSGSIZE        EMSGSIZE
#define WSAEPROTOTYPE      EPROTOTYPE
#define WSAENOPROTOOPT     ENOPROTOOPT
#define WSAEPROTONOSUPPORT EPROTONOSUPPORT
#define WSAESOCKTNOSUPPORT ESOCKTNOSUPPORT
#define WSAEOPNOTSUPP      EOPNOTSUPP
#define WSAEPFNOSUPPORT    EPFNOSUPPORT
#define WSAEAFNOSUPPORT    EAFNOSUPPORT
#define WSAEADDRINUSE      EADDRINUSE
#define WSAEADDRNOTAVAIL   EADDRNOTAVAIL
#define WSAENETDOWN        ENETDOWN
#define WSAENETUNREACH     ENETUNREACH
#define WSAENETRESET       ENETRESET
#define WSAECONNABORTED    ECONNABORTED
#define WSAECONNRESET      ECONNRESET
#define WSAENOBUFS         ENOBUFS
#define WSAEISCONN         EISCONN
#define WSAENOTCONN        ENOTCONN
#define WSAESHUTDOWN       ESHUTDOWN
#define WSAETOOMANYREFS    ETOOMANYREFS
#define WSAETIMEDOUT       ETIMEDOUT
#define WSAECONNREFUSED    ECONNREFUSED
#define WSAELOOP           ELOOP
#define WSAENAMETOOLONG    ENAMETOOLONG
#define WSAEHOSTDOWN       EHOSTDOWN

#undef INVALID_SOCKET
#undef SOCKET_ERROR
#define INVALID_SOCKET 	-1
#define SOCKET_ERROR 	-1

#define SOCKET int
typedef int WSAsize_t;

#endif // NET_OGC_H
