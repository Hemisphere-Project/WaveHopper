<?php
// GitHub webhook deploy receiver.
//
// GitHub POSTs push events here; a valid HMAC (X-Hub-Signature-256, secret
// shared with the webhook config) on a push to main triggers `git pull` of
// the server clone this docroot lives in. The secret lives OUTSIDE the
// docroot at <repo-root>/.deploy-secret (gitignored, chmod 600) — leaking it
// only lets someone trigger a pull of the public repo, nothing else.
//
// Setup: repo Settings → Webhooks → add
//   payload URL https://waverz.net/api/deploy.php, content type json,
//   secret = contents of .deploy-secret, events: just pushes.

declare(strict_types=1);

const REPO_DEPTH = 4;  // api → public → web → players → repo root

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

$out = shell_exec('cd ' . escapeshellarg($repoRoot) . ' && git pull --ff-only 2>&1 && echo deployed $(git rev-parse --short HEAD)');
header('Content-Type: text/plain');
echo $out ?: "pull produced no output\n";
