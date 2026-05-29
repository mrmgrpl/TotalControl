-- TotalControlData.db — reference database schema
-- Create with: sqlite3 TotalControlData.db < schema_data.sql
-- Then populate using DB Browser for SQLite or any SQLite tool.
-- The app opens this file READ-ONLY — it never modifies it.

PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;

-- ── Schema version ────────────────────────────────────────────────────────────
CREATE TABLE IF NOT EXISTS schema_version (
    version  INTEGER NOT NULL
);
INSERT OR IGNORE INTO schema_version VALUES (1);

-- ── Solar Eclipse Catalog ─────────────────────────────────────────────────────
-- Source: NASA Five Millennium Canon of Solar Eclipses (-1999 to +3000)
-- https://eclipse.gsfc.nasa.gov/SEcat5/SEcatalog.html
-- 10 354 entries covering all eclipse types (T, A, H, P)

CREATE TABLE IF NOT EXISTS eclipses (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    nasa_num        INTEGER UNIQUE NOT NULL,  -- NASA catalog number
    date_utc        TEXT    NOT NULL,          -- YYYY-MM-DD
    greatest_utc    TEXT,                      -- HH:MM:SS UTC of greatest eclipse
    delta_t_s       REAL,                      -- ΔT in seconds (TDT − UTC)
    lunation        INTEGER,                   -- Brown lunation number
    saros           INTEGER,                   -- Saros series number
    saros_member    INTEGER,                   -- member number within Saros
    type            TEXT    NOT NULL,          -- T=Total A=Annular H=Hybrid P=Partial
    gamma           REAL,                      -- distance of shadow axis from Earth centre
    magnitude       REAL,                      -- eclipse magnitude
    lat_greatest    REAL,                      -- latitude  of greatest eclipse (°N)
    lon_greatest    REAL,                      -- longitude of greatest eclipse (°E)
    sun_alt_deg     REAL,                      -- Sun altitude at greatest eclipse (°)
    path_width_km   REAL,                      -- central path width (km); NULL for partial
    duration_s      INTEGER,                   -- max totality/annularity (s); NULL for partial
    notes           TEXT
);

CREATE INDEX IF NOT EXISTS idx_eclipses_date ON eclipses (date_utc);
CREATE INDEX IF NOT EXISTS idx_eclipses_type ON eclipses (type);
CREATE INDEX IF NOT EXISTS idx_eclipses_saros ON eclipses (saros);

-- ── Observer locations for specific eclipses ──────────────────────────────────
-- One eclipse can have multiple observer points (e.g. Burgos AND Cairo).

CREATE TABLE IF NOT EXISTS eclipse_locations (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    eclipse_id  INTEGER NOT NULL REFERENCES eclipses(id),
    name        TEXT    NOT NULL,   -- e.g. "Burgos, Spain"
    country     TEXT,
    lat         REAL    NOT NULL,   -- observer latitude  (°N)
    lon         REAL    NOT NULL,   -- observer longitude (°E)
    altitude_m  INTEGER DEFAULT 0,
    c1_utc      TEXT,               -- 1st contact  HH:MM:SS.mmm UTC
    c2_utc      TEXT,               -- 2nd contact (start totality)
    c3_utc      TEXT,               -- 3rd contact (end totality)
    c4_utc      TEXT,               -- 4th contact
    totality_s  INTEGER,            -- totality duration at this location (s)
    tz_iana     TEXT,               -- IANA timezone, e.g. "Europe/Madrid"
    notes       TEXT
);

-- ── Camera specifications ─────────────────────────────────────────────────────
-- Sony Alpha bodies supported by CrSDK.

CREATE TABLE IF NOT EXISTS cameras (
    id               INTEGER PRIMARY KEY AUTOINCREMENT,
    manufacturer     TEXT    NOT NULL DEFAULT 'Sony',
    model            TEXT    NOT NULL UNIQUE,  -- e.g. ILCE-7RM4A
    marketing_name   TEXT,                     -- e.g. "α7R IVA"
    sensor_type      TEXT,                     -- e.g. BSI-CMOS, Exmor RS
    megapixels       REAL,
    sensor_w_mm      REAL,                     -- sensor width  mm
    sensor_h_mm      REAL,                     -- sensor height mm
    crop_factor      REAL    DEFAULT 1.0,
    iso_min          INTEGER,                  -- base ISO (e.g. 100)
    iso_max          INTEGER,                  -- max extended ISO
    ss_max_frac      INTEGER,                  -- fastest shutter as 1/N (e.g. 8000)
    ss_min_s         REAL,                     -- slowest shutter in seconds (e.g. 30)
    mount            TEXT    DEFAULT 'E-mount',
    crsdk_supported  INTEGER DEFAULT 1,        -- 0/1
    usb_protocol     TEXT    DEFAULT 'PTP/IP',
    notes            TEXT
);

-- ── Lenses / optics ───────────────────────────────────────────────────────────

CREATE TABLE IF NOT EXISTS lenses (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    manufacturer    TEXT,
    model           TEXT    NOT NULL,
    focal_min_mm    REAL    NOT NULL,
    focal_max_mm    REAL    NOT NULL,  -- same as focal_min_mm for primes
    aperture_min    REAL    NOT NULL,  -- widest (e.g. 2.8)
    aperture_max    REAL,              -- narrowest (e.g. 22)
    mount           TEXT    DEFAULT 'E-mount',
    notes           TEXT
);

-- ── Planned shooting configurations per eclipse ───────────────────────────────

CREATE TABLE IF NOT EXISTS eclipse_configs (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    eclipse_id      INTEGER NOT NULL REFERENCES eclipses(id),
    location_id     INTEGER REFERENCES eclipse_locations(id),
    camera_id       INTEGER REFERENCES cameras(id),
    lens_id         INTEGER REFERENCES lenses(id),
    focal_mm        REAL,            -- effective focal length (with teleconverter etc.)
    sequence_file   TEXT,            -- path to .json sequence file
    notes           TEXT
);
