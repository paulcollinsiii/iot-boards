Data is sent to the backend over MQTT as a JSON string
Each message should have the following format

JSON Schema
```json
{
  "title": "SensorData",
  "type": "object",
  "properties": {
    "data": {
    "type": "array",
      "items": {
      "type": "object",
        "properties": {
          "sensor": {"type": "string", "description": "Name of the sensor, e.g. temperature"},
          "value": {"type": ["string", "number"]},
          "units": {"type": "string"},
          "timestamp": {"type": "string", "description": "ISO8601 timestamp in UTC time"}
        }
      }
    }
  }
}
```
