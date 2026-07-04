<?php
// LYL Radio now-playing fetcher.
// Source: https://api.lyl.live/graphql — the `calendar` query.
// Returns the full-day schedule as a list of Broadcast objects:
//   { startAt: ISO8601 UTC, duration: "HH:MM:SS.mmm", title: show name, artists: host string }
// We find the current slot by comparing now() against startAt + duration,
// then pick the next entry for `next`.
//
// Mapping: artists → title (show host), title → subtitle (show name)
// Cache key: lyl-graphql (30 s — show-level; calendar covers the full broadcast day)

declare(strict_types=1);

function wh_fetch_nowplaying_lyl_graphql(string $id, array $cfg, array $station): ?array {
    return wh_cached('lyl-graphql', 30, function(): ?array {
        $body = wh_http_get(
            'https://api.lyl.live/graphql',
            ['Content-Type: application/json'],
            4000
        );
        // wh_http_get only does GET; we need POST — use curl directly.
        // Re-implement the POST via curl_init since the lib helper is GET-only.
        $payload = json_encode(['query' => '{ calendar { startAt duration title artists } }']);
        $ch = curl_init('https://api.lyl.live/graphql');
        if ($ch === false) return null;
        curl_setopt_array($ch, [
            CURLOPT_RETURNTRANSFER  => true,
            CURLOPT_POST            => true,
            CURLOPT_POSTFIELDS      => $payload,
            CURLOPT_HTTPHEADER      => [
                'Content-Type: application/json',
                'Accept: application/json',
            ],
            CURLOPT_FOLLOWLOCATION  => true,
            CURLOPT_MAXREDIRS       => 3,
            CURLOPT_CONNECTTIMEOUT  => 2,
            CURLOPT_TIMEOUT_MS      => 4000,
            CURLOPT_USERAGENT       => 'Waverz.net/1.0 (+https://waverz.net)',
            CURLOPT_ENCODING        => '',
            CURLOPT_SSL_VERIFYPEER  => true,
            CURLOPT_SSL_VERIFYHOST  => 2,
        ]);
        $raw  = curl_exec($ch);
        $code = (int)curl_getinfo($ch, CURLINFO_HTTP_CODE);
        curl_close($ch);

        if ($raw === false || $code < 200 || $code >= 300 || !is_string($raw)) return null;
        $json = json_decode($raw, true);
        if (!is_array($json) || !isset($json['data']['calendar']) || !is_array($json['data']['calendar'])) {
            return null;
        }

        $entries = $json['data']['calendar'];
        $now     = time();

        $current = null;
        $next    = null;
        foreach ($entries as $i => $entry) {
            if (!isset($entry['startAt'], $entry['duration'])) continue;

            // Parse UTC ISO 8601 startAt.
            $startTs = strtotime($entry['startAt']);
            if ($startTs === false) continue;

            // Parse duration "HH:MM:SS.mmm".
            $durationSec = 0;
            if (preg_match('/^(\d+):(\d+):(\d+)/', (string)$entry['duration'], $dm)) {
                $durationSec = (int)$dm[1] * 3600 + (int)$dm[2] * 60 + (int)$dm[3];
            }
            $endTs = $startTs + $durationSec;

            if ($startTs <= $now && $now < $endTs) {
                $current = $entry;
                $current['_startTs'] = $startTs;
                $current['_endTs']   = $endTs;
                // Look ahead for next non-null entry.
                for ($j = $i + 1; $j < count($entries); $j++) {
                    if (isset($entries[$j]['startAt'])) {
                        $next = $entries[$j];
                        break;
                    }
                }
                break;
            }
        }

        if ($current === null) return null;

        $cleanStr = function(?string $raw): ?string {
            if ($raw === null) return null;
            $s = trim(html_entity_decode($raw, ENT_QUOTES | ENT_HTML5, 'UTF-8'));
            return $s !== '' ? $s : null;
        };

        $result = [
            'title'    => $cleanStr($current['artists'] ?? null),  // show host
            'subtitle' => $cleanStr($current['title']   ?? null),  // show name
            'starts'   => gmdate('Y-m-d\TH:i:s\Z', $current['_startTs']),
            'ends'     => gmdate('Y-m-d\TH:i:s\Z', $current['_endTs']),
        ];

        if ($result['title'] === null) return null;

        if ($next !== null) {
            $nextTitle = $cleanStr($next['artists'] ?? null) ?? $cleanStr($next['title'] ?? null);
            if ($nextTitle !== null) {
                $result['next'] = [
                    'title'  => $nextTitle,
                    'starts' => isset($next['startAt'])
                        ? gmdate('Y-m-d\TH:i:s\Z', (int)strtotime($next['startAt']))
                        : null,
                ];
            }
        }

        return $result;
    });
}
