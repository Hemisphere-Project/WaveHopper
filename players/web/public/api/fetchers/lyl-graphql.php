<?php
// LYL Radio now-playing fetcher.
// Source: LYL's Strapi GraphQL API (default https://strapi.lyl.live/graphql,
// overridable via nowPlaying.endpoint). The old api.lyl.live/graphql died in
// 2026 — every call failed and the cache served a July show for months.
//
// One POST fetches both:
//   calendar(from, to) { start end title artists }  — schedule slots, UTC ISO 8601
//   onair { title }                                  — "Artist - Show", no times
// The current slot is the calendar entry containing now(). Slots can overlap
// (seen: 04:00–05:00 and 04:30–05:30), so prefer the one matching `onair`,
// else the latest-starting. Between scheduled slots, fall back to `onair`.
//
// Mapping: artists → title (show host), title → subtitle (show name).
// Cache key: lyl-graphql (30 s — show-level).

declare(strict_types=1);

function wh_fetch_nowplaying_lyl_graphql(string $id, array $cfg, array $station): ?array {
    $endpoint = (string)($cfg['endpoint'] ?? 'https://strapi.lyl.live/graphql');
    return wh_cached('lyl-graphql', 30, function() use ($endpoint): ?array {
        $now = time();
        $payload = json_encode([
            'query' => 'query($from: DateTime!, $to: DateTime!) { '
                . 'onair { title } '
                . 'calendar(from: $from, to: $to) { start end title artists } }',
            'variables' => [
                'from' => gmdate('Y-m-d\TH:i:s\Z', $now - 12 * 3600),
                'to'   => gmdate('Y-m-d\TH:i:s\Z', $now + 12 * 3600),
            ],
        ]);
        $raw = wh_http_post($endpoint, (string)$payload, ['Content-Type: application/json'], 4000);
        if ($raw === null) return null;
        $json = json_decode($raw, true);
        if (!is_array($json) || !isset($json['data']) || !is_array($json['data'])) return null;

        $clean = function($raw): ?string {
            if (!is_string($raw)) return null;
            $s = trim(html_entity_decode($raw, ENT_QUOTES | ENT_HTML5, 'UTF-8'));
            return $s !== '' ? $s : null;
        };
        $onair = $clean($json['data']['onair']['title'] ?? null);

        // Normalise the calendar into [start, end, host, show] slots, sorted.
        $slots = [];
        foreach (($json['data']['calendar'] ?? []) as $e) {
            if (!is_array($e)) continue;
            $s = isset($e['start']) ? strtotime((string)$e['start']) : false;
            $t = isset($e['end']) ? strtotime((string)$e['end']) : false;
            if ($s === false || $t === false || $t <= $s) continue;
            $slots[] = ['start' => $s, 'end' => $t,
                        'host' => $clean($e['artists'] ?? null), 'show' => $clean($e['title'] ?? null)];
        }
        usort($slots, fn($a, $b) => $a['start'] <=> $b['start']);

        $current = null;
        foreach ($slots as $slot) {
            if ($slot['start'] > $now || $now >= $slot['end']) continue;
            $label = ($slot['host'] ?? '') . ' - ' . ($slot['show'] ?? '');
            if ($onair !== null && strcasecmp($label, $onair) === 0) { $current = $slot; break; }
            if ($current === null || $slot['start'] > $current['start']) $current = $slot;
        }

        if ($current !== null && ($current['host'] ?? $current['show']) !== null) {
            $result = [
                'title'    => $current['host'] ?? $current['show'],
                'subtitle' => $current['host'] !== null ? $current['show'] : null,
                'starts'   => gmdate('Y-m-d\TH:i:s\Z', $current['start']),
                'ends'     => gmdate('Y-m-d\TH:i:s\Z', $current['end']),
            ];
            $after = $current['end'];
        } elseif ($onair !== null) {
            // Unscheduled gap: "Artist - Show" → host / show, no times.
            $parts = explode(' - ', $onair, 2);
            $result = [
                'title'    => trim($parts[0]),
                'subtitle' => isset($parts[1]) && trim($parts[1]) !== '' ? trim($parts[1]) : null,
                'starts'   => null,
                'ends'     => null,
            ];
            $after = $now;
        } else {
            return null;
        }

        foreach ($slots as $slot) {
            if ($slot['start'] < $after) continue;
            $nextTitle = $slot['host'] ?? $slot['show'];
            if ($nextTitle === null) continue;
            $result['next'] = ['title' => $nextTitle, 'starts' => gmdate('Y-m-d\TH:i:s\Z', $slot['start'])];
            break;
        }
        return $result;
    });
}
