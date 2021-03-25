CREATE EXTENSION IF NOT EXISTS timescaledb;

CREATE TABLE sensor_data_defined (
  time TIMESTAMPTZ NOT NULL,
  user_id INT,
  device_id INT,
  humidity NUMERIC(6, 3) CHECK (humidity >= 0),
  temprature NUMERIC(10, 4)
);
