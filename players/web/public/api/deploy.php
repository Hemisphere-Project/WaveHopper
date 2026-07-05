<?php
// GitHub webhook deploy receiver — pure PHP (no git, no shell).
//
// This host's web PHP-FPM disables every exec function (exec/shell_exec/
// proc_open/…), so we can't run `git pull`. Instead, on a valid push to main
// we fetch the repo tarball from GitHub and mirror its players/web/public/
// subtree into this docroot. curl/allow_url_fopen/PharData are available.
//
// HMAC secret lives OUTSIDE the docroot at <repo-root>/.deploy-secret
// (gitignored, chmod 600). Runtime state under api/cache/ and host files
// (.user.ini, the Infomaniak maintenance page) are preserved across deploys.
//
// Webhook: repo Settings → Webhooks → payload https://waverz.net/api/deploy.php,
// content type json, secret = contents of .deploy-secret, events: pushes.

declare(strict_types=1);

const REPO = 'Hemisphere-Project/WaveHopper';
const REPO_DEPTH = 4;  // api → public → web → players → repo root
// Never deleted/overwritten by the mirror (host runtime + server-writable).
const PRESERVE = ['api/cache', '.user.ini', '.infomaniak-maintenance.html', '.deployed'];

function fail(int $code, string $msg): void {
    http_response_code($code);
    header('Content-Type: text/plain');
    echo $msg, "\n";
    exit;
}

$repoRoot = dirname(__DIR__, REPO_DEPTH);
$secretFile = $repoRoot . '/.deploy-secret';
if (!is_file($secretFile)) fail(503, 'deploy not configured');
$secret = trim((string)file_get_contents($secretFile));
if ($secret === '') fail(503, 'deploy not configured');

$signature = $_SERVER['HTTP_X_HUB_SIGNATURE_256'] ?? '';
$body = file_get_contents('php://input', false, null, 0, 1 << 20);
if ($body === false || $signature === '') fail(400, 'bad request');
if (!hash_equals('sha256=' . hash_hmac('sha256', $body, $secret), $signature)) {
    fail(403, 'bad signature');
}

$event = $_SERVER['HTTP_X_GITHUB_EVENT'] ?? '';
if ($event === 'ping') fail(200, 'pong');
if ($event !== 'push') fail(200, 'ignored event');

$payload = json_decode($body, true);
if (($payload['ref'] ?? '') !== 'refs/heads/main') fail(200, 'ignored ref');
$sha = (string)($payload['after'] ?? 'main');

// ---- fetch + extract the pushed tree --------------------------------------
$ctx = stream_context_create(['http' => ['timeout' => 90, 'header' => "User-Agent: wh-deploy\r\n"]]);
$tar = @file_get_contents('https://codeload.github.com/' . REPO . '/tar.gz/' . rawurlencode($sha), false, $ctx);
if ($tar === false || strlen($tar) < 1024) fail(502, 'tarball fetch failed');

$work = sys_get_temp_dir() . '/wh-deploy-' . bin2hex(random_bytes(6));
$tgz = $work . '.tar.gz';
@mkdir($work, 0700, true);
file_put_contents($tgz, $tar);
try {
    (new PharData($tgz))->extractTo($work, null, true);
} catch (Throwable $e) {
    @unlink($tgz);
    fail(500, 'extract failed: ' . $e->getMessage());
}
$roots = glob($work . '/*', GLOB_ONLYDIR);
$src = ($roots[0] ?? '') . '/players/web/public';
if (!is_dir($src)) { rrmdir($work); @unlink($tgz); fail(500, 'docroot subtree missing in tarball'); }

// ---- mirror src → docroot (copy changed, remove absent, keep PRESERVE) -----
$docroot = dirname(__DIR__);  // .../players/web/public
$stats = ['copied' => 0, 'removed' => 0];
mirror($src, $docroot, '', $stats);

file_put_contents($docroot . '/.deployed', $sha . "\n");
rrmdir($work);
@unlink($tgz);

header('Content-Type: text/plain');
echo "deployed ", substr($sha, 0, 12), " (", $stats['copied'], " copied, ",
     $stats['removed'], " removed)\n";

// ---------------------------------------------------------------------------
function preserved(string $rel): bool {
    foreach (PRESERVE as $p) {
        if ($rel === $p || strncmp($rel, $p . '/', strlen($p) + 1) === 0) return true;
    }
    return false;
}

function mirror(string $src, string $dst, string $rel, array &$stats): void {
    @mkdir($dst, 0755, true);
    $have = [];
    // Copy/update everything in source.
    foreach (scandir($src) as $name) {
        if ($name === '.' || $name === '..') continue;
        $childRel = $rel === '' ? $name : "$rel/$name";
        if (preserved($childRel)) { $have[$name] = true; continue; }
        $s = "$src/$name";
        $d = "$dst/$name";
        $have[$name] = true;
        if (is_dir($s)) {
            mirror($s, $d, $childRel, $stats);
        } else {
            $new = file_get_contents($s);
            if (!is_file($d) || md5_file($d) !== md5($new)) {
                file_put_contents($d, $new);
                $stats['copied']++;
            }
        }
    }
    // Remove anything in dst not in source (deletions), skipping PRESERVE.
    foreach (scandir($dst) as $name) {
        if ($name === '.' || $name === '..' || isset($have[$name])) continue;
        $childRel = $rel === '' ? $name : "$rel/$name";
        if (preserved($childRel)) continue;
        $d = "$dst/$name";
        if (is_dir($d)) { rrmdir($d); $stats['removed']++; }
        else { @unlink($d); $stats['removed']++; }
    }
}

function rrmdir(string $dir): void {
    if (!is_dir($dir)) { @unlink($dir); return; }
    foreach (scandir($dir) as $name) {
        if ($name === '.' || $name === '..') continue;
        rrmdir("$dir/$name");
    }
    @rmdir($dir);
}
