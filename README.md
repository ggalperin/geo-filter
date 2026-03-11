# geo-filter

An HTTP service that answers: **"Is this IP address located in one of these allowed countries?"**

---

## API

### `POST /check`

**Request body** (JSON):
```json
{
  "ip": "1.2.3.4",
  "allowed_countries": ["US", "GB", "CA"]
}
```

**Response** (JSON):
```json
{ "allowed": true, "country": "US", "ip": "1.2.3.4" }
```

| Field | Type | Description |
|---|---|---|
| `allowed` | bool | `true` if the IP's country is in `allowed_countries` |
| `country` | string | country code resolved from IP, or `""` |
| `ip` | string | Echo of the input IP |
| `reason` | string | Present when `allowed` is `false` with an explanation |

**HTTP status codes**:
- `200` – Successful lookup (even when `allowed` is `false`)
- `400` – Malformed request
- `503` – GeoIP database not yet loaded

---

### `POST /reload`

Forces an immediate hot-reload of the MMDB from disk. Returns `{"reloaded": true}`.

---

## Quick Start

### 1. Build and run with Docker

```bash
docker compose up --build
```

### 2. Test it

```bash
curl -s -X POST http://localhost:8080/check \
  -H "Content-Type: application/json" \
  -d '{"ip":"1.1.1.1","allowed_countries":["US","GB","CA"]}' | jq
```
---

### In-process file watcher

The service's background `DatabaseWatcher` thread polls the file's `mtime` every `DB_RELOAD_INTERVAL` seconds (default 1 hour). If the file has changed since last load, it reloads using an exclusive lock so in-flight requests are never interrupted.

---

## Configuration

All settings via environment variables:

| Variable | Default | Description |
|---|---|---|
| `MMDB_PATH` | `/data/GeoLite2-Country.mmdb` | Path to the MaxMind DB file |
| `LISTEN_HOST` | `0.0.0.0` | Bind address |
| `LISTEN_PORT` | `8080` | TCP port |
| `DB_RELOAD_INTERVAL` | `3600` | Seconds between file-change checks |

---

## Architecture Notes

- **Thread safety**: `GeoDatabase` uses `std::mutex`; concurrent lookups hold locks while a reload holds an exclusive lock.
- **Hot reload**: new MMDB handle is opened before the lock is acquired; in-flight lookups against the old handle complete normally before it is closed.
- **Zero-copy reads**: `MMDB_MODE_MMAP` memory-maps the database file.
- **Graceful shutdown**: `SIGTERM` / `SIGINT` stop the HTTP server cleanly; the watcher thread joins before process exit.
- **Header-only deps**: `cpp-httplib` and `nlohmann/json` are vendored as single headers — no additional package installs.

---
