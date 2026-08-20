#!/bin/bash
# Run a command and report elapsed seconds and peak RSS in KiB, by polling
# /proc. (GNU time is not present in every image, so don't depend on it.)
"$@" &
pid=$!
peak=0
while kill -0 $pid 2>/dev/null; do
  v=$(awk '/^VmHWM:/{print $2}' /proc/$pid/status 2>/dev/null)
  [ -n "${v:-}" ] && [ "$v" -gt "$peak" ] && peak=$v
  sleep 0.2
done
wait $pid; rc=$?
echo "peak_rss_kib=$peak" >&2
exit $rc
