<?php
// AzuraCast now-playing fetcher.
// Works with any AzuraCast tenant that exposes /api/nowplaying/<shortcode>.
// The full endpoint is stored per-station in nowPlaying.endpoint.
//
// Two modes depending on what's on air:
//   live DJ      — live.is_live === true. Use the streamer name as subtitle and
//                  the current song title (if any) as title; if the song is empty
//                  the streamer name becomes the title (TTL 20 s).
//   auto-DJ      — song.title / song.artist drive title / subtitle. When artist is
//                  empty, fall back to the playlist tag (e.g. "[SHOW] - X" → "X").
//
// AzuraCast emits Unix timestamps in UTC for played_at / cued_at — no timezone
// conversion needed (unlike Airtime).
// Cache key: azuracast-<station-id> — one entry per station.

declare(strict_types=1);

function wh_fetch_nowplaying_azuracast(string $id, array $cfg, array $station): ?array {
    $endpoint = isset($cfg['endpoint']) ? trim((string)$cfg['endpoint']) : '';
    if ($endpoint === '') return null;

    $cacheKey = 'azuracast-' . $id;

    return wh_cached($cacheKey, 20, function() use ($endpoint): ?array {
        $json = wh_http_get_json($endpoint);
        if (!is_array($json)) return null;
        if (empty($json['is_online'])) return null;

        $clean = function($raw): ?string {
            if ($raw === null) return null;
            $s = trim(html_entity_decode((string)$raw, ENT_QUOTES | ENT_HTML5, 'UTF-8'));
            return $s !== '' ? $s : null;
        };

        $isoUtc = function($unix): ?string {
            if (!is_numeric($unix) || (int)$unix <= 0) return null;
            return gmdate('Y-m-d\TH:i:s\Z', (int)$unix);
        };

        $live = is_array($json['live'] ?? null) ? $json['live'] : [];
        $np   = is_array($json['now_playing'] ?? null) ? $json['now_playing'] : [];
        $song = is_array($np['song'] ?? null) ? $np['song'] : [];

        $songTitle  = $clean($song['title']  ?? null);
        $songArtist = $clean($song['artist'] ?? null);

        $title = null;
        $subtitle = null;

        if (!empty($live['is_live'])) {
            // Live DJ: streamer headlines. Song title (if any) is the secondary line.
            $streamer = $clean($live['streamer_name'] ?? null);
            $title    = $streamer ?? $songTitle;
            $subtitle = $streamer !== null ? $songTitle : $songArtist;
        } else {
            $title    = $songTitle;
            $subtitle = $songArtist;
            if ($subtitle === null) {
                // Strip "[TAG] - " prefix from playlist name and use it as a soft fallback.
                $playlist = $clean($np['playlist'] ?? null);
                if ($playlist !== null) {
                    $stripped = preg_replace('/^\[[A-Z]+\]\s*-\s*/', '', $playlist);
                    $subtitle = $stripped !== '' ? $stripped : null;
                }
            }
        }

        if ($title === null) return null;

        $playedAt = $np['played_at'] ?? null;
        $duration = $np['duration']  ?? null;

        $result = [
            'title'    => $title,
            'subtitle' => $subtitle,
            'starts'   => $isoUtc($playedAt),
            'ends'     => (is_numeric($playedAt) && is_numeric($duration))
                ? $isoUtc((int)$playedAt + (int)$duration)
                : null,
        ];

        $nextSong = is_array($json['playing_next']['song'] ?? null) ? $json['playing_next']['song'] : null;
        $nextTitle = $nextSong ? $clean($nextSong['title'] ?? null) : null;
        if ($nextTitle !== null) {
            $result['next'] = [
                'title'  => $nextTitle,
                'starts' => $isoUtc($json['playing_next']['cued_at'] ?? null),
            ];
        }

        return $result;
    });
}
