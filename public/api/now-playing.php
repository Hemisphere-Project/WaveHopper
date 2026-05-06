<?php
// Waverz.net — now-playing dispatcher.
// Reads ../stations.json, finds the requested station's `nowPlaying` block,
// loads the matching fetcher in fetchers/<type>.php, and returns a normalized
// JSON shape. File-cache is per fetcher key (some fetchers cover many stations
// from one upstream call, e.g. NTS).

declare(strict_types=1);

require __DIR__ . '/lib/cache.php';
require __DIR__ . '/lib/http.php';
require __DIR__ . '/lib/stations.php';

header('Content-Type: application/json; charset=utf-8');
// Browser cache short — the station list is polled frequently and we want
// the file cache (server-side) to absorb upstream load, not the browser cache.
header('Cache-Control: public, max-age=15');

function wh_fail(int $code, string $msg): void {
    http_response_code($code);
    echo json_encode(['error' => $msg]);
    exit;
}

$id = (string)($_GET['id'] ?? '');
if ($id === '' || !preg_match('/^[a-z0-9-]+$/', $id)) {
    wh_fail(400, 'bad id');
}

$station = wh_station($id);
if ($station === null) {
    wh_fail(404, 'unknown station');
}

$np = $station['nowPlaying'] ?? null;
if (!is_array($np) || empty($np['type']) || $np['type'] === 'none') {
    // Station has no metadata source (or it's been explicitly checked and
    // there's nothing to fetch). 204 = nothing to render, frontend hides card.
    http_response_code(204);
    exit;
}

$type = (string)$np['type'];
if (!preg_match('/^[a-z0-9-]+$/', $type)) {
    wh_fail(500, 'bad fetcher type');
}

$fetcherFile = __DIR__ . "/fetchers/{$type}.php";
if (!is_file($fetcherFile)) {
    wh_fail(501, "no fetcher for {$type}");
}
require $fetcherFile;

$fn = "wh_fetch_nowplaying_" . str_replace('-', '_', $type);
if (!function_exists($fn)) {
    wh_fail(500, "fetcher function missing: {$fn}");
}

$result = $fn($id, $np, $station);
if (!is_array($result)) {
    // Upstream had nothing for this station right now (off-air, between shows).
    http_response_code(204);
    exit;
}

$result['source']    = $type;
$result['fetchedAt'] = time();
echo json_encode($result);
