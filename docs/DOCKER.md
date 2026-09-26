# Docker image

The multi-stage image compiles PageForge with GCC 14, copies only the executable
into a Debian Bookworm runtime, and runs it as the unprivileged `pageforge`
user. Database files live under `/data`; use a named volume to preserve them
across the intentionally short-lived CLI containers.

```bash
docker build --tag pageforge .
docker volume create pageforge-data

docker run --rm --mount source=pageforge-data,target=/data \
  pageforge demo.db init
docker run --rm --mount source=pageforge-data,target=/data \
  pageforge demo.db create-table people id:int name:text active:bool
docker run --rm --mount source=pageforge-data,target=/data \
  pageforge demo.db insert people 1 Ada true
docker run --rm --mount source=pageforge-data,target=/data \
  pageforge demo.db query "SELECT * FROM people ORDER BY id;"
```

For the interactive shell, allocate a terminal and keep standard input open:

```bash
docker run --rm -it --mount source=pageforge-data,target=/data \
  pageforge demo.db shell
```

The image entrypoint is `pageforge`, so arguments after the image name match
the native CLI exactly. A host bind mount may require matching the container
process to the host directory's ownership, for example with
`--user "$(id -u):$(id -g)"`; named volumes avoid that platform-specific setup.

CI builds the image from scratch and runs `tests/docker_smoke.sh`. That test
uses one named volume across separate containers to verify durable table
creation, inserts, ordered SQL output, and the pipe-friendly shell.
