/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2012 Carnegie Mellon University
 *  Copyright (c) 2021      Benjamin Gilbert
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, version 2.1.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with OpenSlide. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "openslide-private.h"

#include <glib.h>

#define DEFAULT_CACHE_SIZE (1024*1024*32)

// hash table key
struct _openslide_cache_key {
  uint64_t binding_id;  // distinguishes values from different slide handles
  void *plane;  // cookie for coordinate plane (level, grid, etc.)
  int64_t x;
  int64_t y;
};

// hash table value
struct _openslide_cache_value {
  GList *link;            // direct pointer to the node in the list
  struct _openslide_cache_key *key; // for removing keys when aged out
  openslide_cache_t *cache; // sadly, for total_bytes and the list

  struct _openslide_cache_entry *entry;  // may outlive the value
};

// datum
struct _openslide_cache_entry {
  gint refcount;  // atomic ops only
  void *data;
  uint64_t size;
};

struct _openslide_cache {
  GMutex mutex;
  GQueue *list;
  GHashTable *hashtable;

  int refcount;
  bool released;
  uint64_t next_binding_id;

  uint64_t capacity;
  uint64_t total_size;

  gint warned_overlarge_entry;
};

// connection between a cache (possibly shared between multiple slide handles)
// and a specific slide handle
struct _openslide_cache_binding {
  GMutex mutex;
  openslide_cache_t *cache;
  uint64_t id;  // unique id assigned by cache upon bind
};

// eviction
// mutex must be held
static void possibly_evict(openslide_cache_t *cache, uint64_t incoming_size) {
  uint64_t size = cache->total_size + incoming_size;
  uint64_t target = cache->capacity;
  g_assert(size > cache->total_size);

  while(size > target) {
    // get key of last element
    struct _openslide_cache_value *value = g_queue_peek_tail(cache->list);
    if (value == NULL) {
      return; // cache is empty
    }
    struct _openslide_cache_key *key = value->key;

    //g_debug("EVICT: size: %d", value->entry->size);

    size -= value->entry->size;

    // remove from hashtable, this will trigger removal from everything
    bool result = g_hash_table_remove(cache->hashtable, key);
    g_assert(result);
  }
}


// hash function helpers
static guint hash_func(gconstpointer key) {
  const struct _openslide_cache_key *c_key = key;

  // assume 32-bit hash
  return (guint) ((c_key->binding_id << 16) ^
                  ((guintptr) c_key->plane) ^
                  ((34369 * (uint64_t) c_key->y) + ((uint64_t) c_key->x)));
}

static gboolean key_equal_func(gconstpointer a,
			       gconstpointer b) {
  const struct _openslide_cache_key *c_a = a;
  const struct _openslide_cache_key *c_b = b;

  return (c_a->binding_id == c_b->binding_id) &&
    (c_a->plane == c_b->plane) &&
    (c_a->x == c_b->x) &&
    (c_a->y == c_b->y);
}

static void hash_destroy_value(gpointer data) {
  struct _openslide_cache_value *value = data;

  // remove the item from the list
  g_queue_delete_link(value->cache->list, value->link);

  // decrement the total size
  g_assert(value->entry->size <= value->cache->total_size);
  value->cache->total_size -= value->entry->size;

  // unref the entry
  _openslide_cache_entry_unref(value->entry);

  // free the value
  g_free(value);
}

openslide_cache_t *_openslide_cache_create(uint64_t capacity_in_bytes) {
  openslide_cache_t *cache = g_new0(openslide_cache_t, 1);

  // init mutex
  g_mutex_init(&cache->mutex);

  // init queue
  cache->list = g_queue_new();

  // init hashtable
  cache->hashtable = g_hash_table_new_full(hash_func,
					   key_equal_func,
					   g_free,
					   hash_destroy_value);

  // init refcount
  cache->refcount = 1;

  // init byte_capacity
  cache->capacity = capacity_in_bytes;

  return cache;
}

static void cache_ref(openslide_cache_t *cache) {
  g_mutex_lock(&cache->mutex);
  cache->refcount++;
  g_mutex_unlock(&cache->mutex);
}

static void cache_unref(openslide_cache_t *cache) {
  g_mutex_lock(&cache->mutex);
  // decrement refcount, return if references remain
  if (--cache->refcount) {
    g_mutex_unlock(&cache->mutex);
    return;
  }
  // clear hashtable (auto-deletes all data)
  g_hash_table_unref(cache->hashtable);
  g_mutex_unlock(&cache->mutex);

  // clear list
  g_queue_free(cache->list);

  // free mutex
  g_mutex_clear(&cache->mutex);

  // destroy struct
  g_free(cache);
}

void _openslide_cache_release(openslide_cache_t *cache) {
  g_mutex_lock(&cache->mutex);
  bool already_released = cache->released;
  cache->released = true;
  g_mutex_unlock(&cache->mutex);
  g_return_if_fail(!already_released);

  cache_unref(cache);
}

struct _openslide_cache_binding *_openslide_cache_binding_create(void) {
  struct _openslide_cache_binding *cb =
    g_new0(struct _openslide_cache_binding, 1);
  g_mutex_init(&cb->mutex);
  cb->cache = _openslide_cache_create(DEFAULT_CACHE_SIZE);
  cb->id = cb->cache->next_binding_id++;
  return cb;
}

void _openslide_cache_binding_set(struct _openslide_cache_binding *cb,
                                  openslide_cache_t *cache) {
  cache_ref(cache);

  g_mutex_lock(&cache->mutex);
  uint64_t id = cache->next_binding_id++;
  g_mutex_unlock(&cache->mutex);

  g_mutex_lock(&cb->mutex);
  openslide_cache_t *old = cb->cache;
  cb->cache = cache;
  cb->id = id;
  g_mutex_unlock(&cb->mutex);

  cache_unref(old);
}

void _openslide_cache_binding_destroy(struct _openslide_cache_binding *cb) {
  g_mutex_lock(&cb->mutex);
  cache_unref(cb->cache);
  g_mutex_unlock(&cb->mutex);

  g_mutex_clear(&cb->mutex);
  g_free(cb);
}

// put and get

// the cache retains one reference, and the caller gets another one.  the
// entry must be unreffed when the caller is done with it.
void _openslide_cache_put(struct _openslide_cache_binding *cb,
			  void *plane,
			  int64_t x,
			  int64_t y,
			  void *data,
			  uint64_t size_in_bytes,
			  struct _openslide_cache_entry **_entry) {
  // always create cache entry for caller's reference
  struct _openslide_cache_entry *entry =
      g_new(struct _openslide_cache_entry, 1);
  // one ref for the caller
  g_atomic_int_set(&entry->refcount, 1);
  entry->data = data;
  entry->size = size_in_bytes;
  *_entry = entry;

  // get cache and lock
  g_mutex_lock(&cb->mutex);
  openslide_cache_t *cache = cb->cache;
  g_mutex_lock(&cache->mutex);

  // don't try to put anything in the cache that cannot possibly fit
  if (size_in_bytes > cache->capacity) {
    //g_debug("refused %p", entry);
    g_mutex_unlock(&cache->mutex);
    _openslide_performance_warn_once(&cache->warned_overlarge_entry,
                                     "Rejecting overlarge cache entry of "
                                     "size %"PRIu64" bytes", size_in_bytes);
    g_mutex_unlock(&cb->mutex);
    return;
  }

  possibly_evict(cache, size_in_bytes); // already checks for wraparound

  // create key
  struct _openslide_cache_key *key = g_new(struct _openslide_cache_key, 1);
  key->binding_id = cb->id;
  key->plane = plane;
  key->x = x;
  key->y = y;

  // create value
  struct _openslide_cache_value *value =
    g_new(struct _openslide_cache_value, 1);
  value->key = key;
  value->cache = cache;
  value->entry = entry;

  // insert at head of queue
  g_queue_push_head(cache->list, value);
  value->link = g_queue_peek_head_link(cache->list);

  // insert into hash table
  g_hash_table_replace(cache->hashtable, key, value);

  // increase size
  cache->total_size += size_in_bytes;

  // another ref for the cache
  g_atomic_int_inc(&entry->refcount);

  // unlock
  g_mutex_unlock(&cache->mutex);
  g_mutex_unlock(&cb->mutex);

  //g_debug("insert %p", entry);
}

// entry must be unreffed when the caller is done with the data
void *_openslide_cache_get(struct _openslide_cache_binding *cb,
			   void *plane,
			   int64_t x,
			   int64_t y,
			   struct _openslide_cache_entry **_entry) {
  // get cache and lock
  g_mutex_lock(&cb->mutex);
  openslide_cache_t *cache = cb->cache;
  g_mutex_lock(&cache->mutex);

  // create key
  struct _openslide_cache_key key = {
    .binding_id = cb->id,
    .plane = plane,
    .x = x,
    .y = y
  };

  // lookup key, maybe return NULL
  struct _openslide_cache_value *value = g_hash_table_lookup(cache->hashtable,
							     &key);
  if (value == NULL) {
    g_mutex_unlock(&cache->mutex);
    g_mutex_unlock(&cb->mutex);
    *_entry = NULL;
    return NULL;
  }

  // if found, move to front of list
  GList *link = value->link;
  g_queue_unlink(cache->list, link);
  g_queue_push_head_link(cache->list, link);

  // acquire entry reference for the caller
  struct _openslide_cache_entry *entry = value->entry;
  g_atomic_int_inc(&entry->refcount);

  //g_debug("cache hit! %p %"PRIu64" %p %"PRId64" %"PRId64, (void *) entry, cb->id, (void *) plane, x, y);

  // unlock
  g_mutex_unlock(&cache->mutex);
  g_mutex_unlock(&cb->mutex);

  // return data
  *_entry = entry;
  return entry->data;
}

// value unref
void _openslide_cache_entry_unref(struct _openslide_cache_entry *entry) {
  //g_debug("unref %p, refs %d", entry, g_atomic_int_get(&entry->refcount));

  if (g_atomic_int_dec_and_test(&entry->refcount)) {
    // free the data
    g_free(entry->data);

    // free the entry
    g_free(entry);

    //g_debug("free %p", entry);
  }
}
