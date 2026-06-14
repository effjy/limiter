// speed_limit.c - Limit combined upload+download speed on wlo1
// Compile: gcc -o speed_limit speed_limit.c
// Run: sudo ./speed_limit <total_kb_per_sec>
// Example: sudo ./speed_limit 500   (500 KB/s total = ~4 Mbps)
//
// Service mode (applies the limit automatically at every boot):
//   sudo ./speed_limit add-service <total_kb_per_sec>
//   sudo ./speed_limit remove-service

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define IFACE        "wlo1"
#define SERVICE_NAME "speed-limit.service"
#define SERVICE_PATH "/etc/systemd/system/" SERVICE_NAME

void run_cmd(const char *cmd, int ignore_error) {
    int ret = system(cmd);
    if (ret != 0 && !ignore_error) {
        fprintf(stderr, "Error executing: %s\n", cmd);
        exit(1);
    }
}

// Write a systemd oneshot unit that reapplies the tc limit at boot.
// Description matches the GUI's format so both tools stay compatible.
static int add_service(int total_kb) {
    int half_kb = total_kb / 2;
    if (half_kb < 1) half_kb = 1;
    int kbps = half_kb * 8;
    int burst_bytes = (kbps * 1000 / 8) / 10;
    if (burst_bytes < 16000) burst_bytes = 16000;

    FILE *fp = fopen(SERVICE_PATH, "w");
    if (!fp) {
        fprintf(stderr, "Could not write %s. Run as root (sudo).\n", SERVICE_PATH);
        return 1;
    }

    fprintf(fp,
        "[Unit]\n"
        "Description=Network Speed Limiter (%d KB/s total on %s)\n"
        "After=network-online.target\n"
        "Wants=network-online.target\n"
        "\n"
        "[Service]\n"
        "Type=oneshot\n"
        "RemainAfterExit=yes\n"
        "ExecStartPre=-/bin/sh -c 'tc qdisc del dev %s root 2>/dev/null'\n"
        "ExecStartPre=-/bin/sh -c 'tc qdisc del dev %s ingress 2>/dev/null'\n"
        "ExecStart=/bin/sh -c 'tc qdisc add dev %s root handle 1: htb default 10'\n"
        "ExecStart=/bin/sh -c 'tc class add dev %s parent 1: classid 1:1 htb rate %dKbit ceil %dKbit'\n"
        "ExecStart=/bin/sh -c 'tc class add dev %s parent 1:1 classid 1:10 htb rate %dKbit ceil %dKbit'\n"
        "ExecStart=/bin/sh -c 'tc filter add dev %s parent 1: protocol ip prio 1 u32 match ip src 0.0.0.0/0 flowid 1:10'\n"
        "ExecStart=/bin/sh -c 'tc qdisc add dev %s handle ffff: ingress'\n"
        "ExecStart=/bin/sh -c 'tc filter add dev %s parent ffff: protocol ip u32 match u32 0 0 police rate %dKbit burst %dKb drop flowid :1'\n"
        "ExecStop=-/bin/sh -c 'tc qdisc del dev %s root 2>/dev/null'\n"
        "ExecStop=-/bin/sh -c 'tc qdisc del dev %s ingress 2>/dev/null'\n"
        "\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n",
        total_kb, IFACE,
        IFACE, IFACE,
        IFACE,
        IFACE, kbps, kbps,
        IFACE, kbps, kbps,
        IFACE,
        IFACE,
        IFACE, kbps, burst_bytes / 1000,
        IFACE, IFACE);

    fclose(fp);

    run_cmd("systemctl daemon-reload", 1);
    run_cmd("systemctl enable " SERVICE_NAME " 2>/dev/null", 1);
    run_cmd("systemctl start " SERVICE_NAME, 1);

    printf("\xe2\x9c\x93 Installed as a service.\n");
    printf("%d KB/s on %s will be applied automatically at boot.\n",
           total_kb, IFACE);
    return 0;
}

static int remove_service(void) {
    run_cmd("systemctl disable --now " SERVICE_NAME " 2>/dev/null", 1);
    if (remove(SERVICE_PATH) != 0) {
        // Either it didn't exist or we lack permission.
        if (access(SERVICE_PATH, F_OK) == 0) {
            fprintf(stderr, "Could not remove %s. Run as root (sudo).\n",
                    SERVICE_PATH);
            return 1;
        }
        printf("No service was installed.\n");
        return 0;
    }
    run_cmd("systemctl daemon-reload", 1);
    printf("Service removed. The limit will no longer start at boot.\n");
    return 0;
}

void cleanup(int sig __attribute__((unused))) {
    printf("\n\nRemoving speed limit...\n");
    run_cmd("tc qdisc del dev wlo1 root 2>/dev/null", 1);
    run_cmd("tc qdisc del dev wlo1 ingress 2>/dev/null", 1);
    printf("Speed limit removed. Exiting.\n");
    exit(0);
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s <total_speed_kB_per_sec>      Apply a limit (Ctrl+C to remove)\n", prog);
    fprintf(stderr, "  %s add-service <total_kB_per_sec> Install a boot-time service\n", prog);
    fprintf(stderr, "  %s remove-service                Remove the boot-time service\n", prog);
    fprintf(stderr, "Example: %s 500   (500 KB/s combined up+down)\n", prog);
}

int main(int argc, char *argv[]) {
    if (argc == 2 && strcmp(argv[1], "remove-service") == 0) {
        return remove_service();
    }
    if (argc == 3 && strcmp(argv[1], "add-service") == 0) {
        int kb = atoi(argv[2]);
        if (kb <= 0) {
            fprintf(stderr, "Speed must be > 0\n");
            return 1;
        }
        return add_service(kb);
    }

    if (argc != 2) {
        usage(argv[0]);
        return 1;
    }

    int total_kb = atoi(argv[1]);
    if (total_kb <= 0) {
        fprintf(stderr, "Speed must be > 0\n");
        return 1;
    }

    // Convert KB/s to kbps (kilobits per second) for tc
    // 1 KB/s = 8 kbps
    int half_kb = total_kb / 2;
    if (half_kb < 1) half_kb = 1;

    int upload_kbps = half_kb * 8;
    int download_kbps = half_kb * 8;

    printf("Limiting %s:\n", IFACE);
    printf("  Upload:   %d KB/s (%d kbps)\n", half_kb, upload_kbps);
    printf("  Download: %d KB/s (%d kbps)\n", half_kb, download_kbps);
    printf("  Combined: %d KB/s\n", half_kb * 2);

    // Set up signal handler for clean exit
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);

    // Clean previous qdiscs (ignore errors if they don't exist)
    run_cmd("tc qdisc del dev wlo1 root 2>/dev/null", 1);
    run_cmd("tc qdisc del dev wlo1 ingress 2>/dev/null", 1);

    // --- Egress (upload) limit ---
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "tc qdisc add dev %s root handle 1: htb default 10", IFACE);
    run_cmd(cmd, 0);

    snprintf(cmd, sizeof(cmd),
             "tc class add dev %s parent 1: classid 1:1 htb rate %dKbit ceil %dKbit",
             IFACE, upload_kbps, upload_kbps);
    run_cmd(cmd, 0);

    snprintf(cmd, sizeof(cmd),
             "tc class add dev %s parent 1:1 classid 1:10 htb rate %dKbit ceil %dKbit",
             IFACE, upload_kbps, upload_kbps);
    run_cmd(cmd, 0);

    // Add a filter to direct all traffic to class 1:10
    snprintf(cmd, sizeof(cmd),
             "tc filter add dev %s parent 1: protocol ip prio 1 u32 match ip src 0.0.0.0/0 flowid 1:10",
             IFACE);
    run_cmd(cmd, 0);

    // --- Ingress (download) limit using police ---
    snprintf(cmd, sizeof(cmd),
             "tc qdisc add dev %s handle ffff: ingress", IFACE);
    run_cmd(cmd, 0);

    // Calculate burst size (10% of rate in bytes, minimum 16KB)
    int burst_bytes = (download_kbps * 1000 / 8) / 10;  // 100ms of traffic
    if (burst_bytes < 16000) burst_bytes = 16000;
    
    snprintf(cmd, sizeof(cmd),
             "tc filter add dev %s parent ffff: protocol ip u32 match u32 0 0 "
             "police rate %dKbit burst %dKb drop flowid :1",
             IFACE, download_kbps, burst_bytes / 1000);
    run_cmd(cmd, 0);

    printf("\n✓ Speed limit active successfully!\n");
    printf("Press Ctrl+C to remove limit and exit.\n\n");

    // Wait for user interrupt
    while (1) {
        sleep(10);
    }

    return 0;
}
