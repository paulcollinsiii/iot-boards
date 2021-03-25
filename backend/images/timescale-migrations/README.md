Creating migrations
-------------------

`docker run -u $(id -u):$(id -g) -v $(pwd)/migrations:/migrations --rm migrate/migrate create -ext sql -dir /migrations -seq -digits 4 MIGRATION_NAME`
