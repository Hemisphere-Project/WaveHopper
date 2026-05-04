<?php
// NTS now-playing fetcher.
// One upstream endpoint covers both live channels — we cache the parsed result
// under a single key ('nts') and pick the right channel by station.channel.
// Mixtapes have no per-track metadata; they are not wired to this fetcher.

declare(strict_types=1);

function wh_fetch_nowplaying_nts(string $id, array $cfg, array $station): ?array {
    $all = wh_cached('nts', 30, function() {
        $json = wh_http_get_json('https://www.nts.live/api/v2/live');
        if (!is_array($json) || !isset($json['results']) || !is_array($json['results'])) {
            return null;
        }
        $out = [];
        foreach ($json['results'] as $row) {
            if (!is_array($row)) continue;
            $ch = isset($row['channel_name']) ? (string)$row['channel_name'] : '';
            if ($ch === '') continue;
            $now  = is_array($row['now']  ?? null) ? $row['now']  : [];
            $next = is_array($row['next'] ?? null) ? $row['next'] : null;

            $title    = isset($now['broadcast_title']) ? (string)$now['broadcast_title'] : null;
            $showName = null;
            $details  = $now['embeds']['details'] ?? null;
            if (is_array($details) && isset($details['name']) && is_string($details['name'])) {
                $showName = $details['name'];
                // Avoid echoing the same string twice when the slot equals the broadcast title.
                if ($title !== null && strcasecmp($showName, $title) === 0) {
                    $showName = null;
                }
            }

            $out[$ch] = [
                'title'    => $title,
                'subtitle' => $showName,
                'starts'   => isset($now['start_timestamp']) ? (string)$now['start_timestamp'] : null,
                'ends'     => isset($now['end_timestamp'])   ? (string)$now['end_timestamp']   : null,
                'next'     => is_array($next) ? [
                    'title'  => isset($next['broadcast_title']) ? (string)$next['broadcast_title'] : null,
                    'starts' => isset($next['start_timestamp']) ? (string)$next['start_timestamp'] : null,
                ] : null,
            ];
        }
        return $out;
    });

    if (!is_array($all)) return null;
    $channel = isset($station['channel']) ? (string)$station['channel'] : '';
    return $all[$channel] ?? null;
}
