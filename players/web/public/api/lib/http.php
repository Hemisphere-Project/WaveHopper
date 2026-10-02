<?php
// Tiny curl wrapper for fetcher use (GET + POST, stream fallback without curl). Conservative timeouts so a stalled
// upstream doesn't tie up a PHP-FPM worker on shared hosting.

declare(strict_types=1);

function wh_http_get(string $url, array $headers = [], int $timeoutMs = 4000): ?string {
    return wh_http_request('GET', $url, $headers, null, $timeoutMs);
}

// POST a body (e.g. a GraphQL query) — set the Content-Type in $headers.
function wh_http_post(string $url, string $body, array $headers = [], int $timeoutMs = 4000): ?string {
    return wh_http_request('POST', $url, $headers, $body, $timeoutMs);
}

// Returns the response body on 2xx, else null.
function wh_http_request(string $method, string $url, array $headers, ?string $body, int $timeoutMs): ?string {
    $userHeaders = array_merge(['Accept: application/json'], $headers);

    if (function_exists('curl_init')) {
        $ch = curl_init($url);
        if ($ch !== false) {
            $opts = [
                CURLOPT_RETURNTRANSFER  => true,
                CURLOPT_FOLLOWLOCATION  => true,
                CURLOPT_MAXREDIRS       => 3,
                CURLOPT_CONNECTTIMEOUT  => 2,
                CURLOPT_TIMEOUT_MS      => $timeoutMs,
                CURLOPT_USERAGENT       => 'Waverz.net/1.0 (+https://waverz.net)',
                CURLOPT_HTTPHEADER      => $userHeaders,
                CURLOPT_ENCODING        => '', // libcurl auto-decompresses gzip/deflate
                CURLOPT_SSL_VERIFYPEER  => true,
                CURLOPT_SSL_VERIFYHOST  => 2,
            ];
            if ($method === 'POST') {
                $opts[CURLOPT_POST] = true;
                $opts[CURLOPT_POSTFIELDS] = $body ?? '';
            }
            curl_setopt_array($ch, $opts);
            $resp = curl_exec($ch);
            $code = (int)curl_getinfo($ch, CURLINFO_HTTP_CODE);
            curl_close($ch);
            if ($resp !== false && $code >= 200 && $code < 300 && is_string($resp)) return $resp;
            return null;
        }
    }

    // Fallback when curl is unavailable (rare on shared hosting but seen).
    // Don't advertise gzip here — file_get_contents won't decompress, and the
    // upstream payloads are small enough that uncompressed is fine.
    $http = [
        'method'  => $method,
        'header'  => implode("\r\n", array_merge(
            ['User-Agent: Waverz.net/1.0 (+https://waverz.net)'],
            $userHeaders,
        )),
        'timeout' => max(1, (int)round($timeoutMs / 1000)),
        'follow_location' => 1,
        'max_redirects'   => 3,
        'ignore_errors'   => true,
    ];
    if ($body !== null) $http['content'] = $body;
    $ctx = stream_context_create([
        'http'  => $http,
        'https' => [
            'verify_peer'      => true,
            'verify_peer_name' => true,
        ],
    ]);
    $resp = @file_get_contents($url, false, $ctx);
    if ($resp === false) return null;
    $status = 0;
    if (isset($http_response_header[0]) && preg_match('#HTTP/\S+\s+(\d+)#', $http_response_header[0], $m)) {
        $status = (int)$m[1];
    }
    if ($status < 200 || $status >= 300) return null;
    return $resp;
}

function wh_http_get_json(string $url, array $headers = [], int $timeoutMs = 4000): ?array {
    $body = wh_http_get($url, $headers, $timeoutMs);
    if ($body === null) return null;
    $data = json_decode($body, true);
    return is_array($data) ? $data : null;
}
