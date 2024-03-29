/*
 * This file Copyright (C) Mnemosyne LLC
 *
 * This file is licensed by the GPL version 2. Works owned by the
 * Transmission project are granted a special exemption to clause 2(b)
 * so that the bulk of its code can remain under the MIT license.
 * This exemption does not extend to derived works not owned by
 * the Transmission project.
 *
 * $Id: rpc-server.c 13226 2012-02-15 01:44:21Z jordan $
 */

#include <assert.h>
#include <errno.h>
#include <string.h> /* memcpy */
#include <limits.h> /* INT_MAX */

#include <unistd.h>    /* close */

#ifdef HAVE_ZLIB
 #include <zlib.h>
#endif

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/http_struct.h> /* TODO: eventually remove this */

#include "transmission.h"
#include "bencode.h"
#include "crypto.h" /* tr_cryptoRandBuf(), tr_ssha1_matches() */
#include "fdlimit.h"
#include "list.h"
#include "net.h"
#include "platform.h" /* tr_getWebClientDir() */
#include "ptrarray.h"
#include "rpcimpl.h"
#include "rpc-server.h"
#include "session.h"
#include "trevent.h"
#include "utils.h"
#include "web.h"

/* session-id is used to make cross-site request forgery attacks difficult.
 * Don't disable this feature unless you really know what you're doing!
 * http://en.wikipedia.org/wiki/Cross-site_request_forgery
 * http://shiflett.org/articles/cross-site-request-forgeries
 * http://www.webappsec.org/lists/websecurity/archive/2008-04/msg00037.html */
#define REQUIRE_SESSION_ID

#define MY_NAME "RPC Server"
#define MY_REALM "Transmission"
#define TR_N_ELEMENTS( ary ) ( sizeof( ary ) / sizeof( *ary ) )

struct tr_rpc_server
{
    bool               isEnabled;
    bool               isPasswordEnabled;
    bool               isWhitelistEnabled;
    tr_port            port;
    char *             url;
    struct in_addr     bindAddress;
    struct evhttp *    httpd;
    tr_session *       session;
    char *             username;
    char *             password;
    char *             whitelistStr;
    tr_list *          whitelist;

    char *             sessionId;
    time_t             sessionIdExpiresAt;

#ifdef HAVE_ZLIB
    bool               isStreamInitialized;
    z_stream           stream;
#endif
};

#define dbgmsg( ... ) \
    do { \
        if( tr_deepLoggingIsActive( ) ) \
            tr_deepLog( __FILE__, __LINE__, MY_NAME, __VA_ARGS__ ); \
    } while( 0 )


/***
****
***/

static char*
get_current_session_id( struct tr_rpc_server * server )
{
    const time_t now = tr_time( );

    if( !server->sessionId || ( now >= server->sessionIdExpiresAt ) )
    {
        int i;
        const int n = 48;
        const char * pool = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        const size_t pool_size = strlen( pool );
        unsigned char * buf = tr_new( unsigned char, n+1 );

        tr_cryptoRandBuf( buf, n );
        for( i=0; i<n; ++i )
            buf[i] = pool[ buf[i] % pool_size ];
        buf[n] = '\0';

        tr_free( server->sessionId );
        server->sessionId = (char*) buf;
        server->sessionIdExpiresAt = now + (60*60); /* expire in an hour */
    }

    return server->sessionId;
}


/**
***
**/

static void
send_simple_response( struct evhttp_request * req,
                      int                     code,
                      const char *            text )
{
    const char *      code_text = tr_webGetResponseStr( code );
    struct evbuffer * body = evbuffer_new( );

    evbuffer_add_printf( body, "<h1>%d: %s</h1>", code, code_text );
    if( text )
        evbuffer_add_printf( body, "%s", text );
    evhttp_send_reply( req, code, code_text, body );

    evbuffer_free( body );
}

struct tr_mimepart
{
    char * headers;
    int headers_len;
    char * body;
    int body_len;
};

static void
tr_mimepart_free( struct tr_mimepart * p )
{
    tr_free( p->body );
    tr_free( p->headers );
    tr_free( p );
}

static void
extract_parts_from_multipart( const struct evkeyvalq * headers,
                              struct evbuffer * body,
                              tr_ptrArray * setme_parts )
{
    const char * content_type = evhttp_find_header( headers, "Content-Type" );
    const char * in = (const char*) evbuffer_pullup( body, -1 );
    size_t inlen = evbuffer_get_length( body );

    const char * boundary_key = "boundary=";
    const char * boundary_key_begin = content_type ? strstr( content_type, boundary_key ) : NULL;
    const char * boundary_val = boundary_key_begin ? boundary_key_begin + strlen( boundary_key ) : "arglebargle";
    char * boundary = tr_strdup_printf( "--%s", boundary_val );
    const size_t boundary_len = strlen( boundary );

    const char * delim = tr_memmem( in, inlen, boundary, boundary_len );
    while( delim )
    {
        size_t part_len;
        const char * part = delim + boundary_len;

        inlen -= ( part - in );
        in = part;

        delim = tr_memmem( in, inlen, boundary, boundary_len );
        part_len = delim ? (size_t)( delim - part ) : inlen;

        if( part_len )
        {
            const char * rnrn = tr_memmem( part, part_len, "\r\n\r\n", 4 );
            if( rnrn )
            {
                struct tr_mimepart * p = tr_new( struct tr_mimepart, 1 );
                p->headers_len = rnrn - part;
                p->headers = tr_strndup( part, p->headers_len );
                p->body_len = (part+part_len) - (rnrn + 4);
                p->body = tr_strndup( rnrn+4, p->body_len );
                tr_ptrArrayAppend( setme_parts, p );
            }
        }
    }

    tr_free( boundary );
}

static void
handle_upload( struct evhttp_request * req,
               struct tr_rpc_server *  server )
{
    if( req->type != EVHTTP_REQ_POST )
    {
        send_simple_response( req, 405, NULL );
    }
    else
    {
        int i;
        int n;
        bool hasSessionId = false;
        tr_ptrArray parts = TR_PTR_ARRAY_INIT;

        const char * query = strchr( req->uri, '?' );
        const bool paused = query && strstr( query + 1, "paused=true" );

        extract_parts_from_multipart( req->input_headers, req->input_buffer, &parts );
        n = tr_ptrArraySize( &parts );

        /* first look for the session id */
        for( i=0; i<n; ++i ) {
            struct tr_mimepart * p = tr_ptrArrayNth( &parts, i );
            if( tr_memmem( p->headers, p->headers_len, TR_RPC_SESSION_ID_HEADER, strlen( TR_RPC_SESSION_ID_HEADER ) ) )
                break;
        }
        if( i<n ) {
            const struct tr_mimepart * p = tr_ptrArrayNth( &parts, i );
            const char * ours = get_current_session_id( server );
            const int ourlen = strlen( ours );
            hasSessionId = ourlen<=p->body_len && !memcmp( p->body, ours, ourlen );
        }

        if( !hasSessionId )
        {
            int code = 409;
            const char * codetext = tr_webGetResponseStr( code );
            struct evbuffer * body = evbuffer_new( );
            evbuffer_add_printf( body, "%s", "{ \"success\": false, \"msg\": \"Bad Session-Id\" }" );;
            evhttp_send_reply( req, code, codetext, body );
            evbuffer_free( body );
        }
        else for( i=0; i<n; ++i )
        {
            struct tr_mimepart * p = tr_ptrArrayNth( &parts, i );
            int body_len = p->body_len;
            tr_benc top, *args;
            tr_benc test;
            bool have_source = false;
            char * body = p->body;

            if( body_len >= 2 && !memcmp( &body[body_len - 2], "\r\n", 2 ) )
                body_len -= 2;

            tr_bencInitDict( &top, 2 );
            tr_bencDictAddStr( &top, "method", "torrent-add" );
            args = tr_bencDictAddDict( &top, "arguments", 2 );
            tr_bencDictAddBool( args, "paused", paused );

            if( tr_urlIsValid( body, body_len ) )
            {
                tr_bencDictAddRaw( args, "filename", body, body_len );
                have_source = true;
            }
            else if( !tr_bencLoad( body, body_len, &test, NULL ) )
            {
                char * b64 = tr_base64_encode( body, body_len, NULL );
                tr_bencDictAddStr( args, "metainfo", b64 );
                tr_free( b64 );
                have_source = true;
            }

            if( have_source )
            {
                struct evbuffer * json = tr_bencToBuf( &top, TR_FMT_JSON );
                tr_rpc_request_exec_json( server->session,
                                          evbuffer_pullup( json, -1 ),
                                          evbuffer_get_length( json ),
                                          NULL, NULL );
                evbuffer_free( json );
            }

            tr_bencFree( &top );
        }

        tr_ptrArrayDestruct( &parts, (PtrArrayForeachFunc)tr_mimepart_free );

        /* send "success" response */
        {
            int code = HTTP_OK;
            const char * codetext = tr_webGetResponseStr( code );
            struct evbuffer * body = evbuffer_new( );
            evbuffer_add_printf( body, "%s", "{ \"success\": true, \"msg\": \"Torrent Added\" }" );;
            evhttp_send_reply( req, code, codetext, body );
            evbuffer_free( body );
        }
    }
}

static const char*
mimetype_guess( const char * path )
{
    unsigned int i;

    const struct
    {
        const char *    suffix;
        const char *    mime_type;
    } types[] = {
        /* these are the ones we need for serving the web client's files... */
        { "css",  "text/css"                  },
        { "gif",  "image/gif"                 },
        { "html", "text/html"                 },
        { "ico",  "image/vnd.microsoft.icon"  },
        { "js",   "application/javascript"    },
        { "png",  "image/png"                 }
    };
    const char * dot = strrchr( path, '.' );

    for( i = 0; dot && i < TR_N_ELEMENTS( types ); ++i )
        if( !strcmp( dot + 1, types[i].suffix ) )
            return types[i].mime_type;

    return "application/octet-stream";
}

static void
add_response( struct evhttp_request * req, struct tr_rpc_server * server,
              struct evbuffer * out, struct evbuffer * content )
{
#ifndef HAVE_ZLIB
    evbuffer_add_buffer( out, content );
#else
    const char * key = "Accept-Encoding";
    const char * encoding = evhttp_find_header( req->input_headers, key );
    const int do_compress = encoding && strstr( encoding, "gzip" );

    if( !do_compress )
    {
        evbuffer_add_buffer( out, content );
    }
    else
    {
        int state;
        struct evbuffer_iovec iovec[1];
        void * content_ptr = evbuffer_pullup( content, -1 );
        const size_t content_len = evbuffer_get_length( content );

        if( !server->isStreamInitialized )
        {
            int compressionLevel;

            server->isStreamInitialized = true;
            server->stream.zalloc = (alloc_func) Z_NULL;
            server->stream.zfree = (free_func) Z_NULL;
            server->stream.opaque = (voidpf) Z_NULL;

            /* zlib's manual says: "Add 16 to windowBits to write a simple gzip header
             * and trailer around the compressed data instead of a zlib wrapper." */
#ifdef TR_LIGHTWEIGHT
            compressionLevel = Z_DEFAULT_COMPRESSION;
#else
            compressionLevel = Z_BEST_COMPRESSION;
#endif
            deflateInit2( &server->stream, compressionLevel, Z_DEFLATED, 15+16, 8, Z_DEFAULT_STRATEGY );
        }

        server->stream.next_in = content_ptr;
        server->stream.avail_in = content_len;

        /* allocate space for the raw data and call deflate() just once --
         * we won't use the deflated data if it's longer than the raw data,
         * so it's okay to let deflate() run out of output buffer space */
        evbuffer_reserve_space( out, content_len, iovec, 1 );
        server->stream.next_out = iovec[0].iov_base;
        server->stream.avail_out = iovec[0].iov_len;
        state = deflate( &server->stream, Z_FINISH );

        if( state == Z_STREAM_END )
        {
            iovec[0].iov_len -= server->stream.avail_out;

#if 0
            fprintf( stderr, "compressed response is %.2f of original (raw==%zu bytes; compressed==%zu)\n",
                             (double)evbuffer_get_length(out)/content_len,
                             content_len, evbuffer_get_length(out) );
#endif
            evhttp_add_header( req->output_headers,
                               "Content-Encoding", "gzip" );
        }
        else
        {
            memcpy( iovec[0].iov_base, content_ptr, content_len );
            iovec[0].iov_len = content_len;
        }

        evbuffer_commit_space( out, iovec, 1 );
        deflateReset( &server->stream );
    }
#endif
}

static void
add_time_header( struct evkeyvalq * headers, const char * key, time_t value )
{
    /* According to RFC 2616 this must follow RFC 1123's date format,
       so use gmtime instead of localtime... */
    char buf[128];
    struct tm tm = *gmtime( &value );
    strftime( buf, sizeof( buf ), "%a, %d %b %Y %H:%M:%S GMT", &tm );
    evhttp_add_header( headers, key, buf );
}

static void
evbuffer_ref_cleanup_tr_free( const void * data UNUSED, size_t datalen UNUSED, void * extra )
{
    tr_free( extra );
}

static void
serve_file( struct evhttp_request * req,
            struct tr_rpc_server *  server,
            const char *            filename )
{
    if( req->type != EVHTTP_REQ_GET )
    {
        evhttp_add_header( req->output_headers, "Allow", "GET" );
        send_simple_response( req, 405, NULL );
    }
    else
    {
        void * file;
        size_t file_len;
        struct evbuffer * content;
        const int error = errno;

        errno = 0;
        file_len = 0;
        file = tr_loadFile( filename, &file_len );
        content = evbuffer_new( );
        evbuffer_add_reference( content, file, file_len, evbuffer_ref_cleanup_tr_free, file );

        if( errno )
        {
            char * tmp = tr_strdup_printf( "%s (%s)", filename, tr_strerror( errno ) );
            send_simple_response( req, HTTP_NOTFOUND, tmp );
            tr_free( tmp );
        }
        else
        {
            struct evbuffer * out;
            const time_t now = tr_time( );

            errno = error;
            out = evbuffer_new( );
            evhttp_add_header( req->output_headers, "Content-Type", mimetype_guess( filename ) );
            add_time_header( req->output_headers, "Date", now );
            add_time_header( req->output_headers, "Expires", now+(24*60*60) );
            add_response( req, server, out, content );
            evhttp_send_reply( req, HTTP_OK, "OK", out );

            evbuffer_free( out );
        }

        evbuffer_free( content );
    }
}

static void
handle_web_client( struct evhttp_request * req,
                   struct tr_rpc_server *  server )
{
    const char * webClientDir = tr_getWebClientDir( server->session );

    if( !webClientDir || !*webClientDir )
    {
        send_simple_response( req, HTTP_NOTFOUND,
            "<p>Couldn't find Transmission's web interface files!</p>"
            "<p>Users: to tell Transmission where to look, "
            "set the TRANSMISSION_WEB_HOME environment "
            "variable to the folder where the web interface's "
            "index.html is located.</p>"
            "<p>Package Builders: to set a custom default at compile time, "
            "#define PACKAGE_DATA_DIR in libtransmission/platform.c "
            "or tweak tr_getClutchDir() by hand.</p>" );
    }
    else
    {
        char * pch;
        char * subpath;

        subpath = tr_strdup( req->uri + strlen( server->url ) + 4 );
        if(( pch = strchr( subpath, '?' )))
            *pch = '\0';

        if( strstr( subpath, ".." ) )
        {
            send_simple_response( req, HTTP_NOTFOUND, "<p>Tsk, tsk.</p>" );
        }
        else
        {
            char * filename = tr_strdup_printf( "%s%s%s",
                                 webClientDir,
                                 TR_PATH_DELIMITER_STR,
                                 subpath && *subpath ? subpath : "index.html" );
            serve_file( req, server, filename );
            tr_free( filename );
        }

        tr_free( subpath );
    }
}

struct rpc_response_data
{
    struct evhttp_request * req;
    struct tr_rpc_server  * server;
};

static void
rpc_response_func( tr_session      * session UNUSED,
                   struct evbuffer * response,
                   void            * user_data )
{
    struct rpc_response_data * data = user_data;
    struct evbuffer * buf = evbuffer_new( );

    add_response( data->req, data->server, buf, response );
    evhttp_add_header( data->req->output_headers,
                           "Content-Type", "application/json; charset=UTF-8" );
    evhttp_send_reply( data->req, HTTP_OK, "OK", buf );

    evbuffer_free( buf );
    tr_free( data );
}

static void
handle_rpc( struct evhttp_request * req,
            struct tr_rpc_server  * server )
{
    struct rpc_response_data * data = tr_new0( struct rpc_response_data, 1 );

    data->req = req;
    data->server = server;

    if( req->type == EVHTTP_REQ_GET )
    {
        const char * q;
        if( ( q = strchr( req->uri, '?' ) ) )
            tr_rpc_request_exec_uri( server->session, q+1, -1, rpc_response_func, data );
    }
    else if( req->type == EVHTTP_REQ_POST )
    {
        tr_rpc_request_exec_json( server->session,
                                  evbuffer_pullup( req->input_buffer, -1 ),
                                  evbuffer_get_length( req->input_buffer ),
                                  rpc_response_func, data );
    }

}

// modify by fork
//
#define RSS_CHECK_FILE "/etc/rsscheck"

static void 
handle_rss( struct evhttp_request * req,
	    struct tr_rpc_server  * server )
{
    FILE * fp = NULL; //rss link store file
    FILE * sp = NULL; //tmp file for del rss link
    FILE * ep = NULL; //err file for debug
    const char * q = NULL;
    const char * s = NULL;
    char method[8]   = {0};
    char buffer[512] = {0};
    char define[128] = {0};
    char rssurl[384] = {0};
    char tmpurl[384] = {0};

    int code = 200;
    const char * codetext = tr_webGetResponseStr(code);
    struct evbuffer * body = evbuffer_new();

    memset(buffer,0,512);
    memset(define,0,128);
    memset(rssurl,0,384);
    memset(tmpurl,0,384);

    //only support get method
    if((q = strstr(req->uri,"?method="))){
    	sscanf(q,"%*[^=]=%3s",method);
	if(!strncmp(method,"get",3)){
	    fp = fopen(RSS_CHECK_FILE,"a+");
	    if(fp){
	    	evbuffer_add_printf(body,"[");
	        while((s = fgets(buffer,512,fp))){
			if(*s == '\n') continue;
  			sscanf(buffer,"%128[^ ] %384[^\n]",define,rssurl);
			evbuffer_add_printf(body,"{\"name\":\"%s\",\"url\":\"%s\"},",define,rssurl);
			memset(buffer,0,512);
			memset(define,0,128);
			memset(buffer,0,384);
		}
		fclose(fp);fp=NULL;
		evbuffer_add_printf(body,"{\"name\":\"\",\"url\":\"\"}");
		evbuffer_add_printf(body,"]");
            }else{
	        evbuffer_add_printf(body,"open failed");
		goto ERROR;
	    }
	}else if(!strncmp(method,"set",3)){
	    if((q = strstr(req->uri,"name="))){
	    	sscanf(q,"%*[^=]=%128[^&]",define);
	    }else{
	        evbuffer_add_printf(body,"name is null");
		goto ERROR;
	    }
	    if((q = strstr(req->uri,"url="))){
	        sscanf(q,"%*[^=]=%384[^&]",rssurl);
	    }else{
	    	evbuffer_add_printf(body,"url is null");
		goto ERROR;
	    }
	    if(strlen(rssurl) && strlen(define)){
		fp = fopen(RSS_CHECK_FILE,"a+");
                if(fp){
                    fprintf(fp,"%s %s\n",define,rssurl);
                    fclose(fp);fp=NULL;
                    evbuffer_add_printf(body,"SET OK");
                }else{
		    evbuffer_add_printf(body,"open failed");
		    goto ERROR;
		}
	    }
	}else if(!strncmp(method,"del",3)){
	    sp = fopen("/etc/.rsstmp","w");
	    if(!sp){
	        evbuffer_add_printf(body,"open failed");
	    	goto ERROR;
	    }

	    fp = fopen(RSS_CHECK_FILE,"r");
	    if(!fp){
	        evbuffer_add_printf(body,"open failed");
		goto ERROR;
	    }

	    if((q = strstr(req->uri,"url="))){
	        sscanf(q,"%*[^=]=%384[^&]",tmpurl);
                while((s = fgets(buffer,512,fp))){
                    sscanf(buffer,"%128[^ ] %384s",define,rssurl);
                    if(!strncmp(rssurl,tmpurl,strlen(rssurl))){
		        continue;
		    }
                    fprintf(sp,"%s %s\n",define,rssurl);
                }
	    }else{
	        evbuffer_add_printf(body,"url is null");
		goto ERROR;
	    }

	    fclose(fp);fp=NULL;
	    fclose(sp);sp=NULL;
/*
	    if(remove(RSS_CHECK_FILE) < 0){
	    	evbuffer_add_printf(body,"remove failed");
		goto ERROR;
	    }
*/

            if(!rename("/etc/.rsstmp",RSS_CHECK_FILE)){
	        evbuffer_add_printf(body,"DEL OK");
	    }else{
	    	/*
	    	ep = fopen("/tmp/rsserr","w");
		if(ep){
			fprintf(ep,"error:%s\n",strerror(errno));
			fclose(ep);
		}
		*/
                evbuffer_add_printf(body,"rename failed");
	    }
	}else{
	    evbuffer_add_printf(body,"unknow method:%s",method);
	}
    }else{
    	evbuffer_add_printf(body,"unknow request:%s",req->uri);
    }


ERROR:
    evhttp_send_reply(req,code,codetext,body);

    if(fp) fclose(fp);
    if(sp) fclose(sp);

    evbuffer_free(body);
}

static bool
isAddressAllowed( const tr_rpc_server * server,
                  const char *          address )
{
    tr_list * l;

    if( !server->isWhitelistEnabled )
        return true;

    for( l=server->whitelist; l!=NULL; l=l->next )
        if( tr_wildmat( address, l->data ) )
            return true;

    return false;
}

static bool
test_session_id( struct tr_rpc_server * server, struct evhttp_request * req )
{
    const char * ours = get_current_session_id( server );
    const char * theirs = evhttp_find_header( req->input_headers, TR_RPC_SESSION_ID_HEADER );
    const bool success =  theirs && !strcmp( theirs, ours );
    return success;
}

static void
handle_request( struct evhttp_request * req, void * arg )
{
    struct tr_rpc_server * server = arg;

    if( req && req->evcon )
    {
        const char * auth;
        char * user = NULL;
        char * pass = NULL;
/*
	FILE* fp = fopen("/tmp/rpccgi","wb");
        if(fp){
		fprintf(fp,"[%s][%s]\n",req->uri,server->url);
		fclose(fp);
        }
*/
        evhttp_add_header( req->output_headers, "Server", MY_REALM );

        auth = evhttp_find_header( req->input_headers, "Authorization" );
        if( auth && !evutil_ascii_strncasecmp( auth, "basic ", 6 ) )
        {
            int    plen;
            char * p = tr_base64_decode( auth + 6, 0, &plen );
            if( p && plen && ( ( pass = strchr( p, ':' ) ) ) )
            {
                user = p;
                *pass++ = '\0';
            }
        }

        if( !strncmp( req->uri + strlen( server->url ), "rss", 3 ) )
        {
	    handle_rss( req, server );
	    tr_free( user );
	    return;
        }

        if( !isAddressAllowed( server, req->remote_host ) )
        {
            send_simple_response( req, 403,
                "<p>Unauthorized IP Address.</p>"
                "<p>Either disable the IP address whitelist or add your address to it.</p>"
                "<p>If you're editing settings.json, see the 'rpc-whitelist' and 'rpc-whitelist-enabled' entries.</p>"
                "<p>If you're still using ACLs, use a whitelist instead. See the transmission-daemon manpage for details.</p>" );
        }
        else if( server->isPasswordEnabled
                 && ( !pass || !user || strcmp( server->username, user )
                                     || !tr_ssha1_matches( server->password,
                                                           pass ) ) )
        {
            evhttp_add_header( req->output_headers,
                               "WWW-Authenticate",
                               "Basic realm=\"" MY_REALM "\"" );
            send_simple_response( req, 401, "Unauthorized User" );
        }
        else if( strncmp( req->uri, server->url, strlen( server->url ) ) )
        {
            char * location = tr_strdup_printf( "%sweb/", server->url );
            evhttp_add_header( req->output_headers, "Location", location );
            send_simple_response( req, HTTP_MOVEPERM, NULL );
            tr_free( location );
        }
        else if( !strncmp( req->uri + strlen( server->url ), "web/", 4 ) )
        {
            handle_web_client( req, server );
        }
        else if( !strncmp( req->uri + strlen( server->url ), "upload", 6 ) )
        {
            handle_upload( req, server );
        }
#ifdef REQUIRE_SESSION_ID
        else if( !test_session_id( server, req ) )
        {
            const char * sessionId = get_current_session_id( server );
            char * tmp = tr_strdup_printf(
                "<p>Your request had an invalid session-id header.</p>"
                "<p>To fix this, follow these steps:"
                "<ol><li> When reading a response, get its X-Transmission-Session-Id header and remember it"
                "<li> Add the updated header to your outgoing requests"
                "<li> When you get this 409 error message, resend your request with the updated header"
                "</ol></p>"
                "<p>This requirement has been added to help prevent "
                "<a href=\"http://en.wikipedia.org/wiki/Cross-site_request_forgery\">CSRF</a> "
                "attacks.</p>"
                "<p><code>%s: %s</code></p>",
                TR_RPC_SESSION_ID_HEADER, sessionId );
            evhttp_add_header( req->output_headers, TR_RPC_SESSION_ID_HEADER, sessionId );
            send_simple_response( req, 409, tmp );
            tr_free( tmp );
        }
#endif
        else if( !strncmp( req->uri + strlen( server->url ), "rpc", 3 ) )
        {
            handle_rpc( req, server );
        }
        else
        {
            send_simple_response( req, HTTP_NOTFOUND, req->uri );
        }

        tr_free( user );
    }
}

static void
startServer( void * vserver )
{
    tr_rpc_server * server  = vserver;
    tr_address addr;

    if( !server->httpd )
    {
        addr.type = TR_AF_INET;
        addr.addr.addr4 = server->bindAddress;
        server->httpd = evhttp_new( server->session->event_base );
        evhttp_bind_socket( server->httpd, tr_address_to_string( &addr ), server->port );
        evhttp_set_gencb( server->httpd, handle_request, server );

    }
}

static void
stopServer( tr_rpc_server * server )
{
    if( server->httpd )
    {
        evhttp_free( server->httpd );
        server->httpd = NULL;
    }
}

static void
onEnabledChanged( void * vserver )
{
    tr_rpc_server * server = vserver;

    if( !server->isEnabled )
        stopServer( server );
    else
        startServer( server );
}

void
tr_rpcSetEnabled( tr_rpc_server * server,
                  bool            isEnabled )
{
    server->isEnabled = isEnabled;

    tr_runInEventThread( server->session, onEnabledChanged, server );
}

bool
tr_rpcIsEnabled( const tr_rpc_server * server )
{
    return server->isEnabled;
}

static void
restartServer( void * vserver )
{
    tr_rpc_server * server = vserver;

    if( server->isEnabled )
    {
        stopServer( server );
        startServer( server );
    }
}

void
tr_rpcSetPort( tr_rpc_server * server,
               tr_port         port )
{
    assert( server != NULL );

    if( server->port != port )
    {
        server->port = port;

        if( server->isEnabled )
            tr_runInEventThread( server->session, restartServer, server );
    }
}

tr_port
tr_rpcGetPort( const tr_rpc_server * server )
{
    return server->port;
}

void
tr_rpcSetUrl( tr_rpc_server * server, const char * url )
{
    char * tmp = server->url;
    server->url = tr_strdup( url );
    dbgmsg( "setting our URL to [%s]", server->url );
    tr_free( tmp );
}

const char*
tr_rpcGetUrl( const tr_rpc_server * server )
{
    return server->url ? server->url : "";
}

void
tr_rpcSetWhitelist( tr_rpc_server * server, const char * whitelistStr )
{
    void * tmp;
    const char * walk;

    /* keep the string */
    tmp = server->whitelistStr;
    server->whitelistStr = tr_strdup( whitelistStr );
    tr_free( tmp );

    /* clear out the old whitelist entries */
    while(( tmp = tr_list_pop_front( &server->whitelist )))
        tr_free( tmp );

    /* build the new whitelist entries */
    for( walk=whitelistStr; walk && *walk; ) {
        const char * delimiters = " ,;";
        const size_t len = strcspn( walk, delimiters );
        char * token = tr_strndup( walk, len );
        tr_list_append( &server->whitelist, token );
        if( strcspn( token, "+-" ) < len )
            tr_ninf( MY_NAME, "Adding address to whitelist: %s (And it has a '+' or '-'!  Are you using an old ACL by mistake?)", token );
        else
            tr_ninf( MY_NAME, "Adding address to whitelist: %s", token );

        if( walk[len]=='\0' )
            break;
        walk += len + 1;
    }
}

const char*
tr_rpcGetWhitelist( const tr_rpc_server * server )
{
    return server->whitelistStr ? server->whitelistStr : "";
}

void
tr_rpcSetWhitelistEnabled( tr_rpc_server  * server,
                           bool             isEnabled )
{
    server->isWhitelistEnabled = isEnabled != 0;
}

bool
tr_rpcGetWhitelistEnabled( const tr_rpc_server * server )
{
    return server->isWhitelistEnabled;
}

/****
*****  PASSWORD
****/

void
tr_rpcSetUsername( tr_rpc_server * server, const char * username )
{
    char * tmp = server->username;
    server->username = tr_strdup( username );
    dbgmsg( "setting our Username to [%s]", server->username );
    tr_free( tmp );
}

const char*
tr_rpcGetUsername( const tr_rpc_server * server )
{
    return server->username ? server->username : "";
}

void
tr_rpcSetPassword( tr_rpc_server * server,
                   const char *    password )
{
    tr_free( server->password );
    if( *password != '{' )
        server->password = tr_ssha1( password );
    else
        server->password = strdup( password );
    dbgmsg( "setting our Password to [%s]", server->password );
}

const char*
tr_rpcGetPassword( const tr_rpc_server * server )
{
    return server->password ? server->password : "" ;
}

void
tr_rpcSetPasswordEnabled( tr_rpc_server * server, bool isEnabled )
{
    server->isPasswordEnabled = isEnabled;
    dbgmsg( "setting 'password enabled' to %d", (int)isEnabled );
}

bool
tr_rpcIsPasswordEnabled( const tr_rpc_server * server )
{
    return server->isPasswordEnabled;
}

const char *
tr_rpcGetBindAddress( const tr_rpc_server * server )
{
    tr_address addr;
    addr.type = TR_AF_INET;
    addr.addr.addr4 = server->bindAddress;
    return tr_address_to_string( &addr );
}

/****
*****  LIFE CYCLE
****/

static void
closeServer( void * vserver )
{
    void * tmp;
    tr_rpc_server * s = vserver;

    stopServer( s );
    while(( tmp = tr_list_pop_front( &s->whitelist )))
        tr_free( tmp );
#ifdef HAVE_ZLIB
    if( s->isStreamInitialized )
        deflateEnd( &s->stream );
#endif
    tr_free( s->url );
    tr_free( s->sessionId );
    tr_free( s->whitelistStr );
    tr_free( s->username );
    tr_free( s->password );
    tr_free( s );
}

void
tr_rpcClose( tr_rpc_server ** ps )
{
    tr_runInEventThread( ( *ps )->session, closeServer, *ps );
    *ps = NULL;
}

tr_rpc_server *
tr_rpcInit( tr_session  * session, tr_benc * settings )
{
    tr_rpc_server * s;
    bool boolVal;
    int64_t i;
    const char * str;
    const char * key;
    tr_address address;

    s = tr_new0( tr_rpc_server, 1 );
    s->session = session;

    key = TR_PREFS_KEY_RPC_ENABLED;
    if( !tr_bencDictFindBool( settings, key, &boolVal ) )
        tr_nerr( MY_NAME, _( "Couldn't find settings key \"%s\"" ), key );
    else
        s->isEnabled = boolVal;

    key = TR_PREFS_KEY_RPC_PORT;
    if( !tr_bencDictFindInt( settings, key, &i ) )
        tr_nerr( MY_NAME, _( "Couldn't find settings key \"%s\"" ), key );
    else
        s->port = i;

    key = TR_PREFS_KEY_RPC_URL;
    if( !tr_bencDictFindStr( settings, TR_PREFS_KEY_RPC_URL, &str ) )
        tr_nerr( MY_NAME, _( "Couldn't find settings key \"%s\"" ), key );
    else
        s->url = tr_strdup( str );

    key = TR_PREFS_KEY_RPC_WHITELIST_ENABLED;
    if( !tr_bencDictFindBool( settings, key, &boolVal ) )
        tr_nerr( MY_NAME, _( "Couldn't find settings key \"%s\"" ), key );
    else
        tr_rpcSetWhitelistEnabled( s, boolVal );

    key = TR_PREFS_KEY_RPC_AUTH_REQUIRED;
    if( !tr_bencDictFindBool( settings, key, &boolVal ) )
        tr_nerr( MY_NAME, _( "Couldn't find settings key \"%s\"" ), key );
    else
        tr_rpcSetPasswordEnabled( s, boolVal );

    key = TR_PREFS_KEY_RPC_WHITELIST;
    if( !tr_bencDictFindStr( settings, key, &str ) && str )
        tr_nerr( MY_NAME, _( "Couldn't find settings key \"%s\"" ), key );
    else
        tr_rpcSetWhitelist( s, str );

    key = TR_PREFS_KEY_RPC_USERNAME;
    if( !tr_bencDictFindStr( settings, key, &str ) )
        tr_nerr( MY_NAME, _( "Couldn't find settings key \"%s\"" ), key );
    else
        tr_rpcSetUsername( s, str );

    key = TR_PREFS_KEY_RPC_PASSWORD;
    if( !tr_bencDictFindStr( settings, key, &str ) )
        tr_nerr( MY_NAME, _( "Couldn't find settings key \"%s\"" ), key );
    else
        tr_rpcSetPassword( s, str );

    key = TR_PREFS_KEY_RPC_BIND_ADDRESS;
    if( !tr_bencDictFindStr( settings, TR_PREFS_KEY_RPC_BIND_ADDRESS, &str ) ) {
        tr_nerr( MY_NAME, _( "Couldn't find settings key \"%s\"" ), key );
        address = tr_inaddr_any;
    } else if( !tr_address_from_string( &address, str ) ) {
        tr_nerr( MY_NAME, _( "%s is not a valid address" ), str );
        address = tr_inaddr_any;
    } else if( address.type != TR_AF_INET ) {
        tr_nerr( MY_NAME, _( "%s is not an IPv4 address. RPC listeners must be IPv4" ), str );
        address = tr_inaddr_any;
    }
    s->bindAddress = address.addr.addr4;

    if( s->isEnabled )
    {
        tr_ninf( MY_NAME, _( "Serving RPC and Web requests on port 127.0.0.1:%d%s" ), (int) s->port, s->url );
        tr_runInEventThread( session, startServer, s );

        if( s->isWhitelistEnabled )
            tr_ninf( MY_NAME, "%s", _( "Whitelist enabled" ) );

        if( s->isPasswordEnabled )
            tr_ninf( MY_NAME, "%s", _( "Password required" ) );
    }

    return s;
}
