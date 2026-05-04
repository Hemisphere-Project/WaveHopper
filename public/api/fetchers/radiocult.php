<?php
// Radiocult now-playing fetcher.
// Source: GET https://api.radiocult.fm/api/station/{slug}/schedule?startDate=...&endDate=...
// Returns { "schedules": [{ startDateUtc, endDateUtc, title, ... }, ...] } sorted by start.
// All timestamps are UTC ISO 8601 (.000Z suffix). No timezone conversion needed.
//
// Mapping: title → title (show name; Noods embeds the host in the title, e.g. "Strung Out w/ Izzy Twist")
//          subtitle → null (no separate host/artist field in the API)
// Cache key: radiocult-<station-id> (30 s — show-level, typically 1-hr slots)
// Slug: taken from nowPlaying.slug in station JSON; falls back to station id.

declare(strict_types=1);

function wh_fetch_nowplaying_radiocult(string $id, array $cfg, array $station): ?array {
    return wh_cached("radiocult-{$id}", 30, function() use ($id, $cfg): ?array {
        $slug   = (string)($cfg['slug'] ?? $id);
        $now    = time();
        $window = $now + 3 * 3600; // 3-hour window covers current + next

        $url = 'https://api.radiocult.fm/api/station/'
             . rawurlencode($slug)
             . '/schedule?startDate='
             . rawurlencode(gmdate('Y-m-d\TH:i:s.000\Z', $now))
             . '&endDate='
             . rawurlencode(gmdate('Y-m-d\TH:i:s.000\Z', $window));

        $raw = wh_http_get($url, [], 4000);
        if ($raw === null) return null;

        $json = json_decode($raw, true);
        if (!is_array($json) || !isset($json['schedules']) || !is_array($json['schedules'])) {
            return null;
        }

        $schedules = $json['schedules'];
        $current   = null;
        $next      = null;

        foreach ($schedules as $i => $entry) {
            if (!isset($entry['startDateUtc'], $entry['endDateUtc'], $entry['title'])) continue;

            $startTs = strtotime((string)$entry['startDateUtc']);
            $endTs   = strtotime((string)$entry['endDateUtc']);
            if ($startTs === false || $endTs === false) continue;

            if ($startTs <= $now && $now < $endTs) {
                $current              = $entry;
                $current['_startTs']  = $startTs;
                $current['_endTs']    = $endTs;
                // Next: first subsequent entry with a title.
                for ($j = $i + 1; $j < count($schedules); $j++) {
                    if (isset($schedules[$j]['startDateUtc'], $schedules[$j]['title'])) {
                        $next = $schedules[$j];
                        break;
                    }
                }
                break;
            }
        }

        if ($current === null) return null;

        $cleanStr = function(string $raw): string {
            return trim(html_entity_decode($raw, ENT_QUOTES | ENT_HTML5, 'UTF-8'));
        };

        $title = $cleanStr((string)$current['title']);
        if ($title === '') return null;

        $result = [
            'title'    => $title,
            'subtitle' => null,
            'starts'   => gmdate('Y-m-d\TH:i:s\Z', $current['_startTs']),
            'ends'     => gmdate('Y-m-d\TH:i:s\Z', $current['_endTs']),
        ];

        if ($next !== null) {
            $nextTitle = $cleanStr((string)($next['title'] ?? ''));
            $nextStart = isset($next['startDateUtc']) ? strtotime((string)$next['startDateUtc']) : false;
            if ($nextTitle !== '' && $nextStart !== false) {
                $result['next'] = [
                    'title'  => $nextTitle,
                    'starts' => gmdate('Y-m-d\TH:i:s\Z', $nextStart),
                ];
            }
        }

        return $result;
    });
}
