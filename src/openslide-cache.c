/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2009 Carnegie Mellon University
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

#include <config.h>

#include <openslide-cache.h>
#include <glib.h>

// hash table key
struct _openslide_cache_key {
  int64_t x;
  int64_t y;
  int32_t level;
};

// hash table value
struct _openslide_cache_value {
  GList *link;            // direct pointer to the node in the list
  struct _openslide_cache_key *key; // for removing keys when aged out
  struct _openslide_cache *cache; // sadly, for total_bytes and the list

  struct _openslide_cache_entry *entry;  // may outlive the value
};

// datum
struct _openslide_cache_entry {
  gint refcount;  // atomic ops only
  void *data;
  int size;
};

struct _openslide_cache {
  GQueue *list;
  GHashTable *hashtable;

  int capacity;
  int total_size;
};

// eviction
static void possibly_evict(struct _openslide_cache *cache, int incoming_size) {
  g_assert(incoming_size >= 0);

  int size = cache->total_size + incoming_size;
  int target = cache->capacity;

  while(size > target) {
    // get key of last element
    struct _openslide_cache_value *value =
      (struct _openslide_cache_value *) g_queue_peek_tail(cache->list);
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
  const struct _openslide_cache_key *c_key = (const struct _openslide_cache_key *) key;

  // assume 32-bit hash

  // take the top 4 bits for level, then 14 bits per x and y,
  // xor it all together
  return (guint) ((c_key->level << 28) ^ (c_key->y << 14) ^ (c_key->x));
}

static gboolean key_equal_func(gconstpointer a,
			       gconstpointer b) {
  const struct _openslide_cache_key *c_a = (const struct _openslide_cache_key *) a;
  const struct _openslide_cache_key *c_b = (const struct _openslide_cache_key *) b;

  return (c_a->x == c_b->x) && (c_a->y == c_b->y) &&
    (c_a->level == c_b->level);
}

static void hash_destroy_key(gpointer data) {
  g_slice_free(struct _openslide_cache_key, data);
}

static void hash_destroy_value(gpointer data) {
  struct _openslide_cache_value *value = (struct _openslide_cache_value *) data;

  // remove the item from the list
  g_queue_delete_link(value->cache->list, value->link);

  // decrement the total size
  value->cache->total_size -= value->entry->size;
  g_assert(value->cache->total_size >= 0);

  // unref the entry
  _openslide_cache_entry_unref(value->entry);

  // free the value
  g_slice_free(struct _openslide_cache_value, value);
}

struct _openslide_cache *_openslide_cache_create(int capacity_in_bytes) {
  struct _openslide_cache *cache = g_slice_new0(struct _openslide_cache);

  // init queue
  cache->list = g_queue_new();

  // init hashtable
  cache->hashtable = g_hash_table_new_full(hash_func,
					   key_equal_func,
					   hash_destroy_key,
					   hash_destroy_value);

  // init byte_capacity
  cache->capacity = capacity_in_bytes;

  return cache;
}

void _openslide_cache_destroy(struct _openslide_cache *cache) {
  // clear hashtable (auto-deletes all data)
  g_hash_table_unref(cache->hashtable);

  // clear list
  g_queue_free(cache->list);

  // destroy struct
  g_slice_free(struct _openslide_cache, cache);
}


int _openslide_cache_get_capacity(struct _openslide_cache *cache) {
  return cache->capacity;
}

void _openslide_cache_set_capacity(struct _openslide_cache *cache,
				   int capacity_in_bytes) {
  g_assert(capacity_in_bytes >= 0);

  cache->capacity = capacity_in_bytes;
  possibly_evict(cache, 0);
}

// put and get

// the cache retains one reference, and the caller gets another one.  the
// entry must be unreffed when the caller is done with it.
void _openslide_cache_put(struct _openslide_cache *cache,
			  int64_t x,
			  int64_t y,
			  int32_t level,
			  void *data,
			  int size_in_bytes,
			  struct _openslide_cache_entry **_entry) {
  // always create cache entry for caller's reference
  struct _openslide_cache_entry *entry =
      g_slice_new(struct _openslide_cache_entry);
  // one ref for the caller
  g_atomic_int_set(&entry->refcount, 1);
  entry->data = data;
  entry->size = size_in_bytes;
  *_entry = entry;

  // don't try to put anything in the cache that cannot possibly fit
  if (size_in_bytes > cache->capacity) {
    //g_debug("refused %p", (void *) entry);
    return;
  }

  possibly_evict(cache, size_in_bytes); // already checks for size >= 0

  // create key
  struct _openslide_cache_key *key = g_slice_new(struct _openslide_cache_key);
  key->x = x;
  key->y = y;
  key->level = level;

  // create value
  struct _openslide_cache_value *value =
    g_slice_new(struct _openslide_cache_value);
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

  //g_debug("insert %p", (void *) entry);
}

// entry must be unreffed when the caller is done with the data
void *_openslide_cache_get(struct _openslide_cache *cache,
			   int64_t x,
			   int64_t y,
			   int32_t level,
			   struct _openslide_cache_entry **entry) {
  // create key
  struct _openslide_cache_key key = { x, y, level };

  // lookup key, maybe return NULL
  struct _openslide_cache_value *value =
    (struct _openslide_cache_value *) g_hash_table_lookup(cache->hashtable,
							  &key);
  if (value == NULL) {
    *entry = NULL;
    return NULL;
  }

  // if found, move to front of list
  GList *link = value->link;
  g_queue_unlink(cache->list, link);
  g_queue_push_head_link(cache->list, link);

  // acquire entry reference for the caller
  g_atomic_int_inc(&value->entry->refcount);

  //g_debug("cache hit! %p %"G_GINT64_FORMAT" %"G_GINT64_FORMAT" %d", (void *) value->entry, x, y, level);

  // return data
  *entry = value->entry;
  return value->entry->data;
}

// value unref
// calls do not need to be serialized
void _openslide_cache_entry_unref(struct _openslide_cache_entry *entry) {
  //g_debug("unref %p, refs %d", (void *) entry, g_atomic_int_get(&entry->refcount));

  if (g_atomic_int_dec_and_test(&entry->refcount)) {
    // free the data
    g_slice_free1(entry->size, entry->data);

    // free the entry
    g_slice_free(struct _openslide_cache_entry, entry);

    //g_debug("free %p", (void *) entry);
  }
}
