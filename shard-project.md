# shard

**Minimal Distributed Task Runner for Everyone**

> Distribute any workload — shell commands or code functions — across your home lab, office machines, and cloud VMs. One binary, one config file, zero infrastructure.

---

## The Problem

Distributed computing today is dominated by heavy, complex tools:

| Tool | Problem |
|---|---|
| Kubernetes | Overkill for 90% of use cases, weeks to learn |
| Ray | Python-only, heavy dependency, complex setup |
| Dask | pandas ecosystem lock-in, hard to deploy |
| Celery | Requires Redis/RabbitMQ, config-heavy |
| MPI | Academic, painful DX |
| Coiled | Managed but expensive, cloud-only |

Meanwhile, real people have real needs:

- A developer has 3 old laptops sitting in a drawer and wants to use them for CI
- A researcher wants to run 1000 simulations across home + a few cloud VMs
- A small team wants to parallelize their build across dev machines
- A hobbyist wants to render a video using every device in the house

**None of these should require Kubernetes.**

---

## What shard Does

```bash
# On any machine, run one command
shard start

# Define your cluster in one file
# cluster.toml
[[nodes]]
name = "laptop"
host = "192.168.1.10"

[[nodes]]
name = "old-desktop"
host = "192.168.1.15"

[[nodes]]
name = "ec2-worker"
host = "ec2-13-58-...compute.amazonaws.com"
key  = "~/.ssh/aws-key.pem"

# Run a task
shard exec "make -j4" --on all
shard exec "python train.py" --on gpu-nodes
shard exec build.sh --distribute
```

**Output:**

```
[laptop]      make -j4                  ✓ completed (2m 14s)
[old-desktop] make -j4                  ✓ completed (5m 03s)  
[ec2-worker]  make -j4                  ✓ completed (1m 47s)

3/3 tasks completed in 5m 03s (fastest parallel)
Total CPU time: 9m 04s (81% speedup vs sequential)
```

---

## Core Philosophy

```
✅ Single binary — no runtime, no dependencies
✅ Works with anything that has SSH
✅ Homogeneous OR heterogeneous clusters
✅ Home + cloud in the same cluster
✅ Config-first, not code-first
✅ Shell commands OR code functions
✅ Terminal-native, web when needed
```

**What we don't do:**
- No custom scheduler DSL
- No proprietary protocol
- No cloud lock-in
- No mandatory agent installation on every node
- No YAML hell

---

## Architecture

```
┌─────────────────────────────────────────────┐
│              shard controller             │
│  (your laptop, or a dedicated coordinator)  │
│                                             │
│   ┌──────────┐  ┌──────────┐  ┌─────────┐  │
│   │ Web UI   │  │  TUI     │  │  CLI    │  │
│   └──────────┘  └──────────┘  └─────────┘  │
│                     │                       │
│              ┌──────┴──────┐                │
│              │  Scheduler  │                │
│              └──────┬──────┘                │
└─────────────────────┼───────────────────────┘
                      │
                 SSH + Agent
                      │
        ┌─────────────┼─────────────┐
        │             │             │
   ┌────▼────┐  ┌─────▼────┐  ┌────▼─────┐
   │ Home PC │  │  Laptop  │  │  EC2 VM  │
   │  agent  │  │  agent   │  │  agent   │
   └─────────┘  └──────────┘  └──────────┘
```

**Two modes:**

**1. Agentless mode** — Uses pure SSH, works on any Linux/Mac with SSH access. Great for one-off jobs, no installation needed.

**2. Agent mode** — Lightweight C binary on each node. Better for continuous use, health monitoring, task queuing.

---

## Technical Stack

| Component | Technology | Why |
|---|---|---|
| Core | C | Single binary, no runtime |
| Config | TOML | Human-readable, familiar |
| Transport | SSH (native) | Works everywhere, secure by default |
| Agent protocol | Custom binary over TCP | Minimal overhead |
| Web UI | HTML + vanilla JS | No build step, no framework |
| TUI | Notcurses | C native, modern (Unicode, 24-bit color, pixel graphics) |
| Storage | SQLite (embedded) | No external DB |

**Single binary, three interfaces:** CLI, TUI, and Web UI all ship in the same C binary. No separate installations, no language runtimes.

---

## Configuration

`cluster.toml` — cluster definition:

```toml
[cluster]
name         = "home-lab"
coordinator  = "localhost"
log_dir      = "~/.shard/logs"

# Home machines
[[nodes]]
name    = "main-laptop"
host    = "192.168.1.10"
user    = "yavuz"
tags    = ["gpu", "fast"]
cpu     = 8
ram_gb  = 16

[[nodes]]
name    = "old-desktop"
host    = "192.168.1.15"
user    = "yavuz"
tags    = ["cpu"]
cpu     = 2
ram_gb  = 4

# Cloud machines
[[nodes]]
name    = "ec2-worker-1"
host    = "ec2-13-58-...compute.amazonaws.com"
user    = "ubuntu"
key     = "~/.ssh/aws-key.pem"
tags    = ["cloud", "cpu"]
cpu     = 4
ram_gb  = 8

[[nodes]]
name    = "gcp-worker"
host    = "35.192.10.20"
user    = "yavuz"
key     = "~/.ssh/gcp-key"
tags    = ["cloud", "gpu"]
cpu     = 8
ram_gb  = 32
```

`tasks.toml` — task definitions (optional, for reusable jobs):

```toml
[[task]]
name        = "build-all-platforms"
description = "Build atomik-ssg for Linux, macOS, Windows"
strategy    = "distribute"

  [[task.step]]
  name = "linux"
  cmd  = "make TARGET=linux"
  on   = ["cpu"]

  [[task.step]]
  name = "macos"
  cmd  = "make TARGET=macos"
  on   = ["main-laptop"]

  [[task.step]]
  name = "windows"
  cmd  = "make TARGET=windows"
  on   = ["cpu"]

[[task]]
name        = "run-simulations"
description = "1000 physics simulations"
strategy    = "map"
count       = 1000
cmd         = "./simulate --seed {index}"
```

---

## CLI Interface

```bash
# Cluster management
shard cluster init                 # create default cluster.toml
shard cluster add-node <name>      # interactive add
shard cluster list                 # show all nodes
shard cluster health               # ping all nodes
shard cluster remove <name>

# Task execution
shard exec "command" --on <tag|node|all>
shard exec-script file.sh --distribute
shard run <task-name>              # from tasks.toml
shard map "cmd {i}" --count 100    # parallel map

# Monitoring
shard status                       # active tasks
shard logs <task-id>               # task output
shard history                      # past runs

# UI
shard ui                           # start web dashboard
shard tui                          # terminal UI

# Agent management
shard agent install <node>         # install agent on remote
shard agent update                 # update all agents
shard agent status
```

---

## Web UI

**Design principles:** No build step, no framework, no npm install. Pure HTML + CSS + vanilla JS served directly from the C binary. Loads in one round-trip.

Start with:
```bash
shard ui
```
Opens the dashboard at `http://localhost:8787`.

**Views:**

**1. Cluster Overview**
```
┌───────────────────────────────────────────────┐
│  shard — home-lab                    ⚙ Menu │
├───────────────────────────────────────────────┤
│  Nodes (4)                                    │
│                                               │
│  ● main-laptop      8 CPU  16 GB   [gpu,fast] │
│    ▓▓▓▓▓░░░░  45% CPU   ▓▓▓░░░░░  22% RAM     │
│                                               │
│  ● old-desktop      2 CPU   4 GB   [cpu]      │
│    ░░░░░░░░░░   3% CPU   ▓░░░░░░░  10% RAM    │
│                                               │
│  ● ec2-worker-1     4 CPU   8 GB   [cloud]    │
│    ▓▓▓▓▓▓▓░░░  71% CPU   ▓▓▓▓░░░░  50% RAM    │
│                                               │
│  ○ gcp-worker       offline                   │
└───────────────────────────────────────────────┘
```

**2. Active Tasks**
```
┌───────────────────────────────────────────────┐
│  Running Tasks (2)                            │
├───────────────────────────────────────────────┤
│                                               │
│  build-all-platforms                          │
│  ┌───────────────────────────────────────┐    │
│  │  linux    [main-laptop]  ▓▓▓▓▓░ 82%   │    │
│  │  macos    [main-laptop]  ▓▓▓░░░ 45%   │    │
│  │  windows  [ec2-worker]   ▓░░░░░ 12%   │    │
│  └───────────────────────────────────────┘    │
│                                               │
│  run-simulations                              │
│  Progress: 247 / 1000 (24.7%)                 │
│  ETA: 18 min · 4 nodes active                 │
└───────────────────────────────────────────────┘
```

**3. Task Details / Live Logs**
```
┌───────────────────────────────────────────────┐
│  Task: build-all-platforms/linux              │
│  Node: main-laptop                            │
│  Duration: 2m 14s · Status: Running           │
├───────────────────────────────────────────────┤
│  $ make TARGET=linux                          │
│  gcc -O2 -o build/linux/atomik-ssg ...        │
│  gcc: compiling src/main.c                    │
│  gcc: compiling src/parser.c                  │
│  gcc: compiling src/render.c                  │
│  linking build/linux/atomik-ssg               │
│  ...                                          │
└───────────────────────────────────────────────┘
```

**4. History & Analytics**
- Timeline of past runs
- Per-node utilization graphs
- Slowest / fastest nodes
- Failure rates

---

## Terminal UI (TUI)

Built with **Notcurses** — a modern C library that goes beyond ncurses with Unicode, 24-bit color, and pixel-level graphics. Ships in the same binary as the CLI:

```bash
shard tui
```

For headless use, SSH sessions, or terminal lovers:

```
┌─ shard ─────────────────────────────────────┐
│                                               │
│  NODES              STATUS      LOAD          │
│  ● main-laptop      online      45% CPU       │
│  ● old-desktop      online       3% CPU       │
│  ● ec2-worker-1     online      71% CPU       │
│  ○ gcp-worker       offline     ---           │
│                                               │
│  ─────────────────────────────────────────    │
│                                               │
│  ACTIVE TASKS                                 │
│                                               │
│  build-all-platforms                          │
│    linux    82% ████████████████░░░           │
│    macos    45% █████████░░░░░░░░░░           │
│    windows  12% ██░░░░░░░░░░░░░░░░░           │
│                                               │
│  ─────────────────────────────────────────    │
│                                               │
│  [q]uit  [l]ogs  [n]ew task  [r]efresh        │
└───────────────────────────────────────────────┘
```

---

## How Distribution Strategies Work

### Strategy 1: Broadcast
Run the same command on all matching nodes.
```bash
shard exec "apt update" --on all
```

### Strategy 2: Distribute
Split work across nodes based on capacity.
```bash
shard exec-script tests.sh --distribute
# Node A gets tests 1-100
# Node B gets tests 101-200
# Node C gets tests 201-250
```

### Strategy 3: Map
Run the same command N times in parallel, distributed across nodes.
```bash
shard map "python simulate.py --seed {i}" --count 1000
```

### Strategy 4: Pipeline
Sequential steps, each on different nodes.
```bash
shard pipeline \
  --step "download.sh" --on cloud \
  --step "process.sh" --on gpu \
  --step "upload.sh" --on cloud
```

### Strategy 5: Function Distribution (Python)
```python
from shard import distribute

@distribute(on="gpu")
def train_model(seed):
    # this runs on a GPU node
    return model.fit(seed=seed)

results = [train_model(s) for s in range(100)]
```

---

## Cloud Integration

shard doesn't lock you into any cloud. You bring your own machines:

**AWS EC2**
```toml
[[nodes]]
name = "ec2-worker"
host = "ec2-....amazonaws.com"
user = "ubuntu"
key  = "~/.ssh/aws.pem"
```

**Google Cloud**
```toml
[[nodes]]
name = "gcp-worker"
host = "34.102.55.10"
user = "yavuz"
key  = "~/.ssh/gcp"
```

**Hetzner, DigitalOcean, Vultr, self-hosted, home lab**
Same TOML, different IP. That's it.

**Optional cloud helpers** (v0.3):
```bash
shard cloud aws create --type t3.medium --count 3
shard cloud aws destroy --tag temporary
```

---

## Roadmap

### v0.1 — MVP ✅ done
- [x] SSH-based agentless mode
- [x] Cluster TOML parsing
- [x] Basic `exec` command with broadcast + distribute
- [x] CLI with node health check
- [x] Simple logging to files

### v0.2 — Web UI ✅ done
- [x] HTTP server in the controller
- [x] Cluster overview page
- [x] Live task monitoring
- [x] Log viewer
- [x] Node resource metrics

### v0.3 — Agent Mode (deferred — last)
- [ ] C agent binary
- [ ] Custom TCP protocol
- [ ] Task queue on each node
- [ ] Auto-reconnect
- [ ] Health monitoring

### v0.4 — TUI ✅ done
- [x] Terminal dashboard *(hand-written termios + ANSI, no Notcurses dependency — keeps the single-binary, zero-dependency promise)*
- [x] Keyboard-driven navigation *(read-only by design; work is started from the CLI)*
- [x] Live log tailing in the detail view
- [x] 24-bit colour node status indicators

### v0.5 — Advanced Scheduling (partly done)
- [x] Map/pipeline strategies
- [x] Retry logic
- [x] Dynamic work dispatch — items handed out as machines finish, so a slow machine takes less instead of holding up the run
- [x] File shipping (`--with`) and inter-step artifact carrying (`produces`)
- [x] Priority queues — `shard queue add/list/run`, priority-ordered, persistent
- [x] Resource-aware placement — `--balance` measures live load; drops machines that do not answer

### v1.0 — Cloud Helpers (2 months)
- [ ] AWS EC2 provisioning
- [ ] GCP integration
- [ ] Cost tracking
- [ ] Auto-scaling policies

---

## Who Is This For?

**Primary users:**
- Developers with home lab / spare machines
- Small teams wanting to parallelize CI without Kubernetes
- Researchers running many simulations
- Video/render workflows
- Data pipeline builders

**Not for:**
- Production Kubernetes replacements
- Real-time low-latency systems (use nats.io, etc.)
- ML training clusters (use Ray/Horovod)

shard is best for **medium-scale batch work across heterogeneous machines**.

---

## Comparison

| Feature | Kubernetes | Ray | Coiled | **shard** |
|---|---|---|---|---|
| Learning curve | Very high | Medium | Low | Low |
| Setup time | Days | Hours | Minutes | Minutes |
| Home + cloud mixing | Complex | Possible | ❌ | ✅ |
| Zero dependencies | ❌ | ❌ | ❌ | ✅ |
| Single binary | ❌ | ❌ | ❌ | ✅ |
| Language | Any | Python | Python | Any (shell) + Python |
| Web UI | Complex | Yes | Yes | Yes (lightweight) |
| Terminal UI | ❌ | ❌ | ❌ | ✅ |
| Free & self-host | ✅ | ✅ | Partial | ✅ |

---

## Revenue Model

**Open source core** (MIT license):
- shard agent
- CLI
- Web UI
- TUI
- Basic cloud integration

**shard Cloud** (SaaS, $19-99/month):
- Hosted coordinator (no need to keep your laptop on)
- Multi-user teams
- Cluster analytics & cost tracking
- Managed cloud provisioning
- Email/Slack alerts

**Enterprise support**:
- Custom integrations
- On-prem installation help
- SLA-backed support
- $500-2000/month per organization

**Freelance opportunities**:
- Custom cluster setup ($500-2000 one-time)
- Training / consulting ($100/hour)
- Custom strategy implementation

---

## Part of the Atomik Ecosystem

```
atomik-ssg    → static site generator
atm-wcet      → WCET analysis
shard       → distributed task runner (this)
...
```

**Shared philosophy:** fast, small, no runtime dependencies, works everywhere.

---

## Getting Started (Planned)

```bash
# Install
brew install yavuzselimsahin/atomik/shard

# Or from source
git clone https://github.com/yavuzselimsahin/shard
cd shard && make

# Initialize
shard cluster init

# Edit cluster.toml to add your machines

# Run something
shard exec "uname -a" --on all
```

---

## License

MIT

---

## Author

**Yavuz Selim Şahin**
Atomik Software Engineering — [atomik.software](https://atomik.software)
