#include <common.h>
#include <lock.h>

#define RSS_CACHE_FILE "/tmp/rsscache" //don't need to fix this 
#define RSS_STATS_FILE "/tmp/rssstats" //don't need to fix this
#define RSS_CHECK_FILE "/etc/rsscheck" //url with passkey saved here
#define RSS_SEEDS_PATH "/mnt/download/rssseeds/" //seeds auto download here
//#define RSS_SEEDS_FILE "seeds_XXXXXX" //not use now...
//#define RSS_SEEDS_USED "/mnt/base/seedused" //now use now...

#define RSS_LIMIT 16
#define URL_LIMIT 512
#define interval  180 

struct rss_site{
	char www[URL_LIMIT];
	char url[URL_LIMIT];
};

struct codemap{
	char code[16];
	char ch;
};

int wgetrun(char* command){
	char tmps[512] = {0};
	char lens[64]  = {0};
	char * lpsave = NULL;
	int ret = 0;

	system(command);

	FILE* fp = fopen(RSS_STATS_FILE,"r");

        if(fp != NULL){
		int bytes = 0;
		while(fgets(tmps,512,fp)){
//			TRACE("%s",tmps);
			if((lpsave = strstr(tmps,"saved")) != NULL){
				ret = 1;
				break;
			}
			memset(tmps,0,512);
		}
	        fclose(fp);
        }else{
		TRACE("fopen failed with command:%s",command);
	}

	return ret;
}

int wgetrss(char* wwwip,char* url){
	char command[512] = {0};

	sprintf(command,"wget '%s' -O '%s' -o '%s' -T10 > /dev/null 2>&1",url,RSS_CACHE_FILE,RSS_STATS_FILE);

	return wgetrun(command);
}

int wgetdown(char* site,char* url,char* name){
	char seedpath[128] = {0};
	char seedused[128] = {0};
        sprintf(seedpath,"%s/%s.torrent\0",RSS_SEEDS_PATH,name);
	sprintf(seedused,"%s/%s.torrent.added\0",RSS_SEEDS_PATH,name);

	char command[512] = {0};
	int ret = 0;
	if(access(seedused,0) == 0){
		TRACE("%s already downloaded",name);
		return 0;
	}

	TRACE("SEEDURL:[%s][%s]",seedpath,url);
	sprintf(command,"wget '%s' -O '%s' -o '%s' -T10 > /dev/null 2>&1",url,seedpath,RSS_STATS_FILE);

	if(wgetrun(command)){
		return 1;	
	}

	return 0;
}

char* htmlcode(char* url,int lens,char* key,char* set){
	char * lpkey = NULL;

	int i = 0;
	int keylen = strlen(key);
	int setlen = strlen(set);
	int urlpos = 0;

	char cmpbox[URL_LIMIT] = {0};

	static char newurl[URL_LIMIT] = {0};
	memset(newurl,0,URL_LIMIT);

	TRACE("start replace url: [%s] to [%s]",key,set);

	for(i;i<lens;i++){
		if(url[i] == key[0]){
			memcpy(cmpbox,&url[i],keylen);
			if(strncmp(cmpbox,key,keylen) == 0){
				TRACE("found %s",key);
				setlen = setlen + urlpos > URL_LIMIT ? URL_LIMIT - urlpos : setlen;
				strcat(&newurl[urlpos],set);
				urlpos += setlen;
				i += keylen - 1;
			}
		}else{
			newurl[urlpos++] = url[i];
		}
	}


	return newurl;
}

int downseed(char* www,char* url){
	char* wwwip = "null";//gethostip(www);
	char* decodeurl = htmlcode(url,URL_LIMIT,"%3D","&");
	TRACE("decode url:[%s]",decodeurl);
	if(!wgetrss(wwwip,decodeurl)) return -1;
		
	FILE* fprss = fopen(RSS_CACHE_FILE,"rb");	
	if(!fprss) return -1;

	char buffer[URL_LIMIT] = {0};
	char * lp = NULL;
	char requrl[URL_LIMIT] = {0};
	char name[128] = {0};

	int name_ok = 0;
	int url_ok = 0;

	while((lp = fgets(buffer,URL_LIMIT,fprss))){
		if(name_ok && url_ok){
			char* downurl = htmlcode(requrl,strlen(requrl),"&amp;","&");
			if(downurl) wgetdown(wwwip,downurl,name);
			memset(requrl,0,URL_LIMIT);
			memset(name,0,128);
			name_ok = url_ok = 0;
		}

		if(strstr(buffer,"<title>")){
			if(strstr(buffer,"![CDATA[")){
				sscanf(buffer,"%*[^[][%*[^[][%[^]]",name);
			}else{
				sscanf(buffer,"%*[^>]>%[^<]",name);
			}
			TRACE("REQ:[%s]",name);
			name_ok = 1;
		}
		if(strstr(buffer,"<enclosure url=\"") && name_ok){
			sscanf(buffer,"%*[^\"]\"%[^\"]",requrl);
			TRACE("REQ:[%s]",requrl);

			url_ok = 1;
		}
		memset(buffer,0,URL_LIMIT);
	}

	fclose(fprss);

	return 0;	
}

int loadcheck(struct rss_site strss[],int size){
	FILE* fp = fopen(RSS_CHECK_FILE,"rb");
	if(!fp) return 0;

	char* linebuf = NULL;
	char buffer[512] = {0};

	int i = 0;
	for(i;i<size;i++){
		memset(strss[i].www,0,URL_LIMIT);
		memset(strss[i].url,0,URL_LIMIT);
		memset(buffer,0,512);
		if(fgets(buffer,URL_LIMIT,fp) == NULL) break;

		sscanf(buffer,"%[^ ] %s",strss[i].www,strss[i].url);
	}

	fclose(fp);

	return i;
}

int main(void){
	struct rss_site strss[RSS_LIMIT];
	int i = 0;

	deamonrun();

	signal(SIGCHLD,SIG_IGN);

	if(check_running() < 0){
                return -1;
        }

	while(1){
		TRACE("Lookup new rss torrent...");

		int lens = loadcheck(strss,RSS_LIMIT);
		for(i=0;i<lens;i++){
			downseed(strss[i].www,strss[i].url);
		}
		
		for(i=0;i<interval;i++){
			sleep(1);
		}
	}

	return 0;
}
