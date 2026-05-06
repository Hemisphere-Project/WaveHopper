<?php
// The Lot Radio now-playing fetcher.
// Source: homepage HTML (React Flight payload) at https://www.thelotradio.com/
// The page embeds the current day's lineup as event objects with summary/start/end.
// No dedicated now-playing JSON endpoint was found, and ffprobe did not expose a
// visible HLS ID3/data stream on the live variant.
//
// Mapping: event.summary -> title (show name / host string as published by The Lot)
//          subtitle      -> null (no separate secondary field in the embedded data)
//          event.start   -> starts
//          event.end     -> ends
//          next.summary  -> next.title
// Cache key: thelot-html (30 s — show-level)

declare(strict_types=1);

function wh_fetch_nowplaying_thelot_html_extract(string $url): ?array {
    $raw = null;
    $tryStream = static function(bool $verifyPeer) use ($url): ?string {
        $ctx = stream_context_create([
            'http' => [
                'method' => 'GET',
                'header' => implode("\r\n", [
                    'Accept: text/html,application/xhtml+xml',
                    'User-Agent: WaveHopper/1.0 (+https://github.com/maigre/WaveHopper)',
                ]),
                'timeout' => 5,
                'follow_location' => 1,
                'max_redirects' => 3,
                'ignore_errors' => true,
            ],
            'https' => [
                'verify_peer' => $verifyPeer,
                'verify_peer_name' => $verifyPeer,
            ],
        ]);
        $body = @file_get_contents($url, false, $ctx);
        if (!is_string($body)) return null;
        $status = 0;
        if (isset($http_response_header[0]) && preg_match('#HTTP/\S+\s+(\d+)#', $http_response_header[0], $m)) {
            $status = (int)$m[1];
        }
        return ($status >= 200 && $status < 300) ? $body : null;
    };

    $tryCurl = static function(bool $verifyPeer) use ($url): ?string {
        if (!function_exists('curl_init')) return null;
        $ch = curl_init($url);
        if ($ch === false) return null;
        curl_setopt_array($ch, [
            CURLOPT_RETURNTRANSFER => true,
            CURLOPT_FOLLOWLOCATION => true,
            CURLOPT_MAXREDIRS => 3,
            CURLOPT_CONNECTTIMEOUT => 2,
            CURLOPT_TIMEOUT_MS => 5000,
            CURLOPT_USERAGENT => 'WaveHopper/1.0 (+https://github.com/maigre/WaveHopper)',
            CURLOPT_HTTPHEADER => ['Accept: text/html,application/xhtml+xml'],
            CURLOPT_ENCODING => '',
            CURLOPT_SSL_VERIFYPEER => $verifyPeer,
            CURLOPT_SSL_VERIFYHOST => $verifyPeer ? 2 : 0,
        ]);
        $body = curl_exec($ch);
        $code = (int)curl_getinfo($ch, CURLINFO_HTTP_CODE);
        curl_close($ch);
        return ($body !== false && $code >= 200 && $code < 300 && is_string($body)) ? $body : null;
    };

    $raw = $tryStream(true)
        ?? $tryCurl(true)
        // Shared hosts sometimes ship stale CA bundles or disable the HTTPS
        // stream wrapper; fall back to relaxed verification for this public,
        // read-only schedule page rather than blanking the now-playing card.
        ?? $tryStream(false)
        ?? $tryCurl(false);
    if ($raw === null) return null;

    $pattern = "/summary\\\\\":\\\\\"([^\\\\\"]+)\\\\\",\\\\\"start\\\\\":\\\\\"([^\\\\\"]+)\\\\\",\\\\\"end\\\\\":\\\\\"([^\\\\\"]+)\\\\\"/";
    if (!preg_match_all($pattern, $raw, $matches, PREG_SET_ORDER)) return null;

    $decode = static function(string $value): string {
        $decoded = json_decode('"' . $value . '"');
        if (!is_string($decoded)) {
            $decoded = str_replace(['\\/', '\\\\'], ['/', '\\'], $value);
        }
        return trim(html_entity_decode($decoded, ENT_QUOTES | ENT_HTML5, 'UTF-8'));
    };

    $events = [];
    foreach ($matches as $match) {
        $title = $decode((string)$match[1]);
        $startRaw = $decode((string)$match[2]);
        $endRaw = $decode((string)$match[3]);
        if ($title === '') continue;

        $startTs = strtotime($startRaw);
        $endTs = strtotime($endRaw);
        if ($startTs === false || $endTs === false || $startTs >= $endTs) continue;

        $rowKey = $title . '|' . $startTs . '|' . $endTs;
        $events[$rowKey] = [
            'title' => $title,
            'start' => $startTs,
            'end' => $endTs,
        ];
    }
    if (!$events) return null;

    $events = array_values($events);
    usort($events, static function(array $a, array $b): int {
        return $a['start'] <=> $b['start'];
    });

    $now = time();
    $current = null;
    $next = null;
    foreach ($events as $index => $event) {
        if ($event['start'] <= $now && $now < $event['end']) {
            $current = $event;
            $next = $events[$index + 1] ?? null;
            break;
        }
    }
    if ($current === null) return null;

    $result = [
        'title' => $current['title'],
        'subtitle' => null,
        'starts' => gmdate('Y-m-d\TH:i:s\Z', $current['start']),
        'ends' => gmdate('Y-m-d\TH:i:s\Z', $current['end']),
    ];
    if (is_array($next) && !empty($next['title'])) {
        $result['next'] = [
            'title' => $next['title'],
            'starts' => gmdate('Y-m-d\TH:i:s\Z', $next['start']),
        ];
    }

    return $result;
}

function wh_fetch_nowplaying_thelot_html(string $id, array $cfg, array $station): ?array {
    $key = 'thelot-html';
    $age = wh_cache_age($key);
    if ($age !== null && $age < 30) {
        $hit = wh_cache_read($key);
        if ($hit !== null) return $hit;
    }

    try {
        $url = (string)($cfg['endpoint'] ?? 'https://www.thelotradio.com/');
        $fresh = wh_fetch_nowplaying_thelot_html_extract($url);
    } catch (\Throwable $e) {
        $fresh = null;
    }

    if (is_array($fresh)) {
        wh_cache_write($key, $fresh);
        return $fresh;
    }

    return wh_cache_read($key);
}