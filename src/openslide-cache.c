/*
 *  OpenSlide, a library for reading whole slide image files
 *
 *  Copyright (c) 2007-2008 Carnegie Mellon University
 *  All rights reserved.
 *
 *  OpenSlide is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 2.
 *
 *  OpenSlide is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with OpenSlide. If not, see <http://www.gnu.org/licenses/>.
 *
 *  Linking OpenSlide statically or dynamically with other modules is
 *  making a combined work based on OpenSlide. Thus, the terms and
 *  conditions of the GNU General Public License cover the whole
 *  combination.
 */

#include <openslide-cache.h>
#include <glib.h>

struct _openslide_cache_key {
  int64_t x;
  int64_t y;
  int32_t layer;
};

struct _openslide_cache_value {
  GList *link;            // direct pointer to the node in the list
  struct _openslide_cache_key *key; // for removing keys when aged out
  struct _openslide_cache *cache; // sadly, for total_bytes and the list

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
    struct _openslide_cache_value *value = g_queue_peek_tail(cache->list);
    struct _openslide_cache_key *key = value->key;

    g_debug("EVICT: size: %d", value->size);

    size -= value->size;

    // remove from hashtable, this will trigger removal from everything
    g_assert(g_hash_table_remove(cache->hashtable, key));
  }
}


// hash function helpers
static guint hash_func(gconstpointer key) {
  const struct _openslide_cache_key *c_key = key;

  // assume 32-bit hash

  // take the top 4 bits for layer, then 14 bits per x and y,
  // xor it all together
  return (c_key->layer << 28) ^ (c_key->y << 14) ^ (c_key->x);
}

static gboolean key_equal_func(gconstpointer a,
			       gconstpointer b) {
  const struct _openslide_cache_key *c_a = a;
  const struct _openslide_cache_key *c_b = b;

  return (c_a->x == c_b->x) && (c_a->y == c_b->y) &&
    (c_a->layer == c_b->layer);
}

static void hash_destroy_key(gpointer data) {
  g_slice_free(struct _openslide_cache_key, data);
}

static void hash_destroy_value(gpointer data) {
  struct _openslide_cache_value *value = data;

  // remove the item from the list
  g_queue_delete_link(value->cache->list, value->link);

  // free the data
  g_free(value->data);

  // decrement the total size
  value->cache->total_size -= value->size;
  g_assert(value->cache->total_size >= 0);

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
  // clear list
  g_queue_free(cache->list);

  // clear hashtable (auto-deletes all data)
  g_hash_table_unref(cache->hashtable);

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
void _openslide_cache_put(struct _openslide_cache *cache,
			  int64_t x,
			  int64_t y,
			  int32_t layer,
			  void *data,
			  int size_in_bytes) {
  possibly_evict(cache, size_in_bytes); // already checks for size >= 0

  // create key
  struct _openslide_cache_key *key = g_slice_new(struct _openslide_cache_key);
  key->x = x;
  key->y = y;
  key->layer = layer;

  // create value
  struct _openslide_cache_value *value =
    g_slice_new(struct _openslide_cache_value);
  value->key = key;
  value->cache = cache;
  value->data = data;
  value->size = size_in_bytes;

  // insert at head of queue
  g_queue_push_head(cache->list, value);
  value->link = g_queue_peek_head_link(cache->list);

  // insert into hash table
  g_hash_table_replace(cache->hashtable, key, value);

  // increase size
  cache->total_size += size_in_bytes;

  // possibly evict once more, this will auto-delete anything too big for
  // the entire cache
  possibly_evict(cache, 0);
}

void *_openslide_cache_get(struct _openslide_cache *cache,
			   int64_t x,
			   int64_t y,
			   int32_t layer) {
  // create key
  struct _openslide_cache_key key = { .x = x, .y = y, .layer = layer };

  // lookup key, maybe return NULL
  struct _openslide_cache_value *value = g_hash_table_lookup(cache->hashtable,
							     &key);
  if (value == NULL) {
    return NULL;
  }

  // if found, move to front of list
  GList *link = value->link;
  g_queue_unlink(cache->list, link);
  g_queue_push_head_link(cache->list, link);

  // return data
  return value->data;
}
