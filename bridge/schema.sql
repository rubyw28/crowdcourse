-- Crowd Course schema for Tiger Data (Postgres + TimescaleDB). Safe to run repeatedly.

-- Every ESP-NOW beacon one bracelet hears from its friend (~10 per second per pair).
CREATE TABLE IF NOT EXISTS readings (
  time      timestamptz NOT NULL,
  bracelet  text        NOT NULL,   -- who heard it
  friend    text        NOT NULL,   -- who sent it
  rssi      smallint    NOT NULL
);
SELECT create_hypertable('readings', by_range('time'), if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS readings_bracelet_time ON readings (bracelet, time DESC);

-- Everything else: pairing, SOS lifecycle, signal lost/found, phones joining, calibration.
CREATE TABLE IF NOT EXISTS events (
  time      timestamptz NOT NULL,
  bracelet  text        NOT NULL,
  friend    text,
  kind      text        NOT NULL,
  detail    jsonb       NOT NULL DEFAULT '{}'
);
SELECT create_hypertable('events', by_range('time'), if_not_exists => TRUE);
CREATE INDEX IF NOT EXISTS events_kind_time ON events (kind, time DESC);

-- 10-second rollup the dashboard charts from, kept fresh by TimescaleDB.
CREATE MATERIALIZED VIEW IF NOT EXISTS readings_10s
WITH (timescaledb.continuous) AS
SELECT time_bucket('10 seconds', time) AS bucket,
       bracelet,
       friend,
       avg(rssi)::real AS rssi,
       count(*)        AS packets
FROM readings
GROUP BY bucket, bracelet, friend
WITH NO DATA;

SELECT add_continuous_aggregate_policy('readings_10s',
  start_offset      => INTERVAL '1 hour',
  end_offset        => INTERVAL '10 seconds',
  schedule_interval => INTERVAL '10 seconds',
  if_not_exists     => TRUE);

-- Pulse and breathing from a Presage check-in, numbers only (no video).
CREATE TABLE IF NOT EXISTS checkins (
  time        timestamptz NOT NULL DEFAULT now(),
  pulse_bpm   real,
  breaths_pm  real,
  detail      jsonb       NOT NULL DEFAULT '{}'
);

-- Readings older than a day move to compressed columnstore, segmented by pair so a
-- pair's history still reads as one contiguous run.
DO $$
BEGIN
  IF NOT (SELECT compression_enabled FROM timescaledb_information.hypertables WHERE hypertable_name = 'readings') THEN
    ALTER TABLE readings SET (
      timescaledb.compress,
      timescaledb.compress_segmentby = 'bracelet, friend',
      timescaledb.compress_orderby = 'time DESC');
  END IF;
END $$;
SELECT add_compression_policy('readings', INTERVAL '1 day', if_not_exists => TRUE);

-- iMessage conversation with the station (Photon), both directions.
CREATE TABLE IF NOT EXISTS messages (
  time       timestamptz NOT NULL DEFAULT now(),
  handle     text        NOT NULL,   -- phone or email of the person texting
  direction  text        NOT NULL,   -- 'in' or 'out'
  body       text        NOT NULL,
  detail     jsonb       NOT NULL DEFAULT '{}'
);
SELECT create_hypertable('messages', by_range('time'), if_not_exists => TRUE);

-- People who texted "watch": they get SOS, lost, and back-in-range texts.
CREATE TABLE IF NOT EXISTS watchers (
  handle     text        PRIMARY KEY,
  space_id   text,
  since      timestamptz NOT NULL DEFAULT now()
);
