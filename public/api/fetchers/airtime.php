<?php
// Airtime / LibreTime now-playing fetcher.
// Works with any Sourcefabric Airtime tenant that exposes /api/live-info-v2.
// The endpoint is stored per-station in nowPlaying.endpoint.
//
// Two modes depending on what's on air:
//   "track"      — auto-DJ is playing a file. metadata.track_title / artist_name
//                  are used directly as title / subtitle (TTL 20 s).
//   "livestream" — a live DJ source is connected. tracks.current.name is empty;
//                  use shows.current.name as the sole title (TTL 30 s).
//
// Timestamps are in the station's local timezone (station.timezone); we
// convert to UTC ISO 8601 for the normalized shape.
// Cache key: airtime-<station-id> — one entry per station.

declare(strict_types=1);

function wh_fetch_nowplaying_airtime(string $id, array $cfg, array $station): ?array {
    $endpoint = isset($cfg['endpoint']) ? trim((string)$cfg['endpoint']) : '';
    if ($endpoint === '') return null;

    $cacheKey = 'airtime-' . $id;

    // Use 20 s TTL — safe for both track-level and show-level data.
    return wh_cached($cacheKey, 20, function() use ($endpoint): ?array {
        $json = wh_http_get_json($endpoint);
        if (!is_array($json)) return null;

        // Determine station timezone for timestamp conversion.
        $tz = isset($json['station']['timezone']) && is_string($json['station']['timezone'])
            ? $json['station']['timezone']
            : 'UTC';

        $toUtc = function(?string $local) use ($tz): ?string {
            if ($local === null || $local === '') return null;
            try {
                $dt = new \DateTime($local, new \DateTimeZone($tz));
                $dt->setTimezone(new \DateTimeZone('UTC'));
                return $dt->format(\DateTime::ATOM);
            } catch (\Throwable $e) {
                return null;
            }
        };

        // Names arrive HTML-entity-encoded (e.g. "&amp;") and sometimes with
        // a leading space — normalise both.
        $cleanName = function(?string $raw): ?string {
            if ($raw === null) return null;
            $s = trim(html_entity_decode($raw, ENT_QUOTES | ENT_HTML5, 'UTF-8'));
            return $s !== '' ? $s : null;
        };

        $trackCurrent = is_array($json['tracks']['current'] ?? null) ? $json['tracks']['current'] : null;
        $trackType    = isset($trackCurrent['type']) ? (string)$trackCurrent['type'] : '';
        $metadata     = is_array($trackCurrent['metadata'] ?? null) ? $trackCurrent['metadata'] : null;

        if ($trackType === 'track' && $metadata !== null) {
            // Auto-DJ: use track-level title + artist from metadata sub-object.
            $title = $cleanName($metadata['track_title'] ?? null);
            if ($title === null) {
                // Fallback: parse the "artist - title" composite name field.
                $composite = $cleanName($trackCurrent['name'] ?? null);
                if ($composite !== null && str_contains($composite, ' - ')) {
                    [$artist, $title] = explode(' - ', $composite, 2);
                    $title = trim($title);
                } else {
                    $title = $composite;
                }
            }
            if ($title === null) return null;

            return [
                'title'    => $title,
                'subtitle' => $cleanName($metadata['artist_name'] ?? null),
                'starts'   => $toUtc($trackCurrent['starts'] ?? null),
                'ends'     => $toUtc($trackCurrent['ends']   ?? null),
            ];
        }

        // Livestream / fallback: show-level data only.
        $showCurrent = is_array($json['shows']['current'] ?? null) ? $json['shows']['current'] : null;
        if ($showCurrent === null) return null;

        $title = $cleanName($showCurrent['name'] ?? null);
        if ($title === null) return null;

        $result = [
            'title'    => $title,
            'subtitle' => null,
            'starts'   => $toUtc($showCurrent['starts'] ?? null),
            'ends'     => $toUtc($showCurrent['ends']   ?? null),
        ];

        // First item in next-shows array, if present.
        $nextShows = is_array($json['shows']['next'] ?? null) ? $json['shows']['next'] : [];
        $nextShow  = !empty($nextShows) && is_array($nextShows[0]) ? $nextShows[0] : null;
        if ($nextShow !== null) {
            $nextTitle = $cleanName($nextShow['name'] ?? null);
            if ($nextTitle !== null) {
                $result['next'] = [
                    'title'  => $nextTitle,
                    'starts' => $toUtc($nextShow['starts'] ?? null),
                ];
            }
        }

        return $result;
    });
}
