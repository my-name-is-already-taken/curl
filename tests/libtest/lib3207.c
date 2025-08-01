/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/
#include "first.h"

#include "memdebug.h"

#ifdef USE_THREADS_POSIX
#include <pthread.h>
#endif

#include "curl_threads.h"

#define THREAD_SIZE 16
#define PER_THREAD_SIZE 8

struct Ctx {
  const char *URL;
  CURLSH *share;
  CURLcode result;
  size_t thread_id;
  struct curl_slist *contents;
};

static size_t write_memory_callback(char *contents, size_t size,
                                    size_t nmemb, void *userp)
{
  /* append the data to contents */
  size_t realsize = size * nmemb;
  struct Ctx *mem = (struct Ctx *)userp;
  char *data = (char *)malloc(realsize + 1);
  struct curl_slist *item_append = NULL;
  if(!data) {
    curl_mprintf("not enough memory (malloc returned NULL)\n");
    return 0;
  }
  memcpy(data, contents, realsize);
  data[realsize] = '\0';
  item_append = curl_slist_append(mem->contents, data);
  free(data);
  if(item_append) {
    mem->contents = item_append;
  }
  else {
    curl_mprintf("not enough memory (curl_slist_append returned NULL)\n");
    return 0;
  }
  return realsize;
}

static CURL_THREAD_RETURN_T CURL_STDCALL test_thread(void *ptr)
{
  struct Ctx *ctx = (struct Ctx *)ptr;
  CURLcode res = CURLE_OK;

  int i;

  /* Loop the transfer and cleanup the handle properly every lap. This will
     still reuse ssl session since the pool is in the shared object! */
  for(i = 0; i < PER_THREAD_SIZE; i++) {
    CURL *curl = curl_easy_init();
    if(curl) {
      curl_easy_setopt(curl, CURLOPT_URL, (char *)CURL_UNCONST(ctx->URL));

      /* use the share object */
      curl_easy_setopt(curl, CURLOPT_SHARE, ctx->share);
      curl_easy_setopt(curl, CURLOPT_CAINFO, libtest_arg2);

      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_callback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, ptr);
      curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

      /* Perform the request, res will get the return code */
      res = curl_easy_perform(curl);

      /* always cleanup */
      curl_easy_cleanup(curl);
      /* Check for errors */
      if(res != CURLE_OK) {
        curl_mfprintf(stderr, "curl_easy_perform() failed: %s\n",
                      curl_easy_strerror(res));
        goto test_cleanup;
      }
    }
  }

test_cleanup:
  ctx->result = res;
  return 0;
}

#if defined(USE_THREADS_POSIX) || defined(USE_THREADS_WIN32)

static void t3207_test_lock(CURL *handle, curl_lock_data data,
                            curl_lock_access laccess, void *useptr)
{
  curl_mutex_t *mutexes = (curl_mutex_t*) useptr;
  (void)handle;
  (void)laccess;
  Curl_mutex_acquire(&mutexes[data]);
}

static void t3207_test_unlock(CURL *handle, curl_lock_data data, void *useptr)
{
  curl_mutex_t *mutexes = (curl_mutex_t*) useptr;
  (void)handle;
  Curl_mutex_release(&mutexes[data]);
}

static void execute(CURLSH *share, struct Ctx *ctx)
{
  size_t i;
  curl_mutex_t mutexes[CURL_LOCK_DATA_LAST - 1];
  curl_thread_t thread[THREAD_SIZE];
  for(i = 0; i < CURL_ARRAYSIZE(mutexes); i++) {
    Curl_mutex_init(&mutexes[i]);
  }
  curl_share_setopt(share, CURLSHOPT_LOCKFUNC, t3207_test_lock);
  curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, t3207_test_unlock);
  curl_share_setopt(share, CURLSHOPT_USERDATA, (void *)mutexes);
  curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);

  for(i = 0; i < CURL_ARRAYSIZE(thread); i++) {
    thread[i] = Curl_thread_create(test_thread, (void *)&ctx[i]);
  }
  for(i = 0; i < CURL_ARRAYSIZE(thread); i++) {
    if(thread[i]) {
      Curl_thread_join(&thread[i]);
      Curl_thread_destroy(&thread[i]);
    }
  }
  curl_share_setopt(share, CURLSHOPT_LOCKFUNC, NULL);
  curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, NULL);
  for(i = 0; i < CURL_ARRAYSIZE(mutexes); i++) {
    Curl_mutex_destroy(&mutexes[i]);
  }
}

#else /* without pthread, run serially */

static void execute(CURLSH *share, struct Ctx *ctx)
{
  size_t i;
  (void)share;
  for(i = 0; i < THREAD_SIZE; i++) {
    test_thread((void *)&ctx[i]);
  }
}

#endif

static CURLcode test_lib3207(char *URL)
{
  CURLcode res = CURLE_OK;
  size_t i;
  CURLSH* share;
  struct Ctx ctx[THREAD_SIZE];

  curl_global_init(CURL_GLOBAL_ALL);

  share = curl_share_init();
  if(!share) {
    curl_mfprintf(stderr, "curl_share_init() failed\n");
    goto test_cleanup;
  }

  for(i = 0; i < CURL_ARRAYSIZE(ctx); i++) {
    ctx[i].share = share;
    ctx[i].URL = URL;
    ctx[i].thread_id = i;
    ctx[i].result = CURLE_OK;
    ctx[i].contents = NULL;
  }

  execute(share, ctx);

  for(i = 0; i < CURL_ARRAYSIZE(ctx); i++) {
    if(ctx[i].result) {
      res = ctx[i].result;
    }
    else {
      struct curl_slist *item = ctx[i].contents;
      while(item) {
        curl_mprintf("%s", item->data);
        item = item->next;
      }
    }
    curl_slist_free_all(ctx[i].contents);
  }

test_cleanup:
  if(share)
    curl_share_cleanup(share);
  curl_global_cleanup();
  return res;
}
