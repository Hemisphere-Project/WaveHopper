<?php
// Telemetry storage bootstrap, shared by the ingest endpoint and the stats
// page. The SQLite file and secrets live OUTSIDE both the docroot and the git
// clone (deploys are git pulls): <clone>/../telemetry/config.php returning
// ['db' => '/abs/path/telemetry.db', 'stats_key' => '<random>'].
// Without a config (local dev) it falls back to a temp DB and key 'dev'.

declare(strict_types=1);

function wh_telemetry_config(): array {
    // __DIR__ = .../players/web/public/api/lib → repo root is 5 up, host dir 6.
    $cfg = dirname(__DIR__, 6) . '/telemetry/config.php';
    if (is_file($cfg)) {
        $c = require $cfg;
        if (is_array($c) && !empty($c['db']) && !empty($c['stats_key'])) return $c;
    }
    return ['db' => sys_get_temp_dir() . '/wh-telemetry.db', 'stats_key' => 'dev'];
}

function wh_telemetry_db(): PDO {
    $cfg = wh_telemetry_config();
    $pdo = new PDO('sqlite:' . $cfg['db']);
    $pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
    $pdo->exec('PRAGMA journal_mode=WAL');
    $pdo->exec('PRAGMA busy_timeout=3000');
    $pdo->exec('CREATE TABLE IF NOT EXISTS installs(
        id TEXT PRIMARY KEY, player TEXT, first_seen INT, last_seen INT,
        tz TEXT, lang TEXT, country TEXT, city TEXT, app TEXT, geo_at INT)');
    $pdo->exec('CREATE TABLE IF NOT EXISTS sessions(
        id INTEGER PRIMARY KEY, install_id TEXT, player TEXT, station TEXT,
        started_at INT, last_seen_at INT)');
    $pdo->exec('CREATE TABLE IF NOT EXISTS events(
        ts INT, install_id TEXT, ev TEXT, station TEXT, player TEXT)');
    $pdo->exec('CREATE TABLE IF NOT EXISTS geo_cache(
        prefix TEXT PRIMARY KEY, country TEXT, city TEXT, ts INT)');
    $pdo->exec('CREATE INDEX IF NOT EXISTS idx_sessions_install
        ON sessions(install_id, last_seen_at)');
    $pdo->exec('CREATE INDEX IF NOT EXISTS idx_sessions_time ON sessions(started_at)');
    $pdo->exec('CREATE INDEX IF NOT EXISTS idx_sessions_station ON sessions(station)');
    $pdo->exec('CREATE INDEX IF NOT EXISTS idx_events_ts ON events(ts)');
    return $pdo;
}
