<?php
// Loads the docroot stations.json once per request and returns by id.
// stations.json is the build artifact produced by `python3 tools/build.py`,
// served directly to the browser AND consumed server-side here. Single source
// of truth at deploy time.

declare(strict_types=1);

function wh_station(string $id): ?array {
    static $byId = null;
    if ($byId === null) {
        $byId = [];
        $path = __DIR__ . '/../../stations.json';
        if (is_file($path)) {
            $raw = @file_get_contents($path);
            if ($raw !== false) {
                $arr = json_decode($raw, true);
                if (is_array($arr)) {
                    foreach ($arr as $s) {
                        if (is_array($s) && isset($s['id']) && is_string($s['id'])) {
                            $byId[$s['id']] = $s;
                        }
                    }
                }
            }
        }
    }
    return $byId[$id] ?? null;
}
