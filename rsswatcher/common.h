#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <termios.h>
#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <net/if_arp.h>
#include <netdb.h>
#include <signal.h>

#ifdef _DEBUG_
#define TRACE(fmt,a...) fprintf(stdout,"%d:"fmt"\n" \
                                ,__LINE__,##a)
#else
#define TRACE(fmt,a...)
#endif

void deamonrun();

char* gethostip(char* www);
char* catchip(char* buffer,int lens);
char* replace(char* buffer,char* src,char* dest,int blen);
int   tcpconnect(int port,char* server);

#endif

