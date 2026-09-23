# AWS test deployment

This is the recommended AWS shape for the one-month Jubi VPS test.

## Network

Use an Ubuntu 24.04 LTS EC2 instance in a normal VPC. The Security Group should
allow inbound SSH (TCP 22) only from your own current public IP. Do not create an
inbound rule for 8877 or Ollama 11434.

Jubi stays on `127.0.0.1:8877`. Ollama stays on
`127.0.0.1:11434`. Access the dashboard with an SSH tunnel from your PC:

```bash
ssh -i your-key.pem -N -L 8877:127.0.0.1:8877 ubuntu@EC2_PUBLIC_IP
```

Then browse to `http://127.0.0.1:8877`.

## Instance sizing

Start with a CPU instance for control-plane testing: dashboard, persistence,
research, routing, approvals and lightweight local inference. For larger local
models, choose an instance with enough RAM or a GPU instance and run Ollama on
that same host. Do not run an expensive GPU continuously just to keep the Jubi
dashboard online.

Use at least 80 GiB of EBS if you plan to keep the full source bundle and
multiple Ollama models. Model files can consume far more disk than Jubi itself.

## Installation

```bash
sudo apt-get update
sudo apt-get install -y git
git clone https://github.com/kautukade/JUBI.git
cd JUBI
git checkout feature/vps-runtime
sudo bash deploy/vps/install.sh --with-hermes --with-browser --start
```

Install Ollama separately using the method you approve. If Ollama is installed
as `ollama.service`, copy the supplied loopback override:

```bash
sudo mkdir -p /etc/systemd/system/ollama.service.d
sudo cp /opt/jubi/deploy/vps/ollama-loopback.conf /etc/systemd/system/ollama.service.d/jubi-loopback.conf
sudo systemctl daemon-reload
sudo systemctl restart ollama
```

Pull models only through an explicit provisioning action. For the compact full-feature test profile:

```bash
sudo /opt/jubi/deploy/vps/provision-models.sh --recommended
sudo systemctl restart jubi
sudo /opt/jubi/deploy/vps/verify.sh
```

This profile uses `qwen3:8b` for general/coding/tool work, `qwen2.5vl:3b`
for vision, and `qwen3-embedding:0.6b` for semantic memory.

## Verify

```bash
sudo /opt/jubi/deploy/vps/verify.sh
sudo systemctl status jubi
sudo journalctl -u jubi --since "10 minutes ago"
```

The verification script fails if Jubi is listening on a wildcard address.

## Cost control

Use AWS Budgets/billing alerts and stop or terminate GPU instances when they are
not needed. EBS volumes and public IPv4 addresses may continue to generate
charges even when compute is stopped, so inspect the AWS bill rather than
assuming a stopped instance costs nothing.

Before deleting the test VPS:

```bash
sudo /opt/jubi/deploy/vps/backup.sh
```

Copy the resulting archive off the instance before termination.
