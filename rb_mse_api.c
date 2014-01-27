// rb_mse_api.c


#include "rb_mse_api.h"

#include "librd/rdmem.h"
#include "librd/rdavl.h"
#include "librd/rdstring.h"
#include "librd/rdlog.h"
#include "jansson.h"
#include "strbuffer.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/queue.h>

static const char mse_api_call_url[] = "api/contextaware/v1/location/clients";

static pthread_mutex_t curl_global_mutex = PTHREAD_MUTEX_INITIALIZER;

static inline uint64_t mac_from_str(const char *mac)
{
  uint64_t one=0,two=0,three=0,four=0,five=0,six=0;
  sscanf(mac,"%lx:%lx:%lx:%lx:%lx:%lx",&one,&two,&three,&four,&five,&six);
  return (((((((((one<<8)+two)<<8)+three)<<8)+four)<<8)+five)<<8)+six;
}


/* ============================================================ *
 *                               rb_mse_api_pos
 * ============================================================ */
 
struct rb_mse_api_pos{
  const char * floor;
  const char * build;
  const char * zone;
};


void rb_mse_pos_destroy(struct rb_mse_api_pos *pos){
  free(pos);
}

const char * rb_mse_pos_floor(const struct rb_mse_api_pos *pos){return pos->floor;}
const char * rb_mse_pos_build(const struct rb_mse_api_pos *pos){return pos->build;}
const char * rb_mse_pos_zone(const struct rb_mse_api_pos *pos){return pos->zone;}



/* ====================================================================== *
 *                             Positions nodes
 * ====================================================================== */

#define MSE_POSITION_LIST_MAGIC 0x12345678

struct mse_positions_list_node{
#ifdef MSE_POSITION_LIST_MAGIC
  uint64_t magic;
#endif
  uint64_t mac;
  struct rb_mse_api_pos * position;
  rd_avl_node_t rd_avl_node;
};

struct rb_mse_api_pos * mse_position(struct mse_positions_list_node *node)
{
  return node->position;
}

int mse_positions_cmp(const void *_node1,const void *_node2)
{
  const struct mse_positions_list_node * node1 = (const struct mse_positions_list_node *)_node1;
  const struct mse_positions_list_node * node2 = (const struct mse_positions_list_node *)_node2;
#ifdef MSE_POSITION_LIST_MAGIC
  assert(_node1!=NULL);
  assert(_node2!=NULL);
  assert(MSE_POSITION_LIST_MAGIC == node1->magic);
  assert(MSE_POSITION_LIST_MAGIC == node2->magic);
#endif
  return node2->mac > node1->mac ? 1 : node2->mac < node1->mac ? -1 : 0; // sizeof(mac1-mac2)>sizeof(int)
}


/* ======================================================================= *
 *                     mse_api structs definitions
 * ======================================================================= */

struct rb_mse_api
{
  // MSE update thread.
  rd_thread_t * rdt;
  /// Curl handler.
  CURL *hnd;
  char * mse_url;
  char * userpwd;

  /// Message lists queue.
  strbuffer_t buffer;
  struct curl_slist * slist;

  /// MACs positions avl
  rd_rwlock_t avl_memctx_rwlock;
  time_t update_time;
  json_t *root;

  rd_avl_t *avl;
  rd_memctx_t memctx;
  
  json_error_t error;
};

/*
 *                               CURL CALLBACKS
 */

static size_t write_function( char *ptr, size_t size, size_t nmemb, void *userdata)
{
  assert(userdata);
  struct rb_mse_api * rb_mse = (struct rb_mse_api *)userdata;

  const int ret = strbuffer_append_bytes(&rb_mse->buffer,ptr,nmemb*size);

  return ret==0 ? size*nmemb : 0;
}

#if 0
static size_t header_function( char * ptr,size_t size, size_t nmemb, void* userdata){
  (void)ptr;
  (void)userdata;
  return size*nmemb; // Bypass the headers
}
#endif


/*
 *                                 rb_mse API
 */

static void curl_setopts(CURL * hnd)
{
  // curl_easy_setopt(hnd, CURLOPT_HEADERFUNCTION, header_function); /* function called for each header received */ 

  curl_easy_setopt(hnd, CURLOPT_PROXY, NULL);
  curl_easy_setopt(hnd, CURLOPT_NOPROGRESS, 1);
  curl_easy_setopt(hnd, CURLOPT_HEADER, 0 /*"Accept: application/json"*/);
  curl_easy_setopt(hnd, CURLOPT_FAILONERROR, 0);
  curl_easy_setopt(hnd, CURLOPT_UPLOAD, 0);
  curl_easy_setopt(hnd, CURLOPT_DIRLISTONLY, 0);
  curl_easy_setopt(hnd, CURLOPT_APPEND, 0);
  curl_easy_setopt(hnd, CURLOPT_NETRC, 0);
  curl_easy_setopt(hnd, CURLOPT_FOLLOWLOCATION, 0);
  curl_easy_setopt(hnd, CURLOPT_UNRESTRICTED_AUTH, 0);
  curl_easy_setopt(hnd, CURLOPT_TRANSFERTEXT, 0);
  curl_easy_setopt(hnd, CURLOPT_PROXYUSERPWD, NULL);
  curl_easy_setopt(hnd, CURLOPT_NOPROXY, NULL);
  curl_easy_setopt(hnd, CURLOPT_RANGE, NULL);
  /* curl_easy_setopt(hnd, CURLOPT_ERRORBUFFER, 0x7fff57485c50); [REMARK] */
  curl_easy_setopt(hnd, CURLOPT_TIMEOUT, 0);
  curl_easy_setopt(hnd, CURLOPT_REFERER, NULL);
  curl_easy_setopt(hnd, CURLOPT_AUTOREFERER, 0);
  curl_easy_setopt(hnd, CURLOPT_USERAGENT, "curl/7.19.7 (x86_64-redhat-linux-gnu) libcurl/7.23.1 OpenSSL/1.0.1c zlib/1.2.6");
  curl_easy_setopt(hnd, CURLOPT_FTPPORT, NULL);
  curl_easy_setopt(hnd, CURLOPT_LOW_SPEED_LIMIT, 0);
  curl_easy_setopt(hnd, CURLOPT_LOW_SPEED_TIME, 0);
  curl_easy_setopt(hnd, CURLOPT_MAX_SEND_SPEED_LARGE, (curl_off_t)0);
  curl_easy_setopt(hnd, CURLOPT_MAX_RECV_SPEED_LARGE, (curl_off_t)0);
  curl_easy_setopt(hnd, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)0);
  curl_easy_setopt(hnd, CURLOPT_COOKIE, NULL);
  curl_easy_setopt(hnd, CURLOPT_SSLCERT, NULL);
  curl_easy_setopt(hnd, CURLOPT_SSLCERTTYPE, NULL);
  curl_easy_setopt(hnd, CURLOPT_SSLKEY, NULL);
  curl_easy_setopt(hnd, CURLOPT_SSLKEYTYPE, NULL);
  curl_easy_setopt(hnd, CURLOPT_KEYPASSWD, NULL);
  curl_easy_setopt(hnd, CURLOPT_SSH_PRIVATE_KEYFILE, NULL);
  curl_easy_setopt(hnd, CURLOPT_SSH_PUBLIC_KEYFILE, NULL);
  curl_easy_setopt(hnd, CURLOPT_SSH_HOST_PUBLIC_KEY_MD5, NULL);
  curl_easy_setopt(hnd, CURLOPT_SSL_VERIFYPEER, 0);
  curl_easy_setopt(hnd, CURLOPT_SSL_VERIFYHOST, 0);
  curl_easy_setopt(hnd, CURLOPT_MAXREDIRS, 50);
  curl_easy_setopt(hnd, CURLOPT_CRLF, 0);
  curl_easy_setopt(hnd, CURLOPT_QUOTE, NULL);
  curl_easy_setopt(hnd, CURLOPT_POSTQUOTE, NULL);
  curl_easy_setopt(hnd, CURLOPT_PREQUOTE, NULL);
  curl_easy_setopt(hnd, CURLOPT_WRITEHEADER, NULL);
  curl_easy_setopt(hnd, CURLOPT_COOKIEFILE, NULL);
  curl_easy_setopt(hnd, CURLOPT_COOKIESESSION, 0);
  curl_easy_setopt(hnd, CURLOPT_SSLVERSION, 0);
  curl_easy_setopt(hnd, CURLOPT_TIMECONDITION, 0);
  curl_easy_setopt(hnd, CURLOPT_TIMEVALUE, 0);
  curl_easy_setopt(hnd, CURLOPT_CUSTOMREQUEST, NULL);
  /* curl_easy_setopt(hnd, CURLOPT_STDERR, 0x7f2abcc03860); [REMARK] */
  curl_easy_setopt(hnd, CURLOPT_HTTPPROXYTUNNEL, 0);
  curl_easy_setopt(hnd, CURLOPT_INTERFACE, NULL);
  curl_easy_setopt(hnd, CURLOPT_KRBLEVEL, NULL);
  curl_easy_setopt(hnd, CURLOPT_TELNETOPTIONS, NULL);
  curl_easy_setopt(hnd, CURLOPT_RANDOM_FILE, NULL);
  curl_easy_setopt(hnd, CURLOPT_EGDSOCKET, NULL);
  curl_easy_setopt(hnd, CURLOPT_CONNECTTIMEOUT, 0);
  curl_easy_setopt(hnd, CURLOPT_ENCODING, NULL);
  curl_easy_setopt(hnd, CURLOPT_FTP_CREATE_MISSING_DIRS, 0);
  curl_easy_setopt(hnd, CURLOPT_IPRESOLVE, 0);
  curl_easy_setopt(hnd, CURLOPT_FTP_ACCOUNT, NULL);
  curl_easy_setopt(hnd, CURLOPT_IGNORE_CONTENT_LENGTH, 0);
  curl_easy_setopt(hnd, CURLOPT_FTP_SKIP_PASV_IP, 0);
  curl_easy_setopt(hnd, CURLOPT_FTP_FILEMETHOD, 0);
  curl_easy_setopt(hnd, CURLOPT_FTP_ALTERNATIVE_TO_USER, NULL);
  curl_easy_setopt(hnd, CURLOPT_SSL_SESSIONID_CACHE, 1);
  /* curl_easy_setopt(hnd, CURLOPT_SOCKOPTFUNCTION, 0x405c90); [REMARK] */
  /* curl_easy_setopt(hnd, CURLOPT_SOCKOPTDATA, 0x7fff57485670); [REMARK] */
  curl_easy_setopt(hnd, CURLOPT_POSTREDIR, 0);
}

static void *rb_mse_autoupdate(void *rb_mse); /* FW declaration */

/* Note: this function assumes rb_mse->avl_memctx_rwlock is locked */
static void rb_mse_clean(struct rb_mse_api * rb_mse)
{
  strbuffer_close(&rb_mse->buffer);
  rd_memctx_freeall(&rb_mse->memctx);
  // rd_memctx_destroy(&rb_mse->memctx);
  rd_avl_destroy(rb_mse->avl);
  rb_mse->avl = rd_avl_init(NULL,mse_positions_cmp, 0);
}



static void process_mse_response(struct rb_mse_api *rb_mse)
{
  json_error_t error;
  const char * text = strbuffer_value(&rb_mse->buffer);
  rb_mse->root = json_loads(text, 0, &error);
  if(rb_mse->root)
  {
    json_t * locations = json_object_get(rb_mse->root, "Locations");
    if(locations)
    {
      json_t * entries = json_object_get(locations, "entries");
      if(entries)
      {
        if(json_is_array(entries))
        {
          rd_rwlock_wrlock(&rb_mse->avl_memctx_rwlock);
          unsigned int i;
          for(i = 0; i < json_array_size(entries); i++)
          {
            json_t *element= json_array_get(entries, i);
            if(element && json_is_object(element))
            {
              const char * macAddress=NULL;
              json_t * json_mapHierarchyString = NULL;
              const char * mapHierarchyString=NULL;
              json_t * json_macAddress = json_object_get(element,"macAddress");

              if(json_is_string(json_macAddress))
              {
                macAddress = json_string_value(json_macAddress);
              }
              else
              {
                rdbg("Could not locate \"macAddress\" element");
              }

              if(NULL!=macAddress)
              {
                json_t * mapInfo = json_object_get(element,"MapInfo");
                if(mapInfo && json_is_object(mapInfo))
                {
                  json_mapHierarchyString = json_object_get(mapInfo,"mapHierarchyString");
                  if(json_mapHierarchyString && json_is_string(json_mapHierarchyString))
                  {
                    mapHierarchyString = json_string_value(json_mapHierarchyString);
                  }
                  else
                  {
                    // rdbg("Could not locate \"mapHierarchyString\" element (pos: %d)",i);
                  }
                }
                else
                {
                  rdbg("Could not locate \"MapInfo\" element");
                }
              }

              if(NULL!=macAddress && NULL!=mapHierarchyString)
              {
                struct mse_positions_list_node * node = rd_memctx_calloc(&rb_mse->memctx,1,sizeof(*node));
                node->position = rd_memctx_calloc(&rb_mse->memctx,1,sizeof(*node->position));
                #ifdef MSE_POSITION_LIST_MAGIC
                node->magic = MSE_POSITION_LIST_MAGIC;
                #endif
                node->mac =  mac_from_str(macAddress);
                //printf("DEBUG: macAddr: %12lx\tmacAddr: %s\n",node->mac,macAddress);
                
                char * map_string = rd_memctx_strdup(&rb_mse->memctx,mapHierarchyString); // Will free() with pos
                char * aux;
                node->position->zone  = strtok_r(map_string,">",&aux);
                if(node->position->zone)
                  node->position->build = strtok_r(NULL,">",&aux);
                if(node->position->build)
                  node->position->floor = strtok_r(NULL,">",&aux);

                // rdbg("Inserting node %lx: %s\n",node->mac,map_string);
                RD_AVL_INSERT(rb_mse->avl,node,rd_avl_node);
              }
            }
            else
            {
              rdbg("Could not get %d element of %s",i, "entries");
            }
          } /* for */
          rd_rwlock_unlock(&rb_mse->avl_memctx_rwlock);
        }
        else
        {
          rdbg("entries is not an array");
        }
      }
    }
    // json_decref(rb_mse->root); //Don't! it will be decref in clean().
  }
  else
  {
    rdbg("Could not get root node");
  }
}

static CURLcode rb_mse_set_curl_url(struct rb_mse_api *rb_mse, bool currently_tracked, int page)
{
  CURLcode ret;
  if(rb_mse->mse_url)
  {
    const char * url_ts = rd_tsprintf("https://%s/%s?currentlyTracked=%s&page=%d",
      rb_mse->mse_url,mse_api_call_url,currently_tracked?"true":"false",page);
    rdbg("Url generated: %s",url_ts);
    if(url_ts){
      ret =  curl_easy_setopt(rb_mse->hnd, CURLOPT_URL, url_ts);
    }else{
      ret = CURLE_OUT_OF_MEMORY;
    }
  }
  else
  {
    ret = CURLE_BAD_FUNCTION_ARGUMENT;
    rdbg("MSE URL was not setted");
  }

  rd_string_thread_cleanup();
  return ret;
}

static void get_and_process_mse_response0(struct rb_mse_api *rb_mse, bool currently_tracked)
{
  bool more_pages = true;
  int current_page = 0;

  while(more_pages)
  {
    rb_mse_set_curl_url(rb_mse,currently_tracked,current_page);
    const CURLcode ret = curl_easy_perform(rb_mse->hnd);
    if(ret==CURLE_OK)
    {
      process_mse_response(rb_mse);
      more_pages = false;
    }
    else
    {
      rdbg("Cannot perform curl request: %s\n",curl_easy_strerror(ret));
    }
  }

  strbuffer_close(&rb_mse->buffer);
  strbuffer_init(&rb_mse->buffer);

  json_decref(rb_mse->root);
  rb_mse->root = NULL;
}

#define get_and_process_mse_tracked(rb_mse) \
  get_and_process_mse_response0(rb_mse, true)

#define get_and_process_mse_nontracked(rb_mse) \
  get_and_process_mse_response0(rb_mse, false)

/**
  Update all macs pos in the MSE
  Note: we expect the message like:
  {
    "Locations":{
        "totalPages":1,
        "currentPage":1,
        "pageSize":1882,
        "entries":[
            {
                "macAddress":"ab:ca:dd:fe:10:10",
                "currentlyTracked":true,
                "confidenceFactor":824.0,
                "band":"UNKNOWN",
                "isGuestUser":false,
                "dot11Status":"PROBING",
                "MapInfo":{
                    "mapHierarchyString":"Area>Build>Floor",
                    "floorRefId":4698041219291283488,
                    "Dimension":{
                        "length":5000.0,
                        "width":6900.0,
                        "height":30.0,
                        "offsetX":21360.9,
                        "offsetY":19891.6,
                        "unit":"FEET"
                    },
                    "Image":{
                        "imageName":"domain_5_1350620105694.jpg"
                    }
                },
                "MapCoordinate":{
                    "x":5275.3,
                    "y":2420.19,
                    "unit":"FEET"
                },
                "Statistics":{
                    "currentServerTime":"2013-12-11T10:00:26.650-0800",
                    "firstLocatedTime":"2013-12-10T11:47:43.172-0800",
                    "lastLocatedTime":"2013-12-11T08:54:04.312-0800"
                },
                "GeoCoordinate":{
                    "lattitude":10.1,
                    "longitude":-12.25,
                    "unit":"DEGREES"
                }
            },
          ]
    }
 */
static void rb_mse_update_macs_pos(struct rb_mse_api *rb_mse)
{
  assert(rb_mse);
  rb_mse_clean(rb_mse);

  get_and_process_mse_nontracked(rb_mse);

  // Note: If we found the same mac, tracked value will overwrite nontracked value
  get_and_process_mse_tracked(rb_mse);

  rdbg("Updated");
}


static void *rb_mse_autoupdate(void *_rb_mse)
{
  assert(_rb_mse);
  struct rb_mse_api * rb_mse = _rb_mse;
  while(rd_currthread_get()->rdt_state != RD_THREAD_S_EXITING)
  {
    rdbg("Updating\n");
    rb_mse_update_macs_pos(rb_mse);
    // rdbg("Updated. Buffer: %s\n",strbuffer_value(&rb_mse->buffer));
    sleep(rb_mse->update_time);
  }
  rd_thread_cleanup();
  return NULL;
}

static CURLcode rb_mse_set_userpwd(struct rb_mse_api *rb_mse, const char *userpwd)
{
  return curl_easy_setopt(rb_mse->hnd, CURLOPT_USERPWD, userpwd);;
}


static bool rb_mse_set_mse_addr(struct rb_mse_api *rb_mse, const char *addr)
{
  rb_mse->mse_url = strdup(addr);
  return rb_mse->mse_url != NULL;
}

/* Public API */

struct rb_mse_api * rb_mse_api_new(time_t update_time, const char *addr, const char * userpwd)
{
  struct rb_mse_api * rb_mse = calloc(1,sizeof(struct rb_mse_api));
  if(rb_mse)
  {
    rb_mse->slist = curl_slist_append(NULL, "Accept: application/json");

    pthread_mutex_lock(&curl_global_mutex);
    curl_global_init(CURL_GLOBAL_SSL);
    pthread_mutex_unlock(&curl_global_mutex);

    rb_mse->hnd = curl_easy_init();
    if(rb_mse->hnd)
    {
      rb_mse_set_mse_addr(rb_mse, addr);
      rb_mse_set_userpwd(rb_mse, userpwd);
      curl_easy_setopt(rb_mse->hnd, CURLOPT_WRITEDATA, rb_mse);               /* void passed to WRITEFUNCTION */
      curl_easy_setopt(rb_mse->hnd, CURLOPT_WRITEFUNCTION, write_function);   /* function called for each data received */ 
      curl_easy_setopt(rb_mse->hnd, CURLOPT_HTTPHEADER, rb_mse->slist);
      curl_setopts(rb_mse->hnd);
    }
    else // curl_easy_init error
    {
      free(rb_mse);
      rb_mse=NULL;
    }

    if(rb_mse)
    {
      strbuffer_init(&rb_mse->buffer);
      rd_memctx_init (&rb_mse->memctx, "rb_mse", RD_MEMCTX_F_TRACK);
      rb_mse->avl = rd_avl_init (NULL, mse_positions_cmp,0);

      rd_rwlock_init(&rb_mse->avl_memctx_rwlock);
      rb_mse->update_time = update_time;
      rd_thread_create(&rb_mse->rdt,"MSE updater",0,rb_mse_autoupdate,rb_mse);

    }
  }

  return rb_mse;
}

int rb_mse_isempty(const struct rb_mse_api *rb_mse)
{
  return rb_mse->memctx.rmc_out==0;
}


const struct rb_mse_api_pos * rb_mse_req_for_mac(struct rb_mse_api *rb_mse,const char *mac)
{
  return rb_mse_req_for_mac_i(rb_mse,mac_from_str(mac));
}

const struct rb_mse_api_pos * rb_mse_req_for_mac_i(struct rb_mse_api *rb_mse,uint64_t mac)
{
  const struct mse_positions_list_node search_node = {
    #ifdef MSE_POSITION_LIST_MAGIC
    .magic = MSE_POSITION_LIST_MAGIC,
    #endif
    .mac = mac
  };

  rd_rwlock_rdlock(&rb_mse->avl_memctx_rwlock);
  const struct mse_positions_list_node * ret_node=rd_avl_find(rb_mse->avl,&search_node,1 /* rlock */);
  rd_rwlock_unlock(&rb_mse->avl_memctx_rwlock);

  return ret_node ? ret_node->position : NULL;
}

void rb_mse_api_destroy(struct rb_mse_api * rb_mse)
{
  void * void_val;
  rd_thread_kill_join(rb_mse->rdt,&void_val);
  rb_mse_clean(rb_mse);
  rd_avl_destroy(rb_mse->avl);
  curl_slist_free_all(rb_mse->slist); /* free the list again */
  free(rb_mse->mse_url);
  curl_easy_cleanup(rb_mse->hnd);
  free(rb_mse);

  pthread_mutex_lock(&curl_global_mutex);
  curl_global_cleanup();
  pthread_mutex_unlock(&curl_global_mutex);
}

