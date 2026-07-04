<?php
// Listener stats dashboard. Access: ?key=<stats_key from the server config>
// (sets a cookie afterwards). Wrong/no key → 404, indistinguishable from
// nothing being here.

declare(strict_types=1);

require dirname(__DIR__) . '/api/lib/telemetry_db.php';

$cfg = wh_telemetry_config();
$key = $_GET['key'] ?? ($_COOKIE['whstats'] ?? '');
if (!hash_equals($cfg['stats_key'], (string)$key)) {
    http_response_code(404);
    exit('not found');
}
if (isset($_GET['key'])) {
    setcookie('whstats', $cfg['stats_key'], time() + 90 * 86400, '/stats/', '', true, true);
}

$db = wh_telemetry_db();
$now = time();
$d30 = $now - 30 * 86400;
$d7 = $now - 7 * 86400;
$d14 = $now - 14 * 86400;

function rows(PDO $db, string $sql, array $args = []): array {
    $q = $db->prepare($sql);
    $q->execute($args);
    return $q->fetchAll(PDO::FETCH_ASSOC);
}

// Daily series: listeners, hours, sessions.
$daily = rows($db, "SELECT date(started_at,'unixepoch') d,
        COUNT(DISTINCT install_id) dau,
        ROUND(SUM(last_seen_at-started_at)/3600.0, 1) hours,
        COUNT(*) sess
    FROM sessions WHERE started_at >= ? GROUP BY d ORDER BY d", [$d30]);
$newPerDay = [];
foreach (rows($db, "SELECT date(first_seen,'unixepoch') d, COUNT(*) n
    FROM installs WHERE first_seen >= ? GROUP BY d", [$d30]) as $r) {
    $newPerDay[$r['d']] = (int)$r['n'];
}

// Station share (30 d, time-weighted).
$stations = rows($db, "SELECT station, ROUND(SUM(last_seen_at-started_at)/3600.0, 2) hours,
        COUNT(DISTINCT install_id) listeners
    FROM sessions WHERE started_at >= ? GROUP BY station ORDER BY hours DESC", [$d30]);

// Durations for avg/median (30 d, ≥10 s to drop connection noise).
$durs = array_map(fn($r) => (int)$r['dur'],
    rows($db, 'SELECT last_seen_at-started_at dur FROM sessions
               WHERE started_at >= ? AND last_seen_at-started_at >= 10', [$d30]));
sort($durs);
$median = $durs ? $durs[intdiv(count($durs), 2)] : 0;
$avg = $durs ? (int)(array_sum($durs) / count($durs)) : 0;

// Hour-of-day heatmap (UTC, 30 d).
$hours = array_fill(0, 24, 0.0);
foreach (rows($db, "SELECT CAST(strftime('%H', started_at,'unixepoch') AS INT) h,
        SUM(last_seen_at-started_at)/3600.0 t
    FROM sessions WHERE started_at >= ? GROUP BY h", [$d30]) as $r) {
    $hours[(int)$r['h']] = (float)$r['t'];
}

// Player + geo splits.
$players = rows($db, "SELECT i.player, COUNT(*) installs FROM installs i GROUP BY i.player");
$geo = rows($db, "SELECT COALESCE(i.country,'?') country, COALESCE(i.city,'') city,
        COUNT(DISTINCT s.install_id) listeners,
        ROUND(SUM(s.last_seen_at-s.started_at)/3600.0, 1) hours
    FROM sessions s JOIN installs i ON i.id = s.install_id
    WHERE s.started_at >= ? GROUP BY country, city ORDER BY hours DESC LIMIT 30", [$d30]);

// Retention: share of active installs (14 d) heard on ≥2 distinct days.
$act = rows($db, "SELECT install_id, COUNT(DISTINCT date(started_at,'unixepoch')) dd
    FROM sessions WHERE started_at >= ? GROUP BY install_id", [$d14]);
$returning = count(array_filter($act, fn($r) => (int)$r['dd'] >= 2));
$retention = $act ? round(100 * $returning / count($act)) : 0;

// Recent activity.
$recent = rows($db, "SELECT substr(install_id,1,8) who, player, station,
        datetime(started_at,'unixepoch') start,
        (last_seen_at-started_at) dur
    FROM sessions ORDER BY last_seen_at DESC LIMIT 25");

$totInstalls = (int)$db->query('SELECT COUNT(*) FROM installs')->fetchColumn();

function fmtDur(int $s): string {
    if ($s >= 3600) return sprintf('%dh%02d', intdiv($s, 3600), intdiv($s % 3600, 60));
    if ($s >= 60) return sprintf('%dm%02d', intdiv($s, 60), $s % 60);
    return $s . 's';
}
function bar(float $v, float $max, string $color = '#44dd44'): string {
    $w = $max > 0 ? max(1, (int)(120 * $v / $max)) : 1;
    return "<svg width='120' height='12'><rect width='$w' height='12' fill='$color'/></svg>";
}
$maxDau = max(1, ...array_map(fn($r) => (int)$r['dau'], $daily ?: [['dau' => 1]]));
$maxH = max(0.01, ...array_map(fn($r) => (float)$r['hours'], $daily ?: [['hours' => 0.01]]));
$maxSt = max(0.01, ...array_map(fn($r) => (float)$r['hours'], $stations ?: [['hours' => 0.01]]));
$maxHour = max(0.01, ...$hours);
?>
<!doctype html>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex">
<title>waverz stats</title>
<style>
  body { background:#0a0a0a; color:#e8e8e8; font:14px/1.5 monospace; margin:2em auto; max-width:900px; padding:0 1em; }
  h1 { color:#fff205; font-size:1.3em; } h2 { color:#55ccff; font-size:1.05em; margin-top:2em; }
  table { border-collapse:collapse; width:100%; } td, th { text-align:left; padding:2px 10px 2px 0; vertical-align:middle; }
  th { color:#666; font-weight:normal; border-bottom:1px solid #2a2a2a; }
  .n { text-align:right; } .dim { color:#666; }
  .cards { display:flex; gap:2em; flex-wrap:wrap; } .card b { display:block; font-size:1.6em; color:#fff205; }
</style>
<h1>~ waverz listeners ~</h1>
<div class="cards">
  <div class="card"><b><?= $totInstalls ?></b>installs ever</div>
  <div class="card"><b><?= count($act) ?></b>active 14d</div>
  <div class="card"><b><?= $retention ?>%</b>returning (≥2 days/14d)</div>
  <div class="card"><b><?= fmtDur($median) ?></b>median session</div>
  <div class="card"><b><?= fmtDur($avg) ?></b>avg session</div>
</div>

<h2>last 30 days</h2>
<table><tr><th>day</th><th>listeners</th><th></th><th class="n">hours</th><th></th><th class="n">sessions</th><th class="n">new</th></tr>
<?php foreach (array_reverse($daily) as $r): ?>
<tr><td><?= $r['d'] ?></td>
  <td class="n"><?= $r['dau'] ?></td><td><?= bar((float)$r['dau'], (float)$maxDau) ?></td>
  <td class="n"><?= $r['hours'] ?></td><td><?= bar((float)$r['hours'], $maxH, '#55ccff') ?></td>
  <td class="n"><?= $r['sess'] ?></td>
  <td class="n dim"><?= $newPerDay[$r['d']] ?? 0 ?></td></tr>
<?php endforeach; ?>
</table>

<h2>stations (30d, by listening time)</h2>
<table><tr><th>station</th><th class="n">hours</th><th></th><th class="n">listeners</th></tr>
<?php foreach ($stations as $r): ?>
<tr><td><?= htmlspecialchars($r['station']) ?></td>
  <td class="n"><?= $r['hours'] ?></td><td><?= bar((float)$r['hours'], $maxSt, '#ff55ff') ?></td>
  <td class="n"><?= $r['listeners'] ?></td></tr>
<?php endforeach; ?>
</table>

<h2>hour of day (UTC, 30d listening hours)</h2>
<table>
<?php for ($h = 0; $h < 24; $h++): ?>
<tr><td class="dim"><?= sprintf('%02d:00', $h) ?></td>
  <td><?= bar($hours[$h], $maxHour, '#ff8800') ?></td>
  <td class="n"><?= round($hours[$h], 1) ?></td></tr>
<?php endfor; ?>
</table>

<h2>where &amp; what</h2>
<table><tr><th>country</th><th>city</th><th class="n">listeners</th><th class="n">hours</th></tr>
<?php foreach ($geo as $r): ?>
<tr><td><?= htmlspecialchars($r['country']) ?></td><td><?= htmlspecialchars($r['city']) ?></td>
  <td class="n"><?= $r['listeners'] ?></td><td class="n"><?= $r['hours'] ?></td></tr>
<?php endforeach; ?>
</table>
<p class="dim">players: <?php foreach ($players as $r) echo htmlspecialchars($r['player']) . '=' . $r['installs'] . '  '; ?></p>

<h2>recent sessions</h2>
<table><tr><th>who</th><th>player</th><th>station</th><th>start (UTC)</th><th class="n">length</th></tr>
<?php foreach ($recent as $r): ?>
<tr><td class="dim"><?= htmlspecialchars($r['who']) ?></td><td><?= htmlspecialchars($r['player']) ?></td>
  <td><?= htmlspecialchars($r['station']) ?></td><td><?= $r['start'] ?></td>
  <td class="n"><?= fmtDur((int)$r['dur']) ?></td></tr>
<?php endforeach; ?>
</table>
