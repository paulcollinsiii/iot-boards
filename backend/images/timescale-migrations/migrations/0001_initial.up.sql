CREATE EXTENSION IF NOT EXISTS timescaledb;

CREATE TABLE sensor_data (
  time TIMESTAMPTZ NOT NULL,
  device_id UUID,
  data jsonb
);
