<?php
// Listener telemetry ingest — docs/CONTENT-API.md §Telemetry is normative.
//
// POST {"v":1,"id":"<uuid>","p":"web|m5cores3|mobile","ev":"start|hb|stop",
//       "st":"<station-id>","tz":"...","lang":"...","app":"..."}
//
// Privacy: the install id is an anonymous random UUID. The client IP is used
// once, in memory, to resolve a coarse location (country/city, cached per /24
// prefix) and is never stored. Sessions are derived server-side from
// heartbeats; stop events only clamp them.

declare(strict_types=1);

require __DIR__ . '/lib/telemetry_db.php';
require __DIR__ . '/lib/stations.php';

const GAP_WINDOWS = ['web' => 200, 'mobile' => 200, 'm5cores3' => 400];
const GEO_REFRESH_S = 30 * 86400;

function bail(int $code): void {
    http_response_code($code);
    exit;
}

if (($_SERVER['REQUEST_METHOD'] ?? '') !== 'POST') bail(405);
$raw = file_get_contents('php://input', false, null, 0, 1024);
if ($raw === false || $raw === '') bail(400);
$in = json_decode($raw, true);
if (!is_array($in) || (int)($in['v'] ?? 0) !== 1) bail(400);

$id = (string)($in['id'] ?? '');
$player = (string)($in['p'] ?? '');
$ev = (string)($in['ev'] ?? '');
$station = (string)($in['st'] ?? '');
if (!preg_match('/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i', $id)) bail(400);
if (!isset(GAP_WINDOWS[$player])) bail(400);
if (!in_array($ev, ['start', 'hb', 'stop'], true)) bail(400);
if ($station === '' || wh_station($station) === null) bail(400);
$tz = substr((string)($in['tz'] ?? ''), 0, 48);
$lang = substr((string)($in['lang'] ?? ''), 0, 16);
$app = substr((string)($in['app'] ?? ''), 0, 24);

$now = time();
$db = wh_telemetry_db();

// Cheap flood guard: ignore sub-10s heartbeat spam per install.
if ($ev === 'hb') {
    $q = $db->prepare("SELECT MAX(ts) FROM events WHERE install_id = ? AND ev = 'hb'");
    $q->execute([$id]);
    $lastHb = (int)$q->fetchColumn();
    if ($lastHb && $now - $lastHb < 10) bail(204);
}

$db->prepare('INSERT INTO events(ts, install_id, ev, station, player) VALUES(?,?,?,?,?)')
   ->execute([$now, $id, $ev, $station, $player]);

// ---- install upsert (+ coarse geo, IP used in-memory only) -----------------
$q = $db->prepare('SELECT country, geo_at FROM installs WHERE id = ?');
$q->execute([$id]);
$known = $q->fetch(PDO::FETCH_ASSOC);

$db->prepare('INSERT INTO installs(id, player, first_seen, last_seen, tz, lang, app, geo_at)
              VALUES(?,?,?,?,?,?,?,0)
              ON CONFLICT(id) DO UPDATE SET last_seen = excluded.last_seen,
                player = excluded.player, tz = excluded.tz, lang = excluded.lang,
                app = excluded.app')
   ->execute([$id, $player, $now, $now, $tz, $lang, $app]);

$needGeo = !$known || $known['country'] === null || ($now - (int)$known['geo_at']) > GEO_REFRESH_S;
if ($needGeo) {
    $ip = $_SERVER['REMOTE_ADDR'] ?? '';
    $geo = wh_geo_lookup($db, $ip, $now);  // never persisted: only its result is
    $db->prepare('UPDATE installs SET country = ?, city = ?, geo_at = ? WHERE id = ?')
       ->execute([$geo['country'], $geo['city'], $now, $id]);
}

// ---- incremental session derivation ----------------------------------------
$gap = GAP_WINDOWS[$player];
$q = $db->prepare('SELECT id, station, last_seen_at FROM sessions
                   WHERE install_id = ? ORDER BY last_seen_at DESC LIMIT 1');
$q->execute([$id]);
$open = $q->fetch(PDO::FETCH_ASSOC);
$continues = $open && ($now - (int)$open['last_seen_at']) <= $gap;

if ($ev === 'stop') {
    if ($continues) {
        $db->prepare('UPDATE sessions SET last_seen_at = ? WHERE id = ?')
           ->execute([$now, $open['id']]);
    }
} elseif ($continues && $open['station'] === $station) {
    $db->prepare('UPDATE sessions SET last_seen_at = ? WHERE id = ?')
       ->execute([$now, $open['id']]);
} else {
    // Station switch within a listen, or a fresh session after a gap.
    $db->prepare('INSERT INTO sessions(install_id, player, station, started_at, last_seen_at)
                  VALUES(?,?,?,?,?)')
       ->execute([$id, $player, $station, $now, $now]);
}

bail(204);

// ---- coarse geo -------------------------------------------------------------
function wh_geo_lookup(PDO $db, string $ip, int $now): array {
    $none = ['country' => null, 'city' => null];
    if ($ip === '' || $ip === '127.0.0.1' || $ip === '::1') return $none;
    // Cache per /24 (v4) / /64-ish (v6) so the external lookup is rare.
    if (strpos($ip, ':') !== false) {
        $prefix = implode(':', array_slice(explode(':', $ip), 0, 4)) . '::';
    } else {
        $prefix = preg_replace('/\.\d+$/', '.0', $ip);
    }
    $q = $db->prepare('SELECT country, city FROM geo_cache WHERE prefix = ?');
    $q->execute([$prefix]);
    if ($row = $q->fetch(PDO::FETCH_ASSOC)) return $row;

    $ctx = stream_context_create(['http' => ['timeout' => 2]]);
    $res = @file_get_contents(
        'http://ip-api.com/json/' . rawurlencode($ip) . '?fields=status,country,city',
        false, $ctx);
    $geo = $none;
    if ($res !== false) {
        $j = json_decode($res, true);
        if (is_array($j) && ($j['status'] ?? '') === 'success') {
            $geo = ['country' => substr((string)$j['country'], 0, 64),
                    'city' => substr((string)$j['city'], 0, 64)];
        }
    }
    $db->prepare('INSERT OR REPLACE INTO geo_cache(prefix, country, city, ts) VALUES(?,?,?,?)')
       ->execute([$prefix, $geo['country'], $geo['city'], $now]);
    return $geo;
}
