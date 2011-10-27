<?php

include_once dirname(__FILE__) . '/../mongodb.module';

/**
 * Backported from Drupal 7 ===========================================
 */

/**
 * Get the cache object for a cache bin.
 */
function mongodb_cache_get_object($bin) {
  static $cache_objects;
  if (!isset($cache_objects[$bin])) {
    $cache_objects[$bin] = new DrupalMongoDBCache($bin);
  }
  return $cache_objects[$bin];
}

/**
 * Return data from the persistent cache
 */
function cache_get($cid, $bin = 'cache') {
  return mongodb_cache_get_object($bin)->get($cid);
}

/**
 * Store data in the persistent cache.
 */
function cache_set($cid, $data, $bin = 'cache', $expire = CACHE_PERMANENT, $headers = NULL) {
  return mongodb_cache_get_object($bin)->set($cid, $data, $expire, $headers);
}

/**
 * Expire data from the cache.
 */
function cache_clear_all($cid = NULL, $bin = NULL, $wildcard = FALSE) {
  if (!isset($cid) && !isset($bin)) {
    // Clear the block cache first, so stale data will
    // not end up in the page cache.
    if (module_exists('block')) {
      cache_clear_all(NULL, 'cache_block');
    }
    cache_clear_all(NULL, 'cache_page');
    return;
  }
  return mongodb_cache_get_object($bin)->clear($cid, $wildcard);
}

/**
 * MonogoDB cache implementation.
 *
 * This is Drupal's default cache implementation. It uses the MongoDB to store
 * cached data. Each cache bin corresponds to a collection by the same name.
 */
class DrupalMongoDBCache {
  protected $bin;

  function __construct($bin) {
    $this->bin = $bin;
  }

  function get($cid) {
    $collection = mongodb_collection($this->bin);
    // Garbage collection necessary when enforcing a minimum cache lifetime.
    $this->garbageCollection($this->bin);
    $cache = $collection->findOne(array('_id' => (string)$cid));
    return $this->prepareItem($cache);
  }

  function getMultiple(&$cids) {
    $collection = mongodb_collection($this->bin);
    // Garbage collection necessary when enforcing a minimum cache lifetime.
    $this->garbageCollection($this->bin);
    $find = array();
    $find['_id']['$in'] = array_map('strval', $cids);
    $result = $collection->find($find);
    $cache = array();
    foreach ($result as $item) {
      $item = $this->prepareItem($item);
      if ($item) {
        $cache[$item->cid] = $item;
      }
    }
    $cids = array_diff($cids, array_keys($cache));
    return $cache;
  }

  /**
   * Garbage collection for get() and getMultiple().
   *
   * @param $bin
   *   The bin being requested.
   */
  protected function garbageCollection() {
    global $user;
    $collection = mongodb_collection($this->bin);

    // Garbage collection necessary when enforcing a minimum cache lifetime.
    $cache_flush = variable_get('cache_flush_' . $this->bin, 0);
    if ($cache_flush && ($cache_flush + variable_get('cache_lifetime', 0) <= $_SERVER['REQUEST_TIME'])) {
      // Reset the variable immediately to prevent a meltdown in heavy load situations.
      variable_set('cache_flush_' . $this->bin, 0);
      // Time to flush old cache data
      $find = array('expire' => array('$lte' => $cache_flush, '$ne' => CACHE_PERMANENT));
      $collection->remove($find);
    }
  }

  /**
   * Prepare a cached item.
   *
   * Checks that items are either permanent or did not expire, and unserializes
   * data as appropriate.
   *
   * @param $cache
   *   An item loaded from cache_get() or cache_get_multiple().
   * @return
   *   The item with data unserialized as appropriate or FALSE if there is no
   *   valid item to load.
   */
  protected function prepareItem($cache) {
    global $user;

    if (!$cache || !isset($cache['data'])) {
      return FALSE;
    }
    unset($cache['_id']);
    $cache = (object)$cache;
    // If the data is permanent or we are not enforcing a minimum cache lifetime
    // always return the cached data.
    // ?
    if ($cache->expire == CACHE_PERMANENT || !variable_get('cache_lifetime', 0)) {
    }
    // If enforcing a minimum cache lifetime, validate that the data is
    // currently valid for this user before we return it by making sure the cache
    // entry was created before the timestamp in the current session's cache
    // timer. The cache variable is loaded into the $user object by _drupal_session_read()
    // in session.inc. If the data is permanent or we're not enforcing a minimum
    // cache lifetime always return the cached data.
    if ($cache->expire != CACHE_PERMANENT && variable_get('cache_lifetime', 0) && $user->cache > $cache->created) {
      // This cache data is too old and thus not valid for us, ignore it.
      return FALSE;
    }
    if ($cache->data instanceof MongoBinData) {
      $cache->data = $cache->data->bin;
    }
    if ($cache->serialized) {
      $cache->data = unserialize($cache->data);
    }

    return $cache;
  }

  function set($cid, $data, $expire = CACHE_PERMANENT, $headers = NULL) {
    $scalar = is_scalar($data);
    $entry = array(
      '_id' => (string)$cid,
      'cid' => (string)$cid,
      'created' => $_SERVER['REQUEST_TIME'],
      'expire' => $expire,
      'headers' => $headers,
      'serialized' => !$scalar,
      'data' => $scalar ? $data : serialize($data),
    );

    // Use MongoBinData for non-UTF8 strings.
    if (is_string($entry['data']) && !drupal_validate_utf8($entry['data'])) {
      $entry['data'] = new MongoBinData($entry['data']);
    }

    try {
      $collection = mongodb_collection($this->bin);
      $collection->save($entry);
    }
    catch (Exception $e) {
      // The database may not be available, so we'll ignore cache_set requests.
    }
  }

  function clear($cid = NULL, $wildcard = FALSE) {
    global $user;

    $collection = mongodb_collection($this->bin);

    if (empty($cid)) {
      if (variable_get('cache_lifetime', 0)) {
        // We store the time in the current user's $user->cache variable which
        // will be saved into the sessions bin by _drupal_session_write(). We then
        // simulate that the cache was flushed for this user by not returning
        // cached data that was cached before the timestamp.
        $user->cache = $_SERVER['REQUEST_TIME'];

        $cache_flush = variable_get('cache_flush_' . $this->bin, 0);
        if ($cache_flush == 0) {
          // This is the first request to clear the cache, start a timer.
          variable_set('cache_flush_' . $this->bin, $_SERVER['REQUEST_TIME']);
        }
        elseif ($_SERVER['REQUEST_TIME'] > ($cache_flush + variable_get('cache_lifetime', 0))) {
          // Clear the cache for everyone, cache_lifetime seconds have
          // passed since the first request to clear the cache.

          $collection->remove(array('expire' => array('$ne' => CACHE_PERMANENT, '$lte' => $_SERVER['REQUEST_TIME'])));
          variable_set('cache_flush_' . $this->bin, 0);
        }
      }
      else {
        // No minimum cache lifetime, flush all temporary cache entries now.
        $collection->remove(array('expire' => array('$ne' => CACHE_PERMANENT, '$lte' => $_SERVER['REQUEST_TIME'])));
      }
    }
    else {
      if ($wildcard) {
        if ($cid == '*') {
          $collection->remove();
        }
        else {
          $collection->remove(array('cid' => new MongoRegex('/'. preg_quote($cid) .'.*/')));
        }
      }
      elseif (is_array($cid)) {
        // Delete in chunks when a large array is passed.
        do {
          $find = array('cid' => array('$in' => array_map('strval', array_splice($cid, 0, 1000))));
          $collection->remove($find);
        }
        while (count($cid));
      }
      else {
        $collection->remove(array('_id' => (string)$cid));
      }
    }
  }

  function isEmpty() {
    return !mongodb_collection($this->bin)->findOne();
  }
}
