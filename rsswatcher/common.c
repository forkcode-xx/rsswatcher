#include <common.h>

void deamonrun(){ 
	int pid; 
	int i; 
	if(pid = fork()) exit(0); 
	else if(pid < 0) exit(1);
	setsid();
	if(pid = fork()) exit(0); 
	else if(pid < 0) exit(1); 
	chdir("/tmp"); 
	umask(0); 
	return; 
}

char* replace(char* buffer,char* src,char* dest,int blen){
	if(src){
		int resultlen = strlen(buffer) + strlen(dest) - strlen(src);
		if(resultlen > blen){
			TRACE("result len is max than buffer size");
			return NULL;
		}

		char* lpsrc = strstr(buffer,src);
		if(!lpsrc){
			TRACE("src not exists");
			return NULL;
		}

		char* lpnew = (char*)malloc(blen);
		memset(lpnew,0,blen);

		char* lpend = lpsrc + strlen(src);
		memcpy(lpnew,lpend,strlen(lpend));
		memcpy(lpsrc,dest,strlen(dest));
		memcpy(lpsrc + strlen(dest),lpnew,strlen(lpnew));

		*(lpsrc + strlen(dest) + strlen(lpnew)) = '\0';

		free(lpnew);

		return buffer;
	}else{
		TRACE("input src is null");
		return NULL;
	}
}

char* gethostip(char* www){
        static char hostIP[32] = {0};

        struct hostent *hptr = gethostbyname(www);
        if(hptr == NULL){
                TRACE("Get %s hostname failed",www);
                return NULL;
        }

        char *ptr,**pptr;
        TRACE("official hostname:%s",hptr->h_name);

        char *ip = inet_ntoa(*((struct in_addr *)hptr->h_addr));

        TRACE("IP:%s",ip);
        memset(hostIP,0,sizeof(hostIP));
        sprintf(hostIP,"%s",ip);

        return hostIP;
}

char* catchip(char* buffer,int lens){
        int i=0,p = 0;
        int b = 0;
        int d = 0;
        static char ip[16] = {0};
        for(i=0;i<lens;i++){
                if(p == 15) break;
                if(buffer[i] == '.'){
                        if(b){
                                ip[p++] = buffer[i];
                                d++;
                        }
                }else if('0' <= buffer[i]){
                        if(buffer[i] <= '9'){
                                b = 1;
                                if(b) ip[p++] = buffer[i];
                        }
                }else{
                        if(d == 3) break;
                        b = 0;
                        memset(ip,0,16);
                }
        }
        return ip;
}

int tcpconnect(int port,char* server){
	struct sockaddr_in addrou;
        int sock = 0;

	memset(&addrou,0,sizeof(struct sockaddr_in));

	addrou.sin_family = AF_INET;
        addrou.sin_addr.s_addr = inet_addr(server);
        addrou.sin_port = htons(port);

	sock = socket(AF_INET,SOCK_STREAM,0);

	struct timeval timeo;
	timeo.tv_sec = 10;
	timeo.tv_usec = 0;

	if(setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,(char*)&timeo,sizeof(timeo)) < 0){
	        TRACE("set socket connect timeout failed...");
		close(sock);
       		return 0;
    	}
/*
	if(setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,(char*)&timeo,sizeof(timeo)) < 0){
                TRACE("set socket recives timeout failed...");
		close(sock);
                return 0;
        }
*/
	if(sock < 0){
                TRACE("create socket failed...");
		return 0;
        }

	if(connect(sock,(struct sockaddr *)&addrou,sizeof(addrou)) < 0){
                TRACE("connectting failed...");
		close(sock);
		return 0;
        }

	return sock;
}
