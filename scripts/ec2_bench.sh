#!/bin/bash
# EC2 benchmark lifecycle: launch, build, run, terminate.
#
# Usage:
#   ./scripts/ec2_bench.sh launch      — spin up instance, install deps, isolate cores
#   ./scripts/ec2_bench.sh build       — rsync repo and build on instance
#   ./scripts/ec2_bench.sh bench [N] [binary]
#       — run benchmark N times (default 10) on build/<binary> (default server_cached).
#       binary: server_cached | server_nocached | server (alias for server_cached)
#   ./scripts/ec2_bench.sh terminate   — terminate instance
#
# One-time setup:
#   1. Fill in CONFIG below.
#   2. Ensure AWS CLI is configured (aws configure).
#   3. Run in order: launch → build → bench → terminate

set -euo pipefail

# ── CONFIG ───────────────────────────────────────────────────────────────────
KEY_NAME="lobook-bench"                # EC2 key pair name (without .pem)
KEY_PATH="$HOME/.ssh/${KEY_NAME}.pem"  # Path to .pem file
SECURITY_GROUP="sg-0af378a950e0e5446"  # Must allow inbound SSH (port 22)
INSTANCE_TYPE="c5.xlarge"             # c5.metal for bare metal
REGION="us-west-2"
# Amazon Linux 2 (x86_64) — update if stale:
AMI_ID="ami-0534a0fd33c655746"
# ─────────────────────────────────────────────────────────────────────────────

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
INSTANCE_ID_FILE="$REPO_DIR/.ec2_instance_id"
REMOTE_DIR="/home/ec2-user/lobook"
REMOTE_USER="ec2-user"

get_ip() {
    local id
    id=$(cat "$INSTANCE_ID_FILE")
    aws ec2 describe-instances \
        --instance-ids "$id" \
        --region "$REGION" \
        --query "Reservations[0].Instances[0].PublicIpAddress" \
        --output text
}

wait_for_ssh() {
    local ip=$1
    echo "Waiting for SSH on $ip..."
    until ssh -o ConnectTimeout=5 -o StrictHostKeyChecking=no \
              -i "$KEY_PATH" "$REMOTE_USER@$ip" "true" 2>/dev/null; do
        sleep 5
    done
    echo "SSH ready."
}

cmd_launch() {
    echo "=== Launching $INSTANCE_TYPE in $REGION ==="
    INSTANCE_ID=$(aws ec2 run-instances \
        --image-id "$AMI_ID" \
        --instance-type "$INSTANCE_TYPE" \
        --key-name "$KEY_NAME" \
        --security-group-ids "$SECURITY_GROUP" \
        --region "$REGION" \
        --instance-market-options '{"MarketType":"spot"}' \
        --tag-specifications "ResourceType=instance,Tags=[{Key=Name,Value=lobook-bench}]" \
        --query "Instances[0].InstanceId" \
        --output text)

    echo "$INSTANCE_ID" > "$INSTANCE_ID_FILE"
    echo "Instance: $INSTANCE_ID"

    echo "Waiting for instance to reach running state..."
    aws ec2 wait instance-running --instance-ids "$INSTANCE_ID" --region "$REGION"

    IP=$(get_ip)
    echo "IP: $IP"
    wait_for_ssh "$IP"

    echo "=== Installing build deps ==="
    ssh -o StrictHostKeyChecking=no -i "$KEY_PATH" "$REMOTE_USER@$IP" \
        "sudo yum install -y cmake3 gcc-c++ make git 2>&1 | tail -5"

    echo "=== Configuring kernel isolation (will reboot) ==="
    scp -o StrictHostKeyChecking=no -i "$KEY_PATH" \
        "$REPO_DIR/scripts/setup_isolated.sh" "$REMOTE_USER@$IP:/tmp/"
    ssh -o StrictHostKeyChecking=no -i "$KEY_PATH" "$REMOTE_USER@$IP" \
        "sudo bash /tmp/setup_isolated.sh setup" || true  # exits 0 after reboot trigger

    echo "Waiting for reboot..."
    sleep 30
    wait_for_ssh "$IP"

    echo "=== Verifying isolation ==="
    ssh -o StrictHostKeyChecking=no -i "$KEY_PATH" "$REMOTE_USER@$IP" \
        "sudo bash /tmp/setup_isolated.sh verify"

    echo ""
    echo "Instance ready. Run: ./scripts/ec2_bench.sh build"
}

cmd_build() {
    IP=$(get_ip)
    echo "=== Syncing repo to $IP ==="
    rsync -az --exclude=".git" --exclude="build" \
        -e "ssh -o StrictHostKeyChecking=no -i $KEY_PATH" \
        "$REPO_DIR/" "$REMOTE_USER@$IP:$REMOTE_DIR/"

    echo "=== Building ==="
    ssh -o StrictHostKeyChecking=no -i "$KEY_PATH" "$REMOTE_USER@$IP" bash <<EOF
        cd $REMOTE_DIR
        mkdir -p build && cd build
        cmake3 .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -5
        make -j\$(nproc) server_cached server_nocached server
        echo "Build done."
EOF
    echo "Ready. Run: ./scripts/ec2_bench.sh bench"
}

cmd_bench() {
    local N=${1:-10}
    local name=${2:-server_cached}
    case "$name" in
        server|server_cached) name="server_cached" ;;
        server_nocached)      name="server_nocached" ;;
        *)
            echo "Unknown binary: $2 (use server_cached, server_nocached, or server)" >&2
            exit 1
            ;;
    esac
    IP=$(get_ip)
    echo "=== Running $name $N times on $IP ==="
    ssh -o StrictHostKeyChecking=no -i "$KEY_PATH" "$REMOTE_USER@$IP" \
        "bash $REMOTE_DIR/scripts/bench_runs.sh $N $REMOTE_DIR/build/$name"
}

cmd_terminate() {
    local id
    id=$(cat "$INSTANCE_ID_FILE")
    echo "Terminating $id..."
    aws ec2 terminate-instances --instance-ids "$id" --region "$REGION"
    rm -f "$INSTANCE_ID_FILE"
    echo "Done."
}

case "${1:-}" in
    launch)    cmd_launch ;;
    build)     cmd_build ;;
    bench)     cmd_bench "${2:-10}" "${3:-}" ;;
    terminate) cmd_terminate ;;
    *)
        echo "Usage: $0 {launch|build|bench [N] [server_cached|server_nocached]|terminate}"
        exit 1
        ;;
esac
