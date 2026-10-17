# Shared helpers for the import repros. Source this file; do not run it.
# Starts Percona Server 8.4 from ./server (debug tarball fetched by bootstrap.sh).

set -u
export LANG=C.UTF-8
REPRO_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BASEDIR=${BASEDIR:-$REPRO_DIR/server}
MYSQLD=$BASEDIR/bin/mysqld
MYSQL=$BASEDIR/bin/mysql
mkdir -p "$REPRO_DIR/run" "$REPRO_DIR/logs"

# Pick a free TCP port starting at $1.
free_port() {
  local p=$1
  while (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null; do p=$((p + 1)); done
  echo $p
}

# setup_instance NAME: sets RUN, DATADIR, SOCK, PORT, ERRLOG for this repro.
setup_instance() {
  NAME=$1
  RUN=$REPRO_DIR/run/$NAME
  DATADIR=$RUN/data
  SOCK=$RUN/mysqld.sock
  ERRLOG=$REPRO_DIR/logs/$NAME.err; : > "$ERRLOG"
  PORT=$(free_port $((23300 + RANDOM % 500)))
  EXTRA_OPTS=${EXTRA_OPTS:-}
}

init_datadir() {
  rm -rf "$RUN" && mkdir -p "$RUN"
  "$MYSQLD" --no-defaults --initialize-insecure --basedir="$BASEDIR" \
    --datadir="$DATADIR" --innodb-use-native-aio=0 $EXTRA_OPTS \
    > "$REPRO_DIR/logs/$NAME.init.log" 2>&1 || {
    echo "initialize failed, see logs/$NAME.init.log"; exit 1; }
}

start_server() {
  "$MYSQLD" --no-defaults --basedir="$BASEDIR" --datadir="$DATADIR" \
    --socket="$SOCK" --port=$PORT --mysqlx=OFF --innodb-use-native-aio=0 \
    --log-error="$ERRLOG" --pid-file="$RUN/mysqld.pid" --secure-file-priv="" \
    $EXTRA_OPTS "$@" &
  MYSQLD_PID=$!
  echo "$MYSQLD_PID" >> "$REPRO_DIR/run/pids"
  local i
  for i in $(seq 1 300); do
    if "$MYSQL" -uroot -S "$SOCK" -e "SELECT 1" >/dev/null 2>&1; then
      "$MYSQL" -uroot -S "$SOCK" -e "CREATE DATABASE IF NOT EXISTS test" >/dev/null 2>&1
      return 0
    fi
    if ! kill -0 $MYSQLD_PID 2>/dev/null; then
      echo "mysqld exited during startup, tail of $ERRLOG:"; tail -20 "$ERRLOG"; return 1
    fi
    sleep 0.2
  done
  echo "mysqld did not start"; return 1
}

stop_server() {
  [ -n "${MYSQLD_PID:-}" ] || return 0
  "$MYSQL" -uroot -S "$SOCK" -e "SHUTDOWN" >/dev/null 2>&1 || kill $MYSQLD_PID 2>/dev/null
  local i
  for i in $(seq 1 300); do kill -0 $MYSQLD_PID 2>/dev/null || break; sleep 0.2; done
  kill -0 $MYSQLD_PID 2>/dev/null && kill -9 $MYSQLD_PID
  wait $MYSQLD_PID 2>/dev/null
  MYSQLD_PID=
}

# Wait for our mysqld to exit on its own (e.g. after an assertion).
wait_server_gone() {
  local i
  for i in $(seq 1 100); do kill -0 $MYSQLD_PID 2>/dev/null || { wait $MYSQLD_PID 2>/dev/null; MYSQLD_PID=; return 0; }; sleep 0.2; done
  return 1
}

restart_server() { stop_server; start_server "$@"; }

# sql "statements": run SQL, echo the statements and the result (errors included).
sql() {
  echo "mysql> $1"
  "$MYSQL" -uroot -S "$SOCK" --table -e "$1" test 2>&1 | sed 's/^/  /'
  return ${PIPESTATUS[0]}
}

sql_val() { "$MYSQL" -uroot -S "$SOCK" -N -B -e "$1" test 2>&1; }

trap 'stop_server' EXIT
