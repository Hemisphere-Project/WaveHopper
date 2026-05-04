<?php
// File cache for now-playing fetchers. Stores JSON-serialisable arrays under
// public/api/cache/<key>.json with mtime-based TTL. On upstream failure,
// returns the last-known-good payload (stale-while-broken) so a flaky NTS API
// doesn't blank the now-playing card.

declare(strict_types=1);

function wh_cache_dir(): string {
    return __DIR__ . '/../cache';
}

function wh_cache_path(string $key): string {
    $safe = preg_replace('/[^a-z0-9_-]/i', '_', $key);
    return wh_cache_dir() . "/{$safe}.json";
}

function wh_cache_read(string $key): ?array {
    $path = wh_cache_path($key);
    if (!is_file($path)) return null;
    $raw = @file_get_contents($path);
    if ($raw === false) return null;
    $data = json_decode($raw, true);
    return is_array($data) ? $data : null;
}

function wh_cache_write(string $key, array $data): void {
    $dir = wh_cache_dir();
    if (!is_dir($dir)) @mkdir($dir, 0775, true);
    $path = wh_cache_path($key);
    $tmp  = $path . '.tmp.' . bin2hex(random_bytes(4));
    if (@file_put_contents($tmp, json_encode($data), LOCK_EX) !== false) {
        @rename($tmp, $path);
    } else {
        @unlink($tmp);
    }
}

function wh_cache_age(string $key): ?int {
    $path = wh_cache_path($key);
    if (!is_file($path)) return null;
    $m = @filemtime($path);
    return $m === false ? null : (time() - $m);
}

/**
 * Return cached value if fresh, else call $fetcher and cache its result.
 * If $fetcher returns null (or throws) and a stale entry exists, return stale.
 */
function wh_cached(string $key, int $ttl, callable $fetcher): ?array {
    $age = wh_cache_age($key);
    if ($age !== null && $age < $ttl) {
        $hit = wh_cache_read($key);
        if ($hit !== null) return $hit;
    }
    try {
        $fresh = $fetcher();
    } catch (\Throwable $e) {
        $fresh = null;
    }
    if (is_array($fresh)) {
        wh_cache_write($key, $fresh);
        return $fresh;
    }
    // Upstream failed — serve stale if we have it.
    return wh_cache_read($key);
}
