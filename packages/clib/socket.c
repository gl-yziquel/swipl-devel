/*  $Id$

    Part of SWI-Prolog

    Author:  Jan Wielemaker
    E-mail:  jan@swi.psy.uva.nl
    WWW:     http://www.swi.psy.uva.nl/projects/SWI-Prolog/
    Copying: GPL-2.  See the file COPYING or http://www.gnu.org

    Copyright (C) 1990-2001 SWI, University of Amsterdam. All rights reserved.
*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This module defines the bottom layer for dealing with TCP stream-sockets
from SWI-Prolog, both server and client side.

The basis of this module was implemented by Gordon Streeter. It has been
redesigned to make it a bit  easier   to  use  and handle problems using
Prolog exceptions instead of special return-values.

The   tcp_select()   call   has   been     replaced    by   SWI-Prolog's
wait_for_input/3.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* You can use it in normal executable, but not in dll???
*/

#ifdef __CYGWIN__
#undef HAVE_H_ERRNO
#endif

#include <SWI-Stream.h>
#include "clib.h"

#include <stdio.h>
#include <errno.h>
#include <malloc.h>
#include <sys/types.h>
#ifdef WIN32
#include <io.h>
#include <winsock.h>
#else
#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_H_ERRNO
extern int h_errno;
#else
#define h_errno errno
#endif
#define closesocket(n) close((n))	/* same on Unix */
#endif
#include <assert.h>
#include <string.h>

static functor_t FUNCTOR_socket1;
static functor_t FUNCTOR_module2;
static functor_t FUNCTOR_ip4;

		 /*******************************
		 *	 ADMINISTRATION		*
		 *******************************/

#define SOCK_INSTREAM	0x01
#define SOCK_OUTSTREAM	0x02
#define SOCK_BIND	0x04		/* What have we done? */
#define SOCK_LISTEN	0x08
#define SOCK_CONNECT	0x10
#define SOCK_ACCEPT	0x20		/* set on accepted sockets */

typedef struct _plsocket
{ struct _plsocket *next;		/* next in list */
  int		    socket;		/* The OS socket */
  int		    flags;		/* Misc flags */
} plsocket;

static plsocket *sockets;

static plsocket *
lookupSocket(int socket)
{ plsocket *p;

  for(p=sockets; p; p = p->next)
  { if ( p->socket == socket )
      return p;
  }

  p = malloc(sizeof(plsocket));
  p->socket = socket;
  p->flags  = 0;
  p->next   = sockets;
  sockets   = p;

  return p;
}


static int
freeSocket(int socket)
{ plsocket **p = &sockets;

  for( ; *p; p = &(*p)->next)
  { if ( (*p)->socket == socket )
    { plsocket *tmp = *p;
      
      *p = tmp->next;
      free(tmp);
      break;
    }
  }

  return closesocket(socket);
}


		 /*******************************
		 *	     CONVERSION		*
		 *******************************/

static int
tcp_get_socket(term_t Socket, int *id)
{ IOSTREAM *s;
  int socket;

  if ( PL_is_functor(Socket, FUNCTOR_socket1) )
  { term_t a = PL_new_term_ref();

    PL_get_arg(1, Socket, a);
    if ( PL_get_integer(a, id) )
      return TRUE;
  }
  
  if ( PL_get_stream_handle(Socket, &s) &&
       (socket = Sfileno(s)) >= 0 )
  { *id = socket;
    return TRUE;
  }

  return pl_error(NULL, 0, NULL, ERR_ARGTYPE, -1, Socket, "socket");
}


static int
tcp_unify_socket(term_t Socket, int id)
{ return PL_unify_term(Socket,
		       PL_FUNCTOR, FUNCTOR_socket1,
		         IntArg(id));
}


		 /*******************************
		 *	      ERRORS		*
		 *******************************/

#ifdef HAVE_H_ERRNO
typedef struct
{ int code;
  const char *string;
} error_codes;

static error_codes h_errno_codes[] = {
#ifdef HOST_NOT_FOUND
    { HOST_NOT_FOUND, "Host not found" },
#endif
#ifdef TRY_AGAIN
    { TRY_AGAIN, "Try Again" },
#endif
#ifdef NO_RECOVERY
    { NO_RECOVERY, "No Recovery" },
#endif
#ifdef NO_DATA
    { NO_DATA, "No Data" },
#endif
#ifdef NO_ADDRESS
    { NO_ADDRESS, "No Address" },
#endif
    {0, NULL}
};

#else /*HAVE_H_ERRNO*/
#define h_errno_codes NULL
typedef void * error_codes;
#endif /*HAVE_H_ERRNO*/

static int
tcp_error(int code, error_codes *map)
{ const char *msg;
  term_t except = PL_new_term_ref();

#ifdef HAVE_H_ERRNO
  static char msgbuf[100];

  if ( map )
  { while( map->code && map->code != code )
      map++;
    if ( map->code )
      msg = map->string;
    else
    { sprintf(msgbuf, "Unknown error %d", code);
      msg = msgbuf;
    }
  } else
#endif
    msg = strerror(code);

  PL_unify_term(except,
		CompoundArg("error", 2),
		  CompoundArg("socket_error", 1),
		    AtomArg(msg),
		  PL_VARIABLE);

  return PL_raise_exception(except);
}

		 /*******************************
		 *	  INITIALISATION	*
		 *******************************/

static int
tcp_init()
{ static int done = FALSE;

  if ( done )
    return TRUE;
  done = TRUE;

#ifdef WIN32
{ WSADATA WSAData;
  int optionValue = SO_SYNCHRONOUS_NONALERT;
  int err;

  if ( WSAStartup(MAKEWORD(1,1), &WSAData) )
  { return PL_warning("tcp_winsock_init - WSAStartup failed.");

    err = setsockopt(INVALID_SOCKET, 
		     SOL_SOCKET, 
		     SO_OPENTYPE, 
		     (char *)&optionValue, 
		     sizeof(optionValue));

    if ( err != NO_ERROR )
      return PL_warning("tcp_winsock_init - setsockopt failed.");
  }
}
#endif

  return TRUE;
}
	

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
socket(-Socket)
    Create a stream inet socket.  The socket is represented by a term of
    the format $socket(Id).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static foreign_t
tcp_socket(term_t Socket)
{ int sock;
	
  if ( !tcp_init() )
    return FALSE;

  if ( (sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    return tcp_error(errno, NULL);

  lookupSocket(sock);			/* register it */

  return tcp_unify_socket(Socket, sock);
}


static foreign_t
tcp_close_socket(term_t Socket)
{ int socket;

  if ( !tcp_get_socket(Socket, &socket) )
    return FALSE;

  closesocket(socket);

  return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Translate a host + port-number into a sockaddr structure.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
tcp_get_port(term_t Port, int *port)
{ char *name;

  if ( PL_get_atom_chars(Port, &name) )
  { struct servent *service;
    
    if ( !(service = getservbyname(name, "tcp")) )
      return tcp_error(errno, NULL);

    *port = ntohs(service->s_port);
    return TRUE;
  }

  if ( PL_get_integer(Port, port) )
    return TRUE;

  return pl_error(NULL, 0, NULL, ERR_ARGTYPE, -1, Port, "port");
}


static int
tcp_get_sockaddr(term_t Address, struct sockaddr_in *addr)
{ struct hostent *host;
  char           *hostName = NULL;
  int		  port;
	
  addr->sin_family = AF_INET;

  if ( PL_is_functor(Address, FUNCTOR_module2) )
  { term_t arg = PL_new_term_ref();

    PL_get_arg(1, Address, arg);
    if ( !PL_get_atom_chars(arg, &hostName) )
      return pl_error(NULL, 0, NULL, ERR_ARGTYPE, 1, arg, "atom");

    PL_get_arg(2, Address, arg);
    if ( !tcp_get_port(arg, &port) )
      return FALSE;
  } else if ( !tcp_get_port(Address, &port) )
    return FALSE;

  if ( hostName )
  { if( !(host = gethostbyname(hostName)) )
      return tcp_error(h_errno, h_errno_codes);
    memcpy(&addr->sin_addr, host->h_addr, host->h_length);
  } else
    addr->sin_addr.s_addr = INADDR_ANY;
	
  addr->sin_port = htons((short)port);

  return TRUE;
}


static int
tcp_get_ip(term_t Ip, struct in_addr *ip)
{ unsigned long hip = 0;

  if ( PL_is_functor(Ip, FUNCTOR_ip4) )
  { int i, ia;
    term_t a = PL_new_term_ref();

    for(i=1; i<=4; i++)
    { PL_get_arg(i, Ip, a);
      if ( PL_get_integer(a, &ia) )
	hip |= ia << ((4-i)*8);
      else
	return FALSE;
    }
    hip = htonl(hip);
    memcpy(ip, &hip, sizeof(hip));

    return TRUE;
  }

  return FALSE;
}


static int
tcp_unify_ip(term_t Ip, struct in_addr *ip, int netorder)
{ unsigned long hip;

  if ( netorder )
    hip = ntohl(ip->s_addr);
  else
    hip = ip->s_addr;

  return PL_unify_term(Ip,
		       PL_FUNCTOR, FUNCTOR_ip4,
		         IntArg((hip >> 24) & 0xff),
		         IntArg((hip >> 16) & 0xff),
		         IntArg((hip >>  8) & 0xff),
		         IntArg((hip >>  0) & 0xff));
}


static foreign_t
tcp_host_to_address(term_t Host, term_t Ip)
{ struct in_addr ip;
  struct hostent *host;
  char *host_name;

  if ( PL_get_atom_chars(Host, &host_name) )
  { if ( (host = gethostbyname(host_name)) )
    { assert(sizeof(ip) == host->h_length);
      memcpy(&ip, host->h_addr, host->h_length);
      return tcp_unify_ip(Ip, &ip, TRUE);
    } else
      return tcp_error(h_errno, h_errno_codes);
  } else if ( tcp_get_ip(Ip, &ip) )
  { if ( (host = gethostbyaddr((char *)&ip, sizeof(ip), AF_INET)) )
      return PL_unify_atom_chars(Host, host->h_name);
    else
      return tcp_error(h_errno, h_errno_codes);
  }

  return FALSE;
}


foreign_t
tcp_bind(term_t Socket, term_t Address)
{ struct sockaddr_in sockaddr;
  int socket;
       
  if ( !tcp_get_socket(Socket, &socket) ||
       !tcp_get_sockaddr(Address, &sockaddr) )
    return FALSE;
	
  if ( bind(socket,
	    (struct sockaddr*)&sockaddr, sizeof(sockaddr)))
    return tcp_error(errno, NULL);

  lookupSocket(socket)->flags |= SOCK_BIND;

  return TRUE;
}


foreign_t
tcp_connect(term_t Socket, term_t Address)
{ struct sockaddr_in sockaddr;
  int socket;
       
  if ( !tcp_get_socket(Socket, &socket) ||
       !tcp_get_sockaddr(Address, &sockaddr) )
    return FALSE;
	
  if ( connect(socket,
	       (struct sockaddr*)&sockaddr, sizeof(sockaddr)))
    return tcp_error(errno, NULL);

  lookupSocket(socket)->flags |= SOCK_CONNECT;

  return TRUE;
}


static foreign_t
tcp_accept(term_t Master, term_t Slave, term_t Peer)
{ int master, slave;
  struct sockaddr_in addr;
  int addrlen = sizeof(addr);
	
  if ( !tcp_get_socket(Master, &master) )
    return FALSE;

  if ( (slave = accept(master, (struct sockaddr*)&addr, &addrlen)) == -1 )
    return tcp_error(errno, NULL);

  lookupSocket(slave)->flags |= SOCK_ACCEPT;
  
  if ( tcp_unify_ip(Peer, &addr.sin_addr, TRUE) &&
       tcp_unify_socket(Slave, slave) )
    return TRUE;

  return FALSE;
}



foreign_t
tcp_listen(term_t Sock, term_t BackLog)
{ int socket;
  int backlog;

  if ( !tcp_get_socket(Sock, &socket) )
    return FALSE;

  if ( !PL_get_integer(BackLog, &backlog) ) 
    return pl_error(NULL, 0, NULL, ERR_ARGTYPE, -1, BackLog, "integer");

  if( listen(socket, backlog) == -1 )
    return tcp_error(errno, NULL);

  lookupSocket(socket)->flags |= SOCK_LISTEN;

  return TRUE;
}


		 /*******************************
		 *	  IO-STREAM STUFF	*
		 *******************************/

#define fdFromHandle(p) ((int)((long)(p)))

static int
tcp_read(void *sock, char *buf, int bufSize)
{ return recv(fdFromHandle(sock), buf, bufSize, 0);
}


static int
tcp_write(void *sock, char * buf, int bufSize)
{ return send(fdFromHandle(sock), buf, bufSize, 0);
}


static long
tcp_seek_null(void *handle, long offset, int whence)
{ return -1;
}


static int
tcp_close_input(void *handle)
{ int socket = fdFromHandle(handle);

  plsocket *s = lookupSocket(socket);
  s->flags &= ~SOCK_INSTREAM;

  if ( !(s->flags & (SOCK_INSTREAM|SOCK_OUTSTREAM)) )
    return freeSocket(socket);

  return 0;
}


static int
tcp_close_output(void *handle)
{ int socket = fdFromHandle(handle);

  plsocket *s = lookupSocket(socket);
  s->flags &= ~SOCK_OUTSTREAM;

  if ( !(s->flags & (SOCK_INSTREAM|SOCK_OUTSTREAM)) )
    return freeSocket(socket);

  return 0;
}


static IOFUNCTIONS readFunctions =
{ tcp_read,
  tcp_write,
  tcp_seek_null,
  tcp_close_input,
};


static IOFUNCTIONS writeFunctions =
{ tcp_read,
  tcp_write,
  tcp_seek_null,
  tcp_close_output,
};


foreign_t
tcp_streams(term_t Socket, term_t Read, term_t Write)
{ IOSTREAM *in, *out;
  int socket;
  plsocket *pls;

  if ( !tcp_get_socket(Socket, &socket) )
    return FALSE;
  
  pls = lookupSocket(socket);
  in  = Snew((void *)(long)socket, SIO_FILE|SIO_INPUT,  &readFunctions);
  if ( !PL_open_stream(Read, in) )
    return FALSE;
  pls->flags |= SOCK_INSTREAM;

  if ( !(pls->flags & SOCK_LISTEN) )
  { out = Snew((void *)(long)socket, SIO_FILE|SIO_OUTPUT, &writeFunctions);
    if ( !PL_open_stream(Write, out) )
      return FALSE;
    pls->flags |= SOCK_OUTSTREAM;
  }

  return TRUE;
}


		 /*******************************
		 *	   BLOCKING IO		*
		 *******************************/
#ifdef WIN32
#define F_SETFL 0
#define O_NONBLOCK 0

static int
fcntl(int fd, int op, int arg)
{ switch(op)
  { case F_SETFL:
      switch(arg)
      { case O_NONBLOCK:
	{ int rval;
	  int non_block;

	  non_block = 1;
	  rval = ioctlsocket(fd, FIONBIO, &non_block);
	  return rval ? -1 : 0;
	}
	default:
	  return -1;
      }
    break;
    default:
      return -1;
  }
}
#endif

static foreign_t
tcp_fcntl(term_t Socket, term_t Cmd, term_t Arg)
{ int socket;
  char *cmd;

  if ( !tcp_get_socket(Socket, &socket) )
    return FALSE;
  if ( !PL_get_atom_chars(Cmd, &cmd) )
    return pl_error(NULL, 0, NULL, ERR_ARGTYPE, 2, Cmd, "atom");

  if ( strcmp(cmd, "setfl") == 0 )
  { char *arg;

    if ( !PL_get_atom_chars(Arg, &arg) )
      return pl_error(NULL, 0, NULL, ERR_ARGTYPE, 3, Arg, "flag");
    if ( strcmp(arg, "nonblock") == 0 )
    { fcntl(socket, F_SETFL, O_NONBLOCK);
      return TRUE;
    }

    return pl_error(NULL, 0, NULL, ERR_ARGTYPE, 3, Arg, "flag");
  }

  return pl_error(NULL, 0, NULL, ERR_ARGTYPE, 3, Arg, "command");
}


static foreign_t
pl_gethostname(term_t name)
{ char buf[256];

  if ( gethostname(buf, sizeof(buf)) == 0 )
    return PL_unify_atom_chars(name, buf);

  return pl_error(NULL, 0, NULL, ERR_ERRNO, errno);
}

install_t
install_socket()
{ FUNCTOR_socket1 = PL_new_functor(PL_new_atom("$socket"), 1);
  FUNCTOR_module2 = PL_new_functor(PL_new_atom(":"), 2);
  FUNCTOR_ip4     = PL_new_functor(PL_new_atom("ip"), 4);
  
  PL_register_foreign("tcp_accept",           3, tcp_accept,          0);
  PL_register_foreign("tcp_bind",             2, tcp_bind,            0);
  PL_register_foreign("tcp_connect",          2, tcp_connect,         0);
  PL_register_foreign("tcp_listen",           2, tcp_listen,          0);
  PL_register_foreign("tcp_open_socket",      3, tcp_streams,         0);
  PL_register_foreign("tcp_socket",           1, tcp_socket,          0);
  PL_register_foreign("tcp_close_socket",     1, tcp_close_socket,    0);
  PL_register_foreign("tcp_fcntl",            3, tcp_fcntl,           0);
  PL_register_foreign("tcp_host_to_address",  2, tcp_host_to_address, 0);
  PL_register_foreign("gettcp_host_to_address",  2, tcp_host_to_address, 0);
  PL_register_foreign("gethostname",          1, pl_gethostname,      0);
}



