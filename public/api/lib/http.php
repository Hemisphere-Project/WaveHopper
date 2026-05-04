<?php
// Tiny curl wrapper for fetcher use. Conservative timeouts so a stalled
// upstream doesn't tie up a PHP-FPM worker on shared hosting.

declare(strict_types=1);

function wh_http_get(string $url, array $headers = [], int $timeoutMs = 4000): ?string {
    $userHeaders = array_merge(['Accept: application/json'], $headers);

    if (function_exists('curl_init')) {
        $ch = curl_init($url);
        if ($ch !== false) {
            curl_setopt_array($ch, [
                CURLOPT_RETURNTRANSFER  => true,
                CURLOPT_FOLLOWLOCATION  => true,
                CURLOPT_MAXREDIRS       => 3,
                CURLOPT_CONNECTTIMEOUT  => 2,
                CURLOPT_TIMEOUT_MS      => $timeoutMs,
                CURLOPT_USERAGENT       => 'WaveHopper/1.0 (+https://github.com/maigre/WaveHopper)',
                CURLOPT_HTTPHEADER      => $userHeaders,
                CURLOPT_ENCODING        => '', // libcurl auto-decompresses gzip/deflate
                CURLOPT_SSL_VERIFYPEER  => true,
                CURLOPT_SSL_VERIFYHOST  => 2,
            ]);
            $body = curl_exec($ch);
            $code = (int)curl_getinfo($ch, CURLINFO_HTTP_CODE);
            curl_close($ch);
            if ($body !== false && $code >= 200 && $code < 300 && is_string($body)) return $body;
            return null;
        }
    }

    // Fallback when curl is unavailable (rare on shared hosting but seen).
    // Don't advertise gzip here — file_get_contents won't decompress, and the
    // upstream payloads are small enough that uncompressed is fine.
    $ctx = stream_context_create([
        'http'  => [
            'method'  => 'GET',
            'header'  => implode("\r\n", array_merge(
                ['User-Agent: WaveHopper/1.0 (+https://github.com/maigre/WaveHopper)'],
                $userHeaders,
            )),
            'timeout' => max(1, (int)round($timeoutMs / 1000)),
            'follow_location' => 1,
            'max_redirects'   => 3,
            'ignore_errors'   => true,
        ],
        'https' => [
            'verify_peer'      => true,
            'verify_peer_name' => true,
        ],
    ]);
    $body = @file_get_contents($url, false, $ctx);
    if ($body === false) return null;
    $status = 0;
    if (isset($http_response_header[0]) && preg_match('#HTTP/\S+\s+(\d+)#', $http_response_header[0], $m)) {
        $status = (int)$m[1];
    }
    if ($status < 200 || $status >= 300) return null;
    return $body;
}

function wh_http_get_json(string $url, array $headers = [], int $timeoutMs = 4000): ?array {
    $body = wh_http_get($url, $headers, $timeoutMs);
    if ($body === null) return null;
    $data = json_decode($body, true);
    return is_array($data) ? $data : null;
}
