# RGW TCMalloc Tuning & Remote Command Relay — Multi-Node Testing Guide

**Branch**: `rgw-tcmalloc-tuning` on `github.com/noobed-max/ceph`
**Scope**: This guide covers testing all three implemented items:

| Item | Feature | Where it runs |
|------|---------|---------------|
| 1 | `rgw_tcmalloc_max_total_thread_cache_bytes` config + `set_heap_property` / `get_heap_property` admin socket commands | RGW node |
| 2 | Periodic memory release (`rgw_enable_periodic_memory_release`, `rgw_periodic_memory_release_interval`) | RGW node |
| 3 | MCommand relay: `ceph rgw heap-property get/set` via MGR, works cross-host | MGR node → RGW node |

> **Note on `heap stats` / `heap dump`**: those subcommands come from upstream PR #67446, which is not merged and not part of this branch. Everything in this guide works without it. `get_heap_property` accepts any tcmalloc/generic property, which covers stats verification (see §4.3).

---

## 1. Topology

Minimum 2 nodes; 3 optional (spread OSDs). The critical requirement is **RGW on a different host from the MGR** so the relay is genuinely cross-host.

```
node-a (10.0.0.1)                 node-b (10.0.0.2)
┌──────────────────────┐          ┌──────────────────────┐
│ MON + MGR + OSD(s)   │◄────────►│ RGW (radosgw)        │
│ (vstart, real IP)    │ messenger│ (manual launch)      │
└──────────────────────┘          └──────────────────────┘
```

Recommended approach for from-source builds: run **vstart on node-a bound to its real IP** (not localhost), then launch **radosgw manually on node-b** with a copied conf + keyring. This avoids full manual mon bootstrap and avoids cephadm (which wants container images, awkward for source builds).

---

## 2. Build (both nodes, or build once + rsync)

On each node (same OS/arch lets you build once on node-a and `rsync` the `build/` tree to node-b):

```bash
git clone https://github.com/noobed-max/ceph.git
cd ceph
git checkout rgw-tcmalloc-tuning
git submodule update --init --recursive
sudo ./install-deps.sh
./do_cmake.sh -DALLOCATOR=tcmalloc -DWITH_MGR_DASHBOARD_FRONTEND=OFF
cd build
ninja -j$(nproc) vstart-base radosgw radosgw-admin
```

Notes:
- `-DALLOCATOR=tcmalloc` is **mandatory** — without it every heap command returns `could not issue heap property command -- not using tcmalloc!`
- `-DWITH_MGR_DASHBOARD_FRONTEND=OFF` skips the npm/nodeenv build (not needed, and broken on PEP-668 distros).
- If RAM < 32GB use `ninja -j4` — RGW debug translation units are memory-hungry.
- node-b strictly only needs the `radosgw` binary (plus libs), but rsyncing the whole build tree is simplest:
  `rsync -a --delete node-a:~/ceph/ ~/ceph/` (source + build, paths must match because build trees embed absolute paths).

---

## 3. Deploy the cluster

### 3.1 node-a: vstart bound to the real IP

```bash
cd ~/ceph/build
# -i <ip>: bind mon to the routable IP so node-b can connect
# no --rgw here: RGW runs on node-b
MON=1 OSD=1 MGR=1 MDS=0 ../src/vstart.sh -n -d -i 10.0.0.1 \
    -o 'mon host = 10.0.0.1'
./bin/ceph -c ceph.conf -s          # expect HEALTH_OK / HEALTH_WARN, 1 mon, 1 mgr, 1 osd
```

Enable the rgw mgr module (needed for the `ceph rgw ...` commands):

```bash
./bin/ceph -c ceph.conf mgr module enable rgw
./bin/ceph -c ceph.conf mgr module ls | grep rgw    # should list rgw as enabled
```

### 3.2 Copy config + keyring to node-b

```bash
scp ~/ceph/build/ceph.conf   node-b:~/ceph/build/ceph.conf
scp ~/ceph/build/keyring     node-b:~/ceph/build/keyring
```

vstart's `ceph.conf` contains a `[client]` section with `keyring = ...` pointing at the vstart keyring path; since the build paths match after rsync, this just works. If you rsynced *before* running vstart, only these two files differ.

### 3.3 node-b: launch RGW with the tuning options

```bash
cd ~/ceph/build
mkdir -p out    # asok/log dir if it doesn't exist

./bin/radosgw -c ceph.conf --name client.rgw.8000 \
    --rgw_frontends "beast port=8000" \
    --admin-socket "$PWD/out/rgw.8000.asok" \
    --log-file "$PWD/out/rgw.8000.log" \
    --debug-rgw 10 --debug-ms 1 \
    --rgw_tcmalloc_max_total_thread_cache_bytes 134217728 \
    --rgw_enable_periodic_memory_release true \
    --rgw_periodic_memory_release_interval 10
```

Verify it registered with the cluster (run on node-a):

```bash
./bin/ceph -c ceph.conf service dump
```

Expected: a `"rgw"` service with one daemon entry. The daemon **key is the rados instance gid** (a number, e.g. `"4127"`); the friendly id (`"8000"`) is in its `metadata.id`. The new CLI commands accept **either**.

```json
"services": {
  "rgw": {
    "daemons": {
      "4127": {
        "addr": "10.0.0.2:0/1234567",
        "metadata": { "id": "8000", "zone_name": "default", ... }
      }
    }
  }
}
```

---

## 4. Test procedures

### 4.1 Item 1 — startup config option applied

On **node-b**:

```bash
grep "tcmalloc.max_total_thread_cache_bytes" out/rgw.8000.log
```

Expected log line:

```
... rgw main: set tcmalloc.max_total_thread_cache_bytes to 134217728
```

Confirm via the admin socket (`ceph` here is node-b's `./bin/ceph`):

```bash
./bin/ceph -c ceph.conf daemon $PWD/out/rgw.8000.asok \
    get_heap_property tcmalloc.max_total_thread_cache_bytes
```

Expected output:

```json
{
    "error": "",
    "success": true,
    "value": 134217728
}
```

### 4.2 Item 1 — local set_heap_property / get_heap_property

On **node-b**:

```bash
# change at runtime
./bin/ceph -c ceph.conf daemon $PWD/out/rgw.8000.asok \
    set_heap_property tcmalloc.max_total_thread_cache_bytes 67108864
# → {"error": "", "success": true}

# read it back
./bin/ceph -c ceph.conf daemon $PWD/out/rgw.8000.asok \
    get_heap_property tcmalloc.max_total_thread_cache_bytes
# → {"error": "", "success": true, "value": 67108864}

# error handling: invalid property
./bin/ceph -c ceph.conf daemon $PWD/out/rgw.8000.asok \
    get_heap_property tcmalloc.no_such_property
# → {"error": "invalid property", "success": false, "value": 0}

# error handling: negative value
./bin/ceph -c ceph.conf daemon $PWD/out/rgw.8000.asok \
    set_heap_property tcmalloc.max_total_thread_cache_bytes -1
# → {"error": "negative value not allowed", "success": false}
```

Other useful readable properties (all via `get_heap_property`):

| Property | Meaning |
|----------|---------|
| `generic.current_allocated_bytes` | Bytes in use by the application |
| `generic.heap_size` | Total heap size |
| `tcmalloc.pageheap_free_bytes` | Free pages held by tcmalloc, not returned to OS |
| `tcmalloc.pageheap_unmapped_bytes` | Pages released back to the OS |
| `tcmalloc.current_total_thread_cache_bytes` | Current thread cache usage |

### 4.3 Item 2 — periodic memory release

RGW was started with release enabled at a 10s interval. The observable effect: `tcmalloc.pageheap_free_bytes` drains to (near) zero and `tcmalloc.pageheap_unmapped_bytes` grows after each cycle; VmRSS drops after load stops.

On **node-b**:

```bash
# 1. generate allocation pressure (see §5 for real S3 load), or simply
#    hammer the frontend with curl for a lightweight test:
for i in $(seq 1 2000); do curl -s -o /dev/null http://localhost:8000/; done

# 2. immediately after load:
./bin/ceph -c ceph.conf daemon $PWD/out/rgw.8000.asok \
    get_heap_property tcmalloc.pageheap_free_bytes
# note the value (nonzero after load)

RGW_PID=$(pgrep -f 'radosgw.*rgw.8000')
grep VmRSS /proc/$RGW_PID/status     # note RSS

# 3. wait one interval (>10s), then re-check:
sleep 15
./bin/ceph -c ceph.conf daemon $PWD/out/rgw.8000.asok \
    get_heap_property tcmalloc.pageheap_free_bytes
# expected: significantly lower (often 0)
./bin/ceph -c ceph.conf daemon $PWD/out/rgw.8000.asok \
    get_heap_property tcmalloc.pageheap_unmapped_bytes
# expected: increased
grep VmRSS /proc/$RGW_PID/status
# expected: lower than step 2
```

With `debug_rgw = 20` the log also shows one line per cycle:
`released free tcmalloc memory to the OS`.

Interval is re-read every cycle, so
`./bin/ceph -c ceph.conf daemon <asok> config set rgw_periodic_memory_release_interval 60`
takes effect from the next tick without restart. Enabling/disabling (`rgw_enable_periodic_memory_release`) requires restart.

### 4.4 Item 3 — remote relay from the MGR node

All commands below run on **node-a** (the MGR node). RGW is on node-b — this validates the cross-host path.

```bash
# by friendly id (metadata.id):
./bin/ceph -c ceph.conf rgw heap-property get 8000 tcmalloc.max_total_thread_cache_bytes
```

Expected output (JSON from the RGW admin socket, relayed back):

```json
{
    "error": "",
    "success": true,
    "value": 67108864
}
```

```bash
# set remotely:
./bin/ceph -c ceph.conf rgw heap-property set 8000 tcmalloc.max_total_thread_cache_bytes 268435456
# → {"error": "", "success": true}

# read back via the gid key instead of the friendly id:
GID=$(./bin/ceph -c ceph.conf service dump -f json | \
      python3 -c 'import json,sys; d=json.load(sys.stdin)["services"]["rgw"]["daemons"]; print([k for k,v in d.items() if isinstance(v,dict)][0])')
./bin/ceph -c ceph.conf rgw heap-property get $GID tcmalloc.max_total_thread_cache_bytes
# → value: 268435456

# confirm on node-b that the remote set really landed:
#   ./bin/ceph -c ceph.conf daemon $PWD/out/rgw.8000.asok \
#       get_heap_property tcmalloc.max_total_thread_cache_bytes
```

**Error handling tests** (node-a):

```bash
# unknown daemon id:
./bin/ceph -c ceph.conf rgw heap-property get nosuchrgw generic.heap_size
# → Error ENOENT: no RGW daemon 'nosuchrgw' found in the service map; known daemons: ['8000']

# stopped daemon: kill radosgw on node-b, wait ~10s, then:
./bin/ceph -c ceph.conf rgw heap-property get 8000 generic.heap_size
# → Error ENOENT (daemon gone from service map / connection dropped)
# if the command was in flight when the connection dropped, it fails with EPIPE:
#   "service daemon connection was reset"
```

Restart RGW on node-b afterwards; within seconds `service dump` shows it again and the relay works — reconnection is handled by the existing MgrClient logic.

### 4.5 Item 3 — relay internals (optional deep verification)

To watch the actual MCommand/MCommandReply exchange, run mgr and rgw with `--debug-ms 1` and grep:

```bash
# node-a mgr log: outgoing relay
grep "relaying command to service daemon" out/mgr.x.log
# → relaying command to service daemon rgw.4127 tid 1 cmd [{"prefix": "get_heap_property", ...}]

# node-a mgr log: reply received
grep "command reply tid" out/mgr.x.log
# → command reply tid 1 r 0

# node-b rgw log (debug-ms 1): the MCommand arriving from the mgr
grep "command(tid" out/rgw.8000.log
```

---

## 5. Realistic S3 load generation

On node-a (or anywhere), create a user:

```bash
./bin/radosgw-admin -c ceph.conf user create --uid=test --display-name=test \
    --access-key=test --secret-key=test
```

With `s3cmd` (pip install s3cmd):

```bash
cat > /tmp/s3cfg <<EOF
[default]
access_key = test
secret_key = test
host_base = 10.0.0.2:8000
host_bucket = 10.0.0.2:8000
use_https = False
EOF

s3cmd -c /tmp/s3cfg mb s3://bench
dd if=/dev/urandom of=/tmp/obj bs=1M count=16
for i in $(seq 1 200); do s3cmd -c /tmp/s3cfg put /tmp/obj s3://bench/obj$i; done
```

Or with awscli: `aws --endpoint-url http://10.0.0.2:8000 s3 cp ...` using the same keys.

**Before/after comparison for the thread cache tuning** (the actual point of Item 1):

1. Run the workload with the tcmalloc default (`rgw_tcmalloc_max_total_thread_cache_bytes = 0`, i.e. 32MB shared) and record throughput + `tcmalloc.current_total_thread_cache_bytes` during load.
2. Restart RGW with `--rgw_tcmalloc_max_total_thread_cache_bytes 268435456` and repeat.
3. Under allocator-bound concurrency (hundreds of parallel small ops), the larger cache reduces central freelist contention. `perf top -p $RGW_PID` during load: time in `tcmalloc::CentralFreeList::*` / `SpinLock` should drop.

---

## 6. Single-node quick smoke test (before multi-node)

Everything except "cross-host" can be verified on one box:

```bash
cd ~/ceph/build
MON=1 OSD=1 MGR=1 MDS=0 RGW=1 ../src/vstart.sh -n -d \
    -o 'rgw_tcmalloc_max_total_thread_cache_bytes = 134217728' \
    -o 'rgw_enable_periodic_memory_release = true' \
    -o 'rgw_periodic_memory_release_interval = 10'
./bin/ceph -c ceph.conf mgr module enable rgw
ls out/*.asok                                  # find the rgw asok (radosgw.8000.asok)
./bin/ceph -c ceph.conf daemon out/radosgw.8000.asok \
    get_heap_property tcmalloc.max_total_thread_cache_bytes
./bin/ceph -c ceph.conf rgw heap-property get 8000 generic.heap_size
../src/stop.sh
```

The relay path (mgr → messenger → rgw → admin socket → reply) is identical co-located and cross-host; only address reachability differs.

---

## 7. Troubleshooting

| Symptom | Cause / Fix |
|---------|-------------|
| `could not issue heap property command -- not using tcmalloc!` | Built without `-DALLOCATOR=tcmalloc`, or gperftools not installed at configure time. Check `grep ALLOCATOR build/CMakeCache.txt`. |
| `no valid command found` for `get_heap_property` on the asok | Running an old radosgw binary — rebuild/redeploy `bin/radosgw` from this branch. |
| `ceph rgw heap-property ...` → `no valid command found` | MGR not rebuilt (`ninja ceph-mgr`) or rgw module not enabled (`ceph mgr module enable rgw`), or mgr not restarted after updating `module.py`. `ceph mgr fail` forces a respawn. |
| `Error ENOENT: unknown service type or no connected daemon: rgw.<gid>` | RGW's MgrClient session isn't established yet (starting up / mgr failover just happened). Wait a few seconds; check `ceph service dump`. |
| Relay command hangs | RGW has no admin socket thread (`admin_socket` option empty?). Verify the asok file exists on the RGW node. The mgr side has no timeout — Ctrl-C and fix the RGW side. |
| `no RGW daemon 'X' found in the service map` | Wrong id. Use `ceph service dump` — either the numeric gid key or `metadata.id` works. |
| mgr crashes loading rgw module | Missing python deps for the mgr's python (PyYAML). `pip install pyyaml` for the interpreter cmake picked (`grep WITH_PYTHON3 build/CMakeCache.txt`). |
| node-b can't connect: `unable to find a keyring` / auth errors | Copy `build/keyring` from node-a and make sure `ceph.conf` keyring path matches. |
| node-b hangs connecting to mon | Mon bound to localhost. Re-run vstart with `-i <real-ip>`; check `ss -ltnp | grep ceph-mon` shows the routable IP, and open firewall ports 3300/6789 + 6800-7300. |
| VmRSS doesn't drop after release | Expected if the heap is genuinely in use (no free pages to return). Check `tcmalloc.pageheap_free_bytes` before concluding; also transparent hugepages can mask madvise effects. |

---

## 8. Teardown

```bash
# node-b
pkill -f 'radosgw.*rgw.8000'

# node-a
cd ~/ceph/build && ../src/stop.sh
rm -rf out dev            # wipe vstart data if you want a clean slate
```

---

## Appendix: what changed on this branch (for review context)

| File | Change |
|------|--------|
| `src/common/options/rgw.yaml.in` | 3 new options: `rgw_tcmalloc_max_total_thread_cache_bytes`, `rgw_enable_periodic_memory_release`, `rgw_periodic_memory_release_interval` |
| `src/rgw/rgw_appmain.cc` | `RGWHeapPropertyHook` (asok `set/get_heap_property`), startup application of the thread cache size, `SafeTimer`-based periodic `ceph_heap_release_free_memory()` |
| `src/rgw/rgw_main.h` | hook pointer, timer + lock members |
| `src/rgw/CMakeLists.txt` | link `heap_profiler` into `radosgw` and `rgw` |
| `src/mgr/DaemonServer.{h,cc}` | track service daemon connections; `send_command_to_service_daemon()`; `handle_command_reply()` (MCommandReply + tid map); cleanup on reset/close |
| `src/mgr/BaseMgrModule.cc` | `ceph_send_command` falls back to the service-daemon relay for non-{mon,osd,mds,pg} types |
| `src/mgr/ActivePyModules.h` | `get_server()` accessor |
| `src/mgr/MgrClient.cc` | dispatch incoming `MCommand` from the mgr to the local admin socket (`queue_tell_command`) |
| `src/pybind/mgr/rgw/module.py` | `rgw heap-property get/set` CLI commands + daemon id resolution (gid or friendly id) |
